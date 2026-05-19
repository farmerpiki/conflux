module;
#include <liburing.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

module conflux.work;

import std;


struct alignas(64) WorkPoolWorker {
	explicit WorkPoolWorker(std::size_t local_capacity)
		: no_stealing_local{local_capacity} {}

	std::mutex mtx;
	std::deque<work_detail::Fn> local{};
	work_detail::MpmcRing<work_detail::Fn> no_stealing_local;
	std::jthread thread{};
};

struct WorkPoolState {
	static constexpr std::uint64_t kNoStealingAdmissionClosed = std::uint64_t{1} << 63;
	static constexpr std::uint64_t kNoStealingAdmissionCountMask = ~kNoStealingAdmissionClosed;
	inline static thread_local WorkPoolState *tls_pool = nullptr;
	inline static thread_local std::size_t tls_worker = work_detail::kNoWorker;

	WorkPoolOptions options{};
	std::vector<std::unique_ptr<WorkPoolWorker>> workers{};
	std::vector<std::unique_ptr<work_detail::MpmcRing<work_detail::Fn>>> inject_rings{};
	std::atomic<std::uint64_t> inject_enqueue_cursor{0};
	std::atomic<std::uint32_t> wake_epoch{0};
	alignas(64) std::atomic<int> parked{0};
	std::atomic<std::size_t> pending{0};
	std::atomic_flag accepting_stopped{};
	std::atomic_flag stopping{};
	std::mutex admission_mtx{};
	std::atomic<std::uint64_t> no_stealing_admission{0};
	alignas(64) std::atomic<std::size_t> stealable_local_jobs{0};
	work_detail::WorkPoolQueueCounters queue_counters{};

	explicit WorkPoolState(WorkPoolOptions opts)
		: options{std::move(opts)} {
		if (options.threads == 0) {
			options.threads = std::max(1U, std::thread::hardware_concurrency());
		}
		std::size_t inject_shards = options.inject_queue_shards;
		if (inject_shards == 0) {
			inject_shards = options.max_inject_queue == 0 ? std::size_t{1} : std::max(std::size_t{1}, std::min(options.threads, options.max_inject_queue));
		}
		options.inject_queue_shards = inject_shards;
		std::size_t const inject_capacity_per_shard = options.max_inject_queue == 0
			? std::size_t{0}
			: (options.max_inject_queue + inject_shards - 1) / inject_shards;
		inject_rings.reserve(inject_shards);
		for (std::size_t shard = 0; shard < inject_shards; ++shard) {
			inject_rings.push_back(std::make_unique<work_detail::MpmcRing<work_detail::Fn>>(inject_capacity_per_shard));
		}
		workers.reserve(options.threads);
		for (std::size_t i = 0; i < options.threads; ++i) {
			workers.push_back(std::make_unique<WorkPoolWorker>(options.local_queue_capacity));
		}
		for (std::size_t i = 0; i < workers.size(); ++i) {
			workers[i]->thread = std::jthread([this, i](std::stop_token const &st) { worker_loop(st, i); });
		}
	}

	~WorkPoolState() {
		stop();
		wait();
	}

