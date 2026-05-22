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
import conflux.small_function;
import conflux.work;
import conflux.work.root;
import conflux.work.carrier;
import conflux.net.io_buffer;
// ---------------------------------------------------------------------------
// conflux.work — outer module symbols
// ---------------------------------------------------------------------------

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
// make_task_source and blocking_join accessible at conflux::work level.
void _check_facade_make_and_join() {
	auto [task, source] = conflux::work::make_task_source<int>();
	(void)source.try_set_value(conflux::work::root::Success<int>{42});
	[[maybe_unused]] auto outcome = conflux::work::blocking_join(std::move(task));
}

} // namespace snapshot_work_facade

namespace snapshot_work_pool_api {

using _WorkPool = ::WorkPool;
using _WorkPoolOptions = ::WorkPoolOptions;
using _WorkPoolQueueMode = ::WorkPoolQueueMode;
using _WorkPoolQueueStats = ::WorkPoolQueueStats;
using _RingLane = ::RingLane;
using _RingLaneOptions = ::RingLaneOptions;

static_assert(std::same_as<decltype(std::declval<::WorkPoolOptions>().queue_mode), ::WorkPoolQueueMode>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolOptions>().inject_queue_shards), std::size_t>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolOptions>().initial_job_slab_slots), std::size_t>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolOptions>().max_job_slab_slots), std::size_t>);
static_assert(::WorkPoolQueueMode::stealing != ::WorkPoolQueueMode::no_stealing);
static_assert(std::same_as<decltype(std::declval<::WorkPool &>().queue_stats()), ::WorkPoolQueueStats>);
static_assert(std::same_as<decltype(std::declval<::WorkPool &>().reset_queue_stats()), void>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolQueueStats>().enqueue_attempts), std::uint64_t>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolQueueStats>().admission_lock_contentions), std::uint64_t>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolQueueStats>().local_lock_contentions), std::uint64_t>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolQueueStats>().steal_lock_contentions), std::uint64_t>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolQueueStats>().futex_waits), std::uint64_t>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolQueueStats>().job_slot_allocations), std::uint64_t>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolQueueStats>().job_slab_allocations), std::uint64_t>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolQueueStats>().queue_full_token_discards), std::uint64_t>);
static_assert(std::same_as<decltype(std::declval<::WorkPoolQueueStats>().remote_free_pushes), std::uint64_t>);

} // namespace snapshot_work_pool_api
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

// E2a: SBO move-only callable (exported from conflux::detail for internal reuse)
using _small_move_only_fn_void = conflux::detail::small_move_only_function<void()>;
using _small_move_only_fn_int = conflux::detail::small_move_only_function<int(int), 64>;

// E4: Source setter API (try_set_value / try_set_exception / try_set_cancelled / try_set_error)
static_assert(
	std::is_same_v<decltype(std::declval<root::TaskSource<int>>().try_set_value(root::Success<int>{})), bool>);
static_assert(
	std::is_same_v<decltype(std::declval<root::TaskSource<int>>().try_set_exception(std::exception_ptr{})), bool>);
static_assert(std::is_same_v<
			  decltype(std::declval<root::TaskSource<int>>().try_set_cancelled(root::work_errc::cancelled_requested)),
			  bool>);
static_assert(std::is_same_v<decltype(std::declval<root::TaskSource<int>>().try_set_error(std::error_code{})), bool>);
static_assert(std::is_same_v<
			  decltype(std::declval<root::TaskSource<int>>().try_set_error(std::error_code{}, std::string_view{})),
			  bool>);

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

template<>
inline constexpr bool enable_address_capability_v<snapshot_root::_TestCap> = true;

} // namespace conflux::work::root
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
static_assert(root::JoinError::reason::consumed_handle == root::JoinError::reason::consumed_handle);
static_assert(root::JoinError::reason::capability_mismatch == root::JoinError::reason::capability_mismatch);
static_assert(root::JoinError::reason::thread_precondition == root::JoinError::reason::thread_precondition);
static_assert(root::JoinError::reason::reentrant_pump == root::JoinError::reason::reentrant_pump);
static_assert(root::JoinError::reason::hop_capability_mismatch == root::JoinError::reason::hop_capability_mismatch);
static_assert(
	root::JoinError::reason::ready_callback_already_installed
	== root::JoinError::reason::ready_callback_already_installed);
static_assert(root::JoinError::reason::lifetime_violation == root::JoinError::reason::lifetime_violation);
// Accessors
static_assert(std::same_as<decltype(std::declval<root::JoinError const &>().reason_code()), root::JoinError::reason>);
static_assert(
	std::same_as<decltype(std::declval<root::JoinError const &>().expected()), std::optional<root::CapabilityId>>);
