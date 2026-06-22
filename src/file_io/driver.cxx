module;
#include <cerrno>
#include <liburing.h>

export module conflux.file_io.driver;

import std;
import conflux.types;
import conflux.work;
import conflux.work.uring_executor;
export import conflux.uring.completion;
export import conflux.file_io.reader;

namespace conflux::file_io {

// ---------------------------------------------------------------------------
// Thread-local FileReader registration.
//
// Each ring runs on a dedicated std::thread; the ring's FileReader is installed at
// run_loop entry and cleared at exit. Handlers that live inside the router
// (no ring context of their own) look up the current reader to decide between
// async uring paths and synchronous fallbacks.
// ---------------------------------------------------------------------------

namespace driver_detail {

inline thread_local FileReader *tls_current_reader{nullptr};
inline thread_local bool tls_blocking_pump_forbidden{false};

} // namespace driver_detail
export FileReader *current_file_reader() noexcept {
	return driver_detail::tls_current_reader;
}
export enum class BlockingPumpPolicy {
	allow,
	forbid,
};
export struct CurrentFileReaderScopeOptions {
	BlockingPumpPolicy blocking_pump = BlockingPumpPolicy::allow;
};
export class CurrentFileReaderScope {
	FileReader *prev_;
	bool prev_blocking_pump_forbidden_;

public:
	explicit CurrentFileReaderScope(
		FileReader *next,
		CurrentFileReaderScopeOptions options = {}) noexcept
		: prev_{driver_detail::tls_current_reader}
		, prev_blocking_pump_forbidden_{driver_detail::tls_blocking_pump_forbidden} {
		driver_detail::tls_current_reader = next;
		driver_detail::tls_blocking_pump_forbidden = options.blocking_pump == BlockingPumpPolicy::forbid;
	}
	~CurrentFileReaderScope() {
		driver_detail::tls_current_reader = prev_;
		driver_detail::tls_blocking_pump_forbidden = prev_blocking_pump_forbidden_;
	}
	CurrentFileReaderScope(CurrentFileReaderScope const &) = delete;
	CurrentFileReaderScope &operator =(CurrentFileReaderScope const &) = delete;
	CurrentFileReaderScope(CurrentFileReaderScope &&) = delete;
	CurrentFileReaderScope &operator =(CurrentFileReaderScope &&) = delete;
};
// ---------------------------------------------------------------------------
// Single-std::thread io_uring driver: pump_until + block_on.
//
// Tests and examples all rolled their own submit/wait_cqe/dispatch loop.
// These primitives factor out that loop and the Flow→std::atomic_flag plumbing.
// HTTP server keeps its own driver because it shares the ring with non-
// file_io ops (Op-tagged user_data); this helper assumes the ring is owned
// solely by FileReader and uses the default 32:32 ud layout unless a
// caller-provided decoder says otherwise.
// ---------------------------------------------------------------------------

export struct DefaultUdDecoder {
	std::pair<std::uint32_t, std::uint32_t> operator ()(
		std::uint64_t ud) const noexcept {
		return {static_cast<std::uint32_t>(ud & 0xFFFFFFFFU), static_cast<std::uint32_t>(ud >> 32U)};
	}
};
export struct PumpTimeout final : std::runtime_error {
	PumpTimeout()
		: std::runtime_error{"conflux.file_io: pump_until budget exhausted"} {}
};
export template<typename Decode = DefaultUdDecoder>
void pump_until(
	FileReader &reader,
	std::atomic_flag const &done,
	std::optional<std::chrono::milliseconds> budget = std::nullopt,
	Decode decode = {}) {
	if (driver_detail::tls_blocking_pump_forbidden) {
		throw std::logic_error{"conflux.file_io: blocking pump is not allowed on this executor thread"};
	}
	auto *ring = reader.ring();
	auto *completions = reader.completions();
	auto const deadline = budget ? std::make_optional(std::chrono::steady_clock::now() + *budget) : std::nullopt;
	while (!done.test(std::memory_order_acquire)) {
		::io_uring_cqe *cqe = nullptr;
		int rc = 0;
		if (deadline) {
			auto const now = std::chrono::steady_clock::now();
			if (now >= *deadline) {
				throw PumpTimeout{};
			}
			auto const remaining_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now);
			auto const wait_ns =
				std::min(remaining_ns, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds{1}));
			__kernel_timespec ts{
				.tv_sec = static_cast<__kernel_time64_t>(wait_ns.count() / 1'000'000'000),
				.tv_nsec = wait_ns.count() % 1'000'000'000};
			rc = ::io_uring_submit_and_wait_timeout(ring, &cqe, 1, &ts, nullptr);
			if (rc == -ETIME) {
				if (std::chrono::steady_clock::now() >= *deadline) {
					throw PumpTimeout{};
				}
				continue;
			}
		} else {
			rc = ::io_uring_submit_and_wait(ring, 1);
			if (rc >= 0) {
				rc = ::io_uring_peek_cqe(ring, &cqe);
			}
		}
		if (rc == -EINTR || rc == -EAGAIN) {
			continue;
		}
		// io_uring_submit_and_wait may report submitted SQEs while no CQE is
		// immediately visible to peek_cqe yet. Treat as transient and keep
		// pumping instead of surfacing a hard failure.
		if (rc >= 0 && cqe == nullptr) {
			continue;
		}
		if (rc < 0 || cqe == nullptr) {
			throw std::runtime_error{std::format("conflux.file_io: submit_and_wait rc={}", rc)};
		}
		std::array<::io_uring_cqe *, 32> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(ring, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				auto const *c = batch[static_cast<std::size_t>(i)];
				auto [slot, gen] = decode(c->user_data);
				completions->dispatch(slot, gen, c->res, conflux::uring::CqeFlags{c->flags});
			}
			::io_uring_cq_advance(ring, n);
			if (done.test(std::memory_order_acquire)) {
				break;
			}
		}
	}
}
export template<typename T, typename Decode = DefaultUdDecoder>
T block_on(
	FileReader &reader,
	conflux::work::root::Task<T> task,
	std::optional<std::chrono::milliseconds> budget = std::nullopt,
	Decode decode = {}) {
	using namespace conflux::work::root;
	struct Slot {
		std::atomic_flag done{};
		std::exception_ptr err{};
		[[no_unique_address]] std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>> value{};
	};
	auto slot = std::make_shared<Slot>();
	auto jh = std::shared_ptr<TaskJoinHandle<T>>{new TaskJoinHandle<T>(into_join_handle(std::move(task)))};
	jh->control().set_on_ready_or_run([slot, jh]() noexcept {
		try {
			auto outcome = blocking_join(std::move(*jh));
			if (outcome.is_failure()) {
				slot->err = std::move(outcome).failure().error;
			} else if (outcome.is_cancelled()) {
				slot->err = std::make_exception_ptr(conflux::work::Cancelled{});
			} else if constexpr (!std::is_void_v<T>) {
				slot->value.emplace(std::move(outcome).success().value);
			}
		} catch (...) { slot->err = std::current_exception(); }
		slot->done.test_and_set(std::memory_order_release);
	});
	pump_until(reader, slot->done, budget, std::move(decode));
	if (slot->err) {
		std::rethrow_exception(slot->err);
	}
	if constexpr (!std::is_void_v<T>) {
		return std::move(*slot->value);
	}
}
export template<typename T, typename Decode = DefaultUdDecoder>
T block_on(
	FileReader &reader,
	conflux::work::root::JoinTask<T> task,
	std::optional<std::chrono::milliseconds> budget = std::nullopt,
	Decode decode = {}) {
	return block_on(reader, std::move(task).detach_to_task(), budget, std::move(decode));
}
template<class T>
concept file_io_root_task_result = requires {
	typename std::remove_cvref_t<T>::value_type;
} && std::same_as<std::remove_cvref_t<T>, conflux::work::root::Task<typename std::remove_cvref_t<T>::value_type>>;
template<class Fn>
using executor_file_io_result_t = std::invoke_result_t<std::decay_t<Fn> &, FileReader &>;
template<class Fn>
concept executor_file_io_task_body = file_io_root_task_result<executor_file_io_result_t<Fn>>;
template<class Fn>
using executor_file_io_value_t = typename std::remove_cvref_t<executor_file_io_result_t<Fn>>::value_type;
export template<class Fn>
	requires executor_file_io_task_body<Fn>
