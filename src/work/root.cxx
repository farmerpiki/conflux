module;
#include <memory>

#ifndef CONFLUX_WORK_CORO_FRAME_POOL
#define CONFLUX_WORK_CORO_FRAME_POOL 0
#endif

#if CONFLUX_WORK_CORO_FRAME_POOL
	#if defined(__has_feature)
		#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
			#define CONFLUX_WORK_TASK_FRAME_POOL_ACTIVE 0
		#else
			#define CONFLUX_WORK_TASK_FRAME_POOL_ACTIVE 1
		#endif
	#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
		#define CONFLUX_WORK_TASK_FRAME_POOL_ACTIVE 0
	#else
		#define CONFLUX_WORK_TASK_FRAME_POOL_ACTIVE 1
	#endif
#else
	#define CONFLUX_WORK_TASK_FRAME_POOL_ACTIVE 0
#endif

#if CONFLUX_WORK_TASK_FRAME_POOL_ACTIVE
	#include <sys/mman.h>
#endif

export module conflux.work.root;

import std;
import conflux.types;

#ifndef CONFLUX_WORK_ALLOC_STATS
#define CONFLUX_WORK_ALLOC_STATS 0
#endif

export namespace conflux::work::root {

enum class CancelReason : std::uint8_t {
	requested,
	abandoned,
	shutdown,
	deadline,
};

enum class WorkState : std::uint8_t {
	pending,
	cancel_requested,
	ready_success,
	ready_failure,
	ready_cancelled,
};

enum class OutcomeKind : std::uint8_t {
	success,
	failure,
	cancelled,
};

enum class join_state : std::uint8_t {
	empty, // moved-from / default-constructed
	joinable, // owns control block, not yet awaited or detached
	joined, // co_await consumed it (E1.y)
	detached, // detach()/abandon_to()/dtor-auto-detach
};
class WorkError : public std::runtime_error {
public:
	explicit WorkError(std::string const &msg) : std::runtime_error{msg} {}
};
struct TaskAllocationStats {
	std::uint64_t control_block_allocations = 0;
	std::uint64_t control_block_deallocations = 0;
	std::uint64_t coroutine_frame_allocations = 0;
	std::uint64_t coroutine_frame_deallocations = 0;
};
namespace detail {

#if CONFLUX_WORK_ALLOC_STATS
inline std::atomic<std::uint64_t> g_control_block_allocations{0};
inline std::atomic<std::uint64_t> g_control_block_deallocations{0};
inline std::atomic<std::uint64_t> g_coroutine_frame_allocations{0};
inline std::atomic<std::uint64_t> g_coroutine_frame_deallocations{0};
inline void note_control_block_allocation() noexcept {
	g_control_block_allocations.fetch_add(1, memory_order_relaxed);
}
inline void note_control_block_deallocation() noexcept {
	g_control_block_deallocations.fetch_add(1, memory_order_relaxed);
}
inline void note_coroutine_frame_allocation() noexcept {
	g_coroutine_frame_allocations.fetch_add(1, memory_order_relaxed);
}
inline void note_coroutine_frame_deallocation() noexcept {
	g_coroutine_frame_deallocations.fetch_add(1, memory_order_relaxed);
}
[[nodiscard]] inline TaskAllocationStats task_allocation_stats_impl() noexcept {
	return {
		.control_block_allocations = g_control_block_allocations.load(memory_order_relaxed),
		.control_block_deallocations = g_control_block_deallocations.load(memory_order_relaxed),
		.coroutine_frame_allocations = g_coroutine_frame_allocations.load(memory_order_relaxed),
		.coroutine_frame_deallocations = g_coroutine_frame_deallocations.load(memory_order_relaxed),
	};
}
inline void reset_task_allocation_stats_impl() noexcept {
	g_control_block_allocations.store(0, memory_order_relaxed);
	g_control_block_deallocations.store(0, memory_order_relaxed);
	g_coroutine_frame_allocations.store(0, memory_order_relaxed);
	g_coroutine_frame_deallocations.store(0, memory_order_relaxed);
}
#else
inline void note_control_block_allocation() noexcept {}
inline void note_control_block_deallocation() noexcept {}
inline void note_coroutine_frame_allocation() noexcept {}
inline void note_coroutine_frame_deallocation() noexcept {}
[[nodiscard]] inline TaskAllocationStats task_allocation_stats_impl() noexcept { return {}; }
inline void reset_task_allocation_stats_impl() noexcept {}
#endif

[[nodiscard]] inline std::exception_ptr normalize_failure_ptr(
	std::exception_ptr const &ep) {
	if (ep) {
		return ep;
	}
	return std::make_exception_ptr(std::runtime_error{"conflux.work.root: normalized null EP"});
}

} // namespace detail
[[nodiscard]] inline TaskAllocationStats task_allocation_stats() noexcept {
	return detail::task_allocation_stats_impl();
}
inline void reset_task_allocation_stats() noexcept {
	detail::reset_task_allocation_stats_impl();
}
struct Failure {
	std::exception_ptr error{};

