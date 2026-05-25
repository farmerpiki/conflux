export module conflux.work.root:tasks;

import std;
import conflux.types;
import conflux.small_function;
import :core;

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
	[[nodiscard]] bool await_ready() const noexcept { return !state_ || state_->ready(); }
	[[nodiscard]] bool await_suspend(
		std::coroutine_handle<> h) noexcept {
		auto result = state_->try_set_on_ready(
			::conflux::detail::small_move_only_function<void()>{[h]() noexcept { h.resume(); }});
		switch (result.status) {
		case ReadyRegistration::installed        : return true;
		case ReadyRegistration::already_ready    :
		case ReadyRegistration::empty            : return false;
		case ReadyRegistration::already_installed: ready_callback_already_installed_ = true; return false;
		}
		std::unreachable();
	}
	decltype(auto) await_resume() {
		if (!state_) [[unlikely]] {
			raise_join_consumed_handle(loc_);
		}
		if (ready_callback_already_installed_) [[unlikely]] {
			throw JoinError{JoinError::reason::ready_callback_already_installed, loc_};
		}
		// Rethrow original exception on Failure (not FailureError wrapper) so
		// existing catch sites work. E2b.2 migrates to FailureError uniformly.
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
};
template<work_value T>
struct OutcomeAwaiter {
	std::shared_ptr<ControlBlockInterface<T>> state_;
	std::source_location loc_{};
	bool ready_callback_already_installed_{false};
	[[nodiscard]] bool await_ready() const noexcept { return !state_ || state_->ready(); }
	[[nodiscard]] bool await_suspend(
		std::coroutine_handle<> h) noexcept {
		auto result = state_->try_set_on_ready(
			::conflux::detail::small_move_only_function<void()>{[h]() noexcept { h.resume(); }});
		switch (result.status) {
		case ReadyRegistration::installed        : return true;
		case ReadyRegistration::already_ready    :
		case ReadyRegistration::empty            : return false;
		case ReadyRegistration::already_installed: ready_callback_already_installed_ = true; return false;
		}
		std::unreachable();
	}
	Outcome<T> await_resume() {
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
	// HACK: GCC 16 module exporter treats `friend abandon_impl(...)` decl + the
	// namespace-scope decl as two distinct ADL candidates. Public-method indirection
	// keeps abandon_impl/join as plain non-friend free functions, eliminating the
	// duplicate. These are detail-level entry points, not part of the user API.
	[[nodiscard]] std::shared_ptr<detail::ControlBlockInterface<T>> consume_for_join() noexcept {
		return consume(join_state::joined);
	}
	[[nodiscard]] std::shared_ptr<detail::ControlBlockInterface<T>> consume_for_abandon() noexcept {
		return consume(join_state::detached);
	}
	[[nodiscard]] typename control_handle_for<Category>::type control() const noexcept {
		return typename control_handle_for<Category>::type{state_};
	}
	void cancel() noexcept {
		if (state_js_ != join_state::empty) {
			auto _ = control().request_cancel();
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
		void unhandled_exception() noexcept { auto _ = this->state_->try_set_exception(std::current_exception()); }
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
	void cancel() noexcept { inner_.cancel(); }
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
	if (task.state_) {
		task.state_->set_spawn_location(loc);
	}
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
//   Cons: Two types, requires understanding the join() → Result
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
	// HACK: see matching note in BasicResult::consume_for_join.
	[[nodiscard]] std::shared_ptr<detail::ControlBlockInterface<T>> consume_for_join() noexcept { return consume(); }
	[[nodiscard]] std::shared_ptr<detail::ControlBlockInterface<T>> consume_for_abandon() noexcept { return consume(); }
	[[nodiscard]] auto outcome() && noexcept -> detail::OutcomeAwaiter<T> {
		return detail::OutcomeAwaiter<T>{consume()};
	}
};
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
	auto state = r.consume_for_abandon();
	if (!state) {
		return; // empty or already-detached/joined — no-op
	}
	state->install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(std::forward<Sink>(sink)));
}
template<work_value T, ControlCategory Category, class Sink>
void abandon_impl(
	BasicJoinHandle<T, Category> &&h,
	Sink &&sink) noexcept {
	auto state = h.consume_for_abandon();
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
	auto state = h.consume();
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
	auto state = h.consume();
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
	auto state = h.consume();
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
template<work_value T, progress_capability Owner>
[[nodiscard]] std::pair<Posted<T>, PostedSource<T>> make_posted_source(
	Owner &owner,
	PostOptions opts = {},
	std::source_location loc = std::source_location::current()) {
	std::shared_ptr<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = detail::make_control_block_shared<T, true>();
	} else {
		state = detail::make_control_block_shared<T, false>();
	}
	state->set_required_capability(capability_id(owner));
	return {Posted<T>::from_state(state, loc), PostedSource<T>::from_state(std::move(state))};
}
template<work_value T, progress_capability Driver>
[[nodiscard]] std::pair<Operation<T>, OperationSource<T>> make_operation_source(
	Driver &driver,
	OperationOptions opts = {},
	std::source_location loc = std::source_location::current()) {
	std::shared_ptr<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = detail::make_control_block_shared<T, true>();
	} else {
		state = detail::make_control_block_shared<T, false>();
	}
	state->set_required_capability(capability_id(driver));
	return {Operation<T>::from_state(state, loc), OperationSource<T>::from_state(std::move(state))};
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
	auto state = joinable.consume_for_join();
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

template<work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Task<T> &&task,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(task.consume_for_join(), std::nullopt, loc);
}
template<progress_capability Owner, work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Owner &owner,
	Posted<T> &&posted,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(
		posted.consume_for_join(),
		std::optional<CapabilityId>{capability_id(owner)},
		loc);
}
template<progress_capability Driver, work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Driver &driver,
	Operation<T> &&op,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(
		op.consume_for_join(),
		std::optional<CapabilityId>{capability_id(driver)},
		loc);
}
template<work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	TaskJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(h.consume_for_join(), std::nullopt, loc);
}
template<progress_capability Owner, work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Owner &owner,
	PostedJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(
		h.consume_for_join(),
		std::optional<CapabilityId>{capability_id(owner)},
		loc);
}
template<progress_capability Driver, work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Driver &driver,
	OperationJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(
		h.consume_for_join(),
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
