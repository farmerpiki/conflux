// Compile-only API snapshot for conflux.work, conflux.work.root, and
// conflux.net.io_buffer.
//
// Every public name exported by these modules must be referenced here.
// Symmetric rule: any addition or deletion in E1–E5 also updates this file.
// Build gate: this TU must compile cleanly on both debug-clang-libcxx and
// debug-gcc-stdcxx before any Phase 1 PR merges.
//
// Compile-only — no main(), no output, no runtime.

import std;
import conflux.types;
import conflux.work;
import conflux.work.root;
import conflux.work.carrier.model_a;
import conflux.work.carrier.model_b;
import conflux.net.io_buffer;

// ---------------------------------------------------------------------------
// conflux.work — outer module symbols
// ---------------------------------------------------------------------------

namespace snapshot_work_outer {

// Legacy outer Task<T> (Flow-backed) — distinct from root::Task<T>
using _Task_outer = ::Task<int>;
using _FlowSource_int = ::FlowSource<int>;
using _TaskPromise_int = ::TaskPromise<int>;

// E2a: UniqueFn retired → detail::small_move_only_function; BufferView alias deleted.

// Outer Cancelled (different from root::Cancelled)
using _Cancelled_outer = ::Cancelled;

// ValueTag and step types (legacy pipe API — deleted in E1.4)
using _ValueTag = ::ValueTag;
template<class Fn>
using _ThenStep_ = ::ThenStep<Fn>;
template<class Fn>
using _FlatThenStep_ = ::FlatThenStep<Fn>;
template<class Fn>
using _ErrorStep_ = ::ErrorStep<Fn>;
template<class Fn>
using _CancelStep_ = ::CancelStep<Fn>;
template<class T>
using _MoveToStep_ = ::MoveToStep<T>;
template<class T>
using _StartOnStep_ = ::StartOnStep<T>;

// Concrete types
using _WorkPoolOptions = ::WorkPoolOptions;
using _RingLaneOptions = ::RingLaneOptions;
using _WorkError_enum = ::WorkError; // outer enum, distinct from root::WorkError class
using _WorkPool = ::WorkPool;
using _RingLane = ::RingLane;

// sync_wait (outer helper over root::Task<T>)
void _check_sync_wait(
	conflux::work::root::Task<int> t) {
	[[maybe_unused]] auto v = sync_wait(std::move(t));
}

// co_spawn (outer helper)
void _check_co_spawn_outer(
	::Task<void> t) {
	co_spawn(std::move(t));
}

// join_all (outer — takes root::Task<Ts>...)
using _join_all_result_2 =
	decltype(join_all(std::declval<conflux::work::root::Task<int>>(), std::declval<conflux::work::root::Task<int>>()));

} // namespace snapshot_work_outer

// ---------------------------------------------------------------------------
// conflux::work façade aliases (E1.0)
// These live at conflux::work level; old ::Task<T> still at global namespace.
// ---------------------------------------------------------------------------

namespace snapshot_work_facade {

// Core canonical types at conflux::work level.
template<class T>
using _Task_ = conflux::work::Task<T>;
template<class T>
using _TaskSource_ = conflux::work::TaskSource<T>;
using _TaskControl = conflux::work::TaskControl;
template<class T>
using _Outcome_ = conflux::work::Outcome<T>;
using _CancelReason = conflux::work::CancelReason;

// Task<T> == root::Task<T> (same type, not just alias-compatible).
static_assert(std::is_same_v<conflux::work::Task<int>, conflux::work::root::Task<int>>);
static_assert(std::is_same_v<conflux::work::TaskSource<int>, conflux::work::root::TaskSource<int>>);
static_assert(std::is_same_v<conflux::work::Outcome<int>, conflux::work::root::Outcome<int>>);
static_assert(std::is_same_v<conflux::work::CancelReason, conflux::work::root::CancelReason>);

// make_task_source and join accessible at conflux::work level.
void _check_facade_make_and_join() {
	auto [task, source] = conflux::work::make_task_source<int>();
	(void)source.try_set_value(conflux::work::root::Success<int>{42});
	[[maybe_unused]] auto outcome = conflux::work::join(std::move(task));
}

} // namespace snapshot_work_facade

