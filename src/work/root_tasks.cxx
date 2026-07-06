module;

#include <memory>
#include <mutex>

export module conflux.work.root:tasks;

import std;
import conflux.types;
import conflux.small_function;
import :core;

namespace conflux::work::root::detail {

struct consume_access;

} // namespace conflux::work::root::detail

export namespace conflux::work::root {

namespace detail {

template<work_value T, bool EnableCancellation>
[[nodiscard]] std::shared_ptr<ControlBlockInterface<T>> make_control_block_shared();

// Coroutine-backed Tasks use EnableCancellation=false: cancellation is cooperative
// only (check stop_token in coroutine body). External request_cancel() requires
// make_task_source(SubmitOptions{.enable_cancellation=true}).
template<work_value T>
struct TaskPromiseReturn {
	std::shared_ptr<ControlBlockInterface<T>> state_{make_control_block_shared<T, false>()};
	void return_value(
		T v) {
		auto _ = state_->try_set_value(Success<T>{std::move(v)});
	}
};
template<>
struct TaskPromiseReturn<void> {
	std::shared_ptr<ControlBlockInterface<void>> state_{make_control_block_shared<void, false>()};
	void return_void() { auto _ = state_->try_set_value(Success<void>{}); }
};
template<work_value T>
struct TaskAwaiter {
	std::shared_ptr<ControlBlockInterface<T>> state_;
	std::source_location loc_{};
	bool ready_callback_already_installed_{false};
	bool ready_callback_installed_{false};
	TaskAwaiter() = default;
	explicit TaskAwaiter(
		std::shared_ptr<ControlBlockInterface<T>> state,
		std::source_location loc = std::source_location::current()) noexcept
		: state_{std::move(state)}
		, loc_{loc} {}
	TaskAwaiter(
		TaskAwaiter &&other) noexcept
		: state_{std::move(other.state_)}
		, loc_{other.loc_}
		, ready_callback_already_installed_{std::exchange(other.ready_callback_already_installed_, false)}
		, ready_callback_installed_{std::exchange(other.ready_callback_installed_, false)} {}
	TaskAwaiter &operator =(
		TaskAwaiter &&other) noexcept {
		if (this != &other) {
			clear_ready_callback_if_installed();
			state_ = std::move(other.state_);
			loc_ = other.loc_;
			ready_callback_already_installed_ = std::exchange(other.ready_callback_already_installed_, false);
			ready_callback_installed_ = std::exchange(other.ready_callback_installed_, false);
		}
		return *this;
	}
	TaskAwaiter(TaskAwaiter const &) = delete;
	TaskAwaiter &operator =(TaskAwaiter const &) = delete;
	~TaskAwaiter() noexcept { clear_ready_callback_if_installed(); }
	[[nodiscard]] bool await_ready() const noexcept { return !state_ || state_->ready(); }
	[[nodiscard]] bool await_suspend(
		std::coroutine_handle<> h) noexcept {
		auto result = state_->try_set_on_ready(
			::conflux::detail::small_move_only_function<void()>{[h]() noexcept { h.resume(); }});
		switch (result.status) {
		case ReadyRegistration::installed        : ready_callback_installed_ = true; return true;
		case ReadyRegistration::already_ready    :
		case ReadyRegistration::empty            : return false;
		case ReadyRegistration::already_installed: ready_callback_already_installed_ = true; return false;
		}
		std::unreachable();
	}
	decltype(auto) await_resume() {
		ready_callback_installed_ = false;
		if (!state_) [[unlikely]] {
			raise_join_consumed_handle(loc_);
		}
		if (ready_callback_already_installed_) [[unlikely]] {
			throw JoinError{JoinError::reason::ready_callback_already_installed, loc_};
		}
		auto outcome = state_->try_take_ready_outcome();
		if (!outcome) [[unlikely]] {
			raise_join_not_ready(loc_);
		}
		return std::move(*outcome).visit([](auto &&arm) -> std::conditional_t<std::is_void_v<T>, void, T> {
			using arm_t = std::remove_cvref_t<decltype(arm)>;
			if constexpr (std::same_as<arm_t, Success<T>>) {
				if constexpr (!std::is_void_v<T>) {
					return std::move(arm.value);
				}
			} else if constexpr (std::same_as<arm_t, Failure>) {
				std::rethrow_exception(arm.error);
			} else {
				throw CancelledError{arm.reason};
			}
		});
	}
	void clear_ready_callback_if_installed() noexcept {
		if (ready_callback_installed_ && state_) {
			auto _ = state_->clear_on_ready();
			ready_callback_installed_ = false;
		}
	}
};
template<work_value T>
struct OutcomeAwaiter {
	std::shared_ptr<ControlBlockInterface<T>> state_;
	std::source_location loc_{};
	bool ready_callback_already_installed_{false};
	bool ready_callback_installed_{false};
	OutcomeAwaiter() = default;
	explicit OutcomeAwaiter(
		std::shared_ptr<ControlBlockInterface<T>> state,
		std::source_location loc = std::source_location::current()) noexcept
		: state_{std::move(state)}
		, loc_{loc} {}
	OutcomeAwaiter(
		OutcomeAwaiter &&other) noexcept
		: state_{std::move(other.state_)}
		, loc_{other.loc_}
		, ready_callback_already_installed_{std::exchange(other.ready_callback_already_installed_, false)}
		, ready_callback_installed_{std::exchange(other.ready_callback_installed_, false)} {}
	OutcomeAwaiter &operator =(
		OutcomeAwaiter &&other) noexcept {
		if (this != &other) {
			clear_ready_callback_if_installed();
			state_ = std::move(other.state_);
			loc_ = other.loc_;
			ready_callback_already_installed_ = std::exchange(other.ready_callback_already_installed_, false);
			ready_callback_installed_ = std::exchange(other.ready_callback_installed_, false);
		}
		return *this;
	}
	OutcomeAwaiter(OutcomeAwaiter const &) = delete;
	OutcomeAwaiter &operator =(OutcomeAwaiter const &) = delete;
	~OutcomeAwaiter() noexcept { clear_ready_callback_if_installed(); }
	[[nodiscard]] bool await_ready() const noexcept { return !state_ || state_->ready(); }
	[[nodiscard]] bool await_suspend(
		std::coroutine_handle<> h) noexcept {
		auto result = state_->try_set_on_ready(
			::conflux::detail::small_move_only_function<void()>{[h]() noexcept { h.resume(); }});
		switch (result.status) {
		case ReadyRegistration::installed        : ready_callback_installed_ = true; return true;
		case ReadyRegistration::already_ready    :
		case ReadyRegistration::empty            : return false;
		case ReadyRegistration::already_installed: ready_callback_already_installed_ = true; return false;
		}
		std::unreachable();
	}
	Outcome<T> await_resume() {
		ready_callback_installed_ = false;
		if (!state_) [[unlikely]] {
			raise_join_consumed_handle(loc_);
		}
		if (ready_callback_already_installed_) [[unlikely]] {
			throw JoinError{JoinError::reason::ready_callback_already_installed, loc_};
		}
		auto outcome = state_->try_take_ready_outcome();
		if (!outcome) [[unlikely]] {
			raise_join_not_ready(loc_);
		}
		return std::move(*outcome);
	}
	void clear_ready_callback_if_installed() noexcept {
		if (ready_callback_installed_ && state_) {
			auto _ = state_->clear_on_ready();
			ready_callback_installed_ = false;
		}
	}
};
template<class A>
using await_resume_t = decltype(std::declval<A &>().await_resume());
template<class A>
concept awaitable = requires(A &a, std::coroutine_handle<> h) {
	{ a.await_ready() } -> std::convertible_to<bool>;
	a.await_suspend(h);
	a.await_resume();
};
template<class A, class T>
concept awaits_outcome = awaitable<A> && std::same_as<await_resume_t<A>, Outcome<T>>;

} // namespace detail
template<work_value T, ControlCategory Category>
class BasicResult {
	std::shared_ptr<detail::ControlBlockInterface<T>> state_{};
	join_state state_js_ = join_state::empty;
	explicit BasicResult(
		std::shared_ptr<detail::ControlBlockInterface<T>> state) noexcept
		: state_{std::move(state)}
		, state_js_{state_ ? join_state::joinable : join_state::empty} {}
	[[nodiscard]] std::shared_ptr<detail::ControlBlockInterface<T>> consume(
		join_state target) noexcept {
		if (state_js_ != join_state::joinable) {
			return {};
		}
		state_js_ = target;
		return std::move(state_);
	}
	void detach_noexcept() noexcept {
		auto loc = state_ ? state_->spawn_location() : std::source_location{};
		abandon_impl(std::move(*this), detail::detach_outcome_sink<T>{loc});
	}
	template<work_value U>
	std::pair<BasicResult<U, ControlCategory::task>, TaskSource<U>> friend make_task_source(
		SubmitOptions,
		std::source_location);
	template<work_value U, progress_capability Owner>
	std::pair<BasicResult<U, ControlCategory::posted>, PostedSource<U>> friend make_posted_source(
		Owner &,
		PostOptions,
		std::source_location);
	template<work_value U, progress_capability Driver>
	std::pair<BasicResult<U, ControlCategory::operation>, OperationSource<U>> friend make_operation_source(
		Driver &,
		OperationOptions,
		std::source_location);
	friend class BasicJoinHandle<T, Category>;
	template<class Fn>
	auto friend spawn(Fn &&, std::source_location) -> std::invoke_result_t<Fn>;
	friend struct detail::consume_access;

public:
	using value_type = T;