	Failure() = delete;
	explicit Failure(
		std::exception_ptr const &ep)
		: error{detail::normalize_failure_ptr(ep)} {}
};
struct Cancelled {
	CancelReason reason = CancelReason::requested;
};
template<typename T>
struct Success {
	T value;
};
template<>
struct Success<void> {};
class FailureError final : public WorkError {
	std::exception_ptr cause_{};

public:
	explicit FailureError(
		std::exception_ptr const &cause)
		: WorkError{"work failed"}
		, cause_{detail::normalize_failure_ptr(cause)} {}
	[[nodiscard]] std::exception_ptr cause() const noexcept { return cause_; }
	[[noreturn]] void rethrow_cause() const { std::rethrow_exception(cause_); }
};
class CancelledError final : public WorkError {
	CancelReason reason_ = CancelReason::requested;

public:
	explicit CancelledError(
		CancelReason reason)
		: WorkError{"work cancelled"}
		, reason_{reason} {}
	[[nodiscard]] CancelReason reason() const noexcept { return reason_; }
};

enum class work_errc : int { // NOLINT(performance-enum-size): int required for std::error_code
	cancelled_requested = 1,
	cancelled_abandoned,
	cancelled_shutdown,
	cancelled_deadline,
	not_live,
	capability_mismatch,
	already_fulfilled,
	already_consumed,
};

[[nodiscard]] std::error_category const &work_category() noexcept;
[[nodiscard]] inline std::error_code make_error_code(
	work_errc e) noexcept {
	return {static_cast<int>(e), work_category()};
}
[[nodiscard]] inline work_errc cancel_reason_errc(
	CancelReason r) noexcept {
	using enum CancelReason;
	switch (r) {
	case requested: return work_errc::cancelled_requested;
	case abandoned: return work_errc::cancelled_abandoned;
	case shutdown : return work_errc::cancelled_shutdown;
	case deadline : return work_errc::cancelled_deadline;
	}
	return work_errc::cancelled_requested;
}
[[nodiscard]] inline CancelReason errc_cancel_reason(
	work_errc e) noexcept {
	using enum work_errc;
	switch (e) {
	case cancelled_requested: return CancelReason::requested;
	case cancelled_abandoned: return CancelReason::abandoned;
	case cancelled_shutdown : return CancelReason::shutdown;
	case cancelled_deadline : return CancelReason::deadline;
	default                 : return CancelReason::requested;
	}
}
std::error_category const &work_category() noexcept {
	struct impl final : std::error_category {
		[[nodiscard]] char const *name() const noexcept override { return "conflux.work"; }
		[[nodiscard]] std::string message(
			int ev) const override {
			using enum work_errc;
			switch (static_cast<work_errc>(ev)) {
			case cancelled_requested: return "work cancelled (requested)";
			case cancelled_abandoned: return "work cancelled (abandoned)";
			case cancelled_shutdown : return "work cancelled (shutdown)";
			case cancelled_deadline : return "work cancelled (deadline)";
			case not_live           : return "handle not live";
			case capability_mismatch: return "capability mismatch";
			case already_fulfilled  : return "state already fulfilled";
			case already_consumed   : return "state already consumed";
			}
			return "unknown work error";
		}
	};
	static impl const inst{};
	return inst;
}
template<typename T>
concept work_value =
	same_as<std::remove_cv_t<T>, void> || (!std::is_reference_v<T> && std::is_nothrow_move_constructible_v<T>);
template<typename T>
class Outcome final {
	using success_t = Success<T>;
	using storage_t = std::variant<success_t, Failure, Cancelled>;
	storage_t storage_;
	void assert_formed() const noexcept {
		if (storage_.valueless_by_exception()) {
			std::terminate();
		}
	}

public:
	Outcome() = delete;
	Outcome(
		success_t success)
		noexcept(
			std::is_nothrow_move_constructible_v<success_t>)
		: storage_{std::in_place_type<success_t>, move(success)} {}
	Outcome(
		Failure failure) noexcept
		: storage_{std::in_place_type<Failure>, move(failure)} {}
	Outcome(
		Cancelled cancelled) noexcept
		: storage_{std::in_place_type<Cancelled>, move(cancelled)} {}
	Outcome(Outcome const &) = default;
	Outcome(
		Outcome &&other) noexcept
		: storage_{move(other.storage_)} {}
	Outcome &operator =(
		Outcome const &other)
		requires std::copy_constructible<storage_t>
	{
		if (this != std::addressof(other)) {
			Outcome staged{other};
			storage_ = move(staged.storage_);
		}
		return *this;
	}
	Outcome &operator =(
		Outcome &&other) noexcept {
		if (this != std::addressof(other)) {
			storage_ = std::move(other.storage_);
		}
		return *this;
	}
	[[nodiscard]] static Outcome make_success(
		success_t success)
		noexcept(
			std::is_nothrow_move_constructible_v<success_t>) {
		return Outcome{std::move(success)};
	}
	[[nodiscard]] static Outcome make_failure(
		std::exception_ptr const &error) {
		return Outcome{Failure{error}};
	}
	[[nodiscard]] static Outcome make_cancelled(
		CancelReason reason) noexcept {
		return Outcome{Cancelled{reason}};
	}
	[[nodiscard]] OutcomeKind kind() const noexcept {
		assert_formed();
		if (std::holds_alternative<success_t>(storage_)) {
			return OutcomeKind::success;
		}
		if (std::holds_alternative<Failure>(storage_)) {
			return OutcomeKind::failure;
		}
		return OutcomeKind::cancelled;
	}
	[[nodiscard]] bool is_success() const noexcept { return kind() == OutcomeKind::success; }
	[[nodiscard]] bool is_failure() const noexcept { return kind() == OutcomeKind::failure; }
	[[nodiscard]] bool is_cancelled() const noexcept { return kind() == OutcomeKind::cancelled; }
	[[nodiscard]] success_t &success() & noexcept { return std::get<success_t>(storage_); }
	[[nodiscard]] success_t const &success() const & noexcept { return std::get<success_t>(storage_); }
	[[nodiscard]] success_t &&success() && noexcept { return std::get<success_t>(std::move(storage_)); }
	[[nodiscard]] Failure &failure() & noexcept { return std::get<Failure>(storage_); }
	[[nodiscard]] Failure const &failure() const & noexcept { return std::get<Failure>(storage_); }
	[[nodiscard]] Failure &&failure() && noexcept { return std::get<Failure>(std::move(storage_)); }
	[[nodiscard]] Cancelled &cancelled() & noexcept { return std::get<Cancelled>(storage_); }
	[[nodiscard]] Cancelled const &cancelled() const & noexcept { return std::get<Cancelled>(storage_); }
	[[nodiscard]] Cancelled &&cancelled() && noexcept { return std::get<Cancelled>(std::move(storage_)); }
	template<typename F>
		requires std::invocable<F &, success_t &>
			  && std::invocable<F &, Failure &>
			  && std::invocable<F &, Cancelled &>
			  && same_as<std::invoke_result_t<F &, success_t &>, std::invoke_result_t<F &, Failure &>>
			  && same_as<std::invoke_result_t<F &, success_t &>, std::invoke_result_t<F &, Cancelled &>>
	auto visit(
		F &&f)
		& noexcept(
			std::is_nothrow_invocable_v<F &, success_t &>
			&& std::is_nothrow_invocable_v<F &, Failure &>
			&& std::is_nothrow_invocable_v<F &, Cancelled &>) -> std::invoke_result_t<F &, success_t &> {
		switch (kind()) {
		case OutcomeKind::success  : return invoke(f, success());
		case OutcomeKind::failure  : return invoke(f, failure());
		case OutcomeKind::cancelled: return invoke(f, cancelled());
		}
		std::unreachable();
	}
	template<typename F>
		requires std::invocable<F &, success_t const &>
			  && std::invocable<F &, Failure const &>
			  && std::invocable<F &, Cancelled const &>
			  && same_as<std::invoke_result_t<F &, success_t const &>, std::invoke_result_t<F &, Failure const &>>
			  && same_as<std::invoke_result_t<F &, success_t const &>, std::invoke_result_t<F &, Cancelled const &>>
	auto visit(
		F &&f)
		const & noexcept(
			std::is_nothrow_invocable_v<F &, success_t const &>
			&& std::is_nothrow_invocable_v<F &, Failure const &>
			&& std::is_nothrow_invocable_v<F &, Cancelled const &>) -> std::invoke_result_t<F &, success_t const &> {
		switch (kind()) {
		case OutcomeKind::success  : return invoke(f, success());
		case OutcomeKind::failure  : return invoke(f, failure());
		case OutcomeKind::cancelled: return invoke(f, cancelled());
		}
		std::unreachable();
	}
	template<typename F>
		requires std::invocable<F &, success_t &&>
			  && std::invocable<F &, Failure &&>
			  && std::invocable<F &, Cancelled &&>
			  && same_as<std::invoke_result_t<F &, success_t &&>, std::invoke_result_t<F &, Failure &&>>
			  && same_as<std::invoke_result_t<F &, success_t &&>, std::invoke_result_t<F &, Cancelled &&>>
	auto visit(
		F &&f)
		&& noexcept(
			std::is_nothrow_invocable_v<F &, success_t &&>
			&& std::is_nothrow_invocable_v<F &, Failure &&>
			&& std::is_nothrow_invocable_v<F &, Cancelled &&>) -> std::invoke_result_t<F &, success_t &&> {
		switch (kind()) {
		case OutcomeKind::success  : return invoke(f, move(*this).success());
		case OutcomeKind::failure  : return invoke(f, move(*this).failure());
		case OutcomeKind::cancelled: return invoke(f, move(*this).cancelled());
		}
		std::unreachable();
	}
	[[nodiscard]] T &value() & {
		switch (kind()) {
		case OutcomeKind::success  : return success().value;
		case OutcomeKind::failure  : rethrow_exception(failure().error);
		case OutcomeKind::cancelled: throw CancelledError{cancelled().reason};
		}
		std::unreachable();
	}
	[[nodiscard]] T const &value() const & {
		switch (kind()) {
		case OutcomeKind::success  : return success().value;
		case OutcomeKind::failure  : rethrow_exception(failure().error);
		case OutcomeKind::cancelled: throw CancelledError{cancelled().reason};
		}
		std::unreachable();
	}
	[[nodiscard]] T value() && {
		switch (kind()) {
		case OutcomeKind::success  : return move(success().value);
		case OutcomeKind::failure  : rethrow_exception(move(*this).failure().error);
		case OutcomeKind::cancelled: throw CancelledError{cancelled().reason};
		}
		std::unreachable();
	}
	template<class OnSuccess, class OnFailure, class OnCancelled>
		requires std::invocable<OnSuccess, T &&>
			  && std::invocable<OnFailure, Failure const &>
			  && std::invocable<OnCancelled, Cancelled const &>
			  && same_as<std::invoke_result_t<OnSuccess, T &&>, std::invoke_result_t<OnFailure, Failure const &>>
			  && same_as<std::invoke_result_t<OnSuccess, T &&>, std::invoke_result_t<OnCancelled, Cancelled const &>>
	auto match(
		OnSuccess &&on_success,
		OnFailure &&on_failure,
		OnCancelled &&on_cancelled) && -> std::invoke_result_t<OnSuccess, T &&> {
		switch (kind()) {
		case OutcomeKind::success  : return invoke(forward<OnSuccess>(on_success), move(success().value));
		case OutcomeKind::failure  : return invoke(forward<OnFailure>(on_failure), failure());
		case OutcomeKind::cancelled: return invoke(forward<OnCancelled>(on_cancelled), cancelled());
		}
		std::unreachable();
	}
	template<class OnSuccess, class OnFailure, class OnCancelled>
		requires std::invocable<OnSuccess, T const &>
			  && std::invocable<OnFailure, Failure const &>
			  && std::invocable<OnCancelled, Cancelled const &>
			  && same_as<std::invoke_result_t<OnSuccess, T const &>, std::invoke_result_t<OnFailure, Failure const &>>
			  && same_as<
					 std::invoke_result_t<OnSuccess, T const &>,
					 std::invoke_result_t<OnCancelled, Cancelled const &>>
	auto match(
		OnSuccess &&on_success,
		OnFailure &&on_failure,
		OnCancelled &&on_cancelled) const & -> std::invoke_result_t<OnSuccess, T const &> {
		switch (kind()) {
		case OutcomeKind::success  : return invoke(forward<OnSuccess>(on_success), success().value);
		case OutcomeKind::failure  : return invoke(forward<OnFailure>(on_failure), failure());
		case OutcomeKind::cancelled: return invoke(forward<OnCancelled>(on_cancelled), cancelled());
		}
		std::unreachable();
	}
};
template<>
class Outcome<void> final {
	using success_t = Success<void>;
	using storage_t = std::variant<success_t, Failure, Cancelled>;
	storage_t storage_;
	void assert_formed() const noexcept {
		if (storage_.valueless_by_exception()) {
			std::terminate();
		}
	}

public:
	Outcome() = delete;
	Outcome(
		success_t success = success_t{}) noexcept
		: storage_{std::in_place_type<success_t>, success} {}
	Outcome(
		Failure failure) noexcept
		: storage_{std::in_place_type<Failure>, move(failure)} {}
	Outcome(
		Cancelled cancelled) noexcept
		: storage_{std::in_place_type<Cancelled>, cancelled} {}
	Outcome(Outcome const &) = default;
	Outcome(
		Outcome &&other) noexcept
		: storage_{move(other.storage_)} {}
	Outcome &operator =(
		Outcome const &other) {
		if (this != std::addressof(other)) {
			Outcome staged{other};
			storage_ = move(staged.storage_);
		}
		return *this;
	}
	Outcome &operator =(
		Outcome &&other) noexcept {
		if (this != std::addressof(other)) {
			storage_ = std::move(other.storage_);
		}
		return *this;
	}
	[[nodiscard]] static Outcome make_success() noexcept { return Outcome{success_t{}}; }
	[[nodiscard]] static Outcome make_failure(
		std::exception_ptr const &error) {
		return Outcome{Failure{error}};
	}
	[[nodiscard]] static Outcome make_cancelled(
		CancelReason reason) noexcept {
		return Outcome{Cancelled{reason}};
	}
	[[nodiscard]] OutcomeKind kind() const noexcept {
		assert_formed();
		if (std::holds_alternative<success_t>(storage_)) {
			return OutcomeKind::success;
		}
		if (std::holds_alternative<Failure>(storage_)) {
			return OutcomeKind::failure;
		}
		return OutcomeKind::cancelled;
	}
	[[nodiscard]] bool is_success() const noexcept { return kind() == OutcomeKind::success; }
	[[nodiscard]] bool is_failure() const noexcept { return kind() == OutcomeKind::failure; }
	[[nodiscard]] bool is_cancelled() const noexcept { return kind() == OutcomeKind::cancelled; }
	[[nodiscard]] success_t &success() & { return std::get<success_t>(storage_); }
	[[nodiscard]] success_t const &success() const & { return std::get<success_t>(storage_); }
	[[nodiscard]] success_t &&success() && { return std::get<success_t>(std::move(storage_)); }
	[[nodiscard]] Failure &failure() & { return std::get<Failure>(storage_); }
	[[nodiscard]] Failure const &failure() const & { return std::get<Failure>(storage_); }
	[[nodiscard]] Failure &&failure() && { return std::get<Failure>(std::move(storage_)); }
	[[nodiscard]] Cancelled &cancelled() & { return std::get<Cancelled>(storage_); }
	[[nodiscard]] Cancelled const &cancelled() const & { return std::get<Cancelled>(storage_); }
	[[nodiscard]] Cancelled &&cancelled() && { return std::get<Cancelled>(std::move(storage_)); }
	template<typename F>
		requires std::invocable<F &, success_t &>
			  && std::invocable<F &, Failure &>
			  && std::invocable<F &, Cancelled &>
			  && same_as<std::invoke_result_t<F &, success_t &>, std::invoke_result_t<F &, Failure &>>
			  && same_as<std::invoke_result_t<F &, success_t &>, std::invoke_result_t<F &, Cancelled &>>
	auto visit(
		F &&f)
		& noexcept(
			std::is_nothrow_invocable_v<F &, success_t &>
			&& std::is_nothrow_invocable_v<F &, Failure &>
			&& std::is_nothrow_invocable_v<F &, Cancelled &>) -> std::invoke_result_t<F &, success_t &> {
		switch (kind()) {
		case OutcomeKind::success  : return invoke(f, success());
		case OutcomeKind::failure  : return invoke(f, failure());
		case OutcomeKind::cancelled: return invoke(f, cancelled());
		}
		std::unreachable();
	}
	template<typename F>
		requires std::invocable<F &, success_t const &>
			  && std::invocable<F &, Failure const &>
			  && std::invocable<F &, Cancelled const &>
			  && same_as<std::invoke_result_t<F &, success_t const &>, std::invoke_result_t<F &, Failure const &>>
			  && same_as<std::invoke_result_t<F &, success_t const &>, std::invoke_result_t<F &, Cancelled const &>>
	auto visit(
		F &&f)
		const & noexcept(
			std::is_nothrow_invocable_v<F &, success_t const &>
			&& std::is_nothrow_invocable_v<F &, Failure const &>
			&& std::is_nothrow_invocable_v<F &, Cancelled const &>) -> std::invoke_result_t<F &, success_t const &> {
		switch (kind()) {
		case OutcomeKind::success  : return invoke(f, success());
		case OutcomeKind::failure  : return invoke(f, failure());
		case OutcomeKind::cancelled: return invoke(f, cancelled());
		}
		std::unreachable();
	}
	template<typename F>
		requires std::invocable<F &, success_t &&>
			  && std::invocable<F &, Failure &&>
			  && std::invocable<F &, Cancelled &&>
			  && same_as<std::invoke_result_t<F &, success_t &&>, std::invoke_result_t<F &, Failure &&>>
			  && same_as<std::invoke_result_t<F &, success_t &&>, std::invoke_result_t<F &, Cancelled &&>>
	auto visit(
		F &&f)
		&& noexcept(
			std::is_nothrow_invocable_v<F &, success_t &&>
			&& std::is_nothrow_invocable_v<F &, Failure &&>
			&& std::is_nothrow_invocable_v<F &, Cancelled &&>) -> std::invoke_result_t<F &, success_t &&> {
		switch (kind()) {
		case OutcomeKind::success  : return invoke(f, move(*this).success());
		case OutcomeKind::failure  : return invoke(f, move(*this).failure());
		case OutcomeKind::cancelled: return invoke(f, move(*this).cancelled());
		}
		std::unreachable();
	}
	void value() const {
		switch (kind()) {
		case OutcomeKind::success  : return;
		case OutcomeKind::failure  : rethrow_exception(failure().error);
		case OutcomeKind::cancelled: throw CancelledError{cancelled().reason};
		}
		std::unreachable();
	}
	template<class OnSuccess, class OnFailure, class OnCancelled>
		requires std::invocable<OnSuccess>
			  && std::invocable<OnFailure, Failure const &>
			  && std::invocable<OnCancelled, Cancelled const &>
			  && same_as<std::invoke_result_t<OnSuccess>, std::invoke_result_t<OnFailure, Failure const &>>
			  && same_as<std::invoke_result_t<OnSuccess>, std::invoke_result_t<OnCancelled, Cancelled const &>>
	auto match(
		OnSuccess &&on_success,
		OnFailure &&on_failure,
		OnCancelled &&on_cancelled) const -> std::invoke_result_t<OnSuccess> {
		switch (kind()) {
		case OutcomeKind::success  : return invoke(forward<OnSuccess>(on_success));
		case OutcomeKind::failure  : return invoke(forward<OnFailure>(on_failure), failure());
		case OutcomeKind::cancelled: return invoke(forward<OnCancelled>(on_cancelled), cancelled());
		}
		std::unreachable();
	}
};
template<work_value T>
[[nodiscard]] T value(
	Outcome<T> &&outcome) {
	return move(outcome).visit([](auto &&arm) -> T {
		using arm_t = std::remove_cvref_t<decltype(arm)>;
		if constexpr (same_as<arm_t, Success<T>>) {
			return move(arm.value);
		} else if constexpr (same_as<arm_t, Failure>) {
			throw FailureError{arm.error};
		} else {
			throw CancelledError{arm.reason};
		}
	});
}
inline void value(
	Outcome<void> &&outcome) {
	move(outcome).visit([](auto &&arm) -> void {
		using arm_t = std::remove_cvref_t<decltype(arm)>;
		if constexpr (same_as<arm_t, Success<void>>) {
			return;
		} else if constexpr (same_as<arm_t, Failure>) {
			throw FailureError{arm.error};
		} else {
			throw CancelledError{arm.reason};
		}
	});
}

enum class ControlCategory : std::uint8_t {
	task,
	posted,
	operation,
};
struct CapabilityId {
	void const *address = nullptr;
	void const *type_tag = nullptr;
	[[nodiscard]] bool friend operator ==(
		CapabilityId const &a,
		CapabilityId const &b) noexcept {
		return a.address == b.address && a.type_tag == b.type_tag;
	}
};

template<class T>
inline constexpr bool enable_address_capability_v = false;
struct capability_id_t {
	template<class Cap>
	[[nodiscard]] auto operator ()(
		Cap const &cap) const
		noexcept(
			noexcept(tag_invoke(*this, cap))) -> decltype(tag_invoke(*this, cap)) {
		return tag_invoke(*this, cap);
	}
	template<class T>
		requires enable_address_capability_v<T>
	[[nodiscard]] CapabilityId friend tag_invoke(
		capability_id_t,
		T const &self) noexcept {
		static unsigned char tag = 0;
		return CapabilityId{
			.address = static_cast<void const *>(std::addressof(self)),
			.type_tag = static_cast<void const *>(&tag),
		};
	}
};
inline constexpr capability_id_t capability_id{};

template<class C>
concept progress_capability = requires(C const &c) {
	{ capability_id(c) } noexcept -> same_as<CapabilityId>;
};
class JoinError : public std::logic_error {
public:
	enum class reason : std::uint8_t {
		consumed_handle,
		capability_mismatch,
		thread_precondition,
		reentrant_pump,
		hop_capability_mismatch,
		ready_callback_already_installed,
		lifetime_violation,
		not_ready,
	};
	explicit JoinError(
		reason r,
		std::source_location loc = std::source_location::current())
		: std::logic_error{make_msg(r)}
		, reason_{r}
		, origin_{loc} {}
	explicit JoinError(
		reason r,
		std::optional<CapabilityId> expected,
		CapabilityId actual,
		std::source_location loc = std::source_location::current())
		: std::logic_error{make_msg(r)}
		, reason_{r}
		, expected_{expected}
		, actual_{actual}
		, origin_{loc} {}
	[[nodiscard]] reason reason_code() const noexcept { return reason_; }
	[[nodiscard]] std::optional<CapabilityId> expected() const noexcept { return expected_; }
	[[nodiscard]] std::optional<CapabilityId> actual() const noexcept { return actual_; }
	[[nodiscard]] std::source_location origin() const noexcept { return origin_; }

private:
	reason reason_{};
	std::optional<CapabilityId> expected_{};
	std::optional<CapabilityId> actual_{};
	std::source_location origin_{};
	static std::string make_msg(
		reason r) {
		using enum reason;
		switch (r) {
		case consumed_handle                 : return "PROGRAMMER ERROR (JoinError): co_await or outcome() on moved-from/consumed Task";
		case capability_mismatch             : return "PROGRAMMER ERROR (JoinError): capability mismatch";
		case thread_precondition             : return "PROGRAMMER ERROR (JoinError): thread precondition violated";
		case reentrant_pump                  : return "PROGRAMMER ERROR (JoinError): reentrant pump";
		case hop_capability_mismatch         : return "PROGRAMMER ERROR (JoinError): hop capability mismatch";
		case ready_callback_already_installed: return "PROGRAMMER ERROR (JoinError): ready callback already installed";
		case lifetime_violation              : return "PROGRAMMER ERROR (JoinError): handle not live";
		case not_ready                       : return "PROGRAMMER ERROR (JoinError): ready-only join called before terminal state";
		}
		return "PROGRAMMER ERROR (JoinError): unknown";
	}
};

template<work_value T, ControlCategory Category>
class BasicSource;

template<work_value T, ControlCategory Category>
class BasicResult;

template<work_value T, ControlCategory Category>
class BasicJoinHandle;

struct SubmitOptions;
struct PostOptions;
struct OperationOptions;

enum class ReadyRegistration : std::uint8_t {
	installed,
	already_ready,
	already_installed,
	empty,
};

enum class ClearOnReadyStatus : std::uint8_t {
	cleared,
	in_flight,
	already_terminal,
	not_armed,
};

enum class AbandonStatus : std::uint8_t {
	installed,
	already_abandoned,
	empty,
};
namespace detail {

// Benchmark coverage lives in benchmarks/work_bench.cxx
// (`root/callable_erasure_custom` plus compile-gated std comparator). Keep
// the lower-overhead option as toolchain support matures. Do not regress to
// Fn.
template<typename Signature, std::size_t InlineBytes = 32>
class small_move_only_function;
template<typename R, typename... Args, std::size_t InlineBytes>
class small_move_only_function<R(Args...), InlineBytes> {
	struct storage_t {
		alignas(std::max_align_t) byte bytes[InlineBytes];
	};
	using invoke_fn = R (*)(void *, Args &&...);
	using destroy_fn = void (*)(void *) noexcept;
	using move_fn = void (*)(void *, void *) noexcept;

