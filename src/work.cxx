module;
#include <cassert>
#include <immintrin.h>
#include <liburing.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

export module conflux.work;

import std;
import conflux.types;
export import conflux.work.root;

using namespace std;

export struct Cancelled final : runtime_error {
	Cancelled()
		: runtime_error{"work cancelled"} {}
};

export struct ValueTag;
export template<typename Fn>
struct ThenStep;
export template<typename Fn>
struct FlatThenStep;
export template<typename Fn>
struct ErrorStep;
export template<typename Fn>
struct CancelStep;
export template<typename Target>
struct MoveToStep;
export template<typename Target>
struct StartOnStep;
export template<typename T>
class Task;

namespace work_detail {

constexpr size_t kNoWorker = numeric_limits<size_t>::max();

inline int futex_wait_private(
	atomic<u32> &word,
	u32 expected) noexcept {
	auto *addr = reinterpret_cast<u32 *>(&word); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
	return static_cast<int>(::syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, nullptr, nullptr, 0));
}

inline int futex_wake_private(
	atomic<u32> &word,
	int count) noexcept {
	auto *addr = reinterpret_cast<u32 *>(&word); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
	return static_cast<int>(::syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0));
}

template<typename T>
using StoredValue = conditional_t<is_void_v<T>, monostate, T>;

export template<typename Signature>
class UniqueFn;

template<typename R, typename... Args>
class UniqueFn<R(Args...)> {
	struct Erased {
		virtual ~Erased() = default;
		virtual R call(Args &&...args) = 0;
	};

	template<typename Fn>
	struct Model final : Erased {
		Fn fn;

		explicit Model(
			Fn input)
			: fn{move(input)} {}

		R call(
			Args &&...args) override {
			if constexpr (is_void_v<R>) {
				invoke(fn, forward<Args>(args)...);
			} else {
				return invoke(fn, forward<Args>(args)...);
			}
		}
	};

	unique_ptr<Erased> erased_{};

public:
	UniqueFn() = default;
	UniqueFn(
		nullptr_t) {}
	UniqueFn(UniqueFn &&) noexcept = default;
	UniqueFn &operator =(UniqueFn &&) noexcept = default;
	UniqueFn(UniqueFn const &) = delete;
	UniqueFn &operator =(UniqueFn const &) = delete;

	template<typename Fn>
		requires(!same_as<remove_cvref_t<Fn>, UniqueFn>)
	UniqueFn(
		Fn &&fn)
		: erased_{make_unique<Model<remove_cvref_t<Fn>>>(forward<Fn>(fn))} {}

	[[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(erased_); }

	R operator ()(
		Args... args) {
		assert(erased_);
		return erased_->call(forward<Args>(args)...);
	}
};

enum class OutcomeTag : u8 {
	value,
	error,
	cancelled,
};

template<typename T>
struct Outcome {
	OutcomeTag tag = OutcomeTag::cancelled;
	variant<StoredValue<T>, exception_ptr, monostate> payload{monostate{}};
};

template<>
struct Outcome<void> {
	OutcomeTag tag = OutcomeTag::cancelled;
	exception_ptr error{};
};

template<typename T>
using Continuation = UniqueFn<void(Outcome<T> &&)>;

template<typename T>
struct State {
	mutex mtx;
	condition_variable cv;
	bool ready = false;
	optional<Outcome<T>> outcome{};
	Continuation<T> next{};
};

template<typename T>
using StatePtr = shared_ptr<State<T>>;

template<typename T>
void fulfill(
	StatePtr<T> const &state,
	Outcome<T> outcome) {
	Continuation<T> next;
	{
		scoped_lock const lk{state->mtx};
		if (state->ready) {
			throw logic_error{"work state already fulfilled"};
		}
		state->ready = true;
		if (state->next) {
			next = move(state->next);
		} else {
			assert(!next);
			state->outcome = move(outcome);
			state->cv.notify_all();
			return;
		}
	}
	try {
		next(move(outcome));
	} catch (...) {} // NOLINT(bugprone-empty-catch)
}

template<typename T>
void fulfill_value(
	StatePtr<T> const &state,
	StoredValue<T> value = StoredValue<T>{}) {
	fulfill<T>(state, {.tag = OutcomeTag::value, .payload = move(value)});
}

template<>
void fulfill_value<void>(
	StatePtr<void> const &state,
	StoredValue<void> /*value*/) {
	fulfill<void>(state, {.tag = OutcomeTag::value});
}

template<typename T>
void fulfill_error(
	StatePtr<T> const &state,
	exception_ptr error) {
	fulfill<T>(state, {.tag = OutcomeTag::error, .payload = error});
}

template<>
void fulfill_error<void>(
	StatePtr<void> const &state,
	exception_ptr error) {
	fulfill<void>(state, {.tag = OutcomeTag::error, .error = error});
}

template<typename T>
void fulfill_cancelled(
	StatePtr<T> const &state) {
	fulfill<T>(state, {.tag = OutcomeTag::cancelled, .payload = monostate{}});
}

template<>
void fulfill_cancelled<void>(
	StatePtr<void> const &state) {
	fulfill<void>(state, {.tag = OutcomeTag::cancelled});
}

template<typename T>
void attach(
	StatePtr<T> const &state,
	Continuation<T> next) {
	optional<Outcome<T>> ready{};
	{
		scoped_lock const lk{state->mtx};
		if (state->next) {
			throw logic_error{"work state already consumed"};
		}
		if (state->ready) {
			ready = move(state->outcome);
			state->outcome.reset();
		} else {
			state->next = move(next);
			return;
		}
	}
	try {
		next(move(*ready));
	} catch (...) {} // NOLINT(bugprone-empty-catch)
}

template<typename T>
Outcome<T> await_result(
	StatePtr<T> const &state) {
	unique_lock lk{state->mtx};
	state->cv.wait(lk, [&] { return state->ready; });
	auto out = move(*state->outcome);
	state->outcome.reset();
	return out;
}

class QueueTarget {
public:
	virtual ~QueueTarget() = default;
	virtual bool enqueue(UniqueFn<void()> job) = 0;
};

template<typename T>
class [[nodiscard]] Flow;

template<typename T>
struct IsFlow : false_type {};

template<typename T>
struct IsFlow<Flow<T>> : true_type {};

template<typename T>
inline constexpr bool kIsFlow = IsFlow<remove_cvref_t<T>>::value;

template<typename T>
using FlowValue = typename remove_cvref_t<T>::value_type;

class DetachedErrors {
	mutex mtx_;
	UniqueFn<void(exception_ptr)> handler_{[](exception_ptr error) {
		try {
			rethrow_exception(error);
		} catch (exception const &ex) { println(cerr, "conflux.work detached error: {}", ex.what()); } catch (...) {
			println(cerr, "conflux.work detached error: unknown exception");
		}
	}};

public:
	static DetachedErrors &instance() {
		static auto *errors = new DetachedErrors;
		return *errors;
	}