// ---------------------------------------------------------------------------
// conflux.work.root — canonical types
// ---------------------------------------------------------------------------

namespace snapshot_root {

namespace root = conflux::work::root;

// Enums
using _CancelReason = root::CancelReason;
using _WorkState = root::WorkState;
using _OutcomeKind = root::OutcomeKind;
// JoinContextReason removed in E2b.2 — merged into JoinError::reason
using _ControlCategory = root::ControlCategory;
using _AbandonStatus = root::AbandonStatus;
using _ReadyRegistration = root::ReadyRegistration;
using _ClearOnReadyStatus = root::ClearOnReadyStatus;

// detail enums (inside root::detail)
using _TerminalState = root::detail::TerminalState;
using _ReadyHookState = root::detail::ReadyHookState;

// E2a: SBO move-only callable (exported from root::detail for internal reuse)
using _small_move_only_fn_void = root::detail::small_move_only_function<void()>;
using _small_move_only_fn_int = root::detail::small_move_only_function<int(int), 64>;

// E4: Source setter API (try_set_value / try_set_exception / try_set_cancelled / try_set_error)
static_assert(
	std::is_same_v<decltype(std::declval<root::TaskSource<int>>().try_set_value(root::Success<int>{})), bool>);
static_assert(
	std::is_same_v<decltype(std::declval<root::TaskSource<int>>().try_set_exception(std::exception_ptr{})), bool>);
static_assert(std::is_same_v<
			  decltype(std::declval<root::TaskSource<int>>().try_set_cancelled(root::CancelReason::requested)),
			  bool>);
static_assert(std::is_same_v<decltype(std::declval<root::TaskSource<int>>().try_set_error(std::error_code{})), bool>);

// E4: concept work_handle — satisfied by Task, Posted, Operation, *JoinHandle
static_assert(root::work_handle<root::Task<int>>);
static_assert(root::work_handle<root::Posted<int>>);
static_assert(root::work_handle<root::Operation<int>>);
static_assert(root::work_handle<root::TaskJoinHandle<int>>);
static_assert(root::work_handle<root::PostedJoinHandle<int>>);
static_assert(root::work_handle<root::OperationJoinHandle<int>>);

// E2b.1: work_errc error code domain
using _work_errc = root::work_errc;
static_assert(root::work_errc::cancelled_requested == root::work_errc{1});
static_assert(std::is_same_v<decltype(root::work_category()), std::error_category const &>);
static_assert(std::is_same_v<decltype(root::make_error_code(root::work_errc{})), std::error_code>);
static_assert(std::is_same_v<decltype(root::cancel_reason_errc(root::CancelReason::requested)), root::work_errc>);

// Exception / error types
using _WorkError_class = root::WorkError; // class (distinct from outer WorkError enum)
// JoinContextError removed in E2b.2 — replaced by JoinError
using _FailureError = root::FailureError;
using _CancelledError = root::CancelledError;

// Outcome component types
using _Failure = root::Failure;
using _CancelledRoot = root::Cancelled; // root::Cancelled struct
template<class T>
using _Success_ = root::Success<T>;
template<class T>
using _Outcome_ = root::Outcome<T>;

// Concepts
static_assert(root::work_value<int>);
static_assert(root::work_value<void>);
static_assert(!root::work_value<int &>);

// Capability infrastructure
using _CapabilityId = root::CapabilityId;
using _capability_id_t_ = root::capability_id_t;
[[maybe_unused]] auto _cap_id_cst = root::capability_id;

// E3: enable_address_capability_v<T> opt-in trait
static_assert(!root::enable_address_capability_v<int>);
struct _TestCap {};
} // namespace snapshot_root
namespace conflux::work::root {
template<> inline constexpr bool enable_address_capability_v<snapshot_root::_TestCap> = true;
}
namespace snapshot_root {
static_assert(root::enable_address_capability_v<_TestCap>);

// progress_capability — satisfied by enable_address_capability_v<T> opt-in
static_assert(root::progress_capability<_TestCap>);

// Source type aliases
template<class T>
using _TaskSource_ = root::TaskSource<T>;
template<class T>
using _PostedSource_ = root::PostedSource<T>;
template<class T>
using _OperationSource_ = root::OperationSource<T>;

// Options
using _SubmitOptions = root::SubmitOptions;
using _PostOptions = root::PostOptions;
using _OperationOptions = root::OperationOptions;

// E1.x — join_state enum
static_assert(root::join_state::empty != root::join_state::joinable);
static_assert(root::join_state::joined != root::join_state::detached);

// Result type aliases (the canonical Task/Posted/Operation)
template<class T>
using _TaskRoot_ = root::Task<T>;
template<class T>
using _Posted_ = root::Posted<T>;
template<class T>
using _Operation_ = root::Operation<T>;

// E1.x — JoinTask<T> + require_join + spawn + spawn_strict
template<class T>
using _JoinTask_ = root::JoinTask<T>;
static_assert(std::same_as<root::JoinTask<int>::value_type, int>);
static_assert(std::same_as<root::Task<int>::value_type, int>);

void _e1x_api_check_() {
	auto [task, src] = root::make_task_source<int>();
	// state() returns join_state
	static_assert(std::same_as<decltype(task.state()), root::join_state>);
	// cancel() is callable
	task.cancel();
	// control() works on empty after cancel
	[[maybe_unused]] auto ctl = task.control();
	// detach() rvalue-ref overload
	std::move(task).detach();
	// require_join
	auto [task2, src2] = root::make_task_source<int>();
	auto jt = root::require_join(std::move(task2));
	static_assert(std::same_as<decltype(jt), root::JoinTask<int>>);
	// detach_to_task downgrades JoinTask → Task
	auto t2 = std::move(jt).detach_to_task();
	static_assert(std::same_as<decltype(t2), root::Task<int>>);
	std::move(t2).detach();
	(void)src;
	(void)src2;
}

// E1.x — set_dropped_outcome_sink
void _e1x_sink_check_() {
	root::set_dropped_outcome_sink([](std::source_location, root::OutcomeKind, std::exception_ptr) {});
}

// ---------------------------------------------------------------------------
// E1.y — value-category, co_await, .outcome(), .consume(), JoinError
// ---------------------------------------------------------------------------

// JoinError — E2b.2: full reason enum + capability fields + source_location
using _JoinError = root::JoinError;
static_assert(std::same_as<root::JoinError::reason, root::JoinError::reason>);
// Reason values
static_assert(root::JoinError::reason::consumed_handle                  == root::JoinError::reason::consumed_handle);
static_assert(root::JoinError::reason::capability_mismatch              == root::JoinError::reason::capability_mismatch);
static_assert(root::JoinError::reason::thread_precondition              == root::JoinError::reason::thread_precondition);
static_assert(root::JoinError::reason::reentrant_pump                   == root::JoinError::reason::reentrant_pump);
static_assert(root::JoinError::reason::hop_capability_mismatch          == root::JoinError::reason::hop_capability_mismatch);
static_assert(root::JoinError::reason::ready_callback_already_installed == root::JoinError::reason::ready_callback_already_installed);
static_assert(root::JoinError::reason::lifetime_violation               == root::JoinError::reason::lifetime_violation);
// Accessors
static_assert(std::same_as<decltype(std::declval<root::JoinError const &>().reason_code()), root::JoinError::reason>);
static_assert(std::same_as<decltype(std::declval<root::JoinError const &>().expected()), std::optional<root::CapabilityId>>);
static_assert(std::same_as<decltype(std::declval<root::JoinError const &>().actual()), std::optional<root::CapabilityId>>);
static_assert(std::same_as<decltype(std::declval<root::JoinError const &>().origin()), std::source_location>);

// consume() lvalue/rvalue overloads
void _e1y_consume_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src;
	// lvalue consume() returns rvalue-ref
	static_assert(std::same_as<decltype(task.consume()), root::Task<int> &&>);
	// rvalue consume() also returns rvalue-ref
	static_assert(std::same_as<decltype(std::move(task).consume()), root::Task<int> &&>);
}

