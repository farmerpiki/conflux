module;
#include <cerrno>
#include <cstdint>
#include <liburing.h>
#include <memory>

export module conflux.work.uring_executor;

import std;
import conflux.work;
import conflux.uring;
import conflux.uring.completion;

namespace conflux::work {

export class UringExecutorContext;
export class UringExecutor;

export struct UringExecutorOptions {
	unsigned ring_entries = 256;
	std::size_t completion_slots = 64;
	std::size_t max_submission_queue = 4096;
	std::size_t lane_drain_budget = 0;
	bool require_single_issuer = true;
};

export enum class UringExecutorState {
	starting,
	running,
	stopping,
	draining,
	stopped,
	failed,
};

namespace uring_executor_detail {

inline constexpr std::uint32_t kExecutorSlot = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t kWakeTag = 1;

[[nodiscard]] inline std::uint64_t pack_user_data(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}

[[nodiscard]] inline std::uint32_t user_data_slot(
	std::uint64_t value) noexcept {
	return static_cast<std::uint32_t>(value & 0xFFFFFFFFULL);
}

[[nodiscard]] inline std::uint32_t user_data_generation(
	std::uint64_t value) noexcept {
	return static_cast<std::uint32_t>(value >> 32U);
}

enum class SubmissionState : std::uint8_t {
	queued,
	starting,
	child_bound,
	terminal,
};

struct UringExecutorSharedState;
template<class T, class Fn>
struct Submission;

struct SubmissionBase {
	std::atomic<SubmissionState> record_state{SubmissionState::queued};
	std::atomic<bool> cancel_requested{false};
	std::atomic<root::CancelReason> cancel_reason{root::CancelReason::requested};
	std::atomic<bool> admitted{false};
	std::atomic<bool> admission_released{false};

	virtual ~SubmissionBase() = default;
	virtual void start(UringExecutorSharedState &state) noexcept = 0;
	virtual void cancel(root::CancelReason reason) noexcept = 0;
	virtual void cancel_shutdown() noexcept = 0;
	virtual void fail_enqueue(std::error_code ec, std::string_view message) noexcept = 0;
};

struct UringExecutorSharedState : std::enable_shared_from_this<UringExecutorSharedState> {
	UringExecutorOptions options{};
	std::atomic<UringExecutorState> state{UringExecutorState::starting};
	std::atomic<std::size_t> live_records{0};
	std::atomic<bool> joined{false};

	mutable std::mutex mutex{};
	std::condition_variable cv{};
	std::vector<std::weak_ptr<SubmissionBase>> records{};
	std::error_code startup_error{};
	std::error_code runtime_error{};
	bool startup_ready{false};

	::io_uring ring{};
	bool ring_initialized{false};
	std::atomic<int> ring_fd_value{-1};
	std::unique_ptr<conflux::uring::CompletionTable> completions{};
	std::unique_ptr<RingLane> lane{};
	std::thread::id owner_thread{};
	std::jthread thread{};

	enum class AdmitResult : std::uint8_t {
		admitted,
		stopped,
		full,
	};

	explicit UringExecutorSharedState(
		UringExecutorOptions opts)
		: options{opts} {}

	~UringExecutorSharedState() { join(); }

	[[nodiscard]] std::uint64_t encode(
		std::uint32_t slot,
		std::uint32_t gen) const {
		if (slot == kExecutorSlot) {
			throw std::out_of_range{"conflux.work.uring_executor: reserved completion slot"};
		}
		return pack_user_data(slot, gen);
	}

	[[nodiscard]] std::uint64_t wake_user_data() const noexcept { return pack_user_data(kExecutorSlot, kWakeTag); }

	void start() {
		auto self = shared_from_this();
		thread = std::jthread{[self](std::stop_token st) { self->run(st); }};
	}

	[[nodiscard]] std::error_code wait_started() {
		std::unique_lock lock{mutex};
		cv.wait(lock, [this] { return startup_ready; });
		return startup_error;
	}

	[[nodiscard]] bool on_owner_thread() const noexcept { return std::this_thread::get_id() == owner_thread; }

	[[nodiscard]] int ring_fd() const noexcept { return ring_fd_value.load(std::memory_order_acquire); }

	[[nodiscard]] bool stopped() const noexcept {
		auto const s = state.load(std::memory_order_acquire);
		return s == UringExecutorState::stopped || s == UringExecutorState::failed;
	}