	void handle(
		exception_ptr error) {
		scoped_lock const lk{mtx_};
		handler_(error);
	}

	void set_handler(
		UniqueFn<void(exception_ptr)> handler) {
		scoped_lock const lk{mtx_};
		handler_ = move(handler);
	}
};

template<typename T>
struct FlowAwaiter;

template<typename T>
class [[nodiscard]] Flow {
	StatePtr<T> state_{};

	explicit Flow(
		StatePtr<T> state)
		: state_{move(state)} {}

	template<typename U>
	friend class Flow;

public:
	using value_type = T;

	Flow() = default;
	Flow(Flow const &) = delete;
	Flow &operator =(Flow const &) = delete;
	Flow(Flow &&) noexcept = default;
	Flow &operator =(Flow &&) noexcept = default;

	[[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

	auto operator co_await() { return FlowAwaiter<T>{.state = release_state()}; }

	[[nodiscard]] StatePtr<T> release_state() {
		if (!state_) {
			throw logic_error{"invalid flow"};
		}
		return exchange(state_, nullptr);
	}

	static Flow from_state(
		StatePtr<T> state) {
		return Flow{move(state)};
	}
};

template<typename T>
StoredValue<T> extract_value(
	Outcome<T> &&outcome) {
	if (outcome.tag == OutcomeTag::error) {
		rethrow_exception(get<exception_ptr>(outcome.payload));
	}
	if (outcome.tag == OutcomeTag::cancelled) {
		throw Cancelled{};
	}
	return move(get<StoredValue<T>>(outcome.payload));
}

template<>
StoredValue<void> extract_value<void>(
	Outcome<void> &&outcome) {
	if (outcome.tag == OutcomeTag::error) {
		rethrow_exception(outcome.error);
	}
	if (outcome.tag == OutcomeTag::cancelled) {
		throw Cancelled{};
	}
	return {};
}

template<typename T>
void forward_outcome(
	StatePtr<T> const &dst,
	Outcome<T> outcome) {
	switch (outcome.tag) {
	case OutcomeTag::value    : fulfill<T>(dst, move(outcome)); break;
	case OutcomeTag::error    : fulfill_error<T>(dst, get<exception_ptr>(outcome.payload)); break;
	case OutcomeTag::cancelled: fulfill_cancelled<T>(dst); break;
	}
}

template<>
void forward_outcome<void>(
	StatePtr<void> const &dst,
	Outcome<void> outcome) {
	switch (outcome.tag) {
	case OutcomeTag::value    : fulfill_value<void>(dst); break;
	case OutcomeTag::error    : fulfill_error<void>(dst, outcome.error); break;
	case OutcomeTag::cancelled: fulfill_cancelled<void>(dst); break;
	}
}

template<typename T>
exception_ptr outcome_error(
	Outcome<T> const &outcome) {
	return get<exception_ptr>(outcome.payload);
}

template<>
exception_ptr outcome_error<void>(
	Outcome<void> const &outcome) {
	return outcome.error;
}

template<typename T>
struct FlowAwaiter {
	StatePtr<T> state{};
	optional<Outcome<T>> outcome{};

	[[nodiscard]] bool await_ready() const noexcept { return false; }

	void await_suspend(
		coroutine_handle<> h) {
		attach<T>(state, [this, h](Outcome<T> &&out) mutable {
			outcome.emplace(move(out));
			h.resume();
		});
	}

	decltype(auto) await_resume() {
		if constexpr (is_void_v<T>) {
			(void)extract_value<T>(move(*outcome));
		} else {
			return extract_value<T>(move(*outcome));
		}
	}
};

template<typename T>
void connect(
	Flow<T> upstream,
	StatePtr<T> const &dst) {
	attach<T>(upstream.release_state(), [dst](Outcome<T> &&outcome) mutable {
		forward_outcome<T>(dst, move(outcome));
	});
}

struct ValueTag {
	template<typename T>
	[[nodiscard]] auto operator ()(
		T &&value) const -> Flow<decay_t<T>> {
		using U = decay_t<T>;
		auto state = make_shared<State<U>>();
		fulfill_value<U>(state, U{forward<T>(value)});
		return Flow<U>::from_state(move(state));
	}

	[[nodiscard]] auto operator ()() const -> Flow<void> {
		auto state = make_shared<State<void>>();
		fulfill_value<void>(state);
		return Flow<void>::from_state(move(state));
	}
};

template<typename T>
struct ThenStep {
	T fn;
};

template<typename T>
struct FlatThenStep {
	T fn;
};

template<typename T>
struct ErrorStep {
	T fn;
};

template<typename T>
struct CancelStep {
	T fn;
};

template<typename Target>
struct MoveToStep {
	Target *target = nullptr;
};

template<typename T>
struct StartOnStep {
	T *target = nullptr;
};

template<typename T>
struct RunOnTarget {
	T *target = nullptr;
};

template<typename Fn>
[[nodiscard]] auto then(
	Fn &&fn) {
	return ThenStep<decay_t<Fn>>{forward<Fn>(fn)};
}

template<typename Fn>
[[nodiscard]] auto flat_then(
	Fn &&fn) {
	return FlatThenStep<decay_t<Fn>>{forward<Fn>(fn)};
}

template<typename Fn>
[[nodiscard]] auto on_error(
	Fn &&fn) {
	return ErrorStep<decay_t<Fn>>{forward<Fn>(fn)};
}

template<typename Fn>
[[nodiscard]] auto on_cancel(
	Fn &&fn) {
	return CancelStep<decay_t<Fn>>{forward<Fn>(fn)};
}

template<typename Target>
[[nodiscard]] auto move_to(
	Target &target) {
	return MoveToStep<Target>{&target};
}

template<typename Target>
[[nodiscard]] auto start_on(
	Target &target) {
	return StartOnStep<Target>{&target};
}

template<typename Target, typename Fn>
[[nodiscard]] auto run_on(Target &target, Fn &&fn);

template<typename T>
class JoinState {
public:
	using Result = Flow<T>;
};

} // namespace work_detail

export struct WorkPoolOptions {
	size_t threads = 0;
	size_t max_inject_queue = 4096;
	size_t local_queue_capacity = 1024;
	u32 spin_before_park = 256;
	int numa_node = -1;
	bool pin_workers = false;
	string worker_name_prefix = "conflux-work";
};

export struct RingLaneOptions {
	int ring_fd = -1;
	u64 wake_user_data = 0x434F4E464C5558ULL; // "CONFLUX"
	size_t drain_budget = 0;
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

export template<typename T>
using BufferView = span<T const>;

export struct IoBuffer {
	span<byte const> bytes{};
	shared_ptr<void const> owner{};