	[[nodiscard]] bool is_local_worker() const noexcept {
		return tls_pool == this && tls_worker != work_detail::kNoWorker;
	}
	[[nodiscard]] bool no_stealing_mode() const noexcept {
		return options.queue_mode == WorkPoolQueueMode::no_stealing;
	}
	[[nodiscard]] std::size_t inject_shard_count() const noexcept {
		return inject_rings.size();
	}
	[[nodiscard]] bool begin_no_stealing_admission() noexcept {
		auto state = no_stealing_admission.load(std::memory_order_acquire);
		for (;;) {
			if ((state & kNoStealingAdmissionClosed) != 0
				|| accepting_stopped.test(std::memory_order_acquire)
				|| stopping.test(std::memory_order_acquire)) {
				return false;
			}
			if (no_stealing_admission.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
				if (accepting_stopped.test(std::memory_order_acquire) || stopping.test(std::memory_order_acquire)) {
					end_no_stealing_admission();
					return false;
				}
				return true;
			}
		}
	}
	void end_no_stealing_admission() noexcept {
		no_stealing_admission.fetch_sub(1, std::memory_order_release);
	}
	void close_no_stealing_admission() noexcept {
		no_stealing_admission.fetch_or(kNoStealingAdmissionClosed, std::memory_order_acq_rel);
	}
	void wait_no_stealing_admission_idle() const noexcept {
		while ((no_stealing_admission.load(std::memory_order_acquire) & kNoStealingAdmissionCountMask) != 0) {
			conflux::work::root::detail::cpu_pause();
		}
	}
	void wake_one() noexcept {
		queue_counters.note_wake_one();
		wake_epoch.fetch_add(1, std::memory_order_release);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		if (parked.load(std::memory_order_acquire) > 0) {
			queue_counters.note_wake_one_futex();
			work_detail::futex_wake_private(wake_epoch, 1);
		} else {
			queue_counters.note_wake_one_elided();
		}
	}
	void wake_all() noexcept {
		queue_counters.note_wake_all();
		wake_epoch.fetch_add(1, std::memory_order_release);
		queue_counters.note_wake_all_futex();
		work_detail::futex_wake_private(wake_epoch, static_cast<int>(workers.size()));
	}
	[[nodiscard]] bool push_local(work_detail::Fn job) {
		auto &worker = *workers[tls_worker];
		try {
			auto lk = queue_counters.lock_local(worker.mtx);
			if (worker.local.size() >= options.local_queue_capacity) {
				queue_counters.note_local_push_full();
				return false;
			}
			worker.local.push_back(std::move(job));
		} catch (std::bad_alloc const &) {
			queue_counters.note_local_push_full();
			return false;
		}
		stealable_local_jobs.fetch_add(1, std::memory_order_release);
		pending.fetch_add(1, std::memory_order_release);
		queue_counters.note_local_push();
		return true;
	}
	[[nodiscard]] bool push_no_stealing_local(work_detail::Fn job) noexcept {
		if (options.local_queue_capacity == 0) {
			queue_counters.note_local_push_full();
			return false;
		}
		auto &worker = *workers[tls_worker];
		if (!worker.no_stealing_local.try_push(std::move(job))) {
			queue_counters.note_local_push_full();
			return false;
		}
		pending.fetch_add(1, std::memory_order_release);
		queue_counters.note_local_push();
		return true;
	}
	[[nodiscard]] bool push_inject(work_detail::Fn job) noexcept {
		std::size_t const shards = inject_shard_count();
		if (shards == 0 || options.max_inject_queue == 0) {
			queue_counters.note_inject_push_full();
			return false;
		}
		std::size_t const start = shards == 1 ? std::size_t{0} : inject_enqueue_cursor.fetch_add(1, std::memory_order_relaxed) % shards;
		if (inject_rings[start]->try_push(std::move(job))) {
			pending.fetch_add(1, std::memory_order_release);
			queue_counters.note_inject_push();
			return true;
		}
		queue_counters.note_inject_push_full();
		return false;
	}
	[[nodiscard]] std::optional<work_detail::Fn> pop_local(std::size_t index) {
		queue_counters.note_local_pop_attempt();
		auto &worker = *workers[index];
		auto lk = queue_counters.lock_local(worker.mtx);
		if (worker.local.empty()) {
			return std::nullopt;
		}
		auto job = std::move(worker.local.back());
		worker.local.pop_back();
		stealable_local_jobs.fetch_sub(1, std::memory_order_acq_rel);
		queue_counters.note_local_pop_hit();
		return job;
	}
	[[nodiscard]] std::optional<work_detail::Fn> pop_no_stealing_local(std::size_t index) noexcept {
		queue_counters.note_local_pop_attempt();
		auto job = workers[index]->no_stealing_local.try_pop();
		if (!job) {
			return std::nullopt;
		}
		queue_counters.note_local_pop_hit();
		return job;
	}
	[[nodiscard]] std::optional<work_detail::Fn> pop_inject(std::size_t worker_index) noexcept {
		queue_counters.note_inject_pop_attempt();
		std::size_t const shards = inject_shard_count();
		if (shards == 0) {
			return std::nullopt;
		}
		std::size_t const start = worker_index % shards;
		for (std::size_t offset = 0; offset < shards; ++offset) {
			std::size_t const shard = (start + offset) % shards;
			auto job = inject_rings[shard]->try_pop();
			if (!job) {
				continue;
			}
			queue_counters.note_inject_pop_hit();
			return job;
		}
		return std::nullopt;
	}
	[[nodiscard]] std::optional<work_detail::Fn> steal_work(std::size_t thief) {
		if (workers.size() < 2 || stealable_local_jobs.load(std::memory_order_acquire) == 0) {
			return std::nullopt;
		}
		queue_counters.note_steal_round();
		for (std::size_t offset = 1; offset < workers.size(); ++offset) {
			std::size_t const victim_index = (thief + offset) % workers.size();
			auto &victim = *workers[victim_index];
			queue_counters.note_steal_victim_check();
			auto lk = queue_counters.lock_steal_victim(victim.mtx);
			if (victim.local.empty()) {
				continue;
			}
			queue_counters.note_steal_hit();
			auto job = std::move(victim.local.front());
			victim.local.pop_front();
			stealable_local_jobs.fetch_sub(1, std::memory_order_acq_rel);
			return job;
		}
		return std::nullopt;
	}
	static void maybe_set_name(std::string const &prefix, std::size_t index) noexcept {
		if (prefix.empty()) {
			return;
		}
		auto name = std::format("{}-{}", prefix, index);
		if (name.size() > 15) {
			name.resize(15);
		}
		::pthread_setname_np(::pthread_self(), name.c_str());
	}
	void maybe_pin_worker(std::size_t index) noexcept {
		if (!options.pin_workers) {
			return;
		}
		cpu_set_t set;
		CPU_ZERO(&set);
		unsigned const cpus = std::max(1U, std::thread::hardware_concurrency());
		CPU_SET(index % cpus, &set);
		::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
	}
	void worker_loop(std::stop_token const &st, std::size_t index) {
		tls_pool = this;
		tls_worker = index;
		maybe_set_name(options.worker_name_prefix, index);
		maybe_pin_worker(index);
		while (!st.stop_requested() && !stopping.test(std::memory_order_acquire)) {
			auto job = no_stealing_mode() ? pop_no_stealing_local(index) : pop_local(index);
			if (!job) {
				job = pop_inject(index);
			}
			if (!job && !no_stealing_mode()) {
				job = steal_work(index);
			}
			if (job) {
				queue_counters.note_job_run();
				try {
					(*job)();
				} catch (...) {
					if (options.raw_exception_sink) {
						try {
							options.raw_exception_sink(std::current_exception());
						} catch (...) {} // NOLINT(bugprone-empty-catch)
					}
				}
				pending.fetch_sub(1, std::memory_order_release);
				continue;
			}
			auto const has_pending = [&] { return pending.load(std::memory_order_relaxed) > 0; };
			bool spun = false;
			for (std::uint32_t s = 0; s < options.spin_before_park && !spun; ++s) {
				conflux::work::root::detail::cpu_pause();
				spun = has_pending();
			}
			if (!spun) {
				queue_counters.note_park_attempt();
				parked.fetch_add(1, std::memory_order_acq_rel);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				if (pending.load(std::memory_order_acquire) > 0 || stopping.test(std::memory_order_acquire)) {
					queue_counters.note_park_recheck_skip();
					parked.fetch_sub(1, std::memory_order_acq_rel);
				} else {
					std::uint32_t const epoch = wake_epoch.load(std::memory_order_acquire);
					queue_counters.note_futex_wait();
					work_detail::futex_wait_private(wake_epoch, epoch);
					parked.fetch_sub(1, std::memory_order_acq_rel);
				}
			}
		}
		tls_pool = nullptr;
		tls_worker = work_detail::kNoWorker;
	}
	[[nodiscard]] bool enqueue_stealing(work_detail::Fn job) {
		auto admission = queue_counters.lock_admission(admission_mtx);
		if (accepting_stopped.test(std::memory_order_acquire) || stopping.test(std::memory_order_acquire)) {
			queue_counters.note_enqueue_stopped_rejection();
			return false;
		}
		bool const queued = is_local_worker() ? push_local(std::move(job)) : push_inject(std::move(job));
		if (!queued) {
			queue_counters.note_enqueue_full_rejection();
			return false;
		}
		admission.unlock();
		wake_one();
		return true;
	}
	[[nodiscard]] bool enqueue_no_stealing(work_detail::Fn job) noexcept {
		if (!begin_no_stealing_admission()) {
			queue_counters.note_enqueue_stopped_rejection();
			return false;
		}
		bool const queued = is_local_worker() ? push_no_stealing_local(std::move(job)) : push_inject(std::move(job));
		end_no_stealing_admission();
		if (!queued) {
			queue_counters.note_enqueue_full_rejection();
			return false;
		}
		wake_one();
		return true;
	}
	void stop_no_stealing() noexcept {
		accepting_stopped.test_and_set(std::memory_order_acq_rel);
		close_no_stealing_admission();
		wait_no_stealing_admission_idle();
		if (!stopping.test_and_set(std::memory_order_acq_rel)) {
			for (auto &worker: workers) {
				worker->thread.request_stop();
			}
			wake_all();
		}
	}
	void drain_and_stop_no_stealing() noexcept {
		accepting_stopped.test_and_set(std::memory_order_acq_rel);
		close_no_stealing_admission();
		wait_no_stealing_admission_idle();
		while (pending.load(std::memory_order_acquire) > 0) {
			wake_all();
			std::this_thread::yield();
		}
		stop_no_stealing();
		wait();
	}
	void discard_queued_jobs() noexcept {
		pending.store(0, std::memory_order_release);
	}
	[[nodiscard]] bool enqueue(work_detail::Fn job) {
		queue_counters.note_enqueue_attempt();
		if (no_stealing_mode()) {
			return enqueue_no_stealing(std::move(job));
		}
		return enqueue_stealing(std::move(job));
	}
	void stop() noexcept {
		if (no_stealing_mode()) {
			stop_no_stealing();
			return;
		}
		auto admission = queue_counters.lock_admission(admission_mtx);
		accepting_stopped.test_and_set(std::memory_order_acq_rel);
		if (!stopping.test_and_set(std::memory_order_acq_rel)) {
			for (auto &worker: workers) {
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
			auto admission = queue_counters.lock_admission(admission_mtx);
			accepting_stopped.test_and_set(std::memory_order_acq_rel);
		}
		while (pending.load(std::memory_order_acquire) > 0) {
			wake_all();
			std::this_thread::yield();
		}
		stop();
		wait();
	}
	void wait() noexcept {
		for (auto &worker: workers) {
			if (worker->thread.joinable()) {
				worker->thread.join();
			}
		}
		discard_queued_jobs();
	}
};

struct RingLaneState {
	RingLaneOptions options{};
	std::mutex mtx{};
	std::deque<conflux::work::root::detail::small_move_only_function<void()>> queue{};
	std::atomic_flag stopped{};
	std::atomic_flag wake_pending{};
	std::thread::id owner{std::this_thread::get_id()};