	BasicResult() = default;
	[[nodiscard]] static BasicResult from_state(
		std::shared_ptr<detail::ControlBlockInterface<T>> state,
		std::source_location loc = std::source_location::current()) noexcept {
		if (state) {
			state->set_spawn_location(loc);
		}
		return BasicResult{std::move(state)};
	}
	BasicResult(
		BasicResult &&other) noexcept
		: state_{std::move(other.state_)}
		, state_js_{std::exchange(other.state_js_, join_state::empty)} {}
	BasicResult &operator =(
		BasicResult &&other) noexcept {
		if (this != &other) {
			if (state_js_ == join_state::joinable) {
				detach_noexcept();
			}
			state_ = std::move(other.state_);
			state_js_ = std::exchange(other.state_js_, join_state::empty);
		}
		return *this;
	}
	BasicResult(BasicResult const &) = delete;
	BasicResult &operator =(BasicResult const &) = delete;
	~BasicResult() noexcept {
		if (state_js_ == join_state::joinable) {
			detach_noexcept();
		}
	}
	[[nodiscard]] join_state state() const noexcept { return state_js_; }
	// Named std::move alias; lvalue-friendly (R2 #2 v4 fix).
	[[nodiscard]] BasicResult &&consume() & noexcept { return std::move(*this); }
	[[nodiscard]] BasicResult &&consume() && noexcept { return std::move(*this); }
	[[nodiscard]] typename control_handle_for<Category>::type control() const noexcept {
		return typename control_handle_for<Category>::type{state_};
	}
	void set_spawn_location(
		std::source_location loc) noexcept {
		if (state_) {
			state_->set_spawn_location(loc);
		}
	}
	void cancel(
		CancelReason reason = CancelReason::requested) noexcept {
		if (state_js_ != join_state::empty) {
			auto _ = control().request_cancel(reason);
		}
	}
	void detach() && noexcept {
		if (state_js_ == join_state::joinable) {
			detach_noexcept();
		}
	}
	template<class Sink>
		requires abandon_sink<Sink, T>
	void abandon_to(
		Sink &&sink) && noexcept {
		if (state_js_ == join_state::joinable) {
			abandon_impl(std::move(*this), std::forward<Sink>(sink));
		}
	}
	// Hard contract: only rvalue can be awaited (R4 v6 #9).
	[[nodiscard("Task must be consumed: use co_await std::move(task) or co_await task.consume()")]] auto
	operator co_await() & = delete;
	[[nodiscard]] auto operator co_await() && noexcept { return detail::TaskAwaiter<T>{consume(join_state::joined)}; }
	[[nodiscard]] auto outcome() && noexcept -> detail::OutcomeAwaiter<T> {
		return detail::OutcomeAwaiter<T>{consume(join_state::joined)};
	}
	struct promise_type : detail::TaskPromiseReturn<T> {
		static_assert(Category == ControlCategory::task, "promise_type only available on Task<T>");