	IoBuffer() = default;
	explicit IoBuffer(
		span<byte const> view)
		: bytes{view} {}

	[[nodiscard]] static IoBuffer from_string(
		string value) {
		auto owned = make_shared<string const>(move(value));
		auto view = span{
			reinterpret_cast<byte const *>(owned->data()),
			owned->size()}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
		return IoBuffer{view, move(owned)};
	}

private:
	IoBuffer(
		span<byte const> view,
		shared_ptr<void const> keep_alive)
		: bytes{view}
		, owner{move(keep_alive)} {}
};

export struct BufferList {
	vector<span<byte const>> segments{};
};

export struct IoPlan {
	enum class Kind : u8 {
		callback,
	};

	Kind kind = Kind::callback;
	work_detail::UniqueFn<void()> callback{};

	[[nodiscard]] static IoPlan call(
		work_detail::UniqueFn<void()> fn) {
		return IoPlan{.kind = Kind::callback, .callback = move(fn)};
	}
};

export class WorkPool final : public work_detail::QueueTarget {
	struct Worker {
		mutex mtx;
		deque<work_detail::UniqueFn<void()>> local{};
		jthread thread{};
	};

	WorkPoolOptions options_{};
	vector<unique_ptr<Worker>> workers_{};
	mutex inject_mtx_{};
	deque<work_detail::UniqueFn<void()>> inject_{};
	atomic<u32> wake_epoch_{0};
	atomic<size_t> pending_{0};
	atomic_flag stopping_{};

	inline static thread_local WorkPool *tls_pool_ = nullptr;
	inline static thread_local size_t tls_worker_ = work_detail::kNoWorker;

	[[nodiscard]] bool is_local_worker() const noexcept {
		return tls_pool_ == this && tls_worker_ != work_detail::kNoWorker;
	}

	void wake_one() noexcept {
		wake_epoch_.fetch_add(1, memory_order_release);
		work_detail::futex_wake_private(wake_epoch_, 1);
	}