	explicit RingLaneState(RingLaneOptions opts)
		: options{std::move(opts)} {}
	[[nodiscard]] bool is_owner_thread() const noexcept {
		return std::this_thread::get_id() == owner;
	}
	[[nodiscard]] bool wake_ring() noexcept {
		if (options.ring_fd < 0) {
			return false;
		}
		io_uring_sqe sqe{};
		io_uring_prep_msg_ring(&sqe, options.ring_fd, 0, options.wake_user_data, 0);
		return io_uring_register_sync_msg(&sqe) == 0;
	}
	static void run_inline(conflux::work::root::detail::small_move_only_function<void()> job) {
		try {
			job();
		} catch (...) {} // NOLINT(bugprone-empty-catch)
	}
};

[[nodiscard]] static WorkPoolState *work_pool_state(void *state) noexcept {
	return static_cast<WorkPoolState *>(state);
}

[[nodiscard]] static RingLaneState *ring_lane_state(void *state) noexcept {
	return static_cast<RingLaneState *>(state);
}

WorkPool::WorkPool(WorkPoolOptions options)
	: state_{new WorkPoolState{std::move(options)}} {}

WorkPool::~WorkPool() {
	delete work_pool_state(state_);
}

bool WorkPool::enqueue(work_detail::Fn job) {
	return work_pool_state(state_)->enqueue(std::move(job));
}

void WorkPool::stop() noexcept {
	work_pool_state(state_)->stop();
}

void WorkPool::drain_and_stop() noexcept {
	work_pool_state(state_)->drain_and_stop();
}

void WorkPool::wait() noexcept {
	work_pool_state(state_)->wait();
}

bool WorkPool::stopped() const noexcept {
	return work_pool_state(state_)->accepting_stopped.test(std::memory_order_acquire);
}

WorkPoolQueueStats WorkPool::queue_stats() const noexcept {
	return work_pool_state(state_)->queue_counters.snapshot();
}

void WorkPool::reset_queue_stats() noexcept {
	work_pool_state(state_)->queue_counters.reset();
}

RingLane::RingLane(RingLaneOptions options)
	: state_{new RingLaneState{std::move(options)}} {}

RingLane::~RingLane() {
	delete ring_lane_state(state_);
}

bool RingLane::enqueue(conflux::work::root::detail::small_move_only_function<void()> job) {
	auto *state = ring_lane_state(state_);
	if (state->stopped.test(std::memory_order_acquire)) {
		return false;
	}
	if (state->is_owner_thread() && state->options.allow_inline_on_owner) {
		RingLaneState::run_inline(std::move(job));
		return true;
	}
	bool need_wake = false;
	{
		std::scoped_lock const lk{state->mtx};
		need_wake = state->queue.empty();
		state->queue.push_back(std::move(job));
		if (need_wake && !state->wake_pending.test_and_set(std::memory_order_acq_rel)) {
			if (!state->wake_ring()) {
				state->queue.pop_back();
                                state->wake_pending.clear(std::memory_order_release);
				return false;
			}
		}
	}
	return true;
}

void RingLane::adopt_current_thread() noexcept {
	ring_lane_state(state_)->owner = std::this_thread::get_id();
}

std::size_t RingLane::drain() {
	auto *state = ring_lane_state(state_);
	if (!state->is_owner_thread()) {
		throw conflux::work::root::JoinError{conflux::work::root::JoinError::reason::thread_precondition};
	}
	std::size_t ran = 0;
	std::size_t const budget = state->options.drain_budget == 0 ? std::numeric_limits<std::size_t>::max() : state->options.drain_budget;
	while (ran < budget) {
		conflux::work::root::detail::small_move_only_function<void()> job;
		{
			std::scoped_lock const lk{state->mtx};
			if (state->queue.empty()) {
                                state->wake_pending.clear(std::memory_order_release);
				break;
			}
			job = std::move(state->queue.front());
			state->queue.pop_front();
			if (state->queue.empty()) {
                                state->wake_pending.clear(std::memory_order_release);
			}
		}
		RingLaneState::run_inline(std::move(job));
		++ran;
	}
	if (ran == budget) {
		std::scoped_lock const lk{state->mtx};
		if (!state->queue.empty() && !state->wake_pending.test_and_set(std::memory_order_acq_rel)) {
			auto _ = state->wake_ring();
		}
	}
	return ran;
}

void RingLane::stop() noexcept {
        ring_lane_state(state_)->stopped.test_and_set(std::memory_order_release);
}

bool RingLane::stopped() const noexcept {
	return ring_lane_state(state_)->stopped.test(std::memory_order_acquire);
}

bool RingLane::on_owner_thread() const noexcept {
	return ring_lane_state(state_)->is_owner_thread();
}

int RingLane::ring_fd() const noexcept {
	return ring_lane_state(state_)->options.ring_fd;
}