		static void *operator new(
			std::size_t size) {
			auto *hdr = detail::allocate_task_coroutine_frame(size);
			detail::note_coroutine_frame_allocation();
			return hdr + 1;
		}
		static void operator delete(
			void *p) noexcept {
			detail::note_coroutine_frame_deallocation();
			detail::deallocate_task_coroutine_frame(static_cast<detail::TaskFrameHeader *>(p) - 1);
		}
		static void operator delete(
			void *p,
			std::size_t) noexcept {
			detail::note_coroutine_frame_deallocation();
			detail::deallocate_task_coroutine_frame(static_cast<detail::TaskFrameHeader *>(p) - 1);
		}

		[[nodiscard]] BasicResult get_return_object() noexcept;
		[[nodiscard]] std::suspend_never initial_suspend() const noexcept { return {}; }
		[[nodiscard]] std::suspend_never final_suspend() const noexcept { return {}; }
		void unhandled_exception() noexcept {
			try {
				throw;
			} catch (CancelledError const &err) {
				auto const allow_abandoned = err.reason() == CancelReason::abandoned;
				auto _ = this->state_->try_set_cancelled(err.reason(), allow_abandoned);
			} catch (...) { auto _ = this->state_->try_set_exception(std::current_exception()); }
		}
	};
};
template<work_value T, ControlCategory Category>
BasicResult<T, Category> BasicResult<T, Category>::promise_type::get_return_object() noexcept {
	return BasicResult<T, Category>::from_state(this->state_, std::source_location{});
}
template<work_value T>
using Task = BasicResult<T, ControlCategory::task>;

template<work_value T>
using Posted = BasicResult<T, ControlCategory::posted>;

template<work_value T>
using Operation = BasicResult<T, ControlCategory::operation>;

class Cancellation {
	TaskControl control_{};

public:
	Cancellation() = default;
	explicit Cancellation(
		TaskControl control) noexcept
		: control_{std::move(control)} {}
	[[nodiscard]] bool requested() const noexcept { return control_.cancel_requested(); }
	[[nodiscard]] CancelReason reason(
		CancelReason fallback = CancelReason::requested) const noexcept {
		auto current = control_.cancellation_reason();
		return current ? *current : fallback;
	}
	[[nodiscard]] std::stop_token stop_token() const noexcept { return control_.stop_token(); }
	void throw_if_requested() const {
		if (requested()) {
			throw CancelledError{reason()};
		}
	}
	template<work_value T>
	class child_cancel_awaiter {
		TaskControl parent_control_;
		std::uint64_t generation_{};
		detail::TaskAwaiter<T> inner_;
		bool armed_{};

	public:
		child_cancel_awaiter(
			Cancellation parent,
			Task<T> task) noexcept
			: parent_control_{std::move(parent.control_)}
			, generation_{parent_control_.bind_child_for_cancellation(task.control())}
			, inner_{std::move(task).operator co_await()}
			, armed_{generation_ != 0} {}
		child_cancel_awaiter(
			child_cancel_awaiter &&other) noexcept
			: parent_control_{std::move(other.parent_control_)}
			, generation_{std::exchange(other.generation_, 0)}
			, inner_{std::move(other.inner_)}
			, armed_{std::exchange(other.armed_, false)} {}
		child_cancel_awaiter &operator =(child_cancel_awaiter &&) = delete;
		child_cancel_awaiter(child_cancel_awaiter const &) = delete;
		child_cancel_awaiter &operator =(child_cancel_awaiter const &) = delete;
		~child_cancel_awaiter() noexcept {
			if (armed_) {
				parent_control_.clear_child_for_cancellation(generation_);
			}
		}
		[[nodiscard]] bool await_ready() const noexcept { return inner_.await_ready(); }
		[[nodiscard]] bool await_suspend(
			std::coroutine_handle<> h) noexcept {
			return inner_.await_suspend(h);
		}
		decltype(auto) await_resume() {
			if (armed_) {
				parent_control_.clear_child_for_cancellation(generation_);
				armed_ = false;
			}
			return inner_.await_resume();
		}
	};
	template<work_value T>
	[[nodiscard]] child_cancel_awaiter<T> await(
		Task<T> task) const noexcept {
		return child_cancel_awaiter<T>{*this, std::move(task)};
	}
};

// JoinTask<T> — strict variant: dtor on joinable state calls std::terminate.
// No combinators (use std::move(jt).detach_to_task() to compose).
// Obtain via require_join(task, loc) or spawn_strict(fn, loc).
template<work_value T>
class JoinTask {
	Task<T> inner_{};
	std::source_location origin_{};

public:
	using value_type = T;

