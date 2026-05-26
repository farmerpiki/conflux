module;
#include <liburing.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef CONFLUX_WORK_QUEUE_STATS
	#define CONFLUX_WORK_QUEUE_STATS 0
#endif

export module conflux.work:api;

import std;
import conflux.types;
import conflux.small_function;
export import conflux.work.root;
import conflux.work.race;
export struct Cancelled final : std::runtime_error {
	Cancelled()
		: std::runtime_error{"work cancelled"} {}
};
export struct WorkPoolQueueStats {
	std::uint64_t enqueue_attempts = 0;
	std::uint64_t enqueue_stopped_rejections = 0;
	std::uint64_t enqueue_full_rejections = 0;
	std::uint64_t admission_lock_acquisitions = 0;
	std::uint64_t admission_lock_contentions = 0;
	std::uint64_t local_lock_acquisitions = 0;
	std::uint64_t local_lock_contentions = 0;
	std::uint64_t steal_lock_acquisitions = 0;
	std::uint64_t steal_lock_contentions = 0;
	std::uint64_t local_pushes = 0;
	std::uint64_t local_push_full = 0;
	std::uint64_t inject_pushes = 0;
	std::uint64_t inject_push_full = 0;
	std::uint64_t local_pop_attempts = 0;
	std::uint64_t local_pop_hits = 0;
	std::uint64_t inject_pop_attempts = 0;
	std::uint64_t inject_pop_hits = 0;
	std::uint64_t steal_rounds = 0;
	std::uint64_t steal_victim_checks = 0;
	std::uint64_t steal_hits = 0;
	std::uint64_t jobs_run = 0;
	std::uint64_t wake_one_calls = 0;
	std::uint64_t wake_one_futex_wakes = 0;
	std::uint64_t wake_one_elided_no_parked = 0;
	std::uint64_t wake_all_calls = 0;
	std::uint64_t wake_all_futex_wakes = 0;
	std::uint64_t park_attempts = 0;
	std::uint64_t park_recheck_skips = 0;
	std::uint64_t futex_waits = 0;
	std::uint64_t job_slot_allocations = 0;
	std::uint64_t job_slab_allocations = 0;
	std::uint64_t job_slab_id_reuses = 0;
	std::uint64_t job_slab_releases = 0;
	std::uint64_t job_allocation_failures = 0;
	std::uint64_t queue_full_token_discards = 0;
	std::uint64_t remote_free_pushes = 0;
	std::uint64_t remote_free_fallbacks = 0;
	std::uint64_t remote_free_drained = 0;
	std::uint64_t token_take_failures = 0;
};
namespace work_detail {

constexpr std::size_t kNoWorker = std::numeric_limits<std::size_t>::max();

using Fn = ::conflux::detail::small_move_only_function<void()>;
inline int futex_wait_private(
	std::atomic<std::uint32_t> &word,
	std::uint32_t expected) noexcept {
	auto *addr = reinterpret_cast<std::uint32_t *>(&word); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
	return static_cast<int>(::syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0));
}
inline int futex_wake_private(
	std::atomic<std::uint32_t> &word,
	int count) noexcept {
	auto *addr = reinterpret_cast<std::uint32_t *>(&word); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
	return static_cast<int>(::syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0));
}
class QueueTarget {
public:
	virtual ~QueueTarget() = default;
	virtual bool enqueue(::conflux::detail::small_move_only_function<void()> job) = 0;
};
// Vyukov-style MPMC bounded ring buffer, lock-free. Capacity rounded up to
// next power-of-2 at construction. try_push/try_pop are noexcept because item
// move-assign is noexcept for all WorkPool queue payloads.
template<typename T>
class MpmcRing {
	struct Slot {
		std::atomic<std::size_t> seq{0};
		T item{};
	};
	std::unique_ptr<Slot[]> slots_;
	std::size_t capacity_{};
	std::size_t mask_{};
	alignas(64) std::atomic<std::size_t> head_{0};
	alignas(64) std::atomic<std::size_t> tail_{0};

public:
	explicit MpmcRing(
		std::size_t capacity) {
		if (capacity == 0) {
			return;
		}
		std::size_t cap = 1;
		while (cap < capacity) {
			cap <<= 1;
		}
		capacity_ = cap;
		mask_ = cap - 1;
		slots_ = std::make_unique<Slot[]>(cap);
		for (std::size_t i = 0; i < cap; ++i) {
			slots_[i].seq.store(i, std::memory_order_relaxed);
		}
	}
	MpmcRing(MpmcRing const &) = delete;
	MpmcRing &operator =(MpmcRing const &) = delete;
	MpmcRing(MpmcRing &&) = delete;
	MpmcRing &operator =(MpmcRing &&) = delete;
	[[nodiscard]] bool try_push(
		T item) noexcept {
		if (capacity_ == 0) {
			return false;
		}
		std::size_t pos = head_.load(std::memory_order_relaxed);
		for (;;) {
			Slot &slot = slots_[pos & mask_];
			std::size_t const seq = slot.seq.load(std::memory_order_acquire);
			auto const diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
			if (diff == 0) {
				if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
					slot.item = std::move(item);
					slot.seq.store(pos + 1, std::memory_order_release);
					return true;
				}
			} else if (diff < 0) {
				return false;
			} else {
				conflux::work::root::detail::cpu_pause();
				pos = head_.load(std::memory_order_relaxed);
			}
		}
	}
	[[nodiscard]] std::optional<T> try_pop() noexcept {
		if (capacity_ == 0) {
			return std::nullopt;
		}
		std::size_t pos = tail_.load(std::memory_order_relaxed);
		for (;;) {
			Slot &slot = slots_[pos & mask_];
			std::size_t const seq = slot.seq.load(std::memory_order_acquire);
			auto const diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
			if (diff == 0) {
				if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
					T item = std::move(slot.item);
					slot.seq.store(pos + capacity_, std::memory_order_release);
					return std::optional<T>{std::move(item)};
				}
			} else if (diff < 0) {
				return std::nullopt;
			} else {
				conflux::work::root::detail::cpu_pause();
				pos = tail_.load(std::memory_order_relaxed);
			}
		}
	}
};