	void stop() noexcept {
		auto expected = UringExecutorState::running;
		if (state.compare_exchange_strong(
				expected,
				UringExecutorState::stopping,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			cancel_all(root::CancelReason::shutdown);
			wake();
			return;
		}
		if (expected == UringExecutorState::starting) {
			state.store(UringExecutorState::stopping, std::memory_order_release);
		} else if (expected == UringExecutorState::stopping || expected == UringExecutorState::draining) {
			wake();
		}
	}

	void join() noexcept {
		bool expected = false;
		if (!joined.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
			return;
		}
		stop();
		if (thread.joinable() && std::this_thread::get_id() != owner_thread) {
			thread.join();
		}
	}

	[[nodiscard]] AdmitResult admit(
		std::shared_ptr<SubmissionBase> const &submission) {
		std::scoped_lock lock{mutex};
		if (state.load(std::memory_order_acquire) != UringExecutorState::running) {
			return AdmitResult::stopped;
		}
		if (live_records.load(std::memory_order_relaxed) >= options.max_submission_queue) {
			return AdmitResult::full;
		}
		live_records.fetch_add(1, std::memory_order_acq_rel);
		submission->admitted.store(true, std::memory_order_release);
		records.push_back(submission);
		return AdmitResult::admitted;
	}

	void release_admission(
		SubmissionBase &submission) noexcept {
		if (!submission.admitted.exchange(false, std::memory_order_acq_rel)) {
			return;
		}
		bool expected = false;
		if (submission.admission_released.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
			live_records.fetch_sub(1, std::memory_order_acq_rel);
		}
	}

	void wake() noexcept {
		try {
			if (lane != nullptr) {
				auto _ = lane->enqueue([] {});
			}
		} catch (...) {} // NOLINT(bugprone-empty-catch): wake is best-effort during shutdown notification.
		cv.notify_all();
	}

	void cancel_all(
		root::CancelReason reason) noexcept {
		std::vector<std::shared_ptr<SubmissionBase>> snapshot{};
		{
			std::scoped_lock lock{mutex};
			snapshot.reserve(records.size());
			std::erase_if(records, [&snapshot](std::weak_ptr<SubmissionBase> const &weak) {
				if (auto item = weak.lock()) {
					snapshot.push_back(std::move(item));
					return false;
				}
				return true;
			});
		}
		for (auto &record: snapshot) {
			record->cancel(reason);
		}
	}

	void publish_startup(
		UringExecutorState next,
		std::error_code ec = {}) noexcept {
		{
			std::scoped_lock lock{mutex};
			startup_error = ec;
			state.store(next, std::memory_order_release);
			startup_ready = true;
		}
		cv.notify_all();
	}

	[[nodiscard]] std::error_code init_ring() noexcept {
		if (options.ring_entries == 0 || options.max_submission_queue == 0) {
			return std::make_error_code(std::errc::invalid_argument);
		}

		::io_uring_params params{};
		params.flags = IORING_SETUP_SINGLE_ISSUER;
		int rc = ::io_uring_queue_init_params(options.ring_entries, &ring, &params);
		if (rc < 0 && !options.require_single_issuer) {
			params = {};
			rc = ::io_uring_queue_init_params(options.ring_entries, &ring, &params);
		}
		if (rc < 0) {
			return std::error_code{-rc, std::generic_category()};
		}
		ring_initialized = true;
		ring_fd_value.store(ring.ring_fd, std::memory_order_release);
		return {};
	}

	void run(
		std::stop_token st) noexcept {
		owner_thread = std::this_thread::get_id();
		if (auto ec = init_ring(); ec) {
			publish_startup(UringExecutorState::failed, ec);
			return;
		}

		completions = std::make_unique<conflux::uring::CompletionTable>(options.completion_slots);
		lane = std::make_unique<RingLane>(RingLaneOptions{
			.ring_fd = ring.ring_fd,
			.wake_user_data = wake_user_data(),
			.drain_budget = options.lane_drain_budget,
			.allow_inline_on_owner = true});
		lane->adopt_current_thread();
		publish_startup(UringExecutorState::running);
		loop(st);
		lane.reset();
		completions.reset();
		if (ring_initialized) {
			::io_uring_queue_exit(&ring);
			ring_initialized = false;
			ring_fd_value.store(-1, std::memory_order_release);
		}
		state.store(UringExecutorState::stopped, std::memory_order_release);
		cv.notify_all();
	}

	void loop(
		std::stop_token st) noexcept {
		while (!st.stop_requested()) {
			__kernel_timespec ts{.tv_sec = 0, .tv_nsec = 100'000'000};
			::io_uring_cqe *cqe = nullptr;
			int rc = ::io_uring_submit_and_wait_timeout(&ring, &cqe, 1, &ts, nullptr);
			if (rc == -EINTR || rc == -EAGAIN) {
				continue;
			}
			if (rc == -ETIME) {
				on_idle_timeout();
				if (should_exit()) {
					break;
				}
				continue;
			}
			if (rc < 0) {
				runtime_error = std::error_code{-rc, std::generic_category()};
				state.store(UringExecutorState::stopping, std::memory_order_release);
				cancel_all(root::CancelReason::shutdown);
			}
			drain_cqes();
			drain_for_shutdown();
			if (should_exit()) {
				break;
			}
		}
	}

	void on_idle_timeout() noexcept {
		auto const current = state.load(std::memory_order_acquire);
		if (current == UringExecutorState::stopping || current == UringExecutorState::draining) {
			drain_for_shutdown();
		}
	}

	void drain_for_shutdown() noexcept {
		auto const current = state.load(std::memory_order_acquire);
		if (current != UringExecutorState::stopping && current != UringExecutorState::draining) {
			return;
		}
		cancel_all(root::CancelReason::shutdown);
		if (lane != nullptr) {
			auto _ = lane->drain();
		}
		state.store(UringExecutorState::draining, std::memory_order_release);
	}

	[[nodiscard]] bool should_exit() const noexcept {
		auto const current = state.load(std::memory_order_acquire);
		if (current != UringExecutorState::draining && current != UringExecutorState::stopping) {
			return false;
		}
		return live_records.load(std::memory_order_acquire) == 0
			&& (completions == nullptr || completions->pending() == 0);
	}

	void drain_cqes() noexcept {
		std::array<::io_uring_cqe *, 32> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(&ring, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				dispatch_cqe(*batch[static_cast<std::size_t>(i)]);
			}
			::io_uring_cq_advance(&ring, n);
		}
	}