	JoinTask() = default;
	JoinTask(
		Task<T> &&task,
		std::source_location origin) noexcept
		: inner_{std::move(task)}
		, origin_{origin} {}
	JoinTask(JoinTask &&) noexcept = default;
	JoinTask &operator =(
		JoinTask &&other) noexcept {
		if (this != &other) {
			if (inner_.state() == join_state::joinable) {
				std::terminate();
			}
			inner_ = std::move(other.inner_);
			origin_ = other.origin_;
		}
		return *this;
	}
	JoinTask(JoinTask const &) = delete;
	JoinTask &operator =(JoinTask const &) = delete;
	~JoinTask() noexcept {
		if (inner_.state() == join_state::joinable) {
			std::terminate();
		}
	}
	[[nodiscard]] join_state state() const noexcept { return inner_.state(); }
	// Named std::move alias; lvalue-friendly.
	[[nodiscard]] JoinTask &&consume() & noexcept { return std::move(*this); }
	[[nodiscard]] JoinTask &&consume() && noexcept { return std::move(*this); }
	[[nodiscard]] decltype(auto) control() const noexcept { return inner_.control(); }
	void cancel(
		CancelReason reason = CancelReason::requested) noexcept {
		inner_.cancel(reason);
	}
	// Downgrade to Task<T> (enables combinators). Caller accepts auto-detach.
	[[nodiscard]] Task<T> detach_to_task() && noexcept {
		auto out = std::move(inner_);
		inner_ = Task<T>{};
		return out;
	}
	[[nodiscard]] std::source_location origin() const noexcept { return origin_; }
	[[nodiscard("JoinTask must be consumed: use co_await std::move(jt) or co_await jt.consume()")]] auto
	operator co_await() & = delete;
	[[nodiscard]] auto operator co_await() && noexcept { return std::move(inner_).operator co_await(); }
	[[nodiscard]] auto outcome() && noexcept { return std::move(inner_).outcome(); }
};
template<work_value T>
[[nodiscard]] JoinTask<T> require_join(
	Task<T> &&task,
	std::source_location origin = std::source_location::current()) noexcept {
	return JoinTask<T>{std::move(task), origin};
}
// spawn: calls fn() to produce a Task<T>, attaches loc as spawn_loc.
// The returned Task auto-detaches if dropped without co_await.
template<class Fn>
[[nodiscard]] auto spawn(
	Fn &&fn,
	std::source_location loc = std::source_location::current()) -> std::invoke_result_t<Fn> {
	auto task = std::invoke(std::forward<Fn>(fn));
	task.set_spawn_location(loc);
	return task;
}
// spawn_strict: like spawn but returns JoinTask<T> (dtor → terminate).
template<class Fn>
[[nodiscard]] auto spawn_strict(
	Fn &&fn,
	std::source_location loc = std::source_location::current())
	-> JoinTask<typename std::invoke_result_t<Fn>::value_type> {
	return require_join(spawn(std::forward<Fn>(fn), loc), loc);
}
// [REVISIT] Collapse BasicResult/BasicJoinHandle?
// [option] Rename to BasicJoinHandle (join-by-default, explicit abandon())
//   Pros: RAII-safe defaults (prevents leaked/corrupted task state),
//         explicit detach intent, predictable cleanup,
//         aligns with executor best practices.
//   Cons: Slightly verbose for fire-and-forget, requires
//         drop-panic/warn discipline, breaks legacy auto-detach
//         expectations.
// [option] Keep separate (JoinHandle awaits → yields Result)
//   Pros: Zero runtime overhead, clean separation (lifecycle vs. output),
//         matches tokio/async-std patterns, compile-time correctness.
//   Cons: Two types, requires understanding the join handle → Result
//         relationship.
template<work_value T, ControlCategory Category>
class BasicJoinHandle {
	std::shared_ptr<detail::ControlBlockInterface<T>> state_{};
	bool live_ = false;
	explicit BasicJoinHandle(
		std::shared_ptr<detail::ControlBlockInterface<T>> state) noexcept
		: state_{std::move(state)}
		, live_{static_cast<bool>(state_)} {}
	[[nodiscard]] std::shared_ptr<detail::ControlBlockInterface<T>> consume() noexcept {
		if (!live_) {
			return {};
		}
		live_ = false;
		return std::move(state_);
	}

	template<work_value U, class Sink>
		requires abandon_sink<Sink, U>
	AbandonStatus friend try_abandon_to(BasicJoinHandle<U, ControlCategory::task> &&, Sink &&) noexcept;
	template<work_value U, class Sink>
		requires abandon_sink<Sink, U>
	AbandonStatus friend try_abandon_to(BasicJoinHandle<U, ControlCategory::posted> &&, Sink &&) noexcept;
	template<work_value U, class Sink>
		requires abandon_sink<Sink, U>
	AbandonStatus friend try_abandon_to(BasicJoinHandle<U, ControlCategory::operation> &&, Sink &&) noexcept;
	friend struct detail::consume_access;

public:
	using value_type = T;
	[[nodiscard]] static BasicJoinHandle adopt(
		BasicResult<T, Category> &&result) noexcept {
		return BasicJoinHandle{result.consume(join_state::detached)};
	}
	BasicJoinHandle() = default;
	BasicJoinHandle(
		BasicJoinHandle &&other) noexcept
		: state_{std::move(other.state_)}
		, live_{std::exchange(other.live_, false)} {}
	BasicJoinHandle &operator =(
		BasicJoinHandle &&other) noexcept {
		if (this != &other) {
			if (live_ && state_) {
				std::terminate();
			}
			state_ = std::move(other.state_);
			live_ = std::exchange(other.live_, false);
		}
		return *this;
	}
	BasicJoinHandle(BasicJoinHandle const &) = delete;
	BasicJoinHandle &operator =(BasicJoinHandle const &) = delete;
	~BasicJoinHandle() noexcept {
		if (live_ && state_) {
			std::terminate();
		}
	}
	[[nodiscard]] typename control_handle_for<Category>::type control() const noexcept {
		return typename control_handle_for<Category>::type{state_};
	}
	[[nodiscard]] explicit operator bool() const noexcept { return live_; }
	[[nodiscard]] auto outcome() && noexcept -> detail::OutcomeAwaiter<T> {
		return detail::OutcomeAwaiter<T>{consume()};
	}
};

} // namespace conflux::work::root

namespace conflux::work::root::detail {

struct consume_access {
	template<work_value T, ControlCategory Category>
	[[nodiscard]] static std::shared_ptr<ControlBlockInterface<T>> for_join(
		BasicResult<T, Category> &result) noexcept {
		return result.consume(join_state::joined);
	}
	template<work_value T, ControlCategory Category>
	[[nodiscard]] static std::shared_ptr<ControlBlockInterface<T>> for_abandon(
		BasicResult<T, Category> &result) noexcept {
		return result.consume(join_state::detached);
	}
	template<work_value T, ControlCategory Category>
	[[nodiscard]] static std::shared_ptr<ControlBlockInterface<T>> for_join(
		BasicJoinHandle<T, Category> &handle) noexcept {
		return handle.consume();
	}
	template<work_value T, ControlCategory Category>
	[[nodiscard]] static std::shared_ptr<ControlBlockInterface<T>> for_abandon(
		BasicJoinHandle<T, Category> &handle) noexcept {
		return handle.consume();
	}
};

} // namespace conflux::work::root::detail