class WorkPoolQueueCounters {
#if CONFLUX_WORK_QUEUE_STATS
	std::atomic<std::uint64_t> enqueue_attempts_{0};
	std::atomic<std::uint64_t> enqueue_stopped_rejections_{0};
	std::atomic<std::uint64_t> enqueue_full_rejections_{0};
	std::atomic<std::uint64_t> admission_lock_acquisitions_{0};
	std::atomic<std::uint64_t> admission_lock_contentions_{0};
	std::atomic<std::uint64_t> local_lock_acquisitions_{0};
	std::atomic<std::uint64_t> local_lock_contentions_{0};
	std::atomic<std::uint64_t> steal_lock_acquisitions_{0};
	std::atomic<std::uint64_t> steal_lock_contentions_{0};
	std::atomic<std::uint64_t> local_pushes_{0};
	std::atomic<std::uint64_t> local_push_full_{0};
	std::atomic<std::uint64_t> inject_pushes_{0};
	std::atomic<std::uint64_t> inject_push_full_{0};
	std::atomic<std::uint64_t> local_pop_attempts_{0};
	std::atomic<std::uint64_t> local_pop_hits_{0};
	std::atomic<std::uint64_t> inject_pop_attempts_{0};
	std::atomic<std::uint64_t> inject_pop_hits_{0};
	std::atomic<std::uint64_t> steal_rounds_{0};
	std::atomic<std::uint64_t> steal_victim_checks_{0};
	std::atomic<std::uint64_t> steal_hits_{0};
	std::atomic<std::uint64_t> jobs_run_{0};
	std::atomic<std::uint64_t> wake_one_calls_{0};
	std::atomic<std::uint64_t> wake_one_futex_wakes_{0};
	std::atomic<std::uint64_t> wake_one_elided_no_parked_{0};
	std::atomic<std::uint64_t> wake_all_calls_{0};
	std::atomic<std::uint64_t> wake_all_futex_wakes_{0};
	std::atomic<std::uint64_t> park_attempts_{0};
	std::atomic<std::uint64_t> park_recheck_skips_{0};
	std::atomic<std::uint64_t> futex_waits_{0};
	std::atomic<std::uint64_t> queue_full_token_discards_{0};