	void dispatch_cqe(
		::io_uring_cqe const &cqe) noexcept {
		auto const slot = user_data_slot(cqe.user_data);
		auto const gen = user_data_generation(cqe.user_data);
		if (slot == kExecutorSlot) {
			if (gen == kWakeTag && lane != nullptr) {
				auto _ = lane->drain();
			} else {
				runtime_error = std::make_error_code(std::errc::protocol_error);
				state.store(UringExecutorState::stopping, std::memory_order_release);
				cancel_all(root::CancelReason::shutdown);
			}
			return;
		}
		if (completions != nullptr) {
			completions->dispatch(slot, gen, cqe.res, conflux::uring::CqeFlags{cqe.flags});
		}
	}
};

template<class T>
concept root_task_result = requires {
	typename std::remove_cvref_t<T>::value_type;
} && std::same_as<std::remove_cvref_t<T>, root::Task<typename std::remove_cvref_t<T>::value_type>>;

template<class Fn>
using task_result_t = std::invoke_result_t<std::decay_t<Fn> &, ::conflux::work::UringExecutorContext &>;

template<class Fn>
concept uring_executor_task_body = root_task_result<task_result_t<Fn>>;

template<class Fn>
using task_value_t = typename std::remove_cvref_t<task_result_t<Fn>>::value_type;

} // namespace uring_executor_detail

export class UringExecutorContext {
	uring_executor_detail::UringExecutorSharedState *state_{};

	explicit UringExecutorContext(
		uring_executor_detail::UringExecutorSharedState &state) noexcept
		: state_{&state} {}