	storage_t inline_storage_{};
	void *object_ = nullptr;
	invoke_fn invoke_ = nullptr;
	destroy_fn destroy_ = nullptr;
	move_fn move_ = nullptr;
	bool inlined_ = false;
	template<typename F>
	static R invoke_inline(
		void *obj,
		Args &&...args) {
		return invoke(*reinterpret_cast<F *>(obj), forward<Args>(args)...);
	}
	template<typename F>
	static void destroy_inline(
		void *obj) noexcept {
		reinterpret_cast<F *>(obj)->~F();
	}
	template<typename F>
	static void move_inline(
		void *dst,
		void *src) noexcept {
		auto *src_fn = reinterpret_cast<F *>(src);
		new (dst) F(move(*src_fn));
		src_fn->~F();
	}
	template<typename F>
	static R invoke_heap(
		void *obj,
		Args &&...args) {
		return invoke(*reinterpret_cast<F *>(obj), forward<Args>(args)...);
	}
	template<typename F>
	static void destroy_heap(
		void *obj) noexcept {
		delete reinterpret_cast<F *>(obj);
	}
	void reset() noexcept {
		if (invoke_ == nullptr) {
			return;
		}
		destroy_(object_);
		object_ = nullptr;
		invoke_ = nullptr;
		destroy_ = nullptr;
		move_ = nullptr;
		inlined_ = false;
	}
	void move_from(
		small_move_only_function &&other) noexcept {
		invoke_ = other.invoke_;
		destroy_ = other.destroy_;
		move_ = other.move_;
		inlined_ = other.inlined_;

		if (invoke_ == nullptr) {
			object_ = nullptr;
			return;
		}

		if (other.inlined_) {
			object_ = &inline_storage_;
			move_(object_, other.object_);
			other.object_ = nullptr;
			other.invoke_ = nullptr;
			other.destroy_ = nullptr;
			other.move_ = nullptr;
			other.inlined_ = false;
			return;
		}

		object_ = exchange(other.object_, nullptr);
		other.invoke_ = nullptr;
		other.destroy_ = nullptr;
		other.move_ = nullptr;
		other.inlined_ = false;
	}

public:
	small_move_only_function() noexcept = default;
	small_move_only_function(
		std::nullptr_t) noexcept {}
	small_move_only_function(small_move_only_function const &) = delete;
	small_move_only_function &operator =(small_move_only_function const &) = delete;
	small_move_only_function(
		small_move_only_function &&other) noexcept {
		move_from(move(other));
	}
	small_move_only_function &operator =(
		small_move_only_function &&other) noexcept {
		if (this != &other) {
			reset();
			move_from(std::move(other));
		}
		return *this;
	}
	template<typename F>
		requires(!same_as<std::remove_cvref_t<F>, small_move_only_function>)
	small_move_only_function(
		F &&fn) {
		using fn_t = std::remove_cvref_t<F>;
		static_assert(std::is_move_constructible_v<fn_t>);

		if constexpr (
			sizeof(fn_t) <= InlineBytes
			&& alignof(fn_t) <= alignof(storage_t)
			&& std::is_nothrow_move_constructible_v<fn_t>) {
			object_ = &inline_storage_;
			new (object_) fn_t(forward<F>(fn));
			invoke_ = &invoke_inline<fn_t>;
			destroy_ = &destroy_inline<fn_t>;
			move_ = &move_inline<fn_t>;
			inlined_ = true;
		} else {
			object_ = new fn_t(forward<F>(fn));
			invoke_ = &invoke_heap<fn_t>;
			destroy_ = &destroy_heap<fn_t>;
			move_ = nullptr;
			inlined_ = false;
		}
	}
	~small_move_only_function() noexcept { reset(); }
	[[nodiscard]] explicit operator bool() const noexcept { return invoke_ != nullptr; }
	R operator ()(
		Args... args) const { // NOLINT(performance-unnecessary-value-param): pack must be forwarded
		return invoke_(object_, forward<Args>(args)...);
	}
};
struct ReadyRegistrationResult {
	ReadyRegistration status;
	small_move_only_function<void()> rejected_fn;
};

enum class TerminalState : std::uint8_t {
	none,
	success,
	failure,
	cancelled,
};

enum class ReadyHookState : std::uint8_t {
	open,
	armed,
	committing,
	terminal,
	disarmed,
};
// Dropped-outcome sink — stored once at startup; reads use acquire load to
// skip the mutex on the hot path.
struct DroppedOutcomeSinkStore {
	mutex mtx;
	small_move_only_function<void(std::source_location, OutcomeKind, std::exception_ptr)> fn;
	std::atomic<bool> installed{false};
};
inline DroppedOutcomeSinkStore &dropped_outcome_sink_store() noexcept {
	static DroppedOutcomeSinkStore s;
	return s;
}
inline void invoke_dropped_outcome_sink(
	std::source_location loc,
	OutcomeKind kind,
	std::exception_ptr const &cause) noexcept {
	auto &s = dropped_outcome_sink_store();
	if (!s.installed.load(memory_order_acquire)) {
		return;
	}
	lock_guard const lk{s.mtx};
	if (s.fn) {
		s.fn(loc, kind, cause);
	}
}
// Sink installed by Task::detach() / ~Task(). Drops success; forwards
// failure/cancelled to the process-wide dropped-outcome sink if installed.
template<typename T>
struct detach_outcome_sink {
	std::source_location loc;
	void operator ()(
		Failure const &f) const noexcept {
		invoke_dropped_outcome_sink(loc, OutcomeKind::failure, f.error);
	}
	void operator ()(
		Cancelled const & /*c*/) const noexcept {
		static std::exception_ptr const null{};
		invoke_dropped_outcome_sink(loc, OutcomeKind::cancelled, null);
	}
};
[[gnu::always_inline]] inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	asm volatile("yield");
#else
	asm volatile("" ::: "memory");
#endif
}
class ControlBlockBase {
	std::source_location spawn_loc_{};

public:
	ControlBlockBase() noexcept { detail::note_control_block_allocation(); }
	void set_spawn_location(
		std::source_location loc) noexcept {
		spawn_loc_ = loc;
	}
	[[nodiscard]] std::source_location spawn_location() const noexcept { return spawn_loc_; }
	virtual ~ControlBlockBase() noexcept { detail::note_control_block_deallocation(); }
	virtual bool request_cancel() noexcept = 0;
	[[nodiscard]] virtual std::stop_token stop_token() const noexcept = 0;
	[[nodiscard]] virtual bool cancel_requested() const noexcept = 0;
	[[nodiscard]] virtual bool ready() const noexcept = 0;
	[[nodiscard]] virtual WorkState state() const noexcept = 0;
	[[nodiscard]] virtual bool can_join_with(CapabilityId id) const noexcept = 0;
	[[nodiscard]] virtual std::optional<CapabilityId> required_capability() const noexcept = 0;
	virtual bool install_cancel_hook(small_move_only_function<void(CancelReason)> fn) noexcept = 0;
	[[nodiscard]] virtual ReadyRegistrationResult try_set_on_ready(small_move_only_function<void()> fn) noexcept = 0;
	[[nodiscard]] virtual ClearOnReadyStatus clear_on_ready() noexcept = 0;
};
template<work_value T>
class ControlBlockInterface : public ControlBlockBase {
public:
	virtual void set_required_capability(CapabilityId id) noexcept = 0;
	[[nodiscard]] virtual bool try_set_value(Success<T> success) = 0;
	[[nodiscard]] virtual bool try_set_exception(std::exception_ptr error) = 0;
	[[nodiscard]] virtual bool try_set_error(
		std::error_code ec) {
		return try_set_exception(make_exception_ptr(std::system_error(ec)));
	}
	[[nodiscard]] virtual bool try_set_error(
		std::error_code ec,
		std::string_view msg) noexcept {
		try {
			return try_set_exception(make_exception_ptr(std::system_error(ec, std::string{msg})));
		} catch (...) { return try_set_error(ec); }
	}
	[[nodiscard]] virtual bool try_set_cancelled(CancelReason reason, bool allow_abandoned) noexcept = 0;
	[[nodiscard]] virtual Outcome<T> compatibility_blocking_take_outcome() = 0;
	[[nodiscard]] virtual std::optional<Outcome<T>> try_take_ready_outcome() = 0;
	virtual void install_abandon_sink(small_move_only_function<void(Outcome<T> const &)> sink) noexcept = 0;
	[[nodiscard]] virtual AbandonStatus
	try_install_abandon_sink(small_move_only_function<void(Outcome<T> const &)> sink) noexcept = 0;
};
template<work_value T, bool EnableCancellation>
class ControlBlockModel final : public ControlBlockInterface<T> {
	std::atomic<TerminalState> terminal_state_{TerminalState::none};
	std::atomic<ReadyHookState> ready_hook_state_{ReadyHookState::open};
	std::atomic<bool> cancel_requested_{false};
	std::atomic<bool> terminal_claimed_{false};
	// P2b false-sharing fix: hot atomics above land on one cache line;
	// alignas(64) on mtx_ starts cold lock/cv on a fresh line.
	alignas(64) mutable mutex mtx_{};
	std::condition_variable cv_{};
	std::optional<Outcome<T>> outcome_{};
	small_move_only_function<void()> on_ready_fn_{};
	small_move_only_function<void(CancelReason)> hook_fn_{};
	small_move_only_function<void(Outcome<T> const &)> abandon_sink_{};
	bool hook_installed_ = false, hook_claimed_ = false, abandoned_ = false;
	[[no_unique_address]] std::conditional_t<EnableCancellation, std::stop_source, std::monostate> stop_source_{};
	std::atomic<bool> requires_capability_{false};
	std::atomic<void const *> required_capability_address_{nullptr};
	std::atomic<void const *> required_capability_type_tag_{nullptr};
	[[nodiscard]] small_move_only_function<void(CancelReason)> claim_requested_hook_if_present() noexcept {
		std::scoped_lock const lk{mtx_};
		if (!hook_installed_ || hook_claimed_) {
			return {};
		}
		if (terminal_claimed_.load(memory_order_acquire)) {
			return {};
		}
		hook_claimed_ = true;
		return move(hook_fn_);
	}
	void invoke_requested_hook_if_needed() noexcept {
		auto fn = claim_requested_hook_if_present();
		if (!fn) {
			return;
		}
		if (terminal_claimed_.load(memory_order_acquire)) {
			return;
		}
		try {
			fn(CancelReason::requested);
		} catch (...) { std::terminate(); }
	}
	void fire_ready_hook_if_armed_() noexcept {
		auto prev = ReadyHookState::open;
		if (ready_hook_state_.compare_exchange_strong(
				prev,
				ReadyHookState::committing,
				memory_order_acq_rel,
				memory_order_acquire)) {
			ready_hook_state_.store(ReadyHookState::terminal, memory_order_release);
			return;
		}
		if (prev == ReadyHookState::armed) {
			small_move_only_function<void()> fn{};
			{
				std::unique_lock lk{mtx_};
				fn = move(on_ready_fn_);
				ready_hook_state_.store(ReadyHookState::terminal, memory_order_release);
			}
			if (fn) {
				fn();
			}
		} else if (prev == ReadyHookState::disarmed) {
			std::unique_lock lk{mtx_};
			ready_hook_state_.store(ReadyHookState::terminal, memory_order_release);
		}
	}
	[[nodiscard]] bool try_claim_terminal() noexcept {
		bool expected = false;
		return terminal_claimed_.compare_exchange_strong(expected, true, memory_order_acq_rel, memory_order_acquire);
	}
	[[nodiscard]] static WorkState map_terminal(
		TerminalState s) noexcept {
		switch (s) {
		case TerminalState::none     : return WorkState::pending;
		case TerminalState::success  : return WorkState::ready_success;
		case TerminalState::failure  : return WorkState::ready_failure;
		case TerminalState::cancelled: return WorkState::ready_cancelled;
		}
		std::unreachable();
	}
	void run_abandon_path_if_present() noexcept {
		small_move_only_function<void(Outcome<T> const &)> sink{};
		std::optional<Outcome<T>> local{};
		{
			std::unique_lock lk{mtx_};
			if (!abandoned_ || !abandon_sink_ || !outcome_) {
				return;
			}
			sink = move(abandon_sink_);
			abandon_sink_ = nullptr;
			local.emplace(move(*outcome_));
			outcome_.reset();
		}
		try {
			sink(*local);
		} catch (...) { std::terminate(); }
	}

public:
	void set_required_capability(
		CapabilityId id) noexcept override {
		required_capability_address_.store(id.address, memory_order_relaxed);
		required_capability_type_tag_.store(id.type_tag, memory_order_relaxed);
		requires_capability_.store(true, memory_order_release);
	}
	[[nodiscard]] bool can_join_with(
		CapabilityId id) const noexcept override {
		if (!requires_capability_.load(memory_order_acquire)) {
			return true;
		}
		CapabilityId const expected{
			.address = required_capability_address_.load(memory_order_relaxed),
			.type_tag = required_capability_type_tag_.load(memory_order_relaxed),
		};
		return expected == id;
	}
	[[nodiscard]] std::optional<CapabilityId> required_capability() const noexcept override {
		if (!requires_capability_.load(memory_order_acquire)) {
			return nullopt;
		}
		return CapabilityId{
			.address = required_capability_address_.load(memory_order_relaxed),
			.type_tag = required_capability_type_tag_.load(memory_order_relaxed),
		};
	}
	[[nodiscard]] bool request_cancel() noexcept override {
		if (terminal_claimed_.load(memory_order_acquire)) {
			return false;
		}

		bool expected = false;
		if (!cancel_requested_.compare_exchange_strong(expected, true, memory_order_acq_rel, memory_order_acquire)) {
			return false;
		}

		if constexpr (EnableCancellation) {
			auto _ = stop_source_.request_stop();
		}
		invoke_requested_hook_if_needed();
		return true;
	}
	[[nodiscard]] std::stop_token stop_token() const noexcept override {
		if constexpr (EnableCancellation) {
			return stop_source_.get_token();
		} else {
			return std::stop_token{};
		}
	}
	[[nodiscard]] bool cancel_requested() const noexcept override {
		return cancel_requested_.load(memory_order_acquire);
	}
	[[nodiscard]] bool ready() const noexcept override {
		return terminal_state_.load(memory_order_acquire) != TerminalState::none;
	}
	[[nodiscard]] WorkState state() const noexcept override {
		TerminalState const terminal = terminal_state_.load(memory_order_acquire);
		if (terminal != TerminalState::none) {
			return map_terminal(terminal);
		}
		if (cancel_requested_.load(memory_order_acquire)) {
			return WorkState::cancel_requested;
		}
		return WorkState::pending;
	}
	bool install_cancel_hook(
		small_move_only_function<void(CancelReason)> fn) noexcept override {
		if (!fn) {
			return false;
		}
		small_move_only_function<void(CancelReason)> invoke_now{};
		{
			std::scoped_lock const lk{mtx_};
			if (hook_installed_) {
				return false;
			}
			if (terminal_claimed_.load(memory_order_acquire)) {
				return false;
			}
			hook_installed_ = true;
			hook_fn_ = move(fn);
			if (!hook_claimed_
				&& cancel_requested_.load(memory_order_acquire)
				&& !terminal_claimed_.load(memory_order_acquire)) {
				hook_claimed_ = true;
				invoke_now = std::move(hook_fn_);
			}
		}
		if (invoke_now && !terminal_claimed_.load(memory_order_acquire)) {
			try {
				invoke_now(CancelReason::requested);
			} catch (...) { std::terminate(); }
		}
		return true;
	}
	[[nodiscard]] bool try_set_value(
		Success<T> success) override {
		if (!try_claim_terminal()) {
			return false;
		}
		{
			std::unique_lock lk{mtx_};
			outcome_.emplace(Outcome<T>{std::move(success)});
			terminal_state_.store(TerminalState::success, memory_order_release);
		}
		cv_.notify_all();
		fire_ready_hook_if_armed_();
		run_abandon_path_if_present();
		return true;
	}
	[[nodiscard]] bool try_set_exception(
		std::exception_ptr error) override {
		if (!try_claim_terminal()) {
			return false;
		}
		{
			std::unique_lock lk{mtx_};
			outcome_.emplace(Outcome<T>{Failure{error}});
			terminal_state_.store(TerminalState::failure, memory_order_release);
		}
		cv_.notify_all();
		fire_ready_hook_if_armed_();
		run_abandon_path_if_present();
		return true;
	}
	[[nodiscard]] bool try_set_cancelled(
		CancelReason reason,
		bool allow_abandoned) noexcept override {
		if (!allow_abandoned && reason == CancelReason::abandoned) {
			std::terminate();
		}
		if (!try_claim_terminal()) {
			return false;
		}
		{
			std::unique_lock lk{mtx_};
			outcome_.emplace(Outcome<T>{Cancelled{reason}});
			terminal_state_.store(TerminalState::cancelled, memory_order_release);
		}
		cv_.notify_all();
		fire_ready_hook_if_armed_();
		run_abandon_path_if_present();
		return true;
	}
	[[nodiscard]] ReadyRegistrationResult try_set_on_ready(
		small_move_only_function<void()> fn) noexcept override {
		if (!fn) {
			return {ReadyRegistration::empty, std::move(fn)};
		}
		if (ready_hook_state_.load(memory_order_acquire) == ReadyHookState::terminal) {
			return {ReadyRegistration::already_ready, move(fn)};
		}
		std::unique_lock lk{mtx_};
		auto s = ready_hook_state_.load(memory_order_acquire);
		if (s == ReadyHookState::terminal) {
			return {ReadyRegistration::already_ready, move(fn)};
		}
		if (s == ReadyHookState::open) {
			on_ready_fn_ = move(fn);
			auto expected = ReadyHookState::open;
			if (ready_hook_state_.compare_exchange_strong(
					expected,
					ReadyHookState::armed,
					memory_order_acq_rel,
					memory_order_acquire)) {
				return {ReadyRegistration::installed, {}};
			}
			// Lost race vs fire_ready_hook_if_armed_: it CAS'd open→committing
			// before our CAS, then stored terminal. Our fn was never seen by it.
			// Take it back and report already_ready so caller dispatches it.
			auto rejected = move(on_ready_fn_);
			return {ReadyRegistration::already_ready, std::move(rejected)};
		}
		if (s == ReadyHookState::armed || s == ReadyHookState::disarmed) {
			return {ReadyRegistration::already_installed, std::move(fn)};
		}
		return {ReadyRegistration::already_ready, std::move(fn)};
	}
	[[nodiscard]] ClearOnReadyStatus clear_on_ready() noexcept override {
		auto s = ready_hook_state_.load(memory_order_acquire);
		if (s == ReadyHookState::terminal) {
			return ClearOnReadyStatus::already_terminal;
		}
		if (s == ReadyHookState::committing) {
			return ClearOnReadyStatus::in_flight;
		}
		if (s != ReadyHookState::armed) {
			return ClearOnReadyStatus::not_armed;
		}
		std::unique_lock lk{mtx_};
		s = ready_hook_state_.load(memory_order_acquire);
		if (s == ReadyHookState::terminal) {
			return ClearOnReadyStatus::already_terminal;
		}
		if (s == ReadyHookState::committing) {
			return ClearOnReadyStatus::in_flight;
		}
		if (s != ReadyHookState::armed) {
			return ClearOnReadyStatus::not_armed;
		}
		on_ready_fn_ = nullptr;
		ready_hook_state_.store(ReadyHookState::disarmed, memory_order_release);
		return ClearOnReadyStatus::cleared;
	}
	[[nodiscard]] Outcome<T> compatibility_blocking_take_outcome() override {
		auto const terminal = [&] { return terminal_state_.load(memory_order_acquire) != TerminalState::none; };
		// Spin before blocking: avoids condvar futex pair for fast tasks.
		// Release/acquire on terminal_state_ guarantees outcome_ is visible once true.
		static constexpr int kSpinIter = 400;
		bool done = terminal();
		for (int i = 0; !done && i < kSpinIter; ++i) {
			cpu_pause();
			done = terminal();
		}
		std::unique_lock lk{mtx_};
		if (!done) {
			cv_.wait(lk, terminal);
		}
		if (!outcome_) {
			throw std::logic_error{"conflux.work.root: missing terminal outcome"};
		}
		Outcome<T> out = move(*outcome_);
		outcome_.reset();
		return out;
	}
	[[nodiscard]] std::optional<Outcome<T>> try_take_ready_outcome() override {
		if (terminal_state_.load(memory_order_acquire) == TerminalState::none) {
			return nullopt;
		}
		std::unique_lock lk{mtx_};
		if (terminal_state_.load(memory_order_acquire) == TerminalState::none) {
			return nullopt;
		}
		if (!outcome_) {
			throw std::logic_error{"conflux.work.root: missing terminal outcome"};
		}
		Outcome<T> out = move(*outcome_);
		outcome_.reset();
		return std::optional<Outcome<T>>{move(out)};
	}
	void install_abandon_sink(
		small_move_only_function<void(Outcome<T> const &)> sink) noexcept override {
		if (!sink) {
			std::terminate();
		}
		{
			std::scoped_lock const lk{mtx_};
			if (abandoned_) {
				std::terminate();
			}
			abandoned_ = true;
			abandon_sink_ = move(sink);
		}
		run_abandon_path_if_present();
	}
	[[nodiscard]] AbandonStatus try_install_abandon_sink(
		small_move_only_function<void(Outcome<T> const &)> sink) noexcept override {
		if (!sink) {
			return AbandonStatus::empty;
		}
		{
			std::scoped_lock const lk{mtx_};
			if (abandoned_) {
				return AbandonStatus::already_abandoned;
			}
			abandoned_ = true;
			abandon_sink_ = move(sink);
		}
		run_abandon_path_if_present();
		return AbandonStatus::installed;
	}
};
template<bool EnableCancellation>
class ControlBlockModel<void, EnableCancellation> final : public ControlBlockInterface<void> {
	std::atomic<TerminalState> terminal_state_{TerminalState::none};
	std::atomic<ReadyHookState> ready_hook_state_{ReadyHookState::open};
	std::atomic<bool> cancel_requested_{false};
	std::atomic<bool> terminal_claimed_{false};
	// P2b false-sharing fix: hot atomics above land on one cache line;
	// alignas(64) on mtx_ starts cold lock/cv on a fresh line.
	alignas(64) mutable mutex mtx_{};
	std::condition_variable cv_{};
	std::optional<Outcome<void>> outcome_{};
	small_move_only_function<void()> on_ready_fn_{};
	small_move_only_function<void(CancelReason)> hook_fn_{};
	small_move_only_function<void(Outcome<void> const &)> abandon_sink_{};
	bool hook_installed_ = false, hook_claimed_ = false, abandoned_ = false;
	[[no_unique_address]] std::conditional_t<EnableCancellation, std::stop_source, std::monostate> stop_source_{};
	std::atomic<bool> requires_capability_{false};
	std::atomic<void const *> required_capability_address_{nullptr};
	std::atomic<void const *> required_capability_type_tag_{nullptr};
	[[nodiscard]] small_move_only_function<void(CancelReason)> claim_requested_hook_if_present() noexcept {
		std::scoped_lock const lk{mtx_};
		if (!hook_installed_ || hook_claimed_) {
			return {};
		}
		if (terminal_claimed_.load(memory_order_acquire)) {
			return {};
		}
		hook_claimed_ = true;
		return move(hook_fn_);
	}
	void invoke_requested_hook_if_needed() noexcept {
		auto fn = claim_requested_hook_if_present();
		if (!fn) {
			return;
		}
		if (terminal_claimed_.load(memory_order_acquire)) {
			return;
		}
		try {
			fn(CancelReason::requested);
		} catch (...) { std::terminate(); }
	}
	[[nodiscard]] bool try_claim_terminal() noexcept {
		bool expected = false;
		return terminal_claimed_.compare_exchange_strong(expected, true, memory_order_acq_rel, memory_order_acquire);
	}
	[[nodiscard]] static WorkState map_terminal(
		TerminalState s) noexcept {
		switch (s) {
		case TerminalState::none     : return WorkState::pending;
		case TerminalState::success  : return WorkState::ready_success;
		case TerminalState::failure  : return WorkState::ready_failure;
		case TerminalState::cancelled: return WorkState::ready_cancelled;
		}
		std::unreachable();
	}
	void fire_ready_hook_if_armed_() noexcept {
		auto prev = ReadyHookState::open;
		if (ready_hook_state_.compare_exchange_strong(
				prev,
				ReadyHookState::committing,
				memory_order_acq_rel,
				memory_order_acquire)) {
			ready_hook_state_.store(ReadyHookState::terminal, memory_order_release);
			return;
		}
		if (prev == ReadyHookState::armed) {
			small_move_only_function<void()> fn{};
			{
				std::unique_lock lk{mtx_};
				fn = move(on_ready_fn_);
				ready_hook_state_.store(ReadyHookState::terminal, memory_order_release);
			}
			if (fn) {
				fn();
			}
		} else if (prev == ReadyHookState::disarmed) {
			std::unique_lock lk{mtx_};
			ready_hook_state_.store(ReadyHookState::terminal, memory_order_release);
		}
	}
	void run_abandon_path_if_present() noexcept {
		small_move_only_function<void(Outcome<void> const &)> sink{};
		std::optional<Outcome<void>> local{};
		{
			std::unique_lock lk{mtx_};
			if (!abandoned_ || !abandon_sink_ || !outcome_) {
				return;
			}
			sink = move(abandon_sink_);
			abandon_sink_ = nullptr;
			local.emplace(move(*outcome_));
			outcome_.reset();
		}
		try {
			sink(*local);
		} catch (...) { std::terminate(); }
	}

public:
	void set_required_capability(
		CapabilityId id) noexcept override {
		required_capability_address_.store(id.address, memory_order_relaxed);
		required_capability_type_tag_.store(id.type_tag, memory_order_relaxed);
		requires_capability_.store(true, memory_order_release);
	}
	[[nodiscard]] bool can_join_with(
		CapabilityId id) const noexcept override {
		if (!requires_capability_.load(memory_order_acquire)) {
			return true;
		}
		CapabilityId const expected{
			.address = required_capability_address_.load(memory_order_relaxed),
			.type_tag = required_capability_type_tag_.load(memory_order_relaxed),
		};
		return expected == id;
	}
	[[nodiscard]] std::optional<CapabilityId> required_capability() const noexcept override {
		if (!requires_capability_.load(memory_order_acquire)) {
			return nullopt;
		}
		return CapabilityId{
			.address = required_capability_address_.load(memory_order_relaxed),
			.type_tag = required_capability_type_tag_.load(memory_order_relaxed),
		};
	}
	[[nodiscard]] bool request_cancel() noexcept override {
		if (terminal_claimed_.load(memory_order_acquire)) {
			return false;
		}

		bool expected = false;
		if (!cancel_requested_.compare_exchange_strong(expected, true, memory_order_acq_rel, memory_order_acquire)) {
			return false;
		}

		if constexpr (EnableCancellation) {
			auto _ = stop_source_.request_stop();
		}
		invoke_requested_hook_if_needed();
		return true;
	}
	[[nodiscard]] std::stop_token stop_token() const noexcept override {
		if constexpr (EnableCancellation) {
			return stop_source_.get_token();
		} else {
			return std::stop_token{};
		}
	}
	[[nodiscard]] bool cancel_requested() const noexcept override {
		return cancel_requested_.load(memory_order_acquire);
	}
	[[nodiscard]] bool ready() const noexcept override {
		return terminal_state_.load(memory_order_acquire) != TerminalState::none;
	}
	[[nodiscard]] WorkState state() const noexcept override {
		TerminalState const terminal = terminal_state_.load(memory_order_acquire);
		if (terminal != TerminalState::none) {
			return map_terminal(terminal);
		}
		if (cancel_requested_.load(memory_order_acquire)) {
			return WorkState::cancel_requested;
		}
		return WorkState::pending;
	}
	bool install_cancel_hook(
		small_move_only_function<void(CancelReason)> fn) noexcept override {
		if (!fn) {
			return false;
		}
		small_move_only_function<void(CancelReason)> invoke_now{};
		{
			std::scoped_lock const lk{mtx_};
			if (hook_installed_) {
				return false;
			}
			if (terminal_claimed_.load(memory_order_acquire)) {
				return false;
			}
			hook_installed_ = true;
			hook_fn_ = move(fn);
			if (!hook_claimed_
				&& cancel_requested_.load(memory_order_acquire)
				&& !terminal_claimed_.load(memory_order_acquire)) {
				hook_claimed_ = true;
				invoke_now = std::move(hook_fn_);
			}
		}
		if (invoke_now && !terminal_claimed_.load(memory_order_acquire)) {
			try {
				invoke_now(CancelReason::requested);
			} catch (...) { std::terminate(); }
		}
		return true;
	}
	[[nodiscard]] bool try_set_value(
		Success<void> success = Success<void>{}) noexcept override {
		if (!try_claim_terminal()) {
			return false;
		}
		{
			std::unique_lock lk{mtx_};
			outcome_.emplace(Outcome<void>{success});
			terminal_state_.store(TerminalState::success, memory_order_release);
		}
		cv_.notify_all();
		fire_ready_hook_if_armed_();
		run_abandon_path_if_present();
		return true;
	}
	[[nodiscard]] bool try_set_exception(
		std::exception_ptr error) override {
		if (!try_claim_terminal()) {
			return false;
		}
		{
			std::unique_lock lk{mtx_};
			outcome_.emplace(Outcome<void>{Failure{error}});
			terminal_state_.store(TerminalState::failure, memory_order_release);
		}
		cv_.notify_all();
		fire_ready_hook_if_armed_();
		run_abandon_path_if_present();
		return true;
	}
	[[nodiscard]] bool try_set_cancelled(
		CancelReason reason,
		bool allow_abandoned) noexcept override {
		if (!allow_abandoned && reason == CancelReason::abandoned) {
			std::terminate();
		}
		if (!try_claim_terminal()) {
			return false;
		}
		{
			std::unique_lock lk{mtx_};
			outcome_.emplace(Outcome<void>{Cancelled{reason}});
			terminal_state_.store(TerminalState::cancelled, memory_order_release);
		}
		cv_.notify_all();
		fire_ready_hook_if_armed_();
		run_abandon_path_if_present();
		return true;
	}
	[[nodiscard]] ReadyRegistrationResult try_set_on_ready(
		small_move_only_function<void()> fn) noexcept override {
		if (!fn) {
			return {ReadyRegistration::empty, std::move(fn)};
		}
		if (ready_hook_state_.load(memory_order_acquire) == ReadyHookState::terminal) {
			return {ReadyRegistration::already_ready, move(fn)};
		}
		std::unique_lock lk{mtx_};
		auto s = ready_hook_state_.load(memory_order_acquire);
		if (s == ReadyHookState::terminal) {
			return {ReadyRegistration::already_ready, move(fn)};
		}
		if (s == ReadyHookState::open) {
			on_ready_fn_ = move(fn);
			auto expected = ReadyHookState::open;
			if (ready_hook_state_.compare_exchange_strong(
					expected,
					ReadyHookState::armed,
					memory_order_acq_rel,
					memory_order_acquire)) {
				return {ReadyRegistration::installed, {}};
			}
			// Lost race vs fire_ready_hook_if_armed_: it CAS'd open→committing
			// before our CAS, then stored terminal. Our fn was never seen by it.
			// Take it back and report already_ready so caller dispatches it.
			auto rejected = move(on_ready_fn_);
			return {ReadyRegistration::already_ready, std::move(rejected)};
		}
		if (s == ReadyHookState::armed || s == ReadyHookState::disarmed) {
			return {ReadyRegistration::already_installed, std::move(fn)};
		}
		return {ReadyRegistration::already_ready, std::move(fn)};
	}
	[[nodiscard]] ClearOnReadyStatus clear_on_ready() noexcept override {
		auto s = ready_hook_state_.load(memory_order_acquire);
		if (s == ReadyHookState::terminal) {
			return ClearOnReadyStatus::already_terminal;
		}
		if (s == ReadyHookState::committing) {
			return ClearOnReadyStatus::in_flight;
		}
		if (s != ReadyHookState::armed) {
			return ClearOnReadyStatus::not_armed;
		}
		std::unique_lock lk{mtx_};
		s = ready_hook_state_.load(memory_order_acquire);
		if (s == ReadyHookState::terminal) {
			return ClearOnReadyStatus::already_terminal;
		}
		if (s == ReadyHookState::committing) {
			return ClearOnReadyStatus::in_flight;
		}
		if (s != ReadyHookState::armed) {
			return ClearOnReadyStatus::not_armed;
		}
		on_ready_fn_ = nullptr;
		ready_hook_state_.store(ReadyHookState::disarmed, memory_order_release);
		return ClearOnReadyStatus::cleared;
	}
	[[nodiscard]] Outcome<void> compatibility_blocking_take_outcome() override {
		auto const terminal = [&] { return terminal_state_.load(memory_order_acquire) != TerminalState::none; };
		// Spin before blocking: avoids condvar futex pair for fast tasks.
		// Release/acquire on terminal_state_ guarantees outcome_ is visible once true.
		static constexpr int kSpinIter = 400;
		bool done = terminal();
		for (int i = 0; !done && i < kSpinIter; ++i) {
			cpu_pause();
			done = terminal();
		}
		std::unique_lock lk{mtx_};
		if (!done) {
			cv_.wait(lk, terminal);
		}
		if (!outcome_) {
			throw std::logic_error{"conflux.work.root: missing terminal outcome"};
		}
		Outcome<void> out = move(*outcome_);
		outcome_.reset();
		return out;
	}
	[[nodiscard]] std::optional<Outcome<void>> try_take_ready_outcome() override {
		if (terminal_state_.load(memory_order_acquire) == TerminalState::none) {
			return nullopt;
		}
		std::unique_lock lk{mtx_};
		if (terminal_state_.load(memory_order_acquire) == TerminalState::none) {
			return nullopt;
		}
		if (!outcome_) {
			throw std::logic_error{"conflux.work.root: missing terminal outcome"};
		}
		Outcome<void> out = move(*outcome_);
		outcome_.reset();
		return std::optional<Outcome<void>>{move(out)};
	}
	void install_abandon_sink(
		small_move_only_function<void(Outcome<void> const &)> sink) noexcept override {
		if (!sink) {
			std::terminate();
		}
		{
			std::scoped_lock const lk{mtx_};
			if (abandoned_) {
				std::terminate();
			}
			abandoned_ = true;
			abandon_sink_ = move(sink);
		}
		run_abandon_path_if_present();
	}
	[[nodiscard]] AbandonStatus try_install_abandon_sink(
		small_move_only_function<void(Outcome<void> const &)> sink) noexcept override {
		if (!sink) {
			return AbandonStatus::empty;
		}
		{
			std::scoped_lock const lk{mtx_};
			if (abandoned_) {
				return AbandonStatus::already_abandoned;
			}
			abandoned_ = true;
			abandon_sink_ = move(sink);
		}
		run_abandon_path_if_present();
		return AbandonStatus::installed;
	}
};
template<work_value T, bool EnableCancellation>
[[nodiscard]] std::shared_ptr<ControlBlockInterface<T>> make_control_block_shared() {
	using model_t = ControlBlockModel<T, EnableCancellation>;
	return make_shared<model_t>();
}
[[nodiscard]] inline std::pmr::memory_resource &task_coroutine_frame_resource() noexcept {
	// Process-lifetime fallback pool: coroutine frames can be destroyed from any
	// thread, and the pool is intentionally leaked to avoid static-destruction
	// ordering. When CONFLUX_WORK_CORO_FRAME_POOL is enabled this is only the
	// oversize / mmap-failure fallback.
	static auto *resource = new std::pmr::synchronized_pool_resource{};
	return *resource;
}
struct TaskFrameBucket;
struct alignas(std::max_align_t) TaskFrameHeader {
	std::size_t size = 0;
	TaskFrameBucket *bucket = nullptr;
};

#if CONFLUX_WORK_TASK_FRAME_POOL_ACTIVE
struct TaskFrameFreeNode {
	TaskFrameFreeNode *next = nullptr;
};
struct TaskFrameBucket {
	std::size_t payload_size = 0;
	mutex mtx{};
	TaskFrameFreeNode *free = nullptr;
};
[[nodiscard]] constexpr std::size_t align_frame_bytes(
	std::size_t n) noexcept {
	constexpr std::size_t align = alignof(std::max_align_t);
	return (n + align - 1u) & ~(align - 1u);
}
[[nodiscard]] inline TaskFrameBucket *task_frame_bucket_for(
	std::size_t size) noexcept {
	static TaskFrameBucket buckets[] = {
		{.payload_size = 256},
		{.payload_size = 512},
		{.payload_size = 1024},
		{.payload_size = 2048},
		{.payload_size = 4096},
		{.payload_size = 8192},
	};
	for (auto &bucket: buckets) {
		if (size <= bucket.payload_size) {
			return &bucket;
		}
	}
	return nullptr;
}
[[nodiscard]] inline bool refill_task_frame_bucket_locked(
	TaskFrameBucket &bucket) noexcept {
	constexpr std::size_t slab_bytes = 1024u * 1024u;
	std::size_t const block_bytes = align_frame_bytes(sizeof(TaskFrameHeader) + bucket.payload_size);
	std::size_t const allocation_bytes = max(slab_bytes, block_bytes);
	void *raw = mmap(nullptr, allocation_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED) {
		return false;
	}
	auto *cursor = static_cast<std::byte *>(raw);
	std::size_t const count = allocation_bytes / block_bytes;
	for (std::size_t i = 0; i < count; ++i) {
		auto *node = reinterpret_cast<TaskFrameFreeNode *>(cursor + i * block_bytes);
		node->next = bucket.free;
		bucket.free = node;
	}
	return true;
}
[[nodiscard]] inline TaskFrameHeader *try_allocate_pooled_task_frame(
	std::size_t size) noexcept {
	auto *bucket = task_frame_bucket_for(size);
	if (!bucket) {
		return nullptr;
	}
	std::scoped_lock const lk{bucket->mtx};
	if (!bucket->free && !refill_task_frame_bucket_locked(*bucket)) {
		return nullptr;
	}
	auto *node = bucket->free;
	bucket->free = node->next;
	auto *hdr = reinterpret_cast<TaskFrameHeader *>(node);
	::new (static_cast<void *>(hdr)) TaskFrameHeader{.size = size, .bucket = bucket};
	return hdr;
}
inline void deallocate_pooled_task_frame(
	TaskFrameHeader *hdr) noexcept {
	auto *bucket = hdr->bucket;
	hdr->~TaskFrameHeader();
	auto *node = reinterpret_cast<TaskFrameFreeNode *>(hdr);
	std::scoped_lock const lk{bucket->mtx};
	node->next = bucket->free;
	bucket->free = node;
}
#endif
[[nodiscard]] inline TaskFrameHeader *allocate_task_coroutine_frame(
	std::size_t size) {
#if CONFLUX_WORK_TASK_FRAME_POOL_ACTIVE
	if (auto *hdr = try_allocate_pooled_task_frame(size)) {
		return hdr;
	}
#endif
	auto *raw = static_cast<std::byte *>(
		task_coroutine_frame_resource().allocate(size + sizeof(TaskFrameHeader), alignof(std::max_align_t)));
	return ::new (static_cast<void *>(raw)) TaskFrameHeader{.size = size, .bucket = nullptr};
}
inline void deallocate_task_coroutine_frame(
	TaskFrameHeader *hdr) noexcept {
#if CONFLUX_WORK_TASK_FRAME_POOL_ACTIVE
	if (hdr->bucket) {
		deallocate_pooled_task_frame(hdr);
		return;
	}
#endif
	std::size_t const size = hdr->size;
	hdr->~TaskFrameHeader();
	task_coroutine_frame_resource().deallocate(
		hdr, size + sizeof(TaskFrameHeader), alignof(std::max_align_t));
}
// P2b size guard: delta against P2a baseline (432B measured on clang-libcxx +
// libstdc++ on x86_64). P2b padding must not exceed one additional cache line.
#ifndef CONFLUX_WORK_RELAX_CONTROL_BLOCK_SIZE_GUARD
static_assert(
	sizeof(ControlBlockModel<std::monostate, false>) <= 432 + 64,
	"P2b padding regressed vs P2a baseline beyond one cache line — "
	"define CONFLUX_WORK_RELAX_CONTROL_BLOCK_SIZE_GUARD to bypass");
	#if defined(_LIBCPP_VERSION)
inline constexpr std::size_t kControlBlockSizeBudget = 512;
	#elif defined(__GLIBCXX__)
inline constexpr std::size_t kControlBlockSizeBudget = 544;
	#else
inline constexpr std::size_t kControlBlockSizeBudget = 576;
	#endif
static_assert(
	sizeof(ControlBlockModel<std::monostate, false>) <= kControlBlockSizeBudget,
	"P2b padding regressed beyond per-platform budget — "
	"define CONFLUX_WORK_RELAX_CONTROL_BLOCK_SIZE_GUARD to bypass");
#endif
template<ControlCategory Category>
class BasicControl {
	std::shared_ptr<ControlBlockBase> core_{};

