module;
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <liburing.h>

export module conflux.socket_io.blocking;

import std;
import conflux.types;
import conflux.uring;
import conflux.socket_io;
import conflux.socket_io.coro;
import conflux.work;
import conflux.work.race;

namespace wroot = conflux::work::root;

namespace conflux::socket_io {

export struct SyncWaitSocketTaskTimeout final : std::runtime_error {
	SyncWaitSocketTaskTimeout()
		: std::runtime_error{"conflux.socket_io: sync_wait_socket_task budget exhausted"} {}
};

export using BlockOnSocketTaskTimeout = SyncWaitSocketTaskTimeout;

// Single-thread io_uring driver for SocketTaskRing.
// Encoding: low-32 = slot, high-32 = gen.
export template<typename T>
T sync_wait_socket_task(
	SocketTaskRing &ring,
	wroot::Task<T> task,
	std::optional<std::chrono::milliseconds> budget = std::nullopt) {
	using namespace conflux::work::root;
	struct Slot {
		std::atomic_flag done{};
		std::exception_ptr err{};
		[[no_unique_address]] std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>> value{};
	};
	auto slot = std::make_shared<Slot>();
	auto jh = std::make_shared<TaskJoinHandle<T>>(into_join_handle(std::move(task)));
	jh->control().set_on_ready_or_run([slot, jh]() noexcept {
		try {
			auto outcome = blocking_join(std::move(*jh));
			if (outcome.is_failure()) {
				slot->err = std::move(outcome).failure().error;
			} else if (outcome.is_cancelled()) {
				slot->err = std::make_exception_ptr(::Cancelled{});
			} else if constexpr (!std::is_void_v<T>) {
				slot->value.emplace(std::move(outcome).success().value);
			}
		} catch (...) { slot->err = std::current_exception(); }
		slot->done.test_and_set(std::memory_order_release);
	});
	auto *raw = ring.raw().ring().raw();
	auto *ct = &ring.completions();
	auto const deadline = budget ? std::make_optional(std::chrono::steady_clock::now() + *budget) : std::nullopt;
	while (!slot->done.test(std::memory_order_acquire)) {
		::io_uring_cqe *cqe = nullptr;
		int rc = 0;
		if (deadline) {
			__kernel_timespec ts{.tv_sec = 1, .tv_nsec = 0};
			rc = ::io_uring_submit_and_wait_timeout(raw, &cqe, 1, &ts, nullptr);
			if (rc == -ETIME) {
				if (std::chrono::steady_clock::now() > *deadline) {
					throw SyncWaitSocketTaskTimeout{};
				}
				continue;
			}
		} else {
			rc = ::io_uring_submit_and_wait(raw, 1);
			if (rc >= 0) {
				rc = ::io_uring_peek_cqe(raw, &cqe);
			}
		}
		if (rc == -EINTR || rc == -EAGAIN) {
			continue;
		}
		if (rc >= 0 && cqe == nullptr) {
			continue;
		}
		if (rc < 0 || cqe == nullptr) {
			throw std::runtime_error{std::format("conflux.socket_io: sync_wait_socket_task rc={}", rc)};
		}
		std::array<::io_uring_cqe *, 32> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(raw, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				auto const *c = batch[static_cast<std::size_t>(i)];
				auto const ud = c->user_data;
				ct->dispatch(
					static_cast<std::uint32_t>(ud & 0xFFFFFFFFU),
					static_cast<std::uint32_t>(ud >> 32U),
					c->res,
					conflux::uring::CqeFlags{c->flags});
			}
			::io_uring_cq_advance(raw, n);
			if (slot->done.test(std::memory_order_acquire)) {
				break;
			}
		}
	}
	if (slot->err) {
		std::rethrow_exception(slot->err);
	}
	if constexpr (!std::is_void_v<T>) {
		return std::move(*slot->value);
	}
}

export template<typename T>
T block_on_socket_task(
	SocketTaskRing &ring,
	wroot::Task<T> task,
	std::optional<std::chrono::milliseconds> budget = std::nullopt) {
	if constexpr (std::is_void_v<T>) {
		sync_wait_socket_task(ring, std::move(task), budget);
	} else {
		return sync_wait_socket_task(ring, std::move(task), budget);
	}
}

export template<typename T>
conflux::work::race::race_result<T> sync_wait_socket_race(
	SocketTaskRing &ring,
	wroot::Task<conflux::work::race::race_result<T>> task,
	std::optional<std::chrono::milliseconds> budget = std::nullopt) {
	return sync_wait_socket_task(ring, std::move(task), budget);
}

} // namespace conflux::socket_io