	static void add(
		std::atomic<std::uint64_t> &counter) noexcept {
		counter.fetch_add(1, std::memory_order_relaxed);
	}
	static void clear(
		std::atomic<std::uint64_t> &counter) noexcept {
		counter.store(0, std::memory_order_relaxed);
	}
	[[nodiscard]] static std::uint64_t get(
		std::atomic<std::uint64_t> const &counter) noexcept {
		return counter.load(std::memory_order_relaxed);
	}

public:
	void note_enqueue_attempt() noexcept { add(enqueue_attempts_); }
	void note_enqueue_stopped_rejection() noexcept { add(enqueue_stopped_rejections_); }
	void note_enqueue_full_rejection() noexcept { add(enqueue_full_rejections_); }
	void note_admission_lock_acquisition() noexcept { add(admission_lock_acquisitions_); }
	void note_admission_lock_contention() noexcept { add(admission_lock_contentions_); }
	void note_local_lock_acquisition() noexcept { add(local_lock_acquisitions_); }
	void note_local_lock_contention() noexcept { add(local_lock_contentions_); }
	void note_steal_lock_acquisition() noexcept { add(steal_lock_acquisitions_); }
	void note_steal_lock_contention() noexcept { add(steal_lock_contentions_); }
	void note_local_push() noexcept { add(local_pushes_); }
	void note_local_push_full() noexcept { add(local_push_full_); }
	void note_inject_push() noexcept { add(inject_pushes_); }
	void note_inject_push_full() noexcept { add(inject_push_full_); }
	void note_local_pop_attempt() noexcept { add(local_pop_attempts_); }
	void note_local_pop_hit() noexcept { add(local_pop_hits_); }
	void note_inject_pop_attempt() noexcept { add(inject_pop_attempts_); }
	void note_inject_pop_hit() noexcept { add(inject_pop_hits_); }
	void note_steal_round() noexcept { add(steal_rounds_); }
	void note_steal_victim_check() noexcept { add(steal_victim_checks_); }
	void note_steal_hit() noexcept { add(steal_hits_); }
	void note_job_run() noexcept { add(jobs_run_); }
	void note_wake_one() noexcept { add(wake_one_calls_); }
	void note_wake_one_futex() noexcept { add(wake_one_futex_wakes_); }
	void note_wake_one_elided() noexcept { add(wake_one_elided_no_parked_); }
	void note_wake_all() noexcept { add(wake_all_calls_); }
	void note_wake_all_futex() noexcept { add(wake_all_futex_wakes_); }
	void note_park_attempt() noexcept { add(park_attempts_); }
	void note_park_recheck_skip() noexcept { add(park_recheck_skips_); }
	void note_futex_wait() noexcept { add(futex_waits_); }
	void note_queue_full_token_discard() noexcept { add(queue_full_token_discards_); }
	[[nodiscard]] std::unique_lock<std::mutex> lock_admission(
		std::mutex &mtx) {
		if (mtx.try_lock()) {
			note_admission_lock_acquisition();
			return std::unique_lock<std::mutex>{mtx, std::adopt_lock};
		}
		note_admission_lock_contention();
		mtx.lock();
		note_admission_lock_acquisition();
		return std::unique_lock<std::mutex>{mtx, std::adopt_lock};
	}
	[[nodiscard]] std::unique_lock<std::mutex> lock_local(
		std::mutex &mtx) {
		if (mtx.try_lock()) {
			note_local_lock_acquisition();
			return std::unique_lock<std::mutex>{mtx, std::adopt_lock};
		}
		note_local_lock_contention();
		mtx.lock();
		note_local_lock_acquisition();
		return std::unique_lock<std::mutex>{mtx, std::adopt_lock};
	}
	[[nodiscard]] std::unique_lock<std::mutex> lock_steal_victim(
		std::mutex &mtx) {
		if (mtx.try_lock()) {
			note_steal_lock_acquisition();
			return std::unique_lock<std::mutex>{mtx, std::adopt_lock};
		}
		note_steal_lock_contention();
		mtx.lock();
		note_steal_lock_acquisition();
		return std::unique_lock<std::mutex>{mtx, std::adopt_lock};
	}
	[[nodiscard]] WorkPoolQueueStats snapshot() const noexcept {
		return WorkPoolQueueStats{
			.enqueue_attempts = get(enqueue_attempts_),
			.enqueue_stopped_rejections = get(enqueue_stopped_rejections_),
			.enqueue_full_rejections = get(enqueue_full_rejections_),
			.admission_lock_acquisitions = get(admission_lock_acquisitions_),
			.admission_lock_contentions = get(admission_lock_contentions_),
			.local_lock_acquisitions = get(local_lock_acquisitions_),
			.local_lock_contentions = get(local_lock_contentions_),
			.steal_lock_acquisitions = get(steal_lock_acquisitions_),
			.steal_lock_contentions = get(steal_lock_contentions_),
			.local_pushes = get(local_pushes_),
			.local_push_full = get(local_push_full_),
			.inject_pushes = get(inject_pushes_),
			.inject_push_full = get(inject_push_full_),
			.local_pop_attempts = get(local_pop_attempts_),
			.local_pop_hits = get(local_pop_hits_),
			.inject_pop_attempts = get(inject_pop_attempts_),
			.inject_pop_hits = get(inject_pop_hits_),
			.steal_rounds = get(steal_rounds_),
			.steal_victim_checks = get(steal_victim_checks_),
			.steal_hits = get(steal_hits_),
			.jobs_run = get(jobs_run_),
			.wake_one_calls = get(wake_one_calls_),
			.wake_one_futex_wakes = get(wake_one_futex_wakes_),
			.wake_one_elided_no_parked = get(wake_one_elided_no_parked_),
			.wake_all_calls = get(wake_all_calls_),
			.wake_all_futex_wakes = get(wake_all_futex_wakes_),
			.park_attempts = get(park_attempts_),
			.park_recheck_skips = get(park_recheck_skips_),
			.futex_waits = get(futex_waits_),
			.queue_full_token_discards = get(queue_full_token_discards_),
		};
	}
	void reset() noexcept {
		clear(enqueue_attempts_);
		clear(enqueue_stopped_rejections_);
		clear(enqueue_full_rejections_);
		clear(admission_lock_acquisitions_);
		clear(admission_lock_contentions_);
		clear(local_lock_acquisitions_);
		clear(local_lock_contentions_);
		clear(steal_lock_acquisitions_);
		clear(steal_lock_contentions_);
		clear(local_pushes_);
		clear(local_push_full_);
		clear(inject_pushes_);
		clear(inject_push_full_);
		clear(local_pop_attempts_);
		clear(local_pop_hits_);
		clear(inject_pop_attempts_);
		clear(inject_pop_hits_);
		clear(steal_rounds_);
		clear(steal_victim_checks_);
		clear(steal_hits_);
		clear(jobs_run_);
		clear(wake_one_calls_);
		clear(wake_one_futex_wakes_);
		clear(wake_one_elided_no_parked_);
		clear(wake_all_calls_);
		clear(wake_all_futex_wakes_);
		clear(park_attempts_);
		clear(park_recheck_skips_);
		clear(futex_waits_);
		clear(queue_full_token_discards_);
	}
#else
public:
	void note_enqueue_attempt() noexcept {}
	void note_enqueue_stopped_rejection() noexcept {}
	void note_enqueue_full_rejection() noexcept {}
	void note_admission_lock_acquisition() noexcept {}
	void note_admission_lock_contention() noexcept {}
	void note_local_lock_acquisition() noexcept {}
	void note_local_lock_contention() noexcept {}
	void note_steal_lock_acquisition() noexcept {}
	void note_steal_lock_contention() noexcept {}
	void note_local_push() noexcept {}
	void note_local_push_full() noexcept {}
	void note_inject_push() noexcept {}
	void note_inject_push_full() noexcept {}
	void note_local_pop_attempt() noexcept {}
	void note_local_pop_hit() noexcept {}
	void note_inject_pop_attempt() noexcept {}
	void note_inject_pop_hit() noexcept {}
	void note_steal_round() noexcept {}
	void note_steal_victim_check() noexcept {}
	void note_steal_hit() noexcept {}
	void note_job_run() noexcept {}
	void note_wake_one() noexcept {}
	void note_wake_one_futex() noexcept {}
	void note_wake_one_elided() noexcept {}
	void note_wake_all() noexcept {}
	void note_wake_all_futex() noexcept {}
	void note_park_attempt() noexcept {}
	void note_park_recheck_skip() noexcept {}
	void note_futex_wait() noexcept {}
	void note_queue_full_token_discard() noexcept {}
	[[nodiscard]] std::unique_lock<std::mutex> lock_admission(
		std::mutex &mtx) {
		return std::unique_lock<std::mutex>{mtx};
	}
	[[nodiscard]] std::unique_lock<std::mutex> lock_local(
		std::mutex &mtx) {
		return std::unique_lock<std::mutex>{mtx};
	}
	[[nodiscard]] std::unique_lock<std::mutex> lock_steal_victim(
		std::mutex &mtx) {
		return std::unique_lock<std::mutex>{mtx};
	}
	[[nodiscard]] WorkPoolQueueStats snapshot() const noexcept { return {}; }
	void reset() noexcept {}
#endif
};

} // namespace work_detail
export enum class WorkPoolQueueMode : std::uint8_t {
	stealing,
	no_stealing,
};
export struct WorkPoolOptions {
	std::size_t threads = 0;
	std::size_t max_inject_queue = 4096;
	std::size_t inject_queue_shards = 0;
	std::size_t local_queue_capacity = 1024;
	std::size_t initial_job_slab_slots = 256;
	std::size_t max_job_slab_slots = 4096;
	WorkPoolQueueMode queue_mode = WorkPoolQueueMode::stealing;
	std::uint32_t spin_before_park = 256;
	int numa_node = -1;
	bool pin_workers = false;
	std::string worker_name_prefix = "conflux-work";
	std::function<void(std::exception_ptr)> raw_exception_sink{};
};
export struct RingLaneOptions {
	int ring_fd = -1;
	std::uint64_t wake_user_data = 0x434F4E464C5558ULL; // "CONFLUX"
	std::size_t drain_budget = 0;
	bool allow_inline_on_owner = true;
};