static_assert(
	std::same_as<decltype(std::declval<root::JoinError const &>().actual()), std::optional<root::CapabilityId>>);
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
// blocking_join(Task<T>&&) — unevaluated decltype; join remains a compatibility alias.
using _blocking_join_result_int = decltype(root::blocking_join(std::declval<root::Task<int> &&>()));
using _join_result_int = decltype(root::blocking_join(std::declval<root::Task<int> &&>()));
using _try_join_ready_result_int = decltype(root::try_join_ready(std::declval<root::Task<int> &&>()));
using _join_ready_result_int = decltype(root::join_ready(std::declval<root::Task<int> &&>()));

// value(Outcome<T>&&) from root
using _root_value_result = decltype(root::value(std::declval<root::Outcome<int> &&>()));

// AbandonStatus values exist and remain distinct enough for snapshot consumers.
static_assert(root::AbandonStatus::installed != root::AbandonStatus::already_abandoned);

} // namespace snapshot_root
// ---------------------------------------------------------------------------
// conflux.work.carrier.model_a
// ---------------------------------------------------------------------------

namespace snapshot_model_a {
namespace carrier = conflux::work::carrier;
namespace root = conflux::work::root;

template<class T>
using _Chain_ = carrier::Chain<T>;
using _CarrierKind = carrier::CarrierKind;
using _HopCapErr = carrier::HopCapabilityError;
using _AggErr = carrier::AggregateError;
// from_task, map, then, into_ready_task — name-check
void _check_pipeline() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = carrier::from_task(std::move(task));
	auto mapped = carrier::map(std::move(chain), [](int x) { return x + 1; });
	auto result = carrier::into_ready_task(std::move(mapped));
	(void)result;
}
// hop_to_task, unbind
using _hop_to_task_fn = decltype(&carrier::hop_to_task<int>);
using _unbind_fn = decltype(&carrier::unbind<int>);
// E1.z — combinator member functions

struct _DummyCap {};

} // namespace snapshot_model_a
namespace conflux::work::root {

template<>
inline constexpr bool enable_address_capability_v<snapshot_model_a::_DummyCap> = true;

} // namespace conflux::work::root
namespace snapshot_model_a {

void _e1z_then_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = carrier::from_task(std::move(task));
	auto chained = std::move(chain).then([](int x) { return x + 1; });
	static_assert(std::same_as<decltype(chained), carrier::Chain<int>>);
	(void)chained;
}
void _e1z_catch_error_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = carrier::from_task(std::move(task));
	auto recovered = std::move(chain).catch_error([](std::exception_ptr) { return 0; });
	static_assert(std::same_as<decltype(recovered), carrier::Chain<int>>);
	(void)recovered;
}
void _e1z_on_cancel_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = carrier::from_task(std::move(task));
	auto result = std::move(chain).on_cancel([] {});
	static_assert(std::same_as<decltype(result), carrier::Chain<int>>);
	(void)result;
}
void _e1z_recover_cancel_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = carrier::from_task(std::move(task));
	auto result = std::move(chain).recover_cancel([] { return 0; });
	static_assert(std::same_as<decltype(result), carrier::Chain<int>>);
	(void)result;
}
void _e1z_recover_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = carrier::from_task(std::move(task));
	auto result = std::move(chain).recover([](root::Outcome<int>) { return 0; });
	static_assert(std::same_as<decltype(result), carrier::Chain<int>>);
	(void)result;
}
void _e1z_transform_outcome_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = carrier::from_task(std::move(task));
	auto result =
		std::move(chain).transform_outcome([](root::Outcome<int> out) { return root::Outcome<int>{std::move(out)}; });
	static_assert(std::same_as<decltype(result), carrier::Chain<int>>);
	(void)result;
}
void _e1z_schedule_on_check_() {
	_DummyCap cap{};
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = carrier::from_task(std::move(task));
	auto result = std::move(chain).schedule_on(cap);
	static_assert(std::same_as<decltype(result), carrier::Chain<int>>);
	(void)result;
}
void _e1z_then_on_check_() {
	_DummyCap cap{};
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = carrier::from_task(std::move(task));
	auto result = std::move(chain).then_on(cap, [](int x) { return x + 1; });
	static_assert(std::same_as<decltype(result), carrier::Chain<int>>);
	(void)result;
}
void _e1z_into_task_check_() {
	auto [task, src] = root::make_task_source<int>();
	(void)src.try_set_value(root::Success<int>{1});
	auto chain = carrier::from_task(std::move(task));
	auto t = std::move(chain).into_task();
	static_assert(std::same_as<decltype(t), root::Task<int>>);
	std::move(t).detach();
}

} // namespace snapshot_model_a
// ---------------------------------------------------------------------------
// conflux.net.io_buffer (E5 — moved from conflux.work)
// ---------------------------------------------------------------------------

namespace snapshot_net_io_buffer {

using _IoBuffer = ::IoBuffer;
static_assert(std::is_constructible_v<::IoBuffer, std::shared_ptr<std::byte const[]>, std::size_t>);
using _BufferList = ::BufferList;
using _IoPlan = ::IoPlan;

} // namespace snapshot_net_io_buffer