export namespace conflux::work::root {

template<work_value T>
using TaskJoinHandle = BasicJoinHandle<T, ControlCategory::task>;

template<work_value T>
using PostedJoinHandle = BasicJoinHandle<T, ControlCategory::posted>;

template<work_value T>
using OperationJoinHandle = BasicJoinHandle<T, ControlCategory::operation>;

template<class H>
concept work_handle = work_value<typename H::value_type> && requires(H h, H const ch) {
	ch.control();
	requires detail::awaits_outcome<decltype(std::move(h).outcome()), typename H::value_type>;
};
template<class Sink, work_value T>
[[nodiscard]] ::conflux::detail::small_move_only_function<void(Outcome<T> const &)> make_abandon_dispatch_sink(
	Sink &&sink) noexcept {
	using sink_t = std::remove_cvref_t<Sink>;
	if constexpr (std::is_nothrow_invocable_v<sink_t &, Outcome<T> const &>) {
		return ::conflux::detail::small_move_only_function<void(Outcome<T> const &)>{std::forward<Sink>(sink)};
	} else {
		return ::conflux::detail::small_move_only_function<void(Outcome<T> const &)>{
			[sink = std::forward<Sink>(sink)](Outcome<T> const &outcome) mutable noexcept {
				if (outcome.is_failure()) {
					sink(outcome.failure());
				} else if (outcome.is_cancelled()) {
					sink(outcome.cancelled());
				}
			}};
	}
}
template<work_value T, ControlCategory Category, class Sink>
void abandon_impl(
	BasicResult<T, Category> &&r,
	Sink &&sink) noexcept {
	auto state = detail::consume_access::for_abandon(r);
	if (!state) {
		return; // empty or already-detached/joined — no-op
	}
	state->install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(std::forward<Sink>(sink)));
}
template<work_value T, ControlCategory Category, class Sink>
void abandon_impl(
	BasicJoinHandle<T, Category> &&h,
	Sink &&sink) noexcept {
	auto state = detail::consume_access::for_abandon(h);
	if (!state) {
		std::terminate();
	}
	state->install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(std::forward<Sink>(sink)));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	Task<T> &&task,
	Sink &&sink) noexcept {
	abandon_impl(std::move(task), std::forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	Posted<T> &&posted,
	Sink &&sink) noexcept {
	abandon_impl(std::move(posted), std::forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	Operation<T> &&op,
	Sink &&sink) noexcept {
	abandon_impl(std::move(op), std::forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	TaskJoinHandle<T> &&h,
	Sink &&sink) noexcept {
	abandon_impl(std::move(h), std::forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	PostedJoinHandle<T> &&h,
	Sink &&sink) noexcept {
	abandon_impl(std::move(h), std::forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	OperationJoinHandle<T> &&h,
	Sink &&sink) noexcept {
	abandon_impl(std::move(h), std::forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
[[nodiscard]] AbandonStatus try_abandon_to(
	TaskJoinHandle<T> &&h,
	Sink &&sink) noexcept {
	if (!bool(h)) {
		return AbandonStatus::empty;
	}
	auto state = detail::consume_access::for_abandon(h);
	if (!state) {
		return AbandonStatus::empty;
	}
	auto result = state->try_install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(std::forward<Sink>(sink)));
	return result;
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
[[nodiscard]] AbandonStatus try_abandon_to(
	PostedJoinHandle<T> &&h,
	Sink &&sink) noexcept {
	if (!bool(h)) {
		return AbandonStatus::empty;
	}
	auto state = detail::consume_access::for_abandon(h);
	if (!state) {
		return AbandonStatus::empty;
	}
	return state->try_install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(std::forward<Sink>(sink)));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
[[nodiscard]] AbandonStatus try_abandon_to(
	OperationJoinHandle<T> &&h,
	Sink &&sink) noexcept {
	if (!bool(h)) {
		return AbandonStatus::empty;
	}
	auto state = detail::consume_access::for_abandon(h);
	if (!state) {
		return AbandonStatus::empty;
	}
	return state->try_install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(std::forward<Sink>(sink)));
}
struct CarrierDiagnosticSink {
	void (*emit)(char const *msg) noexcept = nullptr;
};
namespace detail {

inline CarrierDiagnosticSink &carrier_diagnostic_sink_() noexcept {
	static CarrierDiagnosticSink sink{};
	return sink;
}

} // namespace detail
inline void set_carrier_diagnostic_sink(
	CarrierDiagnosticSink sink) noexcept {
	detail::carrier_diagnostic_sink_() = sink;
}
inline void emit_carrier_diagnostic(
	char const *msg) noexcept {
	auto &s = detail::carrier_diagnostic_sink_();
	if (s.emit != nullptr) {
		s.emit(msg);
	}
}
template<class Fn>
	requires std::is_invocable_v<Fn &, std::source_location, OutcomeKind, std::exception_ptr>
inline void set_dropped_outcome_sink(
	Fn &&fn) {
	auto &s = detail::dropped_outcome_sink_store();
	std::lock_guard const lk{s.mtx};
	s.fn = ::conflux::detail::small_move_only_function<void(std::source_location, OutcomeKind, std::exception_ptr)>{
		std::forward<Fn>(fn)};
	s.installed.store(true, std::memory_order_release);
}
template<class R, class Sink = drop_on_abandon>
class scoped_abandon {
	std::optional<R> value_{};
	std::optional<Sink> sink_{};
	bool armed_ = false;

public:
	scoped_abandon(
		R &&value,
		Sink sink = {})
		: value_{std::move(value)}
		, sink_{std::move(sink)}
		, armed_{true} {}
	scoped_abandon(
		scoped_abandon &&other) noexcept
		: value_{std::move(other.value_)}
		, sink_{std::move(other.sink_)}
		, armed_{std::exchange(other.armed_, false)} {}
	scoped_abandon &operator =(scoped_abandon &&) = delete;
	scoped_abandon(scoped_abandon const &) = delete;
	scoped_abandon &operator =(scoped_abandon const &) = delete;
	~scoped_abandon() noexcept {
		if (armed_ && value_) {
			abandon_to(std::move(*value_), std::move(*sink_));
		}
	}
	[[nodiscard]] R release() && {
		if (!armed_ || !value_) {
			std::terminate();
		}
		armed_ = false;
		return std::move(*value_);
	}
};
template<class R>
[[nodiscard]] auto guard_abandon(
	R &&value) {
	using result_t = std::remove_cvref_t<R>;
	return scoped_abandon<result_t, drop_on_abandon>{std::forward<R>(value), drop_on_abandon{}};
}
template<work_value T>
[[nodiscard]] std::pair<Task<T>, TaskSource<T>> make_task_source(
	SubmitOptions opts = {},
	std::source_location loc = std::source_location::current()) {
	std::shared_ptr<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = detail::make_control_block_shared<T, true>();
	} else {
		state = detail::make_control_block_shared<T, false>();
	}
	return {Task<T>::from_state(state, loc), TaskSource<T>::from_state(std::move(state))};
}
template<work_value T>
[[nodiscard]] std::pair<Task<T>, std::shared_ptr<TaskSource<T>>> make_shared_task_source(
	SubmitOptions opts = {},
	std::source_location loc = std::source_location::current()) {
	auto [task, src] = make_task_source<T>(opts, loc);
	return {std::move(task), std::shared_ptr<TaskSource<T>>{new TaskSource<T>(std::move(src))}};
}
template<work_value T>
[[nodiscard]] Task<T> make_exception_task(
	std::exception_ptr error,
	SubmitOptions opts = {},
	std::source_location loc = std::source_location::current()) {
	auto [task, src] = make_task_source<T>(opts, loc);
	auto _ = src.try_set_exception(std::move(error));
	return std::move(task);
}
template<work_value T, class Error>
[[nodiscard]] Task<T> make_error_task(
	Error error,
	SubmitOptions opts = {},
	std::source_location loc = std::source_location::current()) {
	return make_exception_task<T>(std::make_exception_ptr(std::move(error)), opts, loc);
}
template<work_value T>
[[nodiscard]] Task<void> bridge_task_to_source(
	std::shared_ptr<TaskSource<T>> src,
	Task<T> task) {
	try {
		if constexpr (std::is_void_v<T>) {
			co_await std::move(task);
			auto _ = src->try_set_value(Success<void>{});
		} else {
			auto value = co_await std::move(task);
			auto _ = src->try_set_value(Success<T>{std::move(value)});
		}
	} catch (CancelledError const &err) { auto _ = src->try_set_cancelled(err.reason()); } catch (...) {
		auto _ = src->try_set_exception(std::current_exception());
	}
}
template<work_value T, typename OnCancel>
	requires std::invocable<OnCancel &, CancelReason>
[[nodiscard]] std::pair<Task<T>, TaskSource<T>> make_cancellable_task_source(
	OnCancel on_cancel,
	std::source_location loc = std::source_location::current()) {
	auto out = make_task_source<T>(SubmitOptions{.enable_cancellation = true}, loc);
	bool const installed = out.second.install_cancel_hook(
		[on_cancel = std::move(on_cancel)](CancelReason reason) mutable { std::invoke(on_cancel, reason); });
	if (!installed) {
		std::terminate();
	}
	return out;
}
template<class Fn>
using cancellable_task_result_t = std::invoke_result_t<std::decay_t<Fn> &, Cancellation>;

template<class T>
struct is_task_result : std::false_type {};
template<work_value T>
struct is_task_result<Task<T>> : std::true_type {
	using value_type = T;
};
template<class T>
concept task_result = is_task_result<std::remove_cvref_t<T>>::value;

template<class Fn>
concept sync_cancellable_task_body = requires { typename cancellable_task_result_t<Fn>; }
								  && work_value<cancellable_task_result_t<Fn>>
								  && (!task_result<cancellable_task_result_t<Fn>>)
								  && std::invocable<std::decay_t<Fn> &, Cancellation>;

template<class Fn>
concept async_cancellable_task_body = requires {
	typename cancellable_task_result_t<Fn>;
} && task_result<cancellable_task_result_t<Fn>> && std::invocable<std::decay_t<Fn> &, Cancellation>;

template<class Fn>
	requires sync_cancellable_task_body<Fn>
[[nodiscard]] auto make_cancellable_task(
	Fn &&fn,
	std::source_location loc = std::source_location::current()) -> Task<cancellable_task_result_t<Fn>> {
	using fn_t = std::decay_t<Fn>;
	using T = cancellable_task_result_t<Fn>;
	auto [task, src] = make_task_source<T>(SubmitOptions{.enable_cancellation = true}, loc);
	auto cancel = Cancellation{task.control()};
	try {
		if constexpr (std::is_void_v<T>) {
			std::invoke(fn_t{std::forward<Fn>(fn)}, cancel);
			auto _ = src.try_set_value(Success<void>{});
		} else {
			auto _ = src.try_set_value(Success<T>{std::invoke(fn_t{std::forward<Fn>(fn)}, cancel)});
		}
	} catch (CancelledError const &err) { auto _ = src.try_set_cancelled(err.reason()); } catch (...) {
		auto _ = src.try_set_exception(std::current_exception());
	}
	return std::move(task);
}
template<work_value T, progress_capability Owner>
[[nodiscard]] std::pair<Posted<T>, PostedSource<T>> make_posted_source(
	Owner &owner,
	PostOptions opts,
	std::source_location loc) {
	std::shared_ptr<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = detail::make_control_block_shared<T, true>();
	} else {
		state = detail::make_control_block_shared<T, false>();
	}
	state->set_required_capability(capability_id(owner));
	return {Posted<T>::from_state(state, loc), PostedSource<T>::from_state(std::move(state))};
}
template<work_value T, progress_capability Owner>
[[nodiscard]] std::pair<Posted<T>, PostedSource<T>> make_posted_source(
	Owner &owner,
	PostOptions opts) {
	return make_posted_source<T>(owner, opts, std::source_location::current());
}
template<work_value T, progress_capability Owner>
[[nodiscard]] std::pair<Posted<T>, PostedSource<T>> make_posted_source(
	Owner &owner) {
	return make_posted_source<T>(owner, PostOptions{}, std::source_location::current());
}
template<work_value T, progress_capability Driver>
[[nodiscard]] std::pair<Operation<T>, OperationSource<T>> make_operation_source(
	Driver &driver,
	OperationOptions opts,
	std::source_location loc) {
	std::shared_ptr<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = detail::make_control_block_shared<T, true>();
	} else {
		state = detail::make_control_block_shared<T, false>();
	}
	state->set_required_capability(capability_id(driver));
	return {Operation<T>::from_state(state, loc), OperationSource<T>::from_state(std::move(state))};
}
template<work_value T, progress_capability Driver>
[[nodiscard]] std::pair<Operation<T>, OperationSource<T>> make_operation_source(
	Driver &driver,
	OperationOptions opts) {
	return make_operation_source<T>(driver, opts, std::source_location::current());
}
template<work_value T, progress_capability Driver>
[[nodiscard]] std::pair<Operation<T>, OperationSource<T>> make_operation_source(
	Driver &driver) {
	return make_operation_source<T>(driver, OperationOptions{}, std::source_location::current());
}
template<work_value T, ControlCategory C>
[[nodiscard]] BasicJoinHandle<T, C> into_join_handle(
	BasicResult<T, C> &&result) noexcept {
	return BasicJoinHandle<T, C>::adopt(std::move(result));
}
template<progress_capability Owner>
[[nodiscard]] bool can_join(
	Owner &owner,
	PostedControl const &control) noexcept {
	return control.can_join_with(capability_id(owner));
}
template<progress_capability Driver>
[[nodiscard]] bool can_join(
	Driver &driver,
	OperationControl const &control) noexcept {
	return control.can_join_with(capability_id(driver));
}
template<progress_capability Cap, work_value T>
[[nodiscard]] bool joinable(
	Cap const &cap,
	PostedJoinHandle<T> const &h) noexcept {
	return h.control().can_join_with(capability_id(cap));
}
template<progress_capability Cap, work_value T>
[[nodiscard]] bool joinable(
	Cap const &cap,
	OperationJoinHandle<T> const &h) noexcept {
	return h.control().can_join_with(capability_id(cap));
}
namespace detail {

template<work_value T>
[[nodiscard]] Outcome<T> take_ready_outcome_or_throw(
	std::shared_ptr<ControlBlockInterface<T>> state,
	std::source_location loc) {
	auto outcome = state->try_take_ready_outcome();
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return std::move(*outcome);
}

template<class Joinable>
[[nodiscard]] bool joinable_is_live(
	Joinable const &joinable) noexcept {
	if constexpr (requires { static_cast<bool>(joinable); }) {
		return static_cast<bool>(joinable);
	} else {
		return joinable.state() == join_state::joinable;
	}
}

template<class Joinable>
[[nodiscard]] std::optional<Outcome<typename Joinable::value_type>> try_consume_ready_join_state_checked(
	Joinable &joinable,
	std::optional<CapabilityId> actual,
	std::source_location loc) {
	using T = typename Joinable::value_type;
	if (!joinable_is_live(joinable)) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	auto control = joinable.control();
	if (actual && !control.can_join_with(*actual)) [[unlikely]] {
		raise_join_capability_mismatch(control.required_capability(), *actual, loc);
	}
	if (!control.ready()) {
		return std::nullopt;
	}
	auto state = consume_access::for_join(joinable);
	if (!state) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	return std::optional<Outcome<T>>{take_ready_outcome_or_throw(std::move(state), loc)};
}

template<work_value T>
[[nodiscard]] Outcome<T> blocking_join_compatibility_adapter(
	std::shared_ptr<ControlBlockInterface<T>> state,
	std::optional<CapabilityId> actual,
	std::source_location loc) {
	if (!state) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	if (actual && !state->can_join_with(*actual)) [[unlikely]] {
		raise_join_capability_mismatch(state->required_capability(), *actual, loc);
	}
	return state->compatibility_blocking_take_outcome();
}

} // namespace detail

template<work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	Task<T> &&task,
	std::source_location loc = std::source_location::current()) {
	return detail::try_consume_ready_join_state_checked(task, std::nullopt, loc);
}
template<progress_capability Owner, work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	Owner &owner,
	Posted<T> &&posted,
	std::source_location loc = std::source_location::current()) {
	return detail::try_consume_ready_join_state_checked(posted, std::optional<CapabilityId>{capability_id(owner)}, loc);
}
template<progress_capability Driver, work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	Driver &driver,
	Operation<T> &&op,
	std::source_location loc = std::source_location::current()) {
	return detail::try_consume_ready_join_state_checked(op, std::optional<CapabilityId>{capability_id(driver)}, loc);
}
template<work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	TaskJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	return detail::try_consume_ready_join_state_checked(h, std::nullopt, loc);
}
template<progress_capability Owner, work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	Owner &owner,
	PostedJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	return detail::try_consume_ready_join_state_checked(h, std::optional<CapabilityId>{capability_id(owner)}, loc);
}
template<progress_capability Driver, work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	Driver &driver,
	OperationJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	return detail::try_consume_ready_join_state_checked(h, std::optional<CapabilityId>{capability_id(driver)}, loc);
}
template<work_value T>
[[nodiscard]] Outcome<T> join_ready(
	Task<T> &&task,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(std::move(task), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return std::move(*outcome);
}
template<progress_capability Owner, work_value T>
[[nodiscard]] Outcome<T> join_ready(
	Owner &owner,
	Posted<T> &&posted,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(owner, std::move(posted), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return std::move(*outcome);
}
template<progress_capability Driver, work_value T>
[[nodiscard]] Outcome<T> join_ready(
	Driver &driver,
	Operation<T> &&op,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(driver, std::move(op), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return std::move(*outcome);
}
template<work_value T>
[[nodiscard]] Outcome<T> join_ready(
	TaskJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(std::move(h), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return std::move(*outcome);
}
template<progress_capability Owner, work_value T>
[[nodiscard]] Outcome<T> join_ready(
	Owner &owner,
	PostedJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(owner, std::move(h), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return std::move(*outcome);
}
template<progress_capability Driver, work_value T>
[[nodiscard]] Outcome<T> join_ready(
	Driver &driver,
	OperationJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(driver, std::move(h), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return std::move(*outcome);
}

template<class Fn>
	requires async_cancellable_task_body<Fn>
[[nodiscard]] auto make_cancellable_task(
	Fn &&fn,
	std::source_location loc = std::source_location::current())
	-> Task<typename is_task_result<std::remove_cvref_t<cancellable_task_result_t<Fn>>>::value_type> {
	using fn_t = std::decay_t<Fn>;
	using child_task_t = std::remove_cvref_t<cancellable_task_result_t<Fn>>;
	using T = typename is_task_result<child_task_t>::value_type;
	auto [task, src] = make_task_source<T>(SubmitOptions{.enable_cancellation = true}, loc);
	auto parent_control = task.control();
	auto shared_src = std::shared_ptr<TaskSource<T>>{new TaskSource<T>(std::move(src))};
	try {
		auto child = std::invoke(fn_t{std::forward<Fn>(fn)}, Cancellation{parent_control});
		auto child_handle =
			std::shared_ptr<TaskJoinHandle<T>>{new TaskJoinHandle<T>(into_join_handle(std::move(child)))};
		auto const generation = parent_control.bind_child_for_cancellation(child_handle->control());
		child_handle->control().set_on_ready_or_run(
			[shared_src, child_handle, parent_control, generation]() mutable noexcept {
				parent_control.clear_child_for_cancellation(generation);
				try {
					auto outcome = join_ready(std::move(*child_handle));
					std::move(outcome).visit([&](auto &&arm) {
						using arm_t = std::remove_cvref_t<decltype(arm)>;
						if constexpr (std::same_as<arm_t, Success<T>>) {
							auto _ = shared_src->try_set_value(std::move(arm));
						} else if constexpr (std::same_as<arm_t, Failure>) {
							auto _ = shared_src->try_set_exception(std::move(arm.error));
						} else {
							auto _ = shared_src->try_set_cancelled(arm.reason);
						}
					});
				} catch (CancelledError const &err) {
					auto _ = shared_src->try_set_cancelled(err.reason());
				} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
			});
	} catch (CancelledError const &err) { auto _ = shared_src->try_set_cancelled(err.reason()); } catch (...) {
		auto _ = shared_src->try_set_exception(std::current_exception());
	}
	return std::move(task);
}

template<work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Task<T> &&task,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(detail::consume_access::for_join(task), std::nullopt, loc);
}
template<progress_capability Owner, work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Owner &owner,
	Posted<T> &&posted,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(
		detail::consume_access::for_join(posted),
		std::optional<CapabilityId>{capability_id(owner)},
		loc);
}
template<progress_capability Driver, work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Driver &driver,
	Operation<T> &&op,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(
		detail::consume_access::for_join(op),
		std::optional<CapabilityId>{capability_id(driver)},
		loc);
}
template<work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	TaskJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(detail::consume_access::for_join(h), std::nullopt, loc);
}
template<progress_capability Owner, work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Owner &owner,
	PostedJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(
		detail::consume_access::for_join(h),
		std::optional<CapabilityId>{capability_id(owner)},
		loc);
}
template<progress_capability Driver, work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Driver &driver,
	OperationJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(
		detail::consume_access::for_join(h),
		std::optional<CapabilityId>{capability_id(driver)},
		loc);
}
template<work_value T>
[[nodiscard]] T value(
	Task<T> &&task) {
	return root::value(blocking_join(std::move(task)));
}
template<progress_capability Owner, work_value T>
[[nodiscard]] T value(
	Owner &owner,
	Posted<T> &&posted) {
	return root::value(blocking_join(owner, std::move(posted)));
}
template<progress_capability Driver, work_value T>
[[nodiscard]] T value(
	Driver &driver,
	Operation<T> &&op) {
	return root::value(blocking_join(driver, std::move(op)));
}
template<work_value T>
[[nodiscard]] T value(
	TaskJoinHandle<T> &&h) {
	return root::value(blocking_join(std::move(h)));
}
template<progress_capability Owner, work_value T>
[[nodiscard]] T value(
	Owner &owner,
	PostedJoinHandle<T> &&h) {
	return root::value(blocking_join(owner, std::move(h)));
}
template<progress_capability Driver, work_value T>
[[nodiscard]] T value(
	Driver &driver,
	OperationJoinHandle<T> &&h) {
	return root::value(blocking_join(driver, std::move(h)));
}
inline void value(
	Task<void> &&task) {
	root::value(blocking_join(std::move(task)));
}
template<progress_capability Owner>
inline void value(
	Owner &owner,
	Posted<void> &&posted) {
	root::value(blocking_join(owner, std::move(posted)));
}
template<progress_capability Driver>
inline void value(
	Driver &driver,
	Operation<void> &&op) {
	root::value(blocking_join(driver, std::move(op)));
}
inline void value(
	TaskJoinHandle<void> &&h) {
	root::value(blocking_join(std::move(h)));
}
template<progress_capability Owner>
inline void value(
	Owner &owner,
	PostedJoinHandle<void> &&h) {
	root::value(blocking_join(owner, std::move(h)));
}
template<progress_capability Driver>
inline void value(
	Driver &driver,
	OperationJoinHandle<void> &&h) {
	root::value(blocking_join(driver, std::move(h)));
}
template<work_value T>
[[nodiscard]] std::pair<TaskControl, TaskSource<T>> make_task_control_source() {
	auto state = detail::make_control_block_shared<T, true>();
	return {TaskControl{state}, TaskSource<T>::from_state(std::move(state))};
}
template<work_value T>
[[nodiscard]] std::pair<TaskControl, TaskSource<T>> make_task_control_source(
	SubmitOptions opts) {
	std::shared_ptr<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = detail::make_control_block_shared<T, true>();
	} else {
		state = detail::make_control_block_shared<T, false>();
	}
	return {TaskControl{state}, TaskSource<T>::from_state(std::move(state))};
}
template<work_value T>
[[nodiscard]] std::pair<PostedControl, PostedSource<T>> make_posted_control_source() {
	auto state = detail::make_control_block_shared<T, true>();
	return {PostedControl{state}, PostedSource<T>::from_state(std::move(state))};
}
template<work_value T>
[[nodiscard]] std::pair<PostedControl, PostedSource<T>> make_posted_control_source(
	PostOptions opts) {
	std::shared_ptr<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = detail::make_control_block_shared<T, true>();
	} else {
		state = detail::make_control_block_shared<T, false>();
	}
	return {PostedControl{state}, PostedSource<T>::from_state(std::move(state))};
}
template<work_value T>
[[nodiscard]] std::pair<OperationControl, OperationSource<T>> make_operation_control_source() {
	auto state = detail::make_control_block_shared<T, true>();
	return {OperationControl{state}, OperationSource<T>::from_state(std::move(state))};
}
template<work_value T>
[[nodiscard]] std::pair<OperationControl, OperationSource<T>> make_operation_control_source(
	OperationOptions opts) {
	std::shared_ptr<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = detail::make_control_block_shared<T, true>();
	} else {
		state = detail::make_control_block_shared<T, false>();
	}
	return {OperationControl{state}, OperationSource<T>::from_state(std::move(state))};
}

} // namespace conflux::work::root