// operator co_await() & = delete — hard contract; deleted overload causes a hard
// error, not a SFINAE failure, so we can't static_assert it here.

// operator co_await() && and outcome() && exist and return awaitables
void _e1y_awaiter_type_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src;
	// co_await rvalue returns something (type-erase to silence nodiscard)
	[[maybe_unused]] auto aw = std::move(task).operator co_await();
	static_assert(requires { aw.await_ready(); });
	static_assert(requires { aw.await_resume(); });
}

void _e1y_outcome_awaiter_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src;
	[[maybe_unused]] auto aw = std::move(task).outcome();
	static_assert(requires { aw.await_ready(); });
	using out_t = decltype(aw.await_resume());
	static_assert(std::same_as<out_t, root::Outcome<int>>);
}

// JoinHandle type aliases
template<class T>
using _TaskJoinHandle_ = root::TaskJoinHandle<T>;
template<class T>
using _PostedJoinHandle_ = root::PostedJoinHandle<T>;
template<class T>
using _OperationJoinHandle_ = root::OperationJoinHandle<T>;

// abandon_sink concept and drop_on_abandon
using _DropOnAbandon = root::drop_on_abandon;

// Control types
using _TaskControl_ = root::detail::BasicControl<root::ControlCategory::task>;
using _PostedControl_ = root::detail::BasicControl<root::ControlCategory::posted>;
using _OperationControl_ = root::detail::BasicControl<root::ControlCategory::operation>;