	template<work_value, ControlCategory>
	friend class BasicSource;
	template<work_value U>
	std::pair<BasicControl<ControlCategory::task>, BasicSource<U, ControlCategory::task>> friend make_task_control_source();
	template<work_value U>
	std::pair<BasicControl<ControlCategory::posted>,
	  BasicSource<U, ControlCategory::posted>> friend make_posted_control_source();
	template<work_value U>
	std::pair<BasicControl<ControlCategory::operation>,
	  BasicSource<U, ControlCategory::operation>> friend make_operation_control_source();

public:
	BasicControl() = default;
	explicit BasicControl(
		std::shared_ptr<ControlBlockBase> core) noexcept
		: core_{move(core)} {}
	[[nodiscard]] bool request_cancel() noexcept { return core_ ? core_->request_cancel() : false; }
	[[nodiscard]] std::stop_token stop_token() const noexcept {
		return core_ ? core_->stop_token() : std::stop_token{};
	}
	[[nodiscard]] bool cancel_requested() const noexcept { return core_ ? core_->cancel_requested() : false; }
	[[nodiscard]] bool ready() const noexcept { return core_ ? core_->ready() : false; }
	[[nodiscard]] WorkState state() const noexcept { return core_ ? core_->state() : WorkState::pending; }
	[[nodiscard]] bool can_join_with(
		CapabilityId id) const noexcept {
		return core_ && core_->can_join_with(id);
	}
	[[nodiscard]] std::optional<CapabilityId> required_capability() const noexcept {
		return core_ ? core_->required_capability() : nullopt;
	}
	[[nodiscard]] static constexpr ControlCategory category() noexcept { return Category; }
	[[nodiscard]] ReadyRegistrationResult try_set_on_ready(
		small_move_only_function<void()> fn) noexcept {
		if (!core_) {
			return {ReadyRegistration::empty, std::move(fn)};
		}
		return core_->try_set_on_ready(move(fn));
	}
	[[nodiscard]] ClearOnReadyStatus clear_on_ready() noexcept {
		if (!core_) {
			return ClearOnReadyStatus::not_armed;
		}
		return core_->clear_on_ready();
	}
	template<class F>
		requires std::invocable<F> && std::is_nothrow_invocable_v<F>
	void set_on_ready_or_run(
		F &&fn) noexcept {
		auto materialised = small_move_only_function<void()>{forward<F>(fn)};
		auto result = try_set_on_ready(move(materialised));
		switch (result.status) {
		case ReadyRegistration::installed: return;
		case ReadyRegistration::already_ready:
			if (result.rejected_fn) {
				result.rejected_fn();
			}
			return;
		case ReadyRegistration::already_installed:
		case ReadyRegistration::empty            : return;
		}
	}
};

} // namespace detail
template<ControlCategory Category>
using BasicControl = detail::BasicControl<Category>;