	template<class, class>
	friend struct uring_executor_detail::Submission;
	friend struct uring_executor_detail::UringExecutorSharedState;

public:
	[[nodiscard]] conflux::uring::RingRef ring() const noexcept { return conflux::uring::RingRef{&state_->ring}; }
	[[nodiscard]] conflux::uring::CompletionTable &completions() const noexcept { return *state_->completions; }
	[[nodiscard]] RingLane &lane() const noexcept { return *state_->lane; }
	[[nodiscard]] std::uint64_t encode(
		std::uint32_t slot,
		std::uint32_t gen) const {
		return state_->encode(slot, gen);
	}
	[[nodiscard]] conflux::uring::UserDataFn user_data_encoder() const {
		auto *state = state_;
		return [state](std::uint32_t slot, std::uint32_t gen) -> std::uint64_t { return state->encode(slot, gen); };
	}
	[[nodiscard]] bool on_owner_thread() const noexcept { return state_->on_owner_thread(); }
};

namespace uring_executor_detail {

template<class T, class Fn>
struct Submission final
	: SubmissionBase
	, std::enable_shared_from_this<Submission<T, Fn>> {
	using source_t = root::TaskSource<T>;

	std::weak_ptr<UringExecutorSharedState> executor{};
	Fn fn;
	source_t source;
	root::TaskControl outer_control;
	std::mutex mutex{};
	root::TaskControl child_control{};
	std::uint64_t child_generation = 0;

	Submission(
		std::shared_ptr<UringExecutorSharedState> state,
		Fn f,
		source_t src,
		root::TaskControl control)
		: executor{std::move(state)}
		, fn{std::move(f)}
		, source{std::move(src)}
		, outer_control{std::move(control)} {}

	void install_cancel_hook() {
		auto weak = this->weak_from_this();
		bool const installed = source.install_cancel_hook([weak](root::CancelReason reason) noexcept {
			if (auto self = weak.lock()) {
				self->cancel(reason);
			}
		});
		if (!installed) {
			commit_exception(
				std::make_exception_ptr(root::WorkError{"conflux.work.uring_executor: cancel hook install failed"}));
		}
	}

	void release() noexcept {
		if (auto state = executor.lock()) {
			state->release_admission(*this);
		}
	}

	void commit_cancel(
		root::CancelReason reason) noexcept {
		auto _ = source.try_set_cancelled(reason);
		release();
	}

	void commit_exception(
		std::exception_ptr error) noexcept {
		try {
			auto _ = source.try_set_exception(error);
		} catch (...) {} // NOLINT(bugprone-empty-catch): commit is noexcept; release admission regardless.
		release();
	}

	void fail_enqueue(
		std::error_code ec,
		std::string_view message) noexcept override {
		SubmissionState expected = SubmissionState::queued;
		if (!record_state.compare_exchange_strong(expected, SubmissionState::terminal, std::memory_order_acq_rel)) {
			return;
		}
		try {
			auto _ = source.try_set_error(ec, message);
		} catch (...) { commit_exception(std::current_exception()); }
		release();
	}

	void cancel_shutdown() noexcept override { cancel(root::CancelReason::shutdown); }

	void cancel(
		root::CancelReason reason) noexcept override {
		cancel_reason.store(reason, std::memory_order_release);
		cancel_requested.store(true, std::memory_order_release);
		auto expected = SubmissionState::queued;
		if (record_state.compare_exchange_strong(expected, SubmissionState::terminal, std::memory_order_acq_rel)) {
			commit_cancel(reason);
			return;
		}
		if (expected == SubmissionState::child_bound) {
			std::scoped_lock lock{mutex};
			auto _ = child_control.request_cancel(reason);
		}
	}

	void start(
		UringExecutorSharedState &state) noexcept override {
		SubmissionState expected = SubmissionState::queued;
		if (!record_state.compare_exchange_strong(expected, SubmissionState::starting, std::memory_order_acq_rel)) {
			return;
		}

		try {
			UringExecutorContext ctx{state};
			auto child = std::invoke(fn, ctx);
			auto child_handle = std::shared_ptr<root::TaskJoinHandle<T>>{
				new root::TaskJoinHandle<T>(root::into_join_handle(std::move(child)))};
			auto const generation = outer_control.bind_child_for_cancellation(child_handle->control());
			bool request_cancel = false;
			root::CancelReason reason = root::CancelReason::requested;
			{
				std::scoped_lock lock{mutex};
				child_control = child_handle->control();
				child_generation = generation;
				request_cancel = cancel_requested.load(std::memory_order_acquire);
				reason = cancel_reason.load(std::memory_order_acquire);
			}
			record_state.store(SubmissionState::child_bound, std::memory_order_release);
			if (request_cancel) {
				auto _ = child_handle->control().request_cancel(reason);
			}
			auto self = this->shared_from_this();
			child_handle->control().set_on_ready_or_run(
				[self, child_handle]() noexcept { self->complete_child(child_handle); });
		} catch (...) {
			record_state.store(SubmissionState::terminal, std::memory_order_release);
			commit_exception(std::current_exception());
		}
	}

	void complete_child(
		std::shared_ptr<root::TaskJoinHandle<T>> const &child_handle) noexcept {
		SubmissionState expected = SubmissionState::child_bound;
		if (!record_state.compare_exchange_strong(expected, SubmissionState::terminal, std::memory_order_acq_rel)) {
			return;
		}
		outer_control.clear_child_for_cancellation(child_generation);
		try {
			auto outcome = root::join_ready(std::move(*child_handle));
			std::move(outcome).visit([this](auto &&arm) {
				using arm_t = std::remove_cvref_t<decltype(arm)>;
				if constexpr (std::same_as<arm_t, root::Success<T>>) {
					auto _ = source.try_set_value(std::move(arm));
				} else if constexpr (std::same_as<arm_t, root::Failure>) {
					auto _ = source.try_set_exception(std::move(arm.error));
				} else {
					auto _ = source.try_set_cancelled(arm.reason);
				}
			});
		} catch (root::CancelledError const &err) { auto _ = source.try_set_cancelled(err.reason()); } catch (...) {
			auto _ = source.try_set_exception(std::current_exception());
		}
		release();
	}
};

} // namespace uring_executor_detail

export class UringExecutor final {
	std::shared_ptr<uring_executor_detail::UringExecutorSharedState> state_{};