// Diagnostic sink
using _CarrierDiagnosticSink_ = root::CarrierDiagnosticSink;

// scoped_abandon helper
template<class R>
using _scoped_abandon_ = root::scoped_abandon<R, root::drop_on_abandon>;

// guard_abandon — name-check
void _check_guard_abandon(
	root::Task<int> &&t) {
	auto g = root::guard_abandon(std::move(t));
	(void)g;
}

// into_join_handle — name-check
void _check_into_join_handle(
	root::Task<int> &&t) {
	[[maybe_unused]] auto jh = root::into_join_handle(std::move(t));
}

// make_task_source
void _check_make_task_source() {
	auto [task, source] = root::make_task_source<int>();
	(void)task;
	(void)source;
}

// make_task_control_source
void _check_make_task_control_source() {
	auto [ctl, source] = root::make_task_control_source<int>();
	(void)ctl;
	(void)source;
}

// make_posted_source
void _check_make_posted_source() {
	_TestCap owner;
	auto [posted, source] = root::make_posted_source<int>(owner);
	(void)posted;
	(void)source;
}

// join(Task<T>&&) — unevaluated decltype
using _join_result_int = decltype(root::join(std::declval<root::Task<int> &&>()));

// value(Outcome<T>&&) from root
using _root_value_result = decltype(root::value(std::declval<root::Outcome<int> &&>()));

// AbandonStatus values exist
static_assert(
	static_cast<int>(root::AbandonStatus::installed) >= 0
	|| static_cast<int>(root::AbandonStatus::already_abandoned) >= 0);

} // namespace snapshot_root

// ---------------------------------------------------------------------------
// conflux.work.carrier.model_a
// ---------------------------------------------------------------------------

namespace snapshot_model_a {

namespace model_a = conflux::work::carrier::model_a;
namespace root = conflux::work::root;

template<class T>
using _Chain_ = model_a::Chain<T>;
using _CarrierKind = model_a::CarrierKind;
using _HopCapErr = model_a::HopCapabilityError;
using _AggErr = model_a::AggregateError;

// from_task, map, then, into_ready_task — name-check
void _check_pipeline() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto mapped = model_a::map(std::move(chain), [](int x) { return x + 1; });
	auto result = model_a::into_ready_task(std::move(mapped));
	(void)result;
}