	void wake_all() noexcept {
		wake_epoch_.fetch_add(1, memory_order_release);
		work_detail::futex_wake_private(wake_epoch_, static_cast<int>(workers_.size()));
	}

	[[nodiscard]] bool push_local(
		work_detail::UniqueFn<void()> job) {
		auto &worker = *workers_[tls_worker_];
		scoped_lock const lk{worker.mtx};
		if (worker.local.size() >= options_.local_queue_capacity) {
			return false;
		}
		worker.local.push_back(move(job));
		pending_.fetch_add(1, memory_order_release);
		return true;
	}

	[[nodiscard]] bool push_inject(
		work_detail::UniqueFn<void()> job) {
		scoped_lock const lk{inject_mtx_};
		if (inject_.size() >= options_.max_inject_queue) {
			return false;
		}
		inject_.push_back(move(job));
		pending_.fetch_add(1, memory_order_release);
		return true;
	}

	[[nodiscard]] optional<work_detail::UniqueFn<void()>> pop_local(
		size_t index) {
		auto &worker = *workers_[index];
		scoped_lock const lk{worker.mtx};
		if (worker.local.empty()) {
			return nullopt;
		}
		auto job = move(worker.local.back());
		worker.local.pop_back();
		return job;
	}

	[[nodiscard]] optional<work_detail::UniqueFn<void()>> pop_inject() {
		scoped_lock const lk{inject_mtx_};
		if (inject_.empty()) {
			return nullopt;
		}
		auto job = move(inject_.front());
		inject_.pop_front();
		return job;
	}

	[[nodiscard]] optional<work_detail::UniqueFn<void()>> steal_work(
		size_t thief) {
		for (size_t offset = 1; offset < workers_.size(); ++offset) {
			size_t const victim_index = (thief + offset) % workers_.size();
			auto &victim = *workers_[victim_index];
			scoped_lock const lk{victim.mtx};
			if (victim.local.empty()) {
				continue;
			}
			auto job = move(victim.local.front());
			victim.local.pop_front();
			return job;
		}
		return nullopt;
	}

	static void maybe_set_name(
		string const &prefix,
		size_t index) noexcept {
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
		size_t index) noexcept {
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
		stop_token const &st,
		size_t index) {
		tls_pool_ = this;
		tls_worker_ = index;
		maybe_set_name(options_.worker_name_prefix, index);
		maybe_pin_worker(index);
		while (!st.stop_requested() && !stopping_.test(memory_order_acquire)) {
			auto job = pop_local(index);
			if (!job) {
				job = pop_inject();
			}
			if (!job) {
				job = steal_work(index);
			}
			if (job) {
				try {
					(*job)();
				} catch (...) {} // NOLINT(bugprone-empty-catch)
				pending_.fetch_sub(1, memory_order_release);
				continue;
			}
			auto const has_pending = [&] { return pending_.load(memory_order_relaxed) > 0; };
			bool spun = false;
			for (u32 s = 0; s < options_.spin_before_park && !spun; ++s) {
				_mm_pause();
				spun = has_pending();
			}
			if (!spun) {
				u32 const epoch = wake_epoch_.load(memory_order_acquire);
				if (pending_.load(memory_order_acquire) == 0 && !stopping_.test(memory_order_acquire)) {
					work_detail::futex_wait_private(wake_epoch_, epoch);
				}
			}
		}
		tls_pool_ = nullptr;
		tls_worker_ = work_detail::kNoWorker;
	}

public:
	explicit WorkPool(
		WorkPoolOptions options = {})
		: options_{move(options)} {
		if (options_.threads == 0) {
			options_.threads = max(1U, thread::hardware_concurrency());
		}
		workers_.reserve(options_.threads);
		for (size_t i = 0; i < options_.threads; ++i) {
			workers_.push_back(make_unique<Worker>());
		}
		for (size_t i = 0; i < workers_.size(); ++i) {
			workers_[i]->thread = jthread([this, i](stop_token const &st) { worker_loop(st, i); });
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
		work_detail::UniqueFn<void()> job) override {
		if (stopping_.test(memory_order_acquire)) {
			return false;
		}
		bool const queued = is_local_worker() ? push_local(move(job)) : push_inject(move(job));
		if (!queued) {
			return false;
		}
		wake_one();
		return true;
	}

	void stop() noexcept {
		if (!stopping_.test_and_set(memory_order_acq_rel)) {
			for (auto &worker: workers_) {
				worker->thread.request_stop();
			}
			wake_all();
		}
	}

	void wait() noexcept {
		for (auto &worker: workers_) {
			if (worker->thread.joinable()) {
				worker->thread.join();
			}
		}
	}

	[[nodiscard]] bool stopped() const noexcept { return stopping_.test(memory_order_acquire); }
};

export class RingLane final : public work_detail::QueueTarget {
	RingLaneOptions options_{};
	mutex mtx_{};
	deque<work_detail::UniqueFn<void()>> queue_{};
	atomic_flag stopped_{};
	atomic_flag wake_pending_{};
	thread::id owner_{this_thread::get_id()};

	[[nodiscard]] bool is_owner_thread() const noexcept { return this_thread::get_id() == owner_; }