using TaskControl = BasicControl<ControlCategory::task>;
using PostedControl = BasicControl<ControlCategory::posted>;
using OperationControl = BasicControl<ControlCategory::operation>;
template<work_value T, ControlCategory Category>
class BasicSource {
	std::shared_ptr<detail::ControlBlockInterface<T>> state_{};
	explicit BasicSource(
		std::shared_ptr<detail::ControlBlockInterface<T>> state) noexcept
		: state_{move(state)} {}
	template<work_value U>
	std::pair<TaskControl, BasicSource<U, ControlCategory::task>> friend make_task_control_source();
	template<work_value U>
	std::pair<PostedControl, BasicSource<U, ControlCategory::posted>> friend make_posted_control_source();
	template<work_value U>
	std::pair<OperationControl, BasicSource<U, ControlCategory::operation>> friend make_operation_control_source();
	template<work_value U>
	std::pair<class BasicResult<U, ControlCategory::task>, BasicSource<U, ControlCategory::task>> friend make_task_source(
		struct SubmitOptions,
		std::source_location);
	template<work_value U, progress_capability Owner>
	std::pair<class BasicResult<U, ControlCategory::posted>, BasicSource<U, ControlCategory::posted>> friend make_posted_source(
		Owner &,
		struct PostOptions,
		std::source_location);
	template<work_value U, progress_capability Driver>
	std::pair<class BasicResult<U, ControlCategory::operation>,
	  BasicSource<
		  U,
		  ControlCategory::
			  operation>> friend make_operation_source(Driver &, struct OperationOptions, std::source_location);

public:
	BasicSource() = default;
	BasicSource(BasicSource &&) noexcept = default;
	BasicSource &operator =(BasicSource &&) noexcept = default;
	BasicSource(BasicSource const &) = delete;
	BasicSource &operator =(BasicSource const &) = delete;
	[[nodiscard]] static BasicSource from_state(
		std::shared_ptr<detail::ControlBlockInterface<T>> state) noexcept {
		return BasicSource{std::move(state)};
	}
	~BasicSource() noexcept {
		if (state_) {
			auto _ = state_->try_set_cancelled(CancelReason::abandoned, true);
		}
	}
	[[nodiscard]] bool try_set_value(
		Success<T> value) {
		return state_ ? state_->try_set_value(move(value)) : false;
	}
	[[nodiscard]] bool try_set_exception(
		std::exception_ptr error) {
		return state_ ? state_->try_set_exception(error) : false;
	}
	[[nodiscard]] bool try_set_cancelled(
		work_errc errc = work_errc::cancelled_requested) noexcept {
		return state_ ? state_->try_set_cancelled(errc_cancel_reason(errc), false) : false;
	}
	[[nodiscard]] bool try_set_error(
		std::error_code ec) {
		return state_ ? state_->try_set_error(ec) : false;
	}
	[[nodiscard]] bool try_set_error(
		std::error_code ec,
		std::string_view msg) noexcept {
		return state_ ? state_->try_set_error(ec, msg) : false;
	}
	[[nodiscard]] bool install_cancel_hook(
		detail::small_move_only_function<void(CancelReason)> fn) noexcept {
		return state_ ? state_->install_cancel_hook(move(fn)) : false;
	}
	[[nodiscard]] std::stop_token stop_token() const noexcept {
		return state_ ? state_->stop_token() : std::stop_token{};
	}
};
template<ControlCategory Category>
class BasicSource<void, Category> {
	std::shared_ptr<detail::ControlBlockInterface<void>> state_{};
	explicit BasicSource(
		std::shared_ptr<detail::ControlBlockInterface<void>> state) noexcept
		: state_{move(state)} {}
	template<work_value U>
	std::pair<TaskControl, BasicSource<U, ControlCategory::task>> friend make_task_control_source();
	template<work_value U>
	std::pair<PostedControl, BasicSource<U, ControlCategory::posted>> friend make_posted_control_source();
	template<work_value U>
	std::pair<OperationControl, BasicSource<U, ControlCategory::operation>> friend make_operation_control_source();
	template<work_value U>
	std::pair<class BasicResult<U, ControlCategory::task>, BasicSource<U, ControlCategory::task>> friend make_task_source(
		struct SubmitOptions,
		std::source_location);
	template<work_value U, progress_capability Owner>
	std::pair<class BasicResult<U, ControlCategory::posted>, BasicSource<U, ControlCategory::posted>> friend make_posted_source(
		Owner &,
		struct PostOptions,
		std::source_location);
	template<work_value U, progress_capability Driver>
	std::pair<class BasicResult<U, ControlCategory::operation>,
	  BasicSource<
		  U,
		  ControlCategory::
			  operation>> friend make_operation_source(Driver &, struct OperationOptions, std::source_location);

public:
	BasicSource() = default;
	BasicSource(BasicSource &&) noexcept = default;
	BasicSource &operator =(BasicSource &&) noexcept = default;
	BasicSource(BasicSource const &) = delete;
	BasicSource &operator =(BasicSource const &) = delete;
	[[nodiscard]] static BasicSource from_state(
		std::shared_ptr<detail::ControlBlockInterface<void>> state) noexcept {
		return BasicSource{std::move(state)};
	}
	~BasicSource() noexcept {
		if (state_) {
			auto _ = state_->try_set_cancelled(CancelReason::abandoned, true);
		}
	}
	[[nodiscard]] bool try_set_value(
		Success<void> value = Success<void>{}) noexcept {
		return state_ ? state_->try_set_value(value) : false;
	}
	[[nodiscard]] bool try_set_exception(
		std::exception_ptr const &error) {
		return state_ ? state_->try_set_exception(error) : false;
	}
	[[nodiscard]] bool try_set_cancelled(
		work_errc errc = work_errc::cancelled_requested) noexcept {
		return state_ ? state_->try_set_cancelled(errc_cancel_reason(errc), false) : false;
	}
	[[nodiscard]] bool try_set_error(
		std::error_code ec) {
		return state_ ? state_->try_set_error(ec) : false;
	}
	[[nodiscard]] bool try_set_error(
		std::error_code ec,
		std::string_view msg) noexcept {
		return state_ ? state_->try_set_error(ec, msg) : false;
	}
	[[nodiscard]] bool install_cancel_hook(
		detail::small_move_only_function<void(CancelReason)> fn) noexcept {
		return state_ ? state_->install_cancel_hook(move(fn)) : false;
	}
	[[nodiscard]] std::stop_token stop_token() const noexcept {
		return state_ ? state_->stop_token() : std::stop_token{};
	}
};
template<work_value T>
using TaskSource = BasicSource<T, ControlCategory::task>;