// hop_to_task, unbind
using _hop_to_task_fn = decltype(&model_a::hop_to_task<int>);
using _unbind_fn = decltype(&model_a::unbind<int>);

// E1.z — combinator member functions

struct _DummyCap {};
} // namespace snapshot_model_a
namespace conflux::work::root {
template<> inline constexpr bool enable_address_capability_v<snapshot_model_a::_DummyCap> = true;
}
namespace snapshot_model_a {

void _e1z_then_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto chained = std::move(chain).then([](int x) { return x + 1; });
	static_assert(std::same_as<decltype(chained), model_a::Chain<int>>);
	(void)chained;
}

void _e1z_catch_error_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto recovered = std::move(chain).catch_error([](std::exception_ptr) { return 0; });
	static_assert(std::same_as<decltype(recovered), model_a::Chain<int>>);
	(void)recovered;
}

void _e1z_on_cancel_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto result = std::move(chain).on_cancel([] {});
	static_assert(std::same_as<decltype(result), model_a::Chain<int>>);
	(void)result;
}

void _e1z_recover_cancel_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto result = std::move(chain).recover_cancel([] { return 0; });
	static_assert(std::same_as<decltype(result), model_a::Chain<int>>);
	(void)result;
}

void _e1z_recover_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto result = std::move(chain).recover([](root::Outcome<int>) { return 0; });
	static_assert(std::same_as<decltype(result), model_a::Chain<int>>);
	(void)result;
}

void _e1z_transform_outcome_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto result =
		std::move(chain).transform_outcome([](root::Outcome<int> out) { return root::Outcome<int>{std::move(out)}; });
	static_assert(std::same_as<decltype(result), model_a::Chain<int>>);
	(void)result;
}

void _e1z_schedule_on_check_() {
	_DummyCap cap{};
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto result = std::move(chain).schedule_on(cap);
	static_assert(std::same_as<decltype(result), model_a::Chain<int>>);
	(void)result;
}

void _e1z_then_on_check_() {
	_DummyCap cap{};
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto result = std::move(chain).then_on(cap, [](int x) { return x + 1; });
	static_assert(std::same_as<decltype(result), model_a::Chain<int>>);
	(void)result;
}

void _e1z_into_task_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto t = std::move(chain).into_task();
	static_assert(std::same_as<decltype(t), root::Task<int>>);
	std::move(t).detach();
}

void _e1z_into_task_unchecked_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto t = std::move(chain).into_task_unchecked();
	static_assert(std::same_as<decltype(t), root::Task<int>>);
	std::move(t).detach();
}

} // namespace snapshot_model_a

// ---------------------------------------------------------------------------
// conflux.work.carrier.model_b (legacy — deprecated in E1.1, deleted in E1.4)
// ---------------------------------------------------------------------------

namespace snapshot_model_b {

namespace model_b = conflux::work::carrier::model_b;
namespace root = conflux::work::root;

template<class T>
using _TaskChain_ = model_b::TaskChain<T>;
template<class T>
using _PostedChain_ = model_b::PostedChain<T>;
template<class T>
using _OperationChain_ = model_b::OperationChain<T>;

void _check_from_task() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{0});
	[[maybe_unused]] auto chain = model_b::from_task(std::move(task));
}

} // namespace snapshot_model_b

// ---------------------------------------------------------------------------
// conflux.net.io_buffer (E5 — moved from conflux.work)
// ---------------------------------------------------------------------------

namespace snapshot_net_io_buffer {

using _IoBuffer = ::IoBuffer;
static_assert(std::is_constructible_v<::IoBuffer, std::shared_ptr<std::byte const[]>, std::size_t>);
using _BufferList = ::BufferList;
using _IoPlan = ::IoPlan;

} // namespace snapshot_net_io_buffer