export enum class WorkError : std::uint8_t {
	stopped,
	queue_full,
	wake_failed,
	submit_failed,
	cancelled,
	owner_violation,
};
export class WorkPool final : public work_detail::QueueTarget {
	void *state_{};

public:
	explicit WorkPool(WorkPoolOptions options = {});
	~WorkPool() override;
	WorkPool(WorkPool const &) = delete;
	WorkPool &operator =(WorkPool const &) = delete;
	WorkPool(WorkPool &&) = delete;
	WorkPool &operator =(WorkPool &&) = delete;
	[[nodiscard]] bool enqueue(work_detail::Fn job) override;
	void stop() noexcept;
	void drain_and_stop() noexcept;
	void wait() noexcept;
	[[nodiscard]] bool stopped() const noexcept;
	[[nodiscard]] WorkPoolQueueStats queue_stats() const noexcept;
	void reset_queue_stats() noexcept;
};
// io_uring-coupled executor: requires a live ring (ring_fd from RingLaneOptions).
// Each non-inline enqueue into an empty queue issues exactly one
// io_uring_register_sync_msg syscall. Uses raw io_uring_sqe only — no conflux.net
// dependency; callers own the ring and drive drain() from CQE handlers.
export class RingLane final : public work_detail::QueueTarget {
	void *state_{};

public:
	explicit RingLane(RingLaneOptions options = {});
	~RingLane() override;
	RingLane(RingLane const &) = delete;
	RingLane &operator =(RingLane const &) = delete;
	RingLane(RingLane &&) = delete;
	RingLane &operator =(RingLane &&) = delete;
	[[nodiscard]] bool enqueue(::conflux::detail::small_move_only_function<void()> job) override;
	void adopt_current_thread() noexcept;
	[[nodiscard]] std::size_t drain();
	void stop() noexcept;
	[[nodiscard]] bool stopped() const noexcept;
	[[nodiscard]] bool on_owner_thread() const noexcept;
	[[nodiscard]] int ring_fd() const noexcept;
};
namespace conflux::work::root {

template<>
inline constexpr bool enable_address_capability_v<WorkPool> = true;
template<>
inline constexpr bool enable_address_capability_v<RingLane> = true;

} // namespace conflux::work::root
export template<typename Target, typename Fn>
[[nodiscard]] auto async_run_on(
	Target &target,
	Fn &&fn) {
	using fn_t = std::decay_t<Fn>;
	using T = std::invoke_result_t<fn_t &>;
	using namespace conflux::work::root;
	auto [task, src] = make_task_source<T>(SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<TaskSource<T>>(std::move(src));
	auto job = [shared_src, fn = fn_t{std::forward<Fn>(fn)}]() mutable {
		try {
			if constexpr (std::is_void_v<T>) {
				fn();
				auto _ = shared_src->try_set_value(Success<T>{});
			} else {
				auto _ = shared_src->try_set_value(Success<T>{fn()});
			}
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	};
	if (!target.enqueue(std::move(job))) {
		auto _ = shared_src->try_set_cancelled(work_errc::cancelled_requested);
	}
	return std::move(task);
}
export template<typename Target, typename Fn>
[[nodiscard]] auto async_run_cancellable_on(
	Target &target,
	Fn &&fn) {
	using fn_t = std::decay_t<Fn>;
	using T = std::invoke_result_t<fn_t &, conflux::work::root::Cancellation>;
	using namespace conflux::work::root;
	auto [task, src] = make_task_source<T>(SubmitOptions{.enable_cancellation = true});
	auto cancel = Cancellation{task.control()};
	auto shared_src = std::make_shared<TaskSource<T>>(std::move(src));
	auto job = [shared_src, cancel, fn = fn_t{std::forward<Fn>(fn)}]() mutable {
		try {
			if (cancel.requested()) {
				auto _ = shared_src->try_set_cancelled(cancel.reason());
				return;
			}
			if constexpr (std::is_void_v<T>) {
				fn(cancel);
				auto _ = shared_src->try_set_value(Success<T>{});
			} else {
				auto _ = shared_src->try_set_value(Success<T>{fn(cancel)});
			}
		} catch (CancelledError const &err) { auto _ = shared_src->try_set_cancelled(err.reason()); } catch (...) {
			auto _ = shared_src->try_set_exception(std::current_exception());
		}
	};
	if (!target.enqueue(std::move(job))) {
		auto _ = shared_src->try_set_cancelled(work_errc::cancelled_requested);
	}
	return std::move(task);
}
export namespace conflux::work::race {

template<typename Target, typename Fn>
[[nodiscard]] auto task_on(
	Target &target,
	std::string_view label,
	Fn &&fn) {
	return candidate(label, ::async_run_cancellable_on(target, std::forward<Fn>(fn)));
}

} // namespace conflux::work::race
// Synchronous blocking wait for a root::Task<T> — no FileReader required.
// Useful when the task completes on a std::thread pool (not io_uring).
export template<typename T>
T sync_wait(
	conflux::work::root::Task<T> task) {
	using namespace conflux::work::root;
	auto outcome = blocking_join(into_join_handle(std::move(task)));
	if (outcome.is_failure()) {
		std::rethrow_exception(std::move(outcome).failure().error);
	}
	if (outcome.is_cancelled()) {
		throw ::Cancelled{};
	}
	if constexpr (!std::is_void_v<T>) {
		return std::move(outcome).success().value;
	}
}
export namespace conflux::work {

template<typename T>
using Task = root::Task<T>;

template<typename T>
using TaskSource = root::TaskSource<T>;

using TaskControl = root::TaskControl;

template<typename T>
using Outcome = root::Outcome<T>;

using CancelReason = root::CancelReason;

using root::blocking_join; // NOLINT(misc-unused-using-decls) — re-export for module consumers
using root::join_ready; // NOLINT(misc-unused-using-decls) — re-export for module consumers
using root::make_task_source; // NOLINT(misc-unused-using-decls)
using root::try_join_ready; // NOLINT(misc-unused-using-decls) — re-export for module consumers

} // namespace conflux::work
// P9 join_all: single-allocation implementation.
// Two allocs total: make_task_source (output control block) +
// std::make_shared<JoinState> (slots, handles, and join state in one block).
// Each input handle stored in JoinState — no per-task shared_ptr.
// The ready callback for slot I fires after the control block lock is dropped,
// calls join_ready() to extract the rvalue outcome without a blocking bridge,
// then decrements remaining. When remaining reaches 0, commits the result.
// Cancel-cascade: first cancellation calls request_cancel() on all sibling
// controls; shared_from_this keepalive prevents JoinState destruction during
// the cascade; all N slots must complete before commit.
namespace join_all_detail {

template<class T>
using JoinResultT = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
class AggregateError : public std::exception {
	std::vector<std::exception_ptr> causes_;

public:
	explicit AggregateError(
		std::vector<std::exception_ptr> causes) noexcept
		: causes_{std::move(causes)} {}
	char const *what() const noexcept override { return "join_all: multiple task failures"; }
	[[nodiscard]] std::span<std::exception_ptr const> causes_view() const noexcept { return causes_; }
};
template<typename... Ts>
struct JoinState : std::enable_shared_from_this<JoinState<Ts...>> {
	using Result = std::tuple<JoinResultT<Ts>...>;
	using Slots = std::tuple<std::optional<JoinResultT<Ts>>...>;

	std::atomic<std::size_t> remaining{sizeof...(Ts)};
	std::mutex mtx;
	std::vector<std::exception_ptr> errors;
	bool any_cancelled = false;
	conflux::work::root::CancelReason cancel_reason = conflux::work::root::CancelReason::requested;
	Slots slots;
	std::tuple<conflux::work::root::TaskJoinHandle<Ts>...> handles;
	conflux::work::root::TaskSource<Result> src;
	JoinState(
		conflux::work::root::TaskSource<Result> s,
		conflux::work::root::TaskJoinHandle<Ts>... hs)
		: handles{std::move(hs)...}
		, src{std::move(s)} {
		errors.reserve(sizeof...(Ts));
	}
	void cancel_all() noexcept {
		auto keepalive = this->shared_from_this();
		auto reason = conflux::work::root::CancelReason::requested;
		{
			std::scoped_lock lk{mtx};
			reason = cancel_reason;
		}
		auto cancel_one = [reason](auto &h) noexcept { auto _ = h.control().request_cancel(reason); };
		std::apply([&](auto &...hs) noexcept { (cancel_one(hs), ...); }, handles);
	}
	void commit() noexcept {
		using namespace conflux::work::root;
		if (any_cancelled) {
			auto _ = src.try_set_cancelled(cancel_reason);
			return;
		}
		if (errors.size() == 1) {
			auto _ = src.try_set_exception(std::move(errors[0]));
			return;
		}
		if (errors.size() > 1) {
			auto _ = src.try_set_exception(std::make_exception_ptr(AggregateError{std::move(errors)}));
			return;
		}
		try {
			auto result = std::apply([](auto &...opts) { return Result{std::move(*opts)...}; }, slots);
			auto _ = src.try_set_value(conflux::work::root::Success<Result>{std::move(result)});
		} catch (...) { auto _ = src.try_set_exception(std::current_exception()); }
	}
	template<std::size_t I>
	void on_ready() noexcept {
		using namespace conflux::work::root;
		using T = std::tuple_element_t<I, std::tuple<Ts...>>;
		auto outcome = join_ready(std::move(std::get<I>(handles)));
		bool should_cancel = false;
		if (outcome.is_success()) {
			if constexpr (std::is_void_v<T>) {
				std::get<I>(slots).emplace();
			} else {
				std::get<I>(slots) = std::move(outcome).success().value;
			}
		} else if (outcome.is_failure()) {
			std::scoped_lock lk{mtx};
			errors.push_back(std::move(outcome).failure().error);
		} else {
			std::scoped_lock lk{mtx};
			if (!any_cancelled) {
				any_cancelled = true;
				cancel_reason = outcome.cancelled().reason;
				should_cancel = true;
			}
		}
		if (should_cancel) {
			cancel_all();
		}
		if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			commit();
		}
	}
};

} // namespace join_all_detail
export template<typename... Ts>
[[nodiscard]] auto join_all(
	conflux::work::root::Task<Ts>... tasks)
	-> conflux::work::root::Task<std::tuple<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>> {
	using namespace conflux::work::root;
	using Result = std::tuple<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>;

	if constexpr (sizeof...(Ts) == 0) {
		auto [t, s] = make_task_source<Result>(SubmitOptions{.enable_cancellation = false});
		auto _ = s.try_set_value(Success<Result>{Result{}});
		return std::move(t);
	}

	auto [root_task, src] = make_task_source<Result>(SubmitOptions{.enable_cancellation = false});
	auto state =
		std::make_shared<join_all_detail::JoinState<Ts...>>(std::move(src), into_join_handle(std::move(tasks))...);

	[&state]<std::size_t... Is>(std::index_sequence<Is...>) {
		auto attach = [&state]<std::size_t I>() noexcept {
			std::get<I>(state->handles).control().set_on_ready_or_run([s = state]() noexcept {
				s->template on_ready<I>();
			});
		};
		(attach.template operator ()<Is>(), ...);
	}(std::index_sequence_for<Ts...>{});

	return std::move(root_task);
}