[[nodiscard]] auto with_current_file_reader(
	conflux::work::UringExecutorContext &ctx,
	Fn &&fn) -> conflux::work::root::Task<executor_file_io_value_t<Fn>> {
	using T = executor_file_io_value_t<Fn>;
	FileReader reader{ctx.ring().raw(), &ctx.completions(), ctx.user_data_encoder()};
	CurrentFileReaderScope const scope{
		&reader,
		CurrentFileReaderScopeOptions{.blocking_pump = BlockingPumpPolicy::forbid}};
	if constexpr (std::is_void_v<T>) {
		co_await std::invoke(std::forward<Fn>(fn), reader);
		co_return;
	} else {
		co_return co_await std::invoke(std::forward<Fn>(fn), reader);
	}
}
export template<class Fn>
	requires executor_file_io_task_body<Fn>
[[nodiscard]] auto async_file_io(
	conflux::work::UringExecutor &executor,
	Fn &&fn) -> conflux::work::root::Task<executor_file_io_value_t<Fn>> {
	using fn_t = std::decay_t<Fn>;
	using T = executor_file_io_value_t<Fn>;
	return executor.async_submit(
		[fn = fn_t{std::forward<Fn>(fn)}](conflux::work::UringExecutorContext &ctx) mutable
			-> conflux::work::root::Task<T> { return with_current_file_reader(ctx, fn); });
}

} // namespace conflux::file_io