	explicit UringExecutor(
		std::shared_ptr<uring_executor_detail::UringExecutorSharedState> state) noexcept
		: state_{std::move(state)} {}

	friend std::expected<std::unique_ptr<UringExecutor>, std::error_code>
	try_make_uring_executor(UringExecutorOptions opts);

public:
	~UringExecutor() { join(); }
	UringExecutor(UringExecutor const &) = delete;
	UringExecutor &operator =(UringExecutor const &) = delete;
	UringExecutor(UringExecutor &&) = delete;
	UringExecutor &operator =(UringExecutor &&) = delete;

	void stop() noexcept { state_->stop(); }
	void join() noexcept { state_->join(); }
	[[nodiscard]] UringExecutorState state() const noexcept { return state_->state.load(std::memory_order_acquire); }
	[[nodiscard]] bool stopped() const noexcept { return state_->stopped(); }
	[[nodiscard]] bool on_owner_thread() const noexcept { return state_->on_owner_thread(); }
	[[nodiscard]] int ring_fd() const noexcept { return state_->ring_fd(); }

	template<class Fn>
		requires uring_executor_detail::uring_executor_task_body<Fn>
	[[nodiscard]] auto async_submit(
		Fn &&fn) -> root::Task<uring_executor_detail::task_value_t<Fn>> {
		using fn_t = std::decay_t<Fn>;
		using T = uring_executor_detail::task_value_t<Fn>;
		if (state_->state.load(std::memory_order_acquire) != UringExecutorState::running) {
			auto [task, src] = root::make_task_source<T>();
			auto _ = src.try_set_cancelled(root::work_errc::cancelled_shutdown);
			return std::move(task);
		}

		auto [task, src] = root::make_task_source<T>();
		auto submission =
			std::shared_ptr<uring_executor_detail::Submission<T, fn_t>>{new uring_executor_detail::Submission<T, fn_t>(
				state_,
				fn_t{std::forward<Fn>(fn)},
				std::move(src),
				task.control())};
		submission->install_cancel_hook();
		if (submission->record_state.load(std::memory_order_acquire)
			== uring_executor_detail::SubmissionState::terminal) {
			return std::move(task);
		}
		auto const admit = state_->admit(submission);
		if (admit == uring_executor_detail::UringExecutorSharedState::AdmitResult::stopped) {
			submission->cancel_shutdown();
			return std::move(task);
		}
		if (admit == uring_executor_detail::UringExecutorSharedState::AdmitResult::full) {
			submission->fail_enqueue(
				std::make_error_code(std::errc::resource_unavailable_try_again),
				"conflux.work.uring_executor: submission queue full");
			return std::move(task);
		}
		if (state_->lane == nullptr
			|| !state_->lane->enqueue([state = state_, submission]() mutable { submission->start(*state); })) {
			submission->fail_enqueue(
				std::make_error_code(std::errc::io_error),
				"conflux.work.uring_executor: ring wake failed");
		}
		return std::move(task);
	}
};

export [[nodiscard]] std::expected<std::unique_ptr<UringExecutor>, std::error_code> try_make_uring_executor(
	UringExecutorOptions opts = {}) {
	if (opts.ring_entries == 0 || opts.max_submission_queue == 0) {
		return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
	}
	auto state = std::shared_ptr<uring_executor_detail::UringExecutorSharedState>{
		new uring_executor_detail::UringExecutorSharedState(opts)};
	state->start();
	if (auto ec = state->wait_started(); ec) {
		state->join();
		return std::unexpected{ec};
	}
	return std::unique_ptr<UringExecutor>{new UringExecutor{std::move(state)}};
}

} // namespace conflux::work
