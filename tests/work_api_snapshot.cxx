// Compile-only API snapshot for conflux.work + conflux.work.root.
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

// ---------------------------------------------------------------------------
// conflux.work — outer module symbols
// ---------------------------------------------------------------------------

namespace snapshot_work_outer {

// Legacy outer Task<T> (Flow-backed) — distinct from root::Task<T>
using _Task_outer = ::Task<int>;
using _FlowSource_int = ::FlowSource<int>;
using _TaskPromise_int = ::TaskPromise<int>;

// Function erasure (internal SBO implementation — exported for internal use)
using _UniqueFn_void = work_detail::UniqueFn<void()>;
using _UniqueFn_int = work_detail::UniqueFn<int(int)>;

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

// BufferView alias
using _BufferView_byte = ::BufferView<std::byte>;

// Concrete types
using _WorkPoolOptions = ::WorkPoolOptions;
using _RingLaneOptions = ::RingLaneOptions;
using _WorkError_enum = ::WorkError; // outer enum, distinct from root::WorkError class
using _IoBuffer = ::IoBuffer;
using _BufferList = ::BufferList;
using _IoPlan = ::IoPlan;
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
	(void)source.commit_success(conflux::work::root::Success<int>{42});
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
using _JoinContextReason = root::JoinContextReason;
using _ControlCategory = root::ControlCategory;
using _AbandonStatus = root::AbandonStatus;
using _ReadyRegistration = root::ReadyRegistration;
using _ClearOnReadyStatus = root::ClearOnReadyStatus;

// detail enums (inside root::detail)
using _TerminalState = root::detail::TerminalState;
using _ReadyHookState = root::detail::ReadyHookState;

// Exception / error types
using _WorkError_class = root::WorkError; // class (distinct from outer WorkError enum)
using _JoinContextError = root::JoinContextError;
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

// capability_id_from_address — verify via concrete derived type
struct _TestCap : root::capability_id_from_address<_TestCap> {};

// progress_capability — satisfied by custom capability_id_from_address types
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

// Result type aliases (the canonical Task/Posted/Operation)
template<class T>
using _TaskRoot_ = root::Task<T>;
template<class T>
using _Posted_ = root::Posted<T>;
template<class T>
using _Operation_ = root::Operation<T>;

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
	(void)src.commit_success(root::Success<int>{1});
	auto chain = model_a::from_task(std::move(task));
	auto mapped = model_a::map(std::move(chain), [](int x) { return x + 1; });
	auto result = model_a::into_ready_task(std::move(mapped));
	(void)result;
}

// hop_to_task, unbind
using _hop_to_task_fn = decltype(&model_a::hop_to_task<int>);
using _unbind_fn = decltype(&model_a::unbind<int>);

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
	(void)src.commit_success(root::Success<int>{0});
	[[maybe_unused]] auto chain = model_b::from_task(std::move(task));
}

} // namespace snapshot_model_b