	[[nodiscard]] bool wake_ring() noexcept {
		if (options_.ring_fd < 0) {
			return false;
		}
		io_uring_sqe sqe{};
		io_uring_prep_msg_ring(&sqe, options_.ring_fd, 0, options_.wake_user_data, 0);
		return io_uring_register_sync_msg(&sqe) == 0;
	}

	void run_inline(
		work_detail::UniqueFn<void()> job) {
		try {
			job();
		} catch (...) {} // NOLINT(bugprone-empty-catch)
	}

public:
	explicit RingLane(
		RingLaneOptions options = {})
		: options_{move(options)} {}

	[[nodiscard]] bool enqueue(
		work_detail::UniqueFn<void()> job) override {
		if (stopped_.test(memory_order_acquire)) {
			return false;
		}
		if (is_owner_thread() && options_.allow_inline_on_owner) {
			run_inline(move(job));
			return true;
		}
		bool need_wake = false;
		{
			scoped_lock const lk{mtx_};
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

	void adopt_current_thread() noexcept { owner_ = this_thread::get_id(); }

	[[nodiscard]] size_t drain() {
		if (!is_owner_thread()) {
			throw logic_error{"ring lane drained from non-owner thread"};
		}
		size_t ran = 0;
		size_t const budget = options_.drain_budget == 0 ? numeric_limits<size_t>::max() : options_.drain_budget;
		while (ran < budget) {
			work_detail::UniqueFn<void()> job;
			{
				scoped_lock const lk{mtx_};
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
			scoped_lock const lk{mtx_};
			if (!queue_.empty() && !wake_pending_.test_and_set(memory_order_acq_rel)) {
				(void)wake_ring();
			}
		}
		return ran;
	}

	void stop() noexcept { stopped_.test_and_set(memory_order_release); }

	[[nodiscard]] bool stopped() const noexcept { return stopped_.test(memory_order_acquire); }
	[[nodiscard]] bool on_owner_thread() const noexcept { return is_owner_thread(); }
	[[nodiscard]] int ring_fd() const noexcept { return options_.ring_fd; }
};

export template<typename T>
using Flow = work_detail::Flow<T>;

export struct ValueTag {
	template<typename T>
	[[nodiscard]] auto operator ()(
		T &&input) const -> Flow<decay_t<T>> {
		using U = decay_t<T>;
		auto state = make_shared<work_detail::State<U>>();
		work_detail::fulfill_value<U>(state, U{forward<T>(input)});
		return Flow<U>::from_state(move(state));
	}

	[[nodiscard]] auto operator ()() const -> Flow<void> {
		auto state = make_shared<work_detail::State<void>>();
		work_detail::fulfill_value<void>(state);
		return Flow<void>::from_state(move(state));
	}
};

export inline constexpr ValueTag value{};

export template<typename Fn>
struct ThenStep {
	Fn fn;
};

export template<typename Fn>
struct FlatThenStep {
	Fn fn;
};

export template<typename Fn>
struct ErrorStep {
	Fn fn;
};

export template<typename Fn>
struct CancelStep {
	Fn fn;
};

export template<typename Target>
struct MoveToStep {
	Target *target = nullptr;
};

export template<typename Target>
struct StartOnStep {
	Target *target = nullptr;
};

export template<typename Fn>
[[nodiscard]] auto then(
	Fn &&fn) {
	return ThenStep<decay_t<Fn>>{forward<Fn>(fn)};
}

export template<typename Fn>
[[nodiscard]] auto flat_then(
	Fn &&fn) {
	return FlatThenStep<decay_t<Fn>>{forward<Fn>(fn)};
}

export template<typename Fn>
[[nodiscard]] auto on_error(
	Fn &&fn) {
	return ErrorStep<decay_t<Fn>>{forward<Fn>(fn)};
}

export template<typename Fn>
[[nodiscard]] auto on_cancel(
	Fn &&fn) {
	return CancelStep<decay_t<Fn>>{forward<Fn>(fn)};
}

export template<typename Target>
[[nodiscard]] auto move_to(
	Target &target) {
	return MoveToStep<Target>{&target};
}

export template<typename Target>
[[nodiscard]] auto start_on(
	Target &target) {
	return StartOnStep<Target>{&target};
}

namespace work_detail {

template<typename T, typename Fn>
struct ThenResultHelper;

template<typename T, typename Fn>
struct FlatThenResultHelper;

template<typename T, typename Fn>
using ErrorResult = invoke_result_t<Fn, exception_ptr>;

template<typename T, typename Fn>
using CancelResult = invoke_result_t<Fn>;

template<typename T, typename Fn>
struct ThenResultHelper {
	using type = invoke_result_t<Fn, T>;
};

template<typename Fn>
struct ThenResultHelper<void, Fn> {
	using type = invoke_result_t<Fn>;
};

template<typename T, typename Fn>
using ThenResult = typename ThenResultHelper<T, Fn>::type;

template<typename T, typename Fn>
struct FlatThenResultHelper {
	using type = invoke_result_t<Fn, T>;
};

template<typename Fn>
struct FlatThenResultHelper<void, Fn> {
	using type = invoke_result_t<Fn>;
};

template<typename T, typename Fn>
using FlatThenResult = typename FlatThenResultHelper<T, Fn>::type;

template<typename T>
struct UnwrapFlow {
	using type = T;
};

template<typename T>
struct UnwrapFlow<Flow<T>> {
	using type = T;
};

template<typename T>
using UnwrapFlowT = typename UnwrapFlow<T>::type;

template<typename Target, typename Fn>
[[nodiscard]] auto run_on(
	Target &target,
	Fn &&fn) {
	using Result = invoke_result_t<decay_t<Fn>>;
	auto state = make_shared<State<Result>>();
	auto job = [state, fn = decay_t<Fn>(forward<Fn>(fn))]() mutable {
		try {
			if constexpr (is_void_v<Result>) {
				fn();
				fulfill_value<Result>(state);
			} else {
				fulfill_value<Result>(state, fn());
			}
		} catch (...) { fulfill_error<Result>(state, current_exception()); }
	};
	if (!target.enqueue(move(job))) {
		fulfill_cancelled<Result>(state);
	}
	return Flow<Result>::from_state(move(state));
}

template<typename T, typename Fn>
auto operator |(
	Flow<T> &&flow,
	::ThenStep<Fn> step) {
	using Result = ThenResult<T, Fn>;
	auto next = make_shared<State<Result>>();
	attach<T>(flow.release_state(), [next, fn = move(step.fn)](Outcome<T> &&outcome) mutable {
		if (outcome.tag == OutcomeTag::error) {
			fulfill_error<Result>(next, outcome_error<T>(outcome));
			return;
		}
		if (outcome.tag == OutcomeTag::cancelled) {
			fulfill_cancelled<Result>(next);
			return;
		}
		try {
			if constexpr (is_void_v<T>) {
				if constexpr (is_void_v<Result>) {
					fn();
					fulfill_value<Result>(next);
				} else {
					fulfill_value<Result>(next, fn());
				}
			} else {
				auto value = move(get<StoredValue<T>>(outcome.payload));
				if constexpr (is_void_v<Result>) {
					fn(move(value));
					fulfill_value<Result>(next);
				} else {
					fulfill_value<Result>(next, fn(move(value)));
				}
			}
		} catch (...) { fulfill_error<Result>(next, current_exception()); }
	});
	return Flow<Result>::from_state(move(next));
}

template<typename T, typename Fn>
auto operator |(
	Flow<T> &&flow,
	::FlatThenStep<Fn> step) {
	using Nested = FlatThenResult<T, Fn>;
	static_assert(kIsFlow<Nested>, "flat_then handler must return Flow<T>");
	using Result = FlowValue<Nested>;
	auto next = make_shared<State<Result>>();
	attach<T>(flow.release_state(), [next, fn = move(step.fn)](Outcome<T> &&outcome) mutable {
		if (outcome.tag == OutcomeTag::error) {
			fulfill_error<Result>(next, outcome_error<T>(outcome));
			return;
		}
		if (outcome.tag == OutcomeTag::cancelled) {
			fulfill_cancelled<Result>(next);
			return;
		}
		try {
			if constexpr (is_void_v<T>) {
				connect<Result>(fn(), next);
			} else {
				auto value = move(get<StoredValue<T>>(outcome.payload));
				connect<Result>(fn(move(value)), next);
			}
		} catch (...) { fulfill_error<Result>(next, current_exception()); }
	});
	return Flow<Result>::from_state(move(next));
}

template<typename T, typename Fn>
auto operator |(
	Flow<T> &&flow,
	::ErrorStep<Fn> step) {
	using Handler = ErrorResult<T, Fn>;
	using Result = UnwrapFlowT<Handler>;
	auto next = make_shared<State<Result>>();
	attach<T>(flow.release_state(), [next, fn = move(step.fn)](Outcome<T> &&outcome) mutable {
		if (outcome.tag == OutcomeTag::value) {
			if constexpr (is_same_v<Result, T>) {
				forward_outcome<T>(next, move(outcome));
			} else if constexpr (is_void_v<T> && is_void_v<Result>) {
				fulfill_value<Result>(next);
			} else if constexpr (!is_void_v<T> && !is_void_v<Result>) {
				fulfill_value<Result>(next, move(get<StoredValue<T>>(outcome.payload)));
			} else {
				static_assert(is_void_v<T> == is_void_v<Result>, "on_error success path must preserve value shape");
			}
			return;
		}
		if (outcome.tag == OutcomeTag::cancelled) {
			fulfill_cancelled<Result>(next);
			return;
		}
		try {
			if constexpr (kIsFlow<Handler>) {
				connect<Result>(fn(outcome_error<T>(outcome)), next);
			} else if constexpr (is_void_v<Result>) {
				fn(outcome_error<T>(outcome));
				fulfill_value<Result>(next);
			} else {
				fulfill_value<Result>(next, fn(outcome_error<T>(outcome)));
			}
		} catch (...) { fulfill_error<Result>(next, current_exception()); }
	});
	return Flow<Result>::from_state(move(next));
}

template<typename T, typename Fn>
auto operator |(
	Flow<T> &&flow,
	::CancelStep<Fn> step) {
	using Handler = CancelResult<T, Fn>;
	using Result = conditional_t<kIsFlow<Handler>, UnwrapFlowT<Handler>, conditional_t<is_void_v<Handler>, T, Handler>>;
	auto next = make_shared<State<Result>>();
	attach<T>(flow.release_state(), [next, fn = move(step.fn)](Outcome<T> &&outcome) mutable {
		if (outcome.tag == OutcomeTag::value) {
			if constexpr (is_void_v<T> && is_void_v<Result>) {
				fulfill_value<Result>(next);
			} else if constexpr (!is_void_v<T> && is_same_v<Result, T>) {
				fulfill_value<Result>(next, move(get<StoredValue<T>>(outcome.payload)));
			} else {
				static_assert(
					is_same_v<Result, T> || (is_void_v<T> && is_void_v<Result>),
					"on_cancel success path must preserve value shape");
			}
			return;
		}
		if (outcome.tag == OutcomeTag::error) {
			fulfill_error<Result>(next, outcome_error<T>(outcome));
			return;
		}
		try {
			if constexpr (kIsFlow<Handler>) {
				connect<Result>(fn(), next);
			} else {
				if constexpr (is_void_v<Handler>) {
					fn();
					if constexpr (is_void_v<Result>) {
						fulfill_value<Result>(next);
					} else {
						fulfill_cancelled<Result>(next);
					}
				} else {
					auto recovered = fn();
					if constexpr (is_void_v<Result>) {
						(void)recovered;
						fulfill_value<Result>(next);
					} else {
						fulfill_value<Result>(next, move(recovered));
					}
				}
			}
		} catch (...) { fulfill_error<Result>(next, current_exception()); }
	});
	return Flow<Result>::from_state(move(next));
}

template<typename T, typename Target>
auto operator |(
	Flow<T> &&flow,
	::MoveToStep<Target> step) {
	auto next = make_shared<State<T>>();
	attach<T>(flow.release_state(), [next, target = step.target](Outcome<T> &&outcome) mutable {
		if (!target->enqueue([next, outcome = move(outcome)]() mutable { forward_outcome<T>(next, move(outcome)); })) {
			fulfill_cancelled<T>(next);
		}
	});
	return Flow<T>::from_state(move(next));
}

template<typename T, typename Target>
auto operator |(
	Flow<T> &&flow,
	::StartOnStep<Target> step) {
	auto next = make_shared<State<T>>();
	attach<T>(flow.release_state(), [next, target = step.target](Outcome<T> &&outcome) mutable {
		if (!target->enqueue([next, outcome = move(outcome)]() mutable { forward_outcome<T>(next, move(outcome)); })) {
			fulfill_cancelled<T>(next);
		}
	});
	return Flow<T>::from_state(move(next));
}

struct JoinShared {
	mutex mtx;
	size_t remaining = 0;
	bool done = false;
};

template<typename... Ts>
auto join_all(
	Flow<Ts>... flows) {
	using Result = tuple<StoredValue<Ts>...>;
	auto next = make_shared<State<Result>>();
	if constexpr (sizeof...(Ts) == 0) {
		fulfill_value<Result>(next);
		return Flow<Result>::from_state(move(next));
	}
	auto shared = make_shared<JoinShared>();
	shared->remaining = sizeof...(Ts);
	auto values = make_shared<tuple<optional<StoredValue<Ts>>...>>();
	auto attach_one = [shared, next, values]<size_t I, typename U>(Flow<U> flow) mutable {
		attach<U>(flow.release_state(), [shared, next, values]<typename V>(Outcome<V> &&outcome) mutable {
			unique_lock lk{shared->mtx};
			if (shared->done) {
				return;
			}
			if (outcome.tag == OutcomeTag::error) {
				shared->done = true;
				auto error = outcome_error<V>(outcome);
				lk.unlock();
				fulfill_error<Result>(next, error);
				return;
			}
			if (outcome.tag == OutcomeTag::cancelled) {
				shared->done = true;
				lk.unlock();
				fulfill_cancelled<Result>(next);
				return;
			}
			get<I>(*values) = move(get<StoredValue<V>>(outcome.payload));
			if (--shared->remaining == 0) {
				shared->done = true;
				auto tuple_result = apply([](auto &...items) { return Result{move(*items)...}; }, *values);
				lk.unlock();
				fulfill_value<Result>(next, move(tuple_result));
			}
		});
	};
	([&]<size_t... Is>(index_sequence<Is...>) {
		(attach_one.template operator ()<Is>(move(flows)), ...);
	}(index_sequence_for<Ts...>{}));
	return Flow<Result>::from_state(move(next));
}

template<typename T>
auto wait(
	Flow<T> flow) {
	auto state = flow.release_state();
	auto outcome = await_result<T>(state);
	if constexpr (is_void_v<T>) {
		(void)extract_value<T>(move(outcome));
		return;
	} else {
		return extract_value<T>(move(outcome));
	}
}

template<typename T>
void spawn(
	Flow<T> flow) {
	attach<T>(flow.release_state(), [](Outcome<T> &&outcome) {
		if (outcome.tag == OutcomeTag::error) {
			DetachedErrors::instance().handle(outcome_error<T>(outcome));
		}
	});
}

} // namespace work_detail

export template<typename Target, typename Fn>
[[nodiscard]] auto run_on(
	Target &target,
	Fn &&fn) {
	return work_detail::run_on(target, forward<Fn>(fn));
}

export template<typename T, typename Fn>
[[nodiscard]] auto operator |(
	Flow<T> &&flow,
	ThenStep<Fn> step) {
	return work_detail::operator |(move(flow), move(step));
}

export template<typename T, typename Fn>
[[nodiscard]] auto operator |(
	Flow<T> &&flow,
	FlatThenStep<Fn> step) {
	return work_detail::operator |(move(flow), move(step));
}

export template<typename T, typename Fn>
[[nodiscard]] auto operator |(
	Flow<T> &&flow,
	ErrorStep<Fn> step) {
	return work_detail::operator |(move(flow), move(step));
}

export template<typename T, typename Fn>
[[nodiscard]] auto operator |(
	Flow<T> &&flow,
	CancelStep<Fn> step) {
	return work_detail::operator |(move(flow), move(step));
}

export template<typename T, typename Target>
[[nodiscard]] auto operator |(
	Flow<T> &&flow,
	MoveToStep<Target> step) {
	return work_detail::operator |(move(flow), move(step));
}

export template<typename T, typename Target>
[[nodiscard]] auto operator |(
	Flow<T> &&flow,
	StartOnStep<Target> step) {
	return work_detail::operator |(move(flow), move(step));
}

export template<typename... Ts>
[[nodiscard]] auto join_all(
	Flow<Ts>... flows) {
	return work_detail::join_all(move(flows)...);
}

export template<typename T>
auto wait(
	Flow<T> flow) {
	return work_detail::wait(move(flow));
}

export template<typename T>
void spawn(
	Flow<T> flow) {
	work_detail::spawn(move(flow));
}

// ---------------------------------------------------------------------------
// FlowSource<T>: externally-resolvable producer for a Flow<T>. Used by code
// that bridges I/O completions (io_uring CQEs, eventfd readiness, etc.) into
// the Flow<T> pipeline without going through run_on / WorkPool.
//
// Copyable (shared-ownership of the control block). Only the first of
// resolve/reject/cancel across all copies takes effect.
// ---------------------------------------------------------------------------

export template<typename T>
class FlowSource {
	struct Control {
		work_detail::StatePtr<T> state{make_shared<work_detail::State<T>>()};
		atomic_flag disarmed_{};
	};
	shared_ptr<Control> ctrl_{make_shared<Control>()};

	[[nodiscard]] bool disarm() const noexcept { return !ctrl_->disarmed_.test_and_set(memory_order_acq_rel); }

public:
	FlowSource() = default;

	[[nodiscard]] Flow<T> flow() const { return Flow<T>::from_state(ctrl_->state); }

	template<typename U = T>
		requires(!is_void_v<U>)
	void resolve(
		U result) const {
		if (!disarm()) {
			return;
		}
		work_detail::fulfill_value<T>(ctrl_->state, move(result));
	}

	template<typename U = T>
		requires(is_void_v<U>)
	void resolve() const {
		if (!disarm()) {
			return;
		}
		work_detail::fulfill_value<T>(ctrl_->state);
	}

	void reject(
		exception_ptr error) const {
		if (!disarm()) {
			return;
		}
		work_detail::fulfill_error<T>(ctrl_->state, error);
	}

	void cancel() const {
		if (!disarm()) {
			return;
		}
		work_detail::fulfill_cancelled<T>(ctrl_->state);
	}

	[[nodiscard]] bool armed() const noexcept { return !ctrl_->disarmed_.test(memory_order_acquire); }
};

// ---------------------------------------------------------------------------
// Task<T>: asio-style coroutine type backed by Flow<T>. `co_await` a Flow<T>
// or another Task<T> inside. The coroutine body runs eagerly (initial_suspend
// = never); the outer Flow resolves when the coroutine returns.
// ---------------------------------------------------------------------------

export template<typename T>
class Task;

template<typename T>
struct TaskPromiseBase {
	FlowSource<T> source{};

	[[nodiscard]] Task<T> get_return_object();
	[[nodiscard]] std::suspend_never initial_suspend() const noexcept { return {}; }
	[[nodiscard]] std::suspend_never final_suspend() const noexcept { return {}; }

	void unhandled_exception() { source.reject(current_exception()); }
};

export template<typename T>
struct TaskPromise : TaskPromiseBase<T> {
	void return_value(
		T v) {
		this->source.resolve(move(v));
	}
};

template<>
struct TaskPromise<void> : TaskPromiseBase<void> {
	void return_void() { source.resolve(); }
};

export template<typename T>
class [[nodiscard]] Task {
	Flow<T> flow_{};

public:
	using promise_type = TaskPromise<T>;

	Task() = default;
	explicit Task(
		Flow<T> flow)
		: flow_{move(flow)} {}
	Task(Task const &) = delete;
	Task &operator =(Task const &) = delete;
	Task(Task &&) noexcept = default;
	Task &operator =(Task &&) noexcept = default;

	[[nodiscard]] bool valid() const noexcept { return flow_.valid(); }

	[[nodiscard]] Flow<T> flow() && { return move(flow_); }

	auto operator co_await() { return flow_.operator co_await(); }
};

template<typename T>
Task<T> TaskPromiseBase<T>::get_return_object() {
	return Task<T>{source.flow()};
}

export template<typename T>
void co_spawn(
	Task<T> task) {
	work_detail::spawn(move(task).flow());
}
