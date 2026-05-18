module;
#include <liburing.h>
#include <linux/futex.h>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef CONFLUX_WORK_QUEUE_STATS
	#define CONFLUX_WORK_QUEUE_STATS 0
#endif

export module conflux.work;

import std;
import conflux.types;
export import conflux.work.root;
export struct Cancelled final : RE {
	Cancelled()
		: RE{"work cancelled"} {}
};
export struct WorkPoolQueueStats {
	u64 enqueue_attempts = 0;
	u64 enqueue_stopped_rejections = 0;
	u64 enqueue_full_rejections = 0;
	u64 admission_lock_acquisitions = 0;
	u64 admission_lock_contentions = 0;
	u64 local_lock_acquisitions = 0;
	u64 local_lock_contentions = 0;
	u64 steal_lock_acquisitions = 0;
	u64 steal_lock_contentions = 0;
	u64 local_pushes = 0;
	u64 local_push_full = 0;
	u64 inject_pushes = 0;
	u64 inject_push_full = 0;
	u64 local_pop_attempts = 0;
	u64 local_pop_hits = 0;
	u64 inject_pop_attempts = 0;
	u64 inject_pop_hits = 0;
	u64 steal_rounds = 0;
	u64 steal_victim_checks = 0;
	u64 steal_hits = 0;
	u64 jobs_run = 0;
	u64 wake_one_calls = 0;
	u64 wake_one_futex_wakes = 0;
	u64 wake_one_elided_no_parked = 0;
	u64 wake_all_calls = 0;
	u64 wake_all_futex_wakes = 0;
	u64 park_attempts = 0;
	u64 park_recheck_skips = 0;
	u64 futex_waits = 0;
	u64 job_slot_allocations = 0;
	u64 job_slab_allocations = 0;
	u64 job_slab_id_reuses = 0;
	u64 job_slab_releases = 0;
	u64 job_allocation_failures = 0;
	u64 queue_full_token_discards = 0;
	u64 remote_free_pushes = 0;
	u64 remote_free_fallbacks = 0;
	u64 remote_free_drained = 0;
	u64 token_take_failures = 0;
};
namespace work_detail {

constexpr SZ kNoWorker = NL<SZ>::max();

using Fn = conflux::work::root::detail::small_move_only_function<void()>;
inline int futex_wait_private(
	Atom<u32> &word,
	u32 expected) noexcept {
	auto *addr = reinterpret_cast<u32 *>(&word); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
	return static_cast<int>(::syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0));
}
inline int futex_wake_private(
	Atom<u32> &word,
	int count) noexcept {
	auto *addr = reinterpret_cast<u32 *>(&word); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
	return static_cast<int>(::syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0));
}
class QueueTarget {
public:
	virtual ~QueueTarget() = default;
	virtual bool enqueue(conflux::work::root::detail::small_move_only_function<void()> job) = 0;
};
// Vyukov-style MPMC bounded ring buffer, lock-free. Capacity rounded up to
// next power-of-2 at construction. try_push/try_pop are noexcept because item
// move-assign is noexcept for all WorkPool queue payloads.
template<typename T>
class MpmcRing {
	struct Slot {
		Atom<SZ> seq{0};
		T item{};
	};
	UP<Slot[]> slots_;
	SZ capacity_{};
	SZ mask_{};
	alignas(64) Atom<SZ> head_{0};
	alignas(64) Atom<SZ> tail_{0};

public:
	explicit MpmcRing(
		SZ capacity) {
		if (capacity == 0) {
			return;
		}
		SZ cap = 1;
		while (cap < capacity) {
			cap <<= 1;
		}
		capacity_ = cap;
		mask_ = cap - 1;
		slots_ = make_unique<Slot[]>(cap);
		for (SZ i = 0; i < cap; ++i) {
			slots_[i].seq.store(i, memory_order_relaxed);
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
		SZ pos = head_.load(memory_order_relaxed);
		for (;;) {
			Slot &slot = slots_[pos & mask_];
			SZ const seq = slot.seq.load(memory_order_acquire);
			auto const diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
			if (diff == 0) {
				if (head_.compare_exchange_weak(pos, pos + 1, memory_order_relaxed)) {
					slot.item = move(item);
					slot.seq.store(pos + 1, memory_order_release);
					return true;
				}
			} else if (diff < 0) {
				return false;
			} else {
				conflux::work::root::detail::cpu_pause();
				pos = head_.load(memory_order_relaxed);
			}
		}
	}
	[[nodiscard]] Opt<T> try_pop() noexcept {
		if (capacity_ == 0) {
			return nullopt;
		}
		SZ pos = tail_.load(memory_order_relaxed);
		for (;;) {
			Slot &slot = slots_[pos & mask_];
			SZ const seq = slot.seq.load(memory_order_acquire);
			auto const diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
			if (diff == 0) {
				if (tail_.compare_exchange_weak(pos, pos + 1, memory_order_relaxed)) {
					T item = move(slot.item);
					slot.seq.store(pos + capacity_, memory_order_release);
					return Opt<T>{move(item)};
				}
			} else if (diff < 0) {
				return nullopt;
			} else {
				conflux::work::root::detail::cpu_pause();
				pos = tail_.load(memory_order_relaxed);
			}
		}
	}
};

class WorkPoolQueueCounters {
#if CONFLUX_WORK_QUEUE_STATS
	Atom<u64> enqueue_attempts_{0};
	Atom<u64> enqueue_stopped_rejections_{0};
	Atom<u64> enqueue_full_rejections_{0};
	Atom<u64> admission_lock_acquisitions_{0};
	Atom<u64> admission_lock_contentions_{0};
	Atom<u64> local_lock_acquisitions_{0};
	Atom<u64> local_lock_contentions_{0};
	Atom<u64> steal_lock_acquisitions_{0};
	Atom<u64> steal_lock_contentions_{0};
	Atom<u64> local_pushes_{0};
	Atom<u64> local_push_full_{0};
	Atom<u64> inject_pushes_{0};
	Atom<u64> inject_push_full_{0};
	Atom<u64> local_pop_attempts_{0};
	Atom<u64> local_pop_hits_{0};
	Atom<u64> inject_pop_attempts_{0};
	Atom<u64> inject_pop_hits_{0};
	Atom<u64> steal_rounds_{0};
	Atom<u64> steal_victim_checks_{0};
	Atom<u64> steal_hits_{0};
	Atom<u64> jobs_run_{0};
	Atom<u64> wake_one_calls_{0};
	Atom<u64> wake_one_futex_wakes_{0};
	Atom<u64> wake_one_elided_no_parked_{0};
	Atom<u64> wake_all_calls_{0};
	Atom<u64> wake_all_futex_wakes_{0};
	Atom<u64> park_attempts_{0};
	Atom<u64> park_recheck_skips_{0};
	Atom<u64> futex_waits_{0};
	Atom<u64> queue_full_token_discards_{0};