template<work_value T>
using PostedSource = BasicSource<T, ControlCategory::posted>;

template<work_value T>
using OperationSource = BasicSource<T, ControlCategory::operation>;
struct SubmitOptions {
	bool enable_cancellation = true;
};
struct PostOptions {
	bool enable_cancellation = true;
};
struct OperationOptions {
	bool enable_cancellation = true;
};

template<ControlCategory Category>
struct control_handle_for;
template<>
struct control_handle_for<ControlCategory::task> {
	using type = TaskControl;
};
template<>
struct control_handle_for<ControlCategory::posted> {
	using type = PostedControl;
};
template<>
struct control_handle_for<ControlCategory::operation> {
	using type = OperationControl;
};

template<class Sink, class T>
concept abandon_sink = std::is_nothrow_move_constructible_v<Sink>
					&& (std::is_nothrow_invocable_v<Sink &, Outcome<T> const &>
						|| (std::is_nothrow_invocable_v<Sink &, Failure const &>
							&& std::is_nothrow_invocable_v<Sink &, Cancelled const &>));
struct drop_on_abandon {
	void operator ()(
		Failure const &) const noexcept {}
	void operator ()(
		Cancelled const &) const noexcept {}
};
// HACK: do not give these cold helpers a `source_location loc = current()` default arg.
// When called from a templated body (e.g. join<T>) instantiated in a consumer TU, GCC 15
// emits a local `.Lsrc_locN` reference to source_location data that is never defined,
// producing an undefined-reference link error. Callers must pass loc explicitly — and
// templated callers must in turn capture it from a non-template default arg of their own.
[[noreturn]] [[gnu::cold]] [[gnu::noinline]] void raise_join_lifetime_violation(
	std::source_location loc) {
	throw JoinError{JoinError::reason::lifetime_violation, loc};
}
[[noreturn]] [[gnu::cold]] [[gnu::noinline]] void raise_join_consumed_handle(
	std::source_location loc) {
	throw JoinError{JoinError::reason::consumed_handle, loc};
}
[[noreturn]] [[gnu::cold]] [[gnu::noinline]] void raise_join_capability_mismatch(
	std::optional<CapabilityId> expected,
	CapabilityId actual,
	std::source_location loc) {
	throw JoinError{JoinError::reason::capability_mismatch, expected, actual, loc};
}
[[noreturn]] [[gnu::cold]] [[gnu::noinline]] void raise_join_not_ready(
	std::source_location loc) {
	throw JoinError{JoinError::reason::not_ready, loc};
}
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
		auto result = state_->try_set_on_ready(small_move_only_function<void()>{[h]() noexcept { h.resume(); }});
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
		return move(*outcome).visit([](auto &&arm) -> std::conditional_t<std::is_void_v<T>, void, T> {
				using arm_t = std::remove_cvref_t<decltype(arm)>;
				if constexpr (same_as<arm_t, Success<T>>) {
					if constexpr (!std::is_void_v<T>) {
						return move(arm.value);
					}
				} else if constexpr (same_as<arm_t, Failure>) {
					rethrow_exception(arm.error);
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
		auto result = state_->try_set_on_ready(small_move_only_function<void()>{[h]() noexcept { h.resume(); }});
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
		return move(*outcome);
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
concept awaits_outcome = awaitable<A> && same_as<await_resume_t<A>, Outcome<T>>;

} // namespace detail
template<work_value T, ControlCategory Category>
class BasicResult {
	std::shared_ptr<detail::ControlBlockInterface<T>> state_{};
	join_state state_js_ = join_state::empty;
	explicit BasicResult(
		std::shared_ptr<detail::ControlBlockInterface<T>> state) noexcept
		: state_{move(state)}
		, state_js_{state_ ? join_state::joinable : join_state::empty} {}
	[[nodiscard]] std::shared_ptr<detail::ControlBlockInterface<T>> consume(
		join_state target) noexcept {
		if (state_js_ != join_state::joinable) {
			return {};
		}
		state_js_ = target;
		return move(state_);
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
		return BasicResult{move(state)};
	}
	BasicResult(
		BasicResult &&other) noexcept
		: state_{move(other.state_)}
		, state_js_{exchange(other.state_js_, join_state::empty)} {}
	BasicResult &operator =(
		BasicResult &&other) noexcept {
		if (this != &other) {
			if (state_js_ == join_state::joinable) {
				detach_noexcept();
			}
			state_ = move(other.state_);
			state_js_ = exchange(other.state_js_, join_state::empty);
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
			abandon_impl(move(*this), forward<Sink>(sink));
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
		void unhandled_exception() noexcept { auto _ = this->state_->try_set_exception(current_exception()); }
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
		: inner_{move(task)}
		, origin_{origin} {}
	JoinTask(JoinTask &&) noexcept = default;
	JoinTask &operator =(
		JoinTask &&other) noexcept {
		if (this != &other) {
			if (inner_.state() == join_state::joinable) {
				std::terminate();
			}
			inner_ = move(other.inner_);
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
		auto out = move(inner_);
		inner_ = Task<T>{};
		return out;
	}
	[[nodiscard]] std::source_location origin() const noexcept { return origin_; }
	[[nodiscard("JoinTask must be consumed: use co_await std::move(jt) or co_await jt.consume()")]] auto
	operator co_await() & = delete;
	[[nodiscard]] auto operator co_await() && noexcept { return move(inner_).operator co_await(); }
	[[nodiscard]] auto outcome() && noexcept { return move(inner_).outcome(); }
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
	auto task = invoke(forward<Fn>(fn));
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
	return require_join(spawn(forward<Fn>(fn), loc), loc);
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
		: state_{move(state)}
		, live_{static_cast<bool>(state_)} {}
	[[nodiscard]] std::shared_ptr<detail::ControlBlockInterface<T>> consume() noexcept {
		if (!live_) {
			return {};
		}
		live_ = false;
		return move(state_);
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
		: state_{move(other.state_)}
		, live_{exchange(other.live_, false)} {}
	BasicJoinHandle &operator =(
		BasicJoinHandle &&other) noexcept {
		if (this != &other) {
			if (live_ && state_) {
				std::terminate();
			}
			state_ = move(other.state_);
			live_ = exchange(other.live_, false);
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
	requires detail::awaits_outcome<decltype(move(h).outcome()), typename H::value_type>;
};
template<class Sink, work_value T>
[[nodiscard]] detail::small_move_only_function<void(Outcome<T> const &)> make_abandon_dispatch_sink(
	Sink &&sink) noexcept {
	using sink_t = std::remove_cvref_t<Sink>;
	if constexpr (std::is_nothrow_invocable_v<sink_t &, Outcome<T> const &>) {
		return detail::small_move_only_function<void(Outcome<T> const &)>{forward<Sink>(sink)};
	} else {
		return detail::small_move_only_function<void(Outcome<T> const &)>{
			[sink = forward<Sink>(sink)](Outcome<T> const &outcome) mutable noexcept {
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
	state->install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(forward<Sink>(sink)));
}
template<work_value T, ControlCategory Category, class Sink>
void abandon_impl(
	BasicJoinHandle<T, Category> &&h,
	Sink &&sink) noexcept {
	auto state = h.consume_for_abandon();
	if (!state) {
		std::terminate();
	}
	state->install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(forward<Sink>(sink)));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	Task<T> &&task,
	Sink &&sink) noexcept {
	abandon_impl(move(task), forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	Posted<T> &&posted,
	Sink &&sink) noexcept {
	abandon_impl(move(posted), forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	Operation<T> &&op,
	Sink &&sink) noexcept {
	abandon_impl(move(op), forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	TaskJoinHandle<T> &&h,
	Sink &&sink) noexcept {
	abandon_impl(move(h), forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	PostedJoinHandle<T> &&h,
	Sink &&sink) noexcept {
	abandon_impl(move(h), forward<Sink>(sink));
}
template<work_value T, class Sink>
	requires abandon_sink<Sink, T>
void abandon_to(
	OperationJoinHandle<T> &&h,
	Sink &&sink) noexcept {
	abandon_impl(move(h), forward<Sink>(sink));
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
	auto result = state->try_install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(forward<Sink>(sink)));
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
	return state->try_install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(forward<Sink>(sink)));
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
	return state->try_install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(forward<Sink>(sink)));
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
	lock_guard const lk{s.mtx};
	s.fn = detail::small_move_only_function<void(std::source_location, OutcomeKind, std::exception_ptr)>{forward<Fn>(fn)};
	s.installed.store(true, memory_order_release);
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
		: value_{move(other.value_)}
		, sink_{move(other.sink_)}
		, armed_{exchange(other.armed_, false)} {}
	scoped_abandon &operator =(scoped_abandon &&) = delete;
	scoped_abandon(scoped_abandon const &) = delete;
	scoped_abandon &operator =(scoped_abandon const &) = delete;
	~scoped_abandon() noexcept {
		if (armed_ && value_) {
			abandon_to(move(*value_), move(*sink_));
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
	return scoped_abandon<result_t, drop_on_abandon>{forward<R>(value), drop_on_abandon{}};
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
	return {Posted<T>::from_state(state, loc), PostedSource<T>::from_state(move(state))};
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
	return {Operation<T>::from_state(state, loc), OperationSource<T>::from_state(move(state))};
}
template<work_value T, ControlCategory C>
[[nodiscard]] BasicJoinHandle<T, C> into_join_handle(
	BasicResult<T, C> &&result) noexcept {
	return BasicJoinHandle<T, C>::adopt(move(result));
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
	return move(*outcome);
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
	if (task.state() != join_state::joinable) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	if (!task.control().ready()) {
		return nullopt;
	}
	auto state = task.consume_for_join();
	if (!state) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	return std::optional<Outcome<T>>{detail::take_ready_outcome_or_throw(move(state), loc)};
}
template<progress_capability Owner, work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	Owner &owner,
	Posted<T> &&posted,
	std::source_location loc = std::source_location::current()) {
	if (posted.state() != join_state::joinable) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	auto control = posted.control();
	if (!control.can_join_with(capability_id(owner))) [[unlikely]] {
		raise_join_capability_mismatch(control.required_capability(), capability_id(owner), loc);
	}
	if (!control.ready()) {
		return nullopt;
	}
	auto state = posted.consume_for_join();
	if (!state) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	return std::optional<Outcome<T>>{detail::take_ready_outcome_or_throw(move(state), loc)};
}
template<progress_capability Driver, work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	Driver &driver,
	Operation<T> &&op,
	std::source_location loc = std::source_location::current()) {
	if (op.state() != join_state::joinable) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	auto control = op.control();
	if (!control.can_join_with(capability_id(driver))) [[unlikely]] {
		raise_join_capability_mismatch(control.required_capability(), capability_id(driver), loc);
	}
	if (!control.ready()) {
		return nullopt;
	}
	auto state = op.consume_for_join();
	if (!state) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	return std::optional<Outcome<T>>{detail::take_ready_outcome_or_throw(move(state), loc)};
}
template<work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	TaskJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	if (!h) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	if (!h.control().ready()) {
		return nullopt;
	}
	auto state = h.consume_for_join();
	if (!state) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	return std::optional<Outcome<T>>{detail::take_ready_outcome_or_throw(move(state), loc)};
}
template<progress_capability Owner, work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	Owner &owner,
	PostedJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	if (!h) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	auto control = h.control();
	if (!control.can_join_with(capability_id(owner))) [[unlikely]] {
		raise_join_capability_mismatch(control.required_capability(), capability_id(owner), loc);
	}
	if (!control.ready()) {
		return nullopt;
	}
	auto state = h.consume_for_join();
	if (!state) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	return std::optional<Outcome<T>>{detail::take_ready_outcome_or_throw(move(state), loc)};
}
template<progress_capability Driver, work_value T>
[[nodiscard]] std::optional<Outcome<T>> try_join_ready(
	Driver &driver,
	OperationJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	if (!h) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	auto control = h.control();
	if (!control.can_join_with(capability_id(driver))) [[unlikely]] {
		raise_join_capability_mismatch(control.required_capability(), capability_id(driver), loc);
	}
	if (!control.ready()) {
		return nullopt;
	}
	auto state = h.consume_for_join();
	if (!state) [[unlikely]] {
		raise_join_lifetime_violation(loc);
	}
	return std::optional<Outcome<T>>{detail::take_ready_outcome_or_throw(move(state), loc)};
}
template<work_value T>
[[nodiscard]] Outcome<T> join_ready(
	Task<T> &&task,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(move(task), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return move(*outcome);
}
template<progress_capability Owner, work_value T>
[[nodiscard]] Outcome<T> join_ready(
	Owner &owner,
	Posted<T> &&posted,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(owner, move(posted), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return move(*outcome);
}
template<progress_capability Driver, work_value T>
[[nodiscard]] Outcome<T> join_ready(
	Driver &driver,
	Operation<T> &&op,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(driver, move(op), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return move(*outcome);
}
template<work_value T>
[[nodiscard]] Outcome<T> join_ready(
	TaskJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(move(h), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return move(*outcome);
}
template<progress_capability Owner, work_value T>
[[nodiscard]] Outcome<T> join_ready(
	Owner &owner,
	PostedJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(owner, move(h), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return move(*outcome);
}
template<progress_capability Driver, work_value T>
[[nodiscard]] Outcome<T> join_ready(
	Driver &driver,
	OperationJoinHandle<T> &&h,
	std::source_location loc = std::source_location::current()) {
	auto outcome = try_join_ready(driver, move(h), loc);
	if (!outcome) [[unlikely]] {
		raise_join_not_ready(loc);
	}
	return move(*outcome);
}

template<work_value T>
[[nodiscard]] Outcome<T> blocking_join(
	Task<T> &&task,
	std::source_location loc = std::source_location::current()) {
	return detail::blocking_join_compatibility_adapter(task.consume_for_join(), nullopt, loc);
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
	return detail::blocking_join_compatibility_adapter(h.consume_for_join(), nullopt, loc);
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
	return root::value(blocking_join(move(task)));
}
template<progress_capability Owner, work_value T>
[[nodiscard]] T value(
	Owner &owner,
	Posted<T> &&posted) {
	return root::value(blocking_join(owner, move(posted)));
}
template<progress_capability Driver, work_value T>
[[nodiscard]] T value(
	Driver &driver,
	Operation<T> &&op) {
	return root::value(blocking_join(driver, move(op)));
}
template<work_value T>
[[nodiscard]] T value(
	TaskJoinHandle<T> &&h) {
	return root::value(blocking_join(move(h)));
}
template<progress_capability Owner, work_value T>
[[nodiscard]] T value(
	Owner &owner,
	PostedJoinHandle<T> &&h) {
	return root::value(blocking_join(owner, move(h)));
}
template<progress_capability Driver, work_value T>
[[nodiscard]] T value(
	Driver &driver,
	OperationJoinHandle<T> &&h) {
	return root::value(blocking_join(driver, move(h)));
}
inline void value(
	Task<void> &&task) {
	root::value(blocking_join(move(task)));
}
template<progress_capability Owner>
inline void value(
	Owner &owner,
	Posted<void> &&posted) {
	root::value(blocking_join(owner, move(posted)));
}
template<progress_capability Driver>
inline void value(
	Driver &driver,
	Operation<void> &&op) {
	root::value(blocking_join(driver, move(op)));
}
inline void value(
	TaskJoinHandle<void> &&h) {
	root::value(blocking_join(move(h)));
}
template<progress_capability Owner>
inline void value(
	Owner &owner,
	PostedJoinHandle<void> &&h) {
	root::value(blocking_join(owner, move(h)));
}
template<progress_capability Driver>
inline void value(
	Driver &driver,
	OperationJoinHandle<void> &&h) {
	root::value(blocking_join(driver, move(h)));
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