	static void add(
		Atom<u64> &counter) noexcept {
		counter.fetch_add(1, memory_order_relaxed);
	}
	static void clear(
		Atom<u64> &counter) noexcept {
		counter.store(0, memory_order_relaxed);
	}
	[[nodiscard]] static u64 get(
		Atom<u64> const &counter) noexcept {
		return counter.load(memory_order_relaxed);
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
export enum class WorkPoolQueueMode : u8 {
	stealing,
	no_stealing,
};
export struct WorkPoolOptions {
	SZ threads = 0;
	SZ max_inject_queue = 4096;
	SZ inject_queue_shards = 0;
	SZ local_queue_capacity = 1024;
	SZ initial_job_slab_slots = 256;
	SZ max_job_slab_slots = 4096;
	WorkPoolQueueMode queue_mode = WorkPoolQueueMode::stealing;
	u32 spin_before_park = 256;
	int numa_node = -1;
	bool pin_workers = false;
	S worker_name_prefix = "conflux-work";
	Fn<void(EP)> raw_exception_sink{};
};
export struct RingLaneOptions {
	int ring_fd = -1;
	u64 wake_user_data = 0x434F4E464C5558ULL; // "CONFLUX"
	SZ drain_budget = 0;
	bool allow_inline_on_owner = true;
};

export enum class WorkError : u8 {
	stopped,
	queue_full,
	wake_failed,
	submit_failed,
	cancelled,
	owner_violation,
};
export class WorkPool final : public work_detail::QueueTarget {
	struct alignas(
		64) Worker {
		explicit Worker(
			SZ local_capacity)
			: no_stealing_local{local_capacity} {}

		mutex mtx;
		deque<work_detail::Fn> local{};
		work_detail::MpmcRing<work_detail::Fn> no_stealing_local;
		jthread thread{};
	};
	WorkPoolOptions options_{};
	V<UP<Worker>> workers_{};
	V<UP<work_detail::MpmcRing<work_detail::Fn>>> inject_rings_{};
	Atom<u64> inject_enqueue_cursor_{0};
	Atom<u32> wake_epoch_{0};
	// parked_ on a separate cache line: producer loads it after every push;
	// isolating prevents wake_epoch_ stores from invalidating this line and
	// adding a cache miss to the no-parked-workers path.
	alignas(64) Atom<int> parked_{0};
	Atom<SZ> pending_{0};
	atomic_flag accepting_stopped_{};
	atomic_flag stopping_{};
	mutex admission_mtx_{};
	Atom<u64> no_stealing_admission_{0};
	work_detail::WorkPoolQueueCounters queue_counters_{};

	static constexpr u64 kNoStealingAdmissionClosed = u64{1} << 63;
	static constexpr u64 kNoStealingAdmissionCountMask = ~kNoStealingAdmissionClosed;

	inline static thread_local WorkPool *tls_pool_ = nullptr;
	inline static thread_local SZ tls_worker_ = work_detail::kNoWorker;
	[[nodiscard]] bool is_local_worker() const noexcept {
		return tls_pool_ == this && tls_worker_ != work_detail::kNoWorker;
	}
	[[nodiscard]] bool no_stealing_mode() const noexcept {
		return options_.queue_mode == WorkPoolQueueMode::no_stealing;
	}
	[[nodiscard]] SZ inject_shard_count() const noexcept { return inject_rings_.size(); }
	[[nodiscard]] bool begin_no_stealing_admission() noexcept {
		auto state = no_stealing_admission_.load(memory_order_acquire);
		for (;;) {
			if ((state & kNoStealingAdmissionClosed) != 0
				|| accepting_stopped_.test(memory_order_acquire)
				|| stopping_.test(memory_order_acquire)) {
				return false;
			}
			if (no_stealing_admission_
					.compare_exchange_weak(state, state + 1, memory_order_acq_rel, memory_order_acquire)) {
				if (accepting_stopped_.test(memory_order_acquire) || stopping_.test(memory_order_acquire)) {
					end_no_stealing_admission();
					return false;
				}
				return true;
			}
		}
	}
	void end_no_stealing_admission() noexcept { no_stealing_admission_.fetch_sub(1, memory_order_release); }
	void close_no_stealing_admission() noexcept {
		no_stealing_admission_.fetch_or(kNoStealingAdmissionClosed, memory_order_acq_rel);
	}
	void wait_no_stealing_admission_idle() const noexcept {
		while ((no_stealing_admission_.load(memory_order_acquire) & kNoStealingAdmissionCountMask) != 0) {
			conflux::work::root::detail::cpu_pause();
		}
	}
	void wake_one() noexcept {
		queue_counters_.note_wake_one();
		// P6 candidate (b) — fence-between: release store on wake_epoch_ +
		// SC fence forms one half of the SC fence pair with the worker's
		// parked_++ + SC fence. Guarantees: if any worker is parked (parked_>0)
		// we issue a wake; if none are parked, the futex_wake is elided.
		wake_epoch_.fetch_add(1, memory_order_release);
		std::atomic_thread_fence(memory_order_seq_cst);
		if (parked_.load(memory_order_acquire) > 0) {
			queue_counters_.note_wake_one_futex();
			work_detail::futex_wake_private(wake_epoch_, 1);
		} else {
			queue_counters_.note_wake_one_elided();
		}
	}
	void wake_all() noexcept {
		queue_counters_.note_wake_all();
		// Unconditional: shutdown must guarantee all parked workers exit.
		wake_epoch_.fetch_add(1, memory_order_release);
		queue_counters_.note_wake_all_futex();
		work_detail::futex_wake_private(wake_epoch_, static_cast<int>(workers_.size()));
	}
	[[nodiscard]] bool push_local(
		work_detail::Fn job) {
		auto &worker = *workers_[tls_worker_];
		try {
			auto lk = queue_counters_.lock_local(worker.mtx);
			if (worker.local.size() >= options_.local_queue_capacity) {
				queue_counters_.note_local_push_full();
				return false;
			}
			worker.local.push_back(move(job));
		} catch (std::bad_alloc const &) {
			queue_counters_.note_local_push_full();
			return false;
		}
		pending_.fetch_add(1, memory_order_release);
		queue_counters_.note_local_push();
		return true;
	}
	[[nodiscard]] bool push_no_stealing_local(
		work_detail::Fn job) noexcept {
		if (options_.local_queue_capacity == 0) {
			queue_counters_.note_local_push_full();
			return false;
		}
		auto &worker = *workers_[tls_worker_];
		if (!worker.no_stealing_local.try_push(move(job))) {
			queue_counters_.note_local_push_full();
			return false;
		}
		pending_.fetch_add(1, memory_order_release);
		queue_counters_.note_local_push();
		return true;
	}
	[[nodiscard]] bool push_inject(
		work_detail::Fn job) noexcept {
		SZ const shards = inject_shard_count();
		if (shards == 0 || options_.max_inject_queue == 0) {
			queue_counters_.note_inject_push_full();
			return false;
		}
		SZ const start =
			shards == 1 ? SZ{0} : static_cast<SZ>(inject_enqueue_cursor_.fetch_add(1, memory_order_relaxed) % shards);
		if (inject_rings_[start]->try_push(move(job))) {
			pending_.fetch_add(1, memory_order_release);
			queue_counters_.note_inject_push();
			return true;
		}
		queue_counters_.note_inject_push_full();
		return false;
	}
	[[nodiscard]] Opt<work_detail::Fn> pop_local(
		SZ index) {
		queue_counters_.note_local_pop_attempt();
		auto &worker = *workers_[index];
		auto lk = queue_counters_.lock_local(worker.mtx);
		if (worker.local.empty()) {
			return nullopt;
		}
		auto job = move(worker.local.back());
		worker.local.pop_back();
		queue_counters_.note_local_pop_hit();
		return job;
	}
	[[nodiscard]] Opt<work_detail::Fn> pop_no_stealing_local(
		SZ index) noexcept {
		queue_counters_.note_local_pop_attempt();
		auto job = workers_[index]->no_stealing_local.try_pop();
		if (!job) {
			return nullopt;
		}
		queue_counters_.note_local_pop_hit();
		return job;
	}
	[[nodiscard]] Opt<work_detail::Fn> pop_inject(
		SZ worker_index) noexcept {
		queue_counters_.note_inject_pop_attempt();
		SZ const shards = inject_shard_count();
		if (shards == 0) {
			return nullopt;
		}
		SZ const start = worker_index % shards;
		for (SZ offset = 0; offset < shards; ++offset) {
			SZ const shard = (start + offset) % shards;
			auto job = inject_rings_[shard]->try_pop();
			if (!job) {
				continue;
			}
			queue_counters_.note_inject_pop_hit();
			return job;
		}
		return nullopt;
	}
	[[nodiscard]] Opt<work_detail::Fn> steal_work(
		SZ thief) {
		queue_counters_.note_steal_round();
		for (SZ offset = 1; offset < workers_.size(); ++offset) {
			SZ const victim_index = (thief + offset) % workers_.size();
			auto &victim = *workers_[victim_index];
			queue_counters_.note_steal_victim_check();
			auto lk = queue_counters_.lock_steal_victim(victim.mtx);
			if (victim.local.empty()) {
				continue;
			}
			queue_counters_.note_steal_hit();
			auto job = move(victim.local.front());
			victim.local.pop_front();
			return job;
		}
		return nullopt;
	}
	static void maybe_set_name(
		S const &prefix,
		SZ index) noexcept {
		if (prefix.empty()) {
			return;
		}
		auto name = format("{}-{}", prefix, index);
		if (name.size() > 15) {
			name.resize(15);
		}
		::pthread_setname_np(::pthread_self(), name.c_str());
	}
	void maybe_pin_worker(
		SZ index) noexcept {
		if (!options_.pin_workers) {
			return;
		}
		cpu_set_t set;
		CPU_ZERO(&set);
		unsigned const cpus = max(1U, thread::hardware_concurrency());
		CPU_SET(index % cpus, &set);
		::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
	}
	void worker_loop(
		std::stop_token const &st,
		SZ index) {
		tls_pool_ = this;
		tls_worker_ = index;
		maybe_set_name(options_.worker_name_prefix, index);
		maybe_pin_worker(index);
		while (!st.stop_requested() && !stopping_.test(memory_order_acquire)) {
			auto job = no_stealing_mode() ? pop_no_stealing_local(index) : pop_local(index);
			if (!job) {
				job = pop_inject(index);
			}
			if (!job && !no_stealing_mode()) {
				job = steal_work(index);
			}
			if (job) {
				queue_counters_.note_job_run();
				try {
					(*job)();
				} catch (...) {
					if (options_.raw_exception_sink) {
						try {
							options_.raw_exception_sink(current_exception());
						} catch (...) {} // NOLINT(bugprone-empty-catch)
					}
				}
				pending_.fetch_sub(1, memory_order_release);
				continue;
			}
			auto const has_pending = [&] { return pending_.load(memory_order_relaxed) > 0; };
			bool spun = false;
			for (u32 s = 0; s < options_.spin_before_park && !spun; ++s) {
				conflux::work::root::detail::cpu_pause();
				spun = has_pending();
			}
			if (!spun) {
				queue_counters_.note_park_attempt();
				// P6 candidate (b) park protocol:
				// 1. Announce parked before the re-check so a concurrent push
				//    that lands after our spin loop sees us parked and wakes us.
				parked_.fetch_add(1, memory_order_acq_rel);
				// 2. SC fence: parked++ is globally visible before pending_ load,
				//    forming the pair with wake_one()'s SC fence.
				std::atomic_thread_fence(memory_order_seq_cst);
				// 3. Re-check: if a producer pushed between our spin loop and
				//    parked++, either pending_ reflects it (→ skip park) or the
				//    producer saw parked_>0 and issued a wake (→ futex_wait
				//    returns EAGAIN immediately on the stale epoch).
				if (pending_.load(memory_order_acquire) > 0 || stopping_.test(memory_order_acquire)) {
					queue_counters_.note_park_recheck_skip();
					parked_.fetch_sub(1, memory_order_acq_rel);
				} else {
					u32 const epoch = wake_epoch_.load(memory_order_acquire);
					queue_counters_.note_futex_wait();
					work_detail::futex_wait_private(wake_epoch_, epoch);
					parked_.fetch_sub(1, memory_order_acq_rel);
				}
			}
		}
		tls_pool_ = nullptr;
		tls_worker_ = work_detail::kNoWorker;
	}
	[[nodiscard]] bool enqueue_stealing(
		work_detail::Fn job) {
		auto admission = queue_counters_.lock_admission(admission_mtx_);
		if (accepting_stopped_.test(memory_order_acquire) || stopping_.test(memory_order_acquire)) {
			queue_counters_.note_enqueue_stopped_rejection();
			return false;
		}
		bool const queued = is_local_worker() ? push_local(move(job)) : push_inject(move(job));
		if (!queued) {
			queue_counters_.note_enqueue_full_rejection();
			return false;
		}
		admission.unlock();
		wake_one();
		return true;
	}
	[[nodiscard]] bool enqueue_no_stealing(
		work_detail::Fn job) noexcept {
		if (!begin_no_stealing_admission()) {
			queue_counters_.note_enqueue_stopped_rejection();
			return false;
		}
		bool const queued = is_local_worker() ? push_no_stealing_local(move(job)) : push_inject(move(job));
		end_no_stealing_admission();
		if (!queued) {
			queue_counters_.note_enqueue_full_rejection();
			return false;
		}
		wake_one();
		return true;
	}
	void stop_no_stealing() noexcept {
		accepting_stopped_.test_and_set(memory_order_acq_rel);
		close_no_stealing_admission();
		wait_no_stealing_admission_idle();
		if (!stopping_.test_and_set(memory_order_acq_rel)) {
			for (auto &worker: workers_) {
				worker->thread.request_stop();
			}
			wake_all();
		}
	}
	void drain_and_stop_no_stealing() noexcept {
		accepting_stopped_.test_and_set(memory_order_acq_rel);
		close_no_stealing_admission();
		wait_no_stealing_admission_idle();
		while (pending_.load(memory_order_acquire) > 0) {
			wake_all();
			std::this_thread::yield();
		}
		stop_no_stealing();
		wait();
	}
		void discard_queued_jobs() noexcept {
			pending_.store(0, memory_order_release);
		}

public:
	explicit WorkPool(
		WorkPoolOptions options = {})
		: options_{move(options)} {
		if (options_.threads == 0) {
			options_.threads = max(1U, thread::hardware_concurrency());
		}
			SZ inject_shards = options_.inject_queue_shards;
			if (inject_shards == 0) {
				inject_shards = options_.max_inject_queue == 0 ? SZ{1} : max(SZ{1}, min(options_.threads, options_.max_inject_queue));
			}
			options_.inject_queue_shards = inject_shards;
			SZ const inject_capacity_per_shard = options_.max_inject_queue == 0
				? SZ{0}
				: (options_.max_inject_queue + inject_shards - 1) / inject_shards;
			inject_rings_.reserve(inject_shards);
			for (SZ shard = 0; shard < inject_shards; ++shard) {
				inject_rings_.push_back(make_unique<work_detail::MpmcRing<work_detail::Fn>>(inject_capacity_per_shard));
			}
		workers_.reserve(options_.threads);
		for (SZ i = 0; i < options_.threads; ++i) {
			workers_.push_back(make_unique<Worker>(options_.local_queue_capacity));
		}
		for (SZ i = 0; i < workers_.size(); ++i) {
			workers_[i]->thread = jthread([this, i](std::stop_token const &st) { worker_loop(st, i); });
		}
	}

	~WorkPool() override {
		stop();
		wait();
	}
	WorkPool(WorkPool const &) = delete;
	WorkPool &operator =(WorkPool const &) = delete;
	WorkPool(WorkPool &&) = delete;
	WorkPool &operator =(WorkPool &&) = delete;
	[[nodiscard]] bool enqueue(
		work_detail::Fn job) override {
		queue_counters_.note_enqueue_attempt();
		if (no_stealing_mode()) {
			return enqueue_no_stealing(move(job));
		}
		return enqueue_stealing(move(job));
	}
	void stop() noexcept {
		if (no_stealing_mode()) {
			stop_no_stealing();
			return;
		}
		auto admission = queue_counters_.lock_admission(admission_mtx_);
		accepting_stopped_.test_and_set(memory_order_acq_rel);
		if (!stopping_.test_and_set(memory_order_acq_rel)) {
			for (auto &worker: workers_) {
				worker->thread.request_stop();
			}
			wake_all();
		}
	}
	void drain_and_stop() noexcept {
		if (no_stealing_mode()) {
			drain_and_stop_no_stealing();
			return;
		}
		{
			auto admission = queue_counters_.lock_admission(admission_mtx_);
			accepting_stopped_.test_and_set(memory_order_acq_rel);
		}
		while (pending_.load(memory_order_acquire) > 0) {
			wake_all();
			std::this_thread::yield();
		}
		stop();
		wait();
	}
	void wait() noexcept {
		for (auto &worker: workers_) {
			if (worker->thread.joinable()) {
				worker->thread.join();
			}
		}
		discard_queued_jobs();
	}
	[[nodiscard]] bool stopped() const noexcept { return accepting_stopped_.test(memory_order_acquire); }
		[[nodiscard]] WorkPoolQueueStats queue_stats() const noexcept {
			return queue_counters_.snapshot();
		}
		void reset_queue_stats() noexcept {
			queue_counters_.reset();
		}
};
// io_uring-coupled executor: requires a live ring (ring_fd from RingLaneOptions).
// Each non-inline enqueue into an empty queue issues exactly one
// io_uring_register_sync_msg syscall. Uses raw io_uring_sqe only — no conflux.net
// dependency; callers own the ring and drive drain() from CQE handlers.
export class RingLane final : public work_detail::QueueTarget {
	RingLaneOptions options_{};
	mutex mtx_{};
	deque<conflux::work::root::detail::small_move_only_function<void()>> queue_{};
	atomic_flag stopped_{};
	atomic_flag wake_pending_{};
	thread::id owner_{std::this_thread::get_id()};
	[[nodiscard]] bool is_owner_thread() const noexcept { return std::this_thread::get_id() == owner_; }
	[[nodiscard]] bool wake_ring() noexcept {
		if (options_.ring_fd < 0) {
			return false;
		}
		io_uring_sqe sqe{};
		io_uring_prep_msg_ring(&sqe, options_.ring_fd, 0, options_.wake_user_data, 0);
		return io_uring_register_sync_msg(&sqe) == 0;
	}
	void run_inline(
		conflux::work::root::detail::small_move_only_function<void()> job) {
		try {
			job();
		} catch (...) {} // NOLINT(bugprone-empty-catch)
	}

public:
	explicit RingLane(
		RingLaneOptions options = {})
		: options_{move(options)} {}
	[[nodiscard]] bool enqueue(
		conflux::work::root::detail::small_move_only_function<void()> job) override {
		if (stopped_.test(memory_order_acquire)) {
			return false;
		}
		if (is_owner_thread() && options_.allow_inline_on_owner) {
			run_inline(move(job));
			return true;
		}
		bool need_wake = false;
		{
			SL const lk{mtx_};
			need_wake = queue_.empty();
			queue_.push_back(move(job));
			if (need_wake && !wake_pending_.test_and_set(memory_order_acq_rel)) {
				if (!wake_ring()) {
					queue_.pop_back();
					wake_pending_.clear(memory_order_release);
					return false;
				}
			}
		}
		return true;
	}
	void adopt_current_thread() noexcept { owner_ = std::this_thread::get_id(); }
	[[nodiscard]] SZ drain() {
		if (!is_owner_thread()) {
			throw conflux::work::root::JoinError{conflux::work::root::JoinError::reason::thread_precondition};
		}
		SZ ran = 0;
		SZ const budget = options_.drain_budget == 0 ? NL<SZ>::max() : options_.drain_budget;
		while (ran < budget) {
			conflux::work::root::detail::small_move_only_function<void()> job;
			{
				SL const lk{mtx_};
				if (queue_.empty()) {
					wake_pending_.clear(memory_order_release);
					break;
				}
				job = move(queue_.front());
				queue_.pop_front();
				if (queue_.empty()) {
					wake_pending_.clear(memory_order_release);
				}
			}
			run_inline(move(job));
			++ran;
		}
		if (ran == budget) {
			SL const lk{mtx_};
			if (!queue_.empty() && !wake_pending_.test_and_set(memory_order_acq_rel)) {
				auto _ = wake_ring();
			}
		}
		return ran;
	}
	void stop() noexcept { stopped_.test_and_set(memory_order_release); }
	[[nodiscard]] bool stopped() const noexcept { return stopped_.test(memory_order_acquire); }
	[[nodiscard]] bool on_owner_thread() const noexcept { return is_owner_thread(); }
	[[nodiscard]] int ring_fd() const noexcept { return options_.ring_fd; }
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
	auto shared_src = make_shared<TaskSource<T>>(move(src));
	auto job = [shared_src, fn = fn_t{forward<Fn>(fn)}]() mutable {
		try {
			if constexpr (std::is_void_v<T>) {
				fn();
				auto _ = shared_src->try_set_value(Success<T>{});
			} else {
				auto _ = shared_src->try_set_value(Success<T>{fn()});
			}
		} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
	};
	if (!target.enqueue(move(job))) {
		auto _ = shared_src->try_set_cancelled(work_errc::cancelled_requested);
	}
	return move(task);
}
// Compatibility alias: run_on_task returns a root::Task<T>, so new code should
// use the async_* spelling until the final release alias-removal pass.
export template<typename Target, typename Fn>
[[nodiscard]] auto run_on_task(
	Target &target,
	Fn &&fn) {
	return async_run_on(target, forward<Fn>(fn));
}
// Synchronous blocking wait for a root::Task<T> — no FileReader required.
// Useful when the task completes on a thread pool (not io_uring).
export template<typename T>
T sync_wait(
	conflux::work::root::Task<T> task) {
	using namespace conflux::work::root;
	auto outcome = blocking_join(into_join_handle(move(task)));
	if (outcome.is_failure()) {
		rethrow_exception(move(outcome).failure().error);
	}
	if (outcome.is_cancelled()) {
		throw ::Cancelled{};
	}
	if constexpr (!std::is_void_v<T>) {
		return move(outcome).success().value;
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
using root::join; // NOLINT(misc-unused-using-decls) — compatibility re-export for module consumers
using root::join_ready; // NOLINT(misc-unused-using-decls) — re-export for module consumers
using root::make_task_source; // NOLINT(misc-unused-using-decls)
using root::try_join_ready; // NOLINT(misc-unused-using-decls) — re-export for module consumers

} // namespace conflux::work
// P9 join_all: single-allocation implementation.
// Two allocs total: make_task_source (output control block) +
// make_shared<JoinState> (slots, handles, and join state in one block).
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
class AggregateError : public exception {
	V<EP> causes_;

public:
	explicit AggregateError(
		V<EP> causes) noexcept
		: causes_{move(causes)} {}
	char const *what() const noexcept override { return "join_all: multiple task failures"; }
	[[nodiscard]] span<EP const> causes_view() const noexcept { return causes_; }
};
template<typename... Ts>
struct JoinState : std::enable_shared_from_this<JoinState<Ts...>> {
	using Result = std::tuple<JoinResultT<Ts>...>;
	using Slots = std::tuple<std::optional<JoinResultT<Ts>>...>;

	Atom<SZ> remaining{sizeof...(Ts)};
	mutex mtx;
	V<EP> errors;
	bool any_cancelled = false;
	Slots slots;
	Tup<conflux::work::root::TaskJoinHandle<Ts>...> handles;
	conflux::work::root::TaskSource<Result> src;
	JoinState(
		conflux::work::root::TaskSource<Result> s,
		conflux::work::root::TaskJoinHandle<Ts>... hs)
		: handles{move(hs)...}
		, src{move(s)} {
		errors.reserve(sizeof...(Ts));
	}
	void cancel_all() noexcept {
		auto keepalive = this->shared_from_this();
		auto cancel_one = [](auto &h) noexcept { auto _ = h.control().request_cancel(); };
		std::apply([&](auto &...hs) noexcept { (cancel_one(hs), ...); }, handles);
	}
	void commit() noexcept {
		using namespace conflux::work::root;
		if (any_cancelled) {
			auto _ = src.try_set_cancelled(work_errc::cancelled_requested);
			return;
		}
		if (errors.size() == 1) {
			auto _ = src.try_set_exception(move(errors[0]));
			return;
		}
		if (errors.size() > 1) {
			auto _ = src.try_set_exception(make_exception_ptr(AggregateError{move(errors)}));
			return;
		}
		try {
			auto result = std::apply([](auto &...opts) { return Result{move(*opts)...}; }, slots);
			auto _ = src.try_set_value(conflux::work::root::Success<Result>{move(result)});
		} catch (...) { auto _ = src.try_set_exception(current_exception()); }
	}
	template<SZ I>
	void on_ready() noexcept {
		using namespace conflux::work::root;
		using T = std::tuple_element_t<I, std::tuple<Ts...>>;
		auto outcome = join_ready(move(std::get<I>(handles)));
		bool should_cancel = false;
		if (outcome.is_success()) {
			if constexpr (std::is_void_v<T>) {
				std::get<I>(slots).emplace();
			} else {
				std::get<I>(slots) = move(outcome).success().value;
			}
		} else if (outcome.is_failure()) {
			SL lk{mtx};
			errors.push_back(move(outcome).failure().error);
		} else {
			SL lk{mtx};
			if (!any_cancelled) {
				any_cancelled = true;
				should_cancel = true;
			}
		}
		if (should_cancel) {
			cancel_all();
		}
		if (remaining.fetch_sub(1, memory_order_acq_rel) == 1) {
			commit();
		}
	}
};

} // namespace join_all_detail
export template<typename... Ts>
[[nodiscard]] auto join_all(
	conflux::work::root::Task<Ts>... tasks)
	-> conflux::work::root::Task<Tup<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>> {
	using namespace conflux::work::root;
	using Result = std::tuple<std::conditional_t<std::is_void_v<Ts>, std::monostate, Ts>...>;

	if constexpr (sizeof...(Ts) == 0) {
		auto [t, s] = make_task_source<Result>(SubmitOptions{.enable_cancellation = false});
		auto _ = s.try_set_value(Success<Result>{Result{}});
		return move(t);
	}

	auto [root_task, src] = make_task_source<Result>(SubmitOptions{.enable_cancellation = false});
	auto state = make_shared<join_all_detail::JoinState<Ts...>>(move(src), into_join_handle(move(tasks))...);

	[&state]<SZ... Is>(std::index_sequence<Is...>) {
		auto attach = [&state]<SZ I>() noexcept {
			std::get<I>(state->handles).control().set_on_ready_or_run([s = state]() noexcept {
				s->template on_ready<I>();
			});
		};
		(attach.template operator ()<Is>(), ...);
	}(std::index_sequence_for<Ts...>{});

	return move(root_task);
}
