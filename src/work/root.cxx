module;

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
import conflux.small_function;

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
	explicit WorkError(
		std::string const &msg)
		: std::runtime_error{msg} {}
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
	g_control_block_allocations.fetch_add(1, std::memory_order_relaxed);
}
inline void note_control_block_deallocation() noexcept {
	g_control_block_deallocations.fetch_add(1, std::memory_order_relaxed);
}
inline void note_coroutine_frame_allocation() noexcept {
	g_coroutine_frame_allocations.fetch_add(1, std::memory_order_relaxed);
}
inline void note_coroutine_frame_deallocation() noexcept {
	g_coroutine_frame_deallocations.fetch_add(1, std::memory_order_relaxed);
}
[[nodiscard]] inline TaskAllocationStats task_allocation_stats_impl() noexcept {
	return {
		.control_block_allocations = g_control_block_allocations.load(std::memory_order_relaxed),
		.control_block_deallocations = g_control_block_deallocations.load(std::memory_order_relaxed),
		.coroutine_frame_allocations = g_coroutine_frame_allocations.load(std::memory_order_relaxed),
		.coroutine_frame_deallocations = g_coroutine_frame_deallocations.load(std::memory_order_relaxed),
	};
}
inline void reset_task_allocation_stats_impl() noexcept {
	g_control_block_allocations.store(0, std::memory_order_relaxed);
	g_control_block_deallocations.store(0, std::memory_order_relaxed);
	g_coroutine_frame_allocations.store(0, std::memory_order_relaxed);
	g_coroutine_frame_deallocations.store(0, std::memory_order_relaxed);
}
#else
inline void note_control_block_allocation() noexcept {}
inline void note_control_block_deallocation() noexcept {}
inline void note_coroutine_frame_allocation() noexcept {}
inline void note_coroutine_frame_deallocation() noexcept {}
[[nodiscard]] inline TaskAllocationStats task_allocation_stats_impl() noexcept {
	return {};
}
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
	std::same_as<std::remove_cv_t<T>, void> || (!std::is_reference_v<T> && std::is_nothrow_move_constructible_v<T>);
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
		: storage_{std::in_place_type<success_t>, std::move(success)} {}
	Outcome(
		Failure failure) noexcept
		: storage_{std::in_place_type<Failure>, std::move(failure)} {}
	Outcome(
		Cancelled cancelled) noexcept
		: storage_{std::in_place_type<Cancelled>, std::move(cancelled)} {}
	Outcome(Outcome const &) = default;
	Outcome(
		Outcome &&other) noexcept
		: storage_{std::move(other.storage_)} {}
	Outcome &operator =(
		Outcome const &other)
		requires std::copy_constructible<storage_t>
	{
		if (this != std::addressof(other)) {
			Outcome staged{other};
			storage_ = std::move(staged.storage_);
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
			  && std::same_as<std::invoke_result_t<F &, success_t &>, std::invoke_result_t<F &, Failure &>>
			  && std::same_as<std::invoke_result_t<F &, success_t &>, std::invoke_result_t<F &, Cancelled &>>
	auto visit(
		F &&f)
		& noexcept(
			std::is_nothrow_invocable_v<F &, success_t &>
			&& std::is_nothrow_invocable_v<F &, Failure &>
			&& std::is_nothrow_invocable_v<F &, Cancelled &>) -> std::invoke_result_t<F &, success_t &> {
		switch (kind()) {
		case OutcomeKind::success  : return std::invoke(f, success());
		case OutcomeKind::failure  : return std::invoke(f, failure());
		case OutcomeKind::cancelled: return std::invoke(f, cancelled());
		}
		std::unreachable();
	}
	template<typename F>
		requires std::invocable<F &, success_t const &>
			  && std::invocable<F &, Failure const &>
			  && std::invocable<F &, Cancelled const &>
			  && std::same_as<std::invoke_result_t<F &, success_t const &>, std::invoke_result_t<F &, Failure const &>>
			  && std::
					 same_as<std::invoke_result_t<F &, success_t const &>, std::invoke_result_t<F &, Cancelled const &>>
	auto visit(
		F &&f)
		const & noexcept(
			std::is_nothrow_invocable_v<F &, success_t const &>
			&& std::is_nothrow_invocable_v<F &, Failure const &>
			&& std::is_nothrow_invocable_v<F &, Cancelled const &>) -> std::invoke_result_t<F &, success_t const &> {
		switch (kind()) {
		case OutcomeKind::success  : return std::invoke(f, success());
		case OutcomeKind::failure  : return std::invoke(f, failure());
		case OutcomeKind::cancelled: return std::invoke(f, cancelled());
		}
		std::unreachable();
	}
	template<typename F>
		requires std::invocable<F &, success_t &&>
			  && std::invocable<F &, Failure &&>
			  && std::invocable<F &, Cancelled &&>
			  && std::same_as<std::invoke_result_t<F &, success_t &&>, std::invoke_result_t<F &, Failure &&>>
			  && std::same_as<std::invoke_result_t<F &, success_t &&>, std::invoke_result_t<F &, Cancelled &&>>
	auto visit(
		F &&f)
		&& noexcept(
			std::is_nothrow_invocable_v<F &, success_t &&>
			&& std::is_nothrow_invocable_v<F &, Failure &&>
			&& std::is_nothrow_invocable_v<F &, Cancelled &&>) -> std::invoke_result_t<F &, success_t &&> {
		switch (kind()) {
		case OutcomeKind::success  : return std::invoke(f, std::move(*this).success());
		case OutcomeKind::failure  : return std::invoke(f, std::move(*this).failure());
		case OutcomeKind::cancelled: return std::invoke(f, std::move(*this).cancelled());
		}
		std::unreachable();
	}
	[[nodiscard]] T &value() & {
		switch (kind()) {
		case OutcomeKind::success  : return success().value;
		case OutcomeKind::failure  : std::rethrow_exception(failure().error);
		case OutcomeKind::cancelled: throw CancelledError{cancelled().reason};
		}
		std::unreachable();
	}
	[[nodiscard]] T const &value() const & {
		switch (kind()) {
		case OutcomeKind::success  : return success().value;
		case OutcomeKind::failure  : std::rethrow_exception(failure().error);
		case OutcomeKind::cancelled: throw CancelledError{cancelled().reason};
		}
		std::unreachable();
	}
	[[nodiscard]] T value() && {
		switch (kind()) {
		case OutcomeKind::success  : return std::move(success().value);
		case OutcomeKind::failure  : std::rethrow_exception(std::move(*this).failure().error);
		case OutcomeKind::cancelled: throw CancelledError{cancelled().reason};
		}
		std::unreachable();
	}
	template<class OnSuccess, class OnFailure, class OnCancelled>
		requires std::invocable<OnSuccess, T &&>
			  && std::invocable<OnFailure, Failure const &>
			  && std::invocable<OnCancelled, Cancelled const &>
			  && std::same_as<std::invoke_result_t<OnSuccess, T &&>, std::invoke_result_t<OnFailure, Failure const &>>
			  && std::same_as<
					 std::invoke_result_t<OnSuccess, T &&>,
					 std::invoke_result_t<OnCancelled, Cancelled const &>>
	auto match(
		OnSuccess &&on_success,
		OnFailure &&on_failure,
		OnCancelled &&on_cancelled) && -> std::invoke_result_t<OnSuccess, T &&> {
		switch (kind()) {
		case OutcomeKind::success  : return std::invoke(std::forward<OnSuccess>(on_success), std::move(success().value));
		case OutcomeKind::failure  : return std::invoke(std::forward<OnFailure>(on_failure), failure());
		case OutcomeKind::cancelled: return std::invoke(std::forward<OnCancelled>(on_cancelled), cancelled());
		}
		std::unreachable();
	}
	template<class OnSuccess, class OnFailure, class OnCancelled>
		requires std::invocable<OnSuccess, T const &>
			  && std::invocable<OnFailure, Failure const &>
			  && std::invocable<OnCancelled, Cancelled const &>
			  && std::same_as<
					 std::invoke_result_t<OnSuccess, T const &>,
					 std::invoke_result_t<OnFailure, Failure const &>>
			  && std::same_as<
					 std::invoke_result_t<OnSuccess, T const &>,
					 std::invoke_result_t<OnCancelled, Cancelled const &>>
	auto match(
		OnSuccess &&on_success,
		OnFailure &&on_failure,
		OnCancelled &&on_cancelled) const & -> std::invoke_result_t<OnSuccess, T const &> {
		switch (kind()) {
		case OutcomeKind::success  : return std::invoke(std::forward<OnSuccess>(on_success), success().value);
		case OutcomeKind::failure  : return std::invoke(std::forward<OnFailure>(on_failure), failure());
		case OutcomeKind::cancelled: return std::invoke(std::forward<OnCancelled>(on_cancelled), cancelled());
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
		: storage_{std::in_place_type<Failure>, std::move(failure)} {}
	Outcome(
		Cancelled cancelled) noexcept
		: storage_{std::in_place_type<Cancelled>, cancelled} {}
	Outcome(Outcome const &) = default;
	Outcome(
		Outcome &&other) noexcept
		: storage_{std::move(other.storage_)} {}
	Outcome &operator =(
		Outcome const &other) {
		if (this != std::addressof(other)) {
			Outcome staged{other};
			storage_ = std::move(staged.storage_);
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
			  && std::same_as<std::invoke_result_t<F &, success_t &>, std::invoke_result_t<F &, Failure &>>
			  && std::same_as<std::invoke_result_t<F &, success_t &>, std::invoke_result_t<F &, Cancelled &>>
	auto visit(
		F &&f)
		& noexcept(
			std::is_nothrow_invocable_v<F &, success_t &>
			&& std::is_nothrow_invocable_v<F &, Failure &>
			&& std::is_nothrow_invocable_v<F &, Cancelled &>) -> std::invoke_result_t<F &, success_t &> {
		switch (kind()) {
		case OutcomeKind::success  : return std::invoke(f, success());
		case OutcomeKind::failure  : return std::invoke(f, failure());
		case OutcomeKind::cancelled: return std::invoke(f, cancelled());
		}
		std::unreachable();
	}
	template<typename F>
		requires std::invocable<F &, success_t const &>
			  && std::invocable<F &, Failure const &>
			  && std::invocable<F &, Cancelled const &>
			  && std::same_as<std::invoke_result_t<F &, success_t const &>, std::invoke_result_t<F &, Failure const &>>
			  && std::
					 same_as<std::invoke_result_t<F &, success_t const &>, std::invoke_result_t<F &, Cancelled const &>>
	auto visit(
		F &&f)
		const & noexcept(
			std::is_nothrow_invocable_v<F &, success_t const &>
			&& std::is_nothrow_invocable_v<F &, Failure const &>
			&& std::is_nothrow_invocable_v<F &, Cancelled const &>) -> std::invoke_result_t<F &, success_t const &> {
		switch (kind()) {
		case OutcomeKind::success  : return std::invoke(f, success());
		case OutcomeKind::failure  : return std::invoke(f, failure());
		case OutcomeKind::cancelled: return std::invoke(f, cancelled());
		}
		std::unreachable();
	}
	template<typename F>
		requires std::invocable<F &, success_t &&>
			  && std::invocable<F &, Failure &&>
			  && std::invocable<F &, Cancelled &&>
			  && std::same_as<std::invoke_result_t<F &, success_t &&>, std::invoke_result_t<F &, Failure &&>>
			  && std::same_as<std::invoke_result_t<F &, success_t &&>, std::invoke_result_t<F &, Cancelled &&>>
	auto visit(
		F &&f)
		&& noexcept(
			std::is_nothrow_invocable_v<F &, success_t &&>
			&& std::is_nothrow_invocable_v<F &, Failure &&>
			&& std::is_nothrow_invocable_v<F &, Cancelled &&>) -> std::invoke_result_t<F &, success_t &&> {
		switch (kind()) {
		case OutcomeKind::success  : return std::invoke(f, std::move(*this).success());
		case OutcomeKind::failure  : return std::invoke(f, std::move(*this).failure());
		case OutcomeKind::cancelled: return std::invoke(f, std::move(*this).cancelled());
		}
		std::unreachable();
	}
	void value() const {
		switch (kind()) {
		case OutcomeKind::success  : return;
		case OutcomeKind::failure  : std::rethrow_exception(failure().error);
		case OutcomeKind::cancelled: throw CancelledError{cancelled().reason};
		}
		std::unreachable();
	}
	template<class OnSuccess, class OnFailure, class OnCancelled>
		requires std::invocable<OnSuccess>
			  && std::invocable<OnFailure, Failure const &>
			  && std::invocable<OnCancelled, Cancelled const &>
			  && std::same_as<std::invoke_result_t<OnSuccess>, std::invoke_result_t<OnFailure, Failure const &>>
			  && std::same_as<std::invoke_result_t<OnSuccess>, std::invoke_result_t<OnCancelled, Cancelled const &>>
	auto match(
		OnSuccess &&on_success,
		OnFailure &&on_failure,
		OnCancelled &&on_cancelled) const -> std::invoke_result_t<OnSuccess> {
		switch (kind()) {
		case OutcomeKind::success  : return std::invoke(std::forward<OnSuccess>(on_success));
		case OutcomeKind::failure  : return std::invoke(std::forward<OnFailure>(on_failure), failure());
		case OutcomeKind::cancelled: return std::invoke(std::forward<OnCancelled>(on_cancelled), cancelled());
		}
		std::unreachable();
	}
};
template<work_value T>
[[nodiscard]] T value(
	Outcome<T> &&outcome) {
	return std::move(outcome).visit([](auto &&arm) -> T {
		using arm_t = std::remove_cvref_t<decltype(arm)>;
		if constexpr (std::same_as<arm_t, Success<T>>) {
			return std::move(arm.value);
		} else if constexpr (std::same_as<arm_t, Failure>) {
			throw FailureError{arm.error};
		} else {
			throw CancelledError{arm.reason};
		}
	});
}
inline void value(
	Outcome<void> &&outcome) {
	std::move(outcome).visit([](auto &&arm) -> void {
		using arm_t = std::remove_cvref_t<decltype(arm)>;
		if constexpr (std::same_as<arm_t, Success<void>>) {
			return;
		} else if constexpr (std::same_as<arm_t, Failure>) {
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
	{ capability_id(c) } noexcept -> std::same_as<CapabilityId>;
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

struct ReadyRegistrationResult {
	ReadyRegistration status;
	::conflux::detail::small_move_only_function<void()> rejected_fn;
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
	std::mutex mtx;
	::conflux::detail::small_move_only_function<void(std::source_location, OutcomeKind, std::exception_ptr)> fn;
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
	if (!s.installed.load(std::memory_order_acquire)) {
		return;
	}
	std::lock_guard const lk{s.mtx};
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
	virtual bool install_cancel_hook(::conflux::detail::small_move_only_function<void(CancelReason)> fn) noexcept = 0;
	[[nodiscard]] virtual ReadyRegistrationResult
	try_set_on_ready(::conflux::detail::small_move_only_function<void()> fn) noexcept = 0;
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
		return try_set_exception(std::make_exception_ptr(std::system_error(ec)));
	}
	[[nodiscard]] virtual bool try_set_error(
		std::error_code ec,
		std::string_view msg) noexcept {
		try {
			return try_set_exception(std::make_exception_ptr(std::system_error(ec, std::string{msg})));
		} catch (...) { return try_set_error(ec); }
	}
	[[nodiscard]] virtual bool try_set_cancelled(CancelReason reason, bool allow_abandoned) noexcept = 0;
	[[nodiscard]] virtual Outcome<T> compatibility_blocking_take_outcome() = 0;
	[[nodiscard]] virtual std::optional<Outcome<T>> try_take_ready_outcome() = 0;
	virtual void
	install_abandon_sink(::conflux::detail::small_move_only_function<void(Outcome<T> const &)> sink) noexcept = 0;
	[[nodiscard]] virtual AbandonStatus
	try_install_abandon_sink(::conflux::detail::small_move_only_function<void(Outcome<T> const &)> sink) noexcept = 0;
};
template<work_value T, bool EnableCancellation>
class ControlBlockModel final : public ControlBlockInterface<T> {
	std::atomic<TerminalState> terminal_state_{TerminalState::none};
	std::atomic<ReadyHookState> ready_hook_state_{ReadyHookState::open};
	std::atomic<bool> cancel_requested_{false};
	std::atomic<bool> terminal_claimed_{false};
	// P2b false-sharing fix: hot atomics above land on one cache line;
	// alignas(64) on mtx_ starts cold lock/cv on a fresh line.
	alignas(64) mutable std::mutex mtx_{};
	std::condition_variable cv_{};
	std::optional<Outcome<T>> outcome_{};
	::conflux::detail::small_move_only_function<void()> on_ready_fn_{};
	::conflux::detail::small_move_only_function<void(CancelReason)> hook_fn_{};
	::conflux::detail::small_move_only_function<void(Outcome<T> const &)> abandon_sink_{};
	bool hook_installed_ = false, hook_claimed_ = false, abandoned_ = false;
	[[no_unique_address]] std::conditional_t<EnableCancellation, std::stop_source, std::monostate> stop_source_{};
	std::atomic<bool> requires_capability_{false};
	std::atomic<void const *> required_capability_address_{nullptr};
	std::atomic<void const *> required_capability_type_tag_{nullptr};
	[[nodiscard]] ::conflux::detail::small_move_only_function<void(CancelReason)>
	claim_requested_hook_if_present() noexcept {
		std::scoped_lock const lk{mtx_};
		if (!hook_installed_ || hook_claimed_) {
			return {};
		}
		if (terminal_claimed_.load(std::memory_order_acquire)) {
			return {};
		}
		hook_claimed_ = true;
		return std::move(hook_fn_);
	}
	void invoke_requested_hook_if_needed() noexcept {
		auto fn = claim_requested_hook_if_present();
		if (!fn) {
			return;
		}
		if (terminal_claimed_.load(std::memory_order_acquire)) {
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
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			ready_hook_state_.store(ReadyHookState::terminal, std::memory_order_release);
			return;
		}
		if (prev == ReadyHookState::armed) {
			::conflux::detail::small_move_only_function<void()> fn{};
			{
				std::unique_lock lk{mtx_};
				fn = std::move(on_ready_fn_);
				ready_hook_state_.store(ReadyHookState::terminal, std::memory_order_release);
			}
			if (fn) {
				fn();
			}
		} else if (prev == ReadyHookState::disarmed) {
			std::unique_lock lk{mtx_};
			ready_hook_state_.store(ReadyHookState::terminal, std::memory_order_release);
		}
	}
	[[nodiscard]] bool try_claim_terminal() noexcept {
		bool expected = false;
		return terminal_claimed_
			.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
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
		::conflux::detail::small_move_only_function<void(Outcome<T> const &)> sink{};
		std::optional<Outcome<T>> local{};
		{
			std::unique_lock lk{mtx_};
			if (!abandoned_ || !abandon_sink_ || !outcome_) {
				return;
			}
			sink = std::move(abandon_sink_);
			abandon_sink_ = nullptr;
			local.emplace(std::move(*outcome_));
			outcome_.reset();
		}
		try {
			sink(*local);
		} catch (...) { std::terminate(); }
	}

public:
	void set_required_capability(
		CapabilityId id) noexcept override {
		required_capability_address_.store(id.address, std::memory_order_relaxed);
		required_capability_type_tag_.store(id.type_tag, std::memory_order_relaxed);
		requires_capability_.store(true, std::memory_order_release);
	}
	[[nodiscard]] bool can_join_with(
		CapabilityId id) const noexcept override {
		if (!requires_capability_.load(std::memory_order_acquire)) {
			return true;
		}
		CapabilityId const expected{
			.address = required_capability_address_.load(std::memory_order_relaxed),
			.type_tag = required_capability_type_tag_.load(std::memory_order_relaxed),
		};
		return expected == id;
	}
	[[nodiscard]] std::optional<CapabilityId> required_capability() const noexcept override {
		if (!requires_capability_.load(std::memory_order_acquire)) {
			return std::nullopt;
		}
		return CapabilityId{
			.address = required_capability_address_.load(std::memory_order_relaxed),
			.type_tag = required_capability_type_tag_.load(std::memory_order_relaxed),
		};
	}
	[[nodiscard]] bool request_cancel() noexcept override {
		if (terminal_claimed_.load(std::memory_order_acquire)) {
			return false;
		}

		bool expected = false;
		if (!cancel_requested_
				 .compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
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
		return cancel_requested_.load(std::memory_order_acquire);
	}
	[[nodiscard]] bool ready() const noexcept override {
		return terminal_state_.load(std::memory_order_acquire) != TerminalState::none;
	}
	[[nodiscard]] WorkState state() const noexcept override {
		TerminalState const terminal = terminal_state_.load(std::memory_order_acquire);
		if (terminal != TerminalState::none) {
			return map_terminal(terminal);
		}
		if (cancel_requested_.load(std::memory_order_acquire)) {
			return WorkState::cancel_requested;
		}
		return WorkState::pending;
	}
	bool install_cancel_hook(
		::conflux::detail::small_move_only_function<void(CancelReason)> fn) noexcept override {
		if (!fn) {
			return false;
		}
		::conflux::detail::small_move_only_function<void(CancelReason)> invoke_now{};
		{
			std::scoped_lock const lk{mtx_};
			if (hook_installed_) {
				return false;
			}
			if (terminal_claimed_.load(std::memory_order_acquire)) {
				return false;
			}
			hook_installed_ = true;
			hook_fn_ = std::move(fn);
			if (!hook_claimed_
				&& cancel_requested_.load(std::memory_order_acquire)
				&& !terminal_claimed_.load(std::memory_order_acquire)) {
				hook_claimed_ = true;
				invoke_now = std::move(hook_fn_);
			}
		}
		if (invoke_now && !terminal_claimed_.load(std::memory_order_acquire)) {
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
			terminal_state_.store(TerminalState::success, std::memory_order_release);
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
			terminal_state_.store(TerminalState::failure, std::memory_order_release);
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
			terminal_state_.store(TerminalState::cancelled, std::memory_order_release);
		}
		cv_.notify_all();
		fire_ready_hook_if_armed_();
		run_abandon_path_if_present();
		return true;
	}
	[[nodiscard]] ReadyRegistrationResult try_set_on_ready(
		::conflux::detail::small_move_only_function<void()> fn) noexcept override {
		if (!fn) {
			return {ReadyRegistration::empty, std::move(fn)};
		}
		if (ready_hook_state_.load(std::memory_order_acquire) == ReadyHookState::terminal) {
			return {ReadyRegistration::already_ready, std::move(fn)};
		}
		std::unique_lock lk{mtx_};
		auto s = ready_hook_state_.load(std::memory_order_acquire);
		if (s == ReadyHookState::terminal) {
			return {ReadyRegistration::already_ready, std::move(fn)};
		}
		if (s == ReadyHookState::open) {
			on_ready_fn_ = std::move(fn);
			auto expected = ReadyHookState::open;
			if (ready_hook_state_.compare_exchange_strong(
					expected,
					ReadyHookState::armed,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				return {ReadyRegistration::installed, {}};
			}
			// Lost race vs fire_ready_hook_if_armed_: it CAS'd open→committing
			// before our CAS, then stored terminal. Our fn was never seen by it.
			// Take it back and report already_ready so caller dispatches it.
			auto rejected = std::move(on_ready_fn_);
			return {ReadyRegistration::already_ready, std::move(rejected)};
		}
		if (s == ReadyHookState::armed || s == ReadyHookState::disarmed) {
			return {ReadyRegistration::already_installed, std::move(fn)};
		}
		return {ReadyRegistration::already_ready, std::move(fn)};
	}
	[[nodiscard]] ClearOnReadyStatus clear_on_ready() noexcept override {
		auto s = ready_hook_state_.load(std::memory_order_acquire);
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
		s = ready_hook_state_.load(std::memory_order_acquire);
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
		ready_hook_state_.store(ReadyHookState::disarmed, std::memory_order_release);
		return ClearOnReadyStatus::cleared;
	}
	[[nodiscard]] Outcome<T> compatibility_blocking_take_outcome() override {
		auto const terminal = [&] { return terminal_state_.load(std::memory_order_acquire) != TerminalState::none; };
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
		Outcome<T> out = std::move(*outcome_);
		outcome_.reset();
		return out;
	}
	[[nodiscard]] std::optional<Outcome<T>> try_take_ready_outcome() override {
		if (terminal_state_.load(std::memory_order_acquire) == TerminalState::none) {
			return std::nullopt;
		}
		std::unique_lock lk{mtx_};
		if (terminal_state_.load(std::memory_order_acquire) == TerminalState::none) {
			return std::nullopt;
		}
		if (!outcome_) {
			throw std::logic_error{"conflux.work.root: missing terminal outcome"};
		}
		Outcome<T> out = std::move(*outcome_);
		outcome_.reset();
		return std::optional<Outcome<T>>{std::move(out)};
	}
	void install_abandon_sink(
		::conflux::detail::small_move_only_function<void(Outcome<T> const &)> sink) noexcept override {
		if (!sink) {
			std::terminate();
		}
		{
			std::scoped_lock const lk{mtx_};
			if (abandoned_) {
				std::terminate();
			}
			abandoned_ = true;
			abandon_sink_ = std::move(sink);
		}
		run_abandon_path_if_present();
	}
	[[nodiscard]] AbandonStatus try_install_abandon_sink(
		::conflux::detail::small_move_only_function<void(Outcome<T> const &)> sink) noexcept override {
		if (!sink) {
			return AbandonStatus::empty;
		}
		{
			std::scoped_lock const lk{mtx_};
			if (abandoned_) {
				return AbandonStatus::already_abandoned;
			}
			abandoned_ = true;
			abandon_sink_ = std::move(sink);
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
	alignas(64) mutable std::mutex mtx_{};
	std::condition_variable cv_{};
	std::optional<Outcome<void>> outcome_{};
	::conflux::detail::small_move_only_function<void()> on_ready_fn_{};
	::conflux::detail::small_move_only_function<void(CancelReason)> hook_fn_{};
	::conflux::detail::small_move_only_function<void(Outcome<void> const &)> abandon_sink_{};
	bool hook_installed_ = false, hook_claimed_ = false, abandoned_ = false;
	[[no_unique_address]] std::conditional_t<EnableCancellation, std::stop_source, std::monostate> stop_source_{};
	std::atomic<bool> requires_capability_{false};
	std::atomic<void const *> required_capability_address_{nullptr};
	std::atomic<void const *> required_capability_type_tag_{nullptr};
	[[nodiscard]] ::conflux::detail::small_move_only_function<void(CancelReason)>
	claim_requested_hook_if_present() noexcept {
		std::scoped_lock const lk{mtx_};
		if (!hook_installed_ || hook_claimed_) {
			return {};
		}
		if (terminal_claimed_.load(std::memory_order_acquire)) {
			return {};
		}
		hook_claimed_ = true;
		return std::move(hook_fn_);
	}
	void invoke_requested_hook_if_needed() noexcept {
		auto fn = claim_requested_hook_if_present();
		if (!fn) {
			return;
		}
		if (terminal_claimed_.load(std::memory_order_acquire)) {
			return;
		}
		try {
			fn(CancelReason::requested);
		} catch (...) { std::terminate(); }
	}
	[[nodiscard]] bool try_claim_terminal() noexcept {
		bool expected = false;
		return terminal_claimed_
			.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
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
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			ready_hook_state_.store(ReadyHookState::terminal, std::memory_order_release);
			return;
		}
		if (prev == ReadyHookState::armed) {
			::conflux::detail::small_move_only_function<void()> fn{};
			{
				std::unique_lock lk{mtx_};
				fn = std::move(on_ready_fn_);
				ready_hook_state_.store(ReadyHookState::terminal, std::memory_order_release);
			}
			if (fn) {
				fn();
			}
		} else if (prev == ReadyHookState::disarmed) {
			std::unique_lock lk{mtx_};
			ready_hook_state_.store(ReadyHookState::terminal, std::memory_order_release);
		}
	}
	void run_abandon_path_if_present() noexcept {
		::conflux::detail::small_move_only_function<void(Outcome<void> const &)> sink{};
		std::optional<Outcome<void>> local{};
		{
			std::unique_lock lk{mtx_};
			if (!abandoned_ || !abandon_sink_ || !outcome_) {
				return;
			}
			sink = std::move(abandon_sink_);
			abandon_sink_ = nullptr;
			local.emplace(std::move(*outcome_));
			outcome_.reset();
		}
		try {
			sink(*local);
		} catch (...) { std::terminate(); }
	}

public:
	void set_required_capability(
		CapabilityId id) noexcept override {
		required_capability_address_.store(id.address, std::memory_order_relaxed);
		required_capability_type_tag_.store(id.type_tag, std::memory_order_relaxed);
		requires_capability_.store(true, std::memory_order_release);
	}
	[[nodiscard]] bool can_join_with(
		CapabilityId id) const noexcept override {
		if (!requires_capability_.load(std::memory_order_acquire)) {
			return true;
		}
		CapabilityId const expected{
			.address = required_capability_address_.load(std::memory_order_relaxed),
			.type_tag = required_capability_type_tag_.load(std::memory_order_relaxed),
		};
		return expected == id;
	}
	[[nodiscard]] std::optional<CapabilityId> required_capability() const noexcept override {
		if (!requires_capability_.load(std::memory_order_acquire)) {
			return std::nullopt;
		}
		return CapabilityId{
			.address = required_capability_address_.load(std::memory_order_relaxed),
			.type_tag = required_capability_type_tag_.load(std::memory_order_relaxed),
		};
	}
	[[nodiscard]] bool request_cancel() noexcept override {
		if (terminal_claimed_.load(std::memory_order_acquire)) {
			return false;
		}

		bool expected = false;
		if (!cancel_requested_
				 .compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
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
		return cancel_requested_.load(std::memory_order_acquire);
	}
	[[nodiscard]] bool ready() const noexcept override {
		return terminal_state_.load(std::memory_order_acquire) != TerminalState::none;
	}
	[[nodiscard]] WorkState state() const noexcept override {
		TerminalState const terminal = terminal_state_.load(std::memory_order_acquire);
		if (terminal != TerminalState::none) {
			return map_terminal(terminal);
		}
		if (cancel_requested_.load(std::memory_order_acquire)) {
			return WorkState::cancel_requested;
		}
		return WorkState::pending;
	}
	bool install_cancel_hook(
		::conflux::detail::small_move_only_function<void(CancelReason)> fn) noexcept override {
		if (!fn) {
			return false;
		}
		::conflux::detail::small_move_only_function<void(CancelReason)> invoke_now{};
		{
			std::scoped_lock const lk{mtx_};
			if (hook_installed_) {
				return false;
			}
			if (terminal_claimed_.load(std::memory_order_acquire)) {
				return false;
			}
			hook_installed_ = true;
			hook_fn_ = std::move(fn);
			if (!hook_claimed_
				&& cancel_requested_.load(std::memory_order_acquire)
				&& !terminal_claimed_.load(std::memory_order_acquire)) {
				hook_claimed_ = true;
				invoke_now = std::move(hook_fn_);
			}
		}
		if (invoke_now && !terminal_claimed_.load(std::memory_order_acquire)) {
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
			terminal_state_.store(TerminalState::success, std::memory_order_release);
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
			terminal_state_.store(TerminalState::failure, std::memory_order_release);
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
			terminal_state_.store(TerminalState::cancelled, std::memory_order_release);
		}
		cv_.notify_all();
		fire_ready_hook_if_armed_();
		run_abandon_path_if_present();
		return true;
	}
	[[nodiscard]] ReadyRegistrationResult try_set_on_ready(
		::conflux::detail::small_move_only_function<void()> fn) noexcept override {
		if (!fn) {
			return {ReadyRegistration::empty, std::move(fn)};
		}
		if (ready_hook_state_.load(std::memory_order_acquire) == ReadyHookState::terminal) {
			return {ReadyRegistration::already_ready, std::move(fn)};
		}
		std::unique_lock lk{mtx_};
		auto s = ready_hook_state_.load(std::memory_order_acquire);
		if (s == ReadyHookState::terminal) {
			return {ReadyRegistration::already_ready, std::move(fn)};
		}
		if (s == ReadyHookState::open) {
			on_ready_fn_ = std::move(fn);
			auto expected = ReadyHookState::open;
			if (ready_hook_state_.compare_exchange_strong(
					expected,
					ReadyHookState::armed,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				return {ReadyRegistration::installed, {}};
			}
			// Lost race vs fire_ready_hook_if_armed_: it CAS'd open→committing
			// before our CAS, then stored terminal. Our fn was never seen by it.
			// Take it back and report already_ready so caller dispatches it.
			auto rejected = std::move(on_ready_fn_);
			return {ReadyRegistration::already_ready, std::move(rejected)};
		}
		if (s == ReadyHookState::armed || s == ReadyHookState::disarmed) {
			return {ReadyRegistration::already_installed, std::move(fn)};
		}
		return {ReadyRegistration::already_ready, std::move(fn)};
	}
	[[nodiscard]] ClearOnReadyStatus clear_on_ready() noexcept override {
		auto s = ready_hook_state_.load(std::memory_order_acquire);
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
		s = ready_hook_state_.load(std::memory_order_acquire);
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
		ready_hook_state_.store(ReadyHookState::disarmed, std::memory_order_release);
		return ClearOnReadyStatus::cleared;
	}
	[[nodiscard]] Outcome<void> compatibility_blocking_take_outcome() override {
		auto const terminal = [&] { return terminal_state_.load(std::memory_order_acquire) != TerminalState::none; };
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
		Outcome<void> out = std::move(*outcome_);
		outcome_.reset();
		return out;
	}
	[[nodiscard]] std::optional<Outcome<void>> try_take_ready_outcome() override {
		if (terminal_state_.load(std::memory_order_acquire) == TerminalState::none) {
			return std::nullopt;
		}
		std::unique_lock lk{mtx_};
		if (terminal_state_.load(std::memory_order_acquire) == TerminalState::none) {
			return std::nullopt;
		}
		if (!outcome_) {
			throw std::logic_error{"conflux.work.root: missing terminal outcome"};
		}
		Outcome<void> out = std::move(*outcome_);
		outcome_.reset();
		return std::optional<Outcome<void>>{std::move(out)};
	}
	void install_abandon_sink(
		::conflux::detail::small_move_only_function<void(Outcome<void> const &)> sink) noexcept override {
		if (!sink) {
			std::terminate();
		}
		{
			std::scoped_lock const lk{mtx_};
			if (abandoned_) {
				std::terminate();
			}
			abandoned_ = true;
			abandon_sink_ = std::move(sink);
		}
		run_abandon_path_if_present();
	}
	[[nodiscard]] AbandonStatus try_install_abandon_sink(
		::conflux::detail::small_move_only_function<void(Outcome<void> const &)> sink) noexcept override {
		if (!sink) {
			return AbandonStatus::empty;
		}
		{
			std::scoped_lock const lk{mtx_};
			if (abandoned_) {
				return AbandonStatus::already_abandoned;
			}
			abandoned_ = true;
			abandon_sink_ = std::move(sink);
		}
		run_abandon_path_if_present();
		return AbandonStatus::installed;
	}
};
template<work_value T, bool EnableCancellation>
[[nodiscard]] std::shared_ptr<ControlBlockInterface<T>> make_control_block_shared() {
	using model_t = ControlBlockModel<T, EnableCancellation>;
	return std::make_shared<model_t>();
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
struct alignas(
	std::max_align_t) TaskFrameHeader {
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
	std::size_t const allocation_bytes = std::max(slab_bytes, block_bytes);
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
	task_coroutine_frame_resource().deallocate(hdr, size + sizeof(TaskFrameHeader), alignof(std::max_align_t));
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
	std::pair<
		BasicControl<ControlCategory::task>,
		BasicSource<U, ControlCategory::task>> friend make_task_control_source();
	template<work_value U>
	std::pair<
		BasicControl<ControlCategory::posted>,
		BasicSource<U, ControlCategory::posted>> friend make_posted_control_source();
	template<work_value U>
	std::pair<
		BasicControl<ControlCategory::operation>,
		BasicSource<U, ControlCategory::operation>> friend make_operation_control_source();

public:
	BasicControl() = default;
	explicit BasicControl(
		std::shared_ptr<ControlBlockBase> core) noexcept
		: core_{std::move(core)} {}
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
		return core_ ? core_->required_capability() : std::nullopt;
	}
	[[nodiscard]] static constexpr ControlCategory category() noexcept { return Category; }
	[[nodiscard]] ReadyRegistrationResult try_set_on_ready(
		::conflux::detail::small_move_only_function<void()> fn) noexcept {
		if (!core_) {
			return {ReadyRegistration::empty, std::move(fn)};
		}
		return core_->try_set_on_ready(std::move(fn));
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
		auto materialised = ::conflux::detail::small_move_only_function<void()>{std::forward<F>(fn)};
		auto result = try_set_on_ready(std::move(materialised));
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
		: state_{std::move(state)} {}
	template<work_value U>
	std::pair<TaskControl, BasicSource<U, ControlCategory::task>> friend make_task_control_source();
	template<work_value U>
	std::pair<PostedControl, BasicSource<U, ControlCategory::posted>> friend make_posted_control_source();
	template<work_value U>
	std::pair<OperationControl, BasicSource<U, ControlCategory::operation>> friend make_operation_control_source();
	template<work_value U>
	std::pair<
		class BasicResult<U, ControlCategory::task>,
		BasicSource<U, ControlCategory::task>> friend make_task_source(struct SubmitOptions, std::source_location);
	template<work_value U, progress_capability Owner>
	std::pair<
		class BasicResult<U, ControlCategory::posted>,
		BasicSource<
			U,
			ControlCategory::posted>> friend make_posted_source(Owner &, struct PostOptions, std::source_location);
	template<work_value U, progress_capability Driver>
	std::pair<
		class BasicResult<U, ControlCategory::operation>,
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
		return state_ ? state_->try_set_value(std::move(value)) : false;
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
		::conflux::detail::small_move_only_function<void(CancelReason)> fn) noexcept {
		return state_ ? state_->install_cancel_hook(std::move(fn)) : false;
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
		: state_{std::move(state)} {}
	template<work_value U>
	std::pair<TaskControl, BasicSource<U, ControlCategory::task>> friend make_task_control_source();
	template<work_value U>
	std::pair<PostedControl, BasicSource<U, ControlCategory::posted>> friend make_posted_control_source();
	template<work_value U>
	std::pair<OperationControl, BasicSource<U, ControlCategory::operation>> friend make_operation_control_source();
	template<work_value U>
	std::pair<
		class BasicResult<U, ControlCategory::task>,
		BasicSource<U, ControlCategory::task>> friend make_task_source(struct SubmitOptions, std::source_location);
	template<work_value U, progress_capability Owner>
	std::pair<
		class BasicResult<U, ControlCategory::posted>,
		BasicSource<
			U,
			ControlCategory::posted>> friend make_posted_source(Owner &, struct PostOptions, std::source_location);
	template<work_value U, progress_capability Driver>
	std::pair<
		class BasicResult<U, ControlCategory::operation>,
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
		::conflux::detail::small_move_only_function<void(CancelReason)> fn) noexcept {
		return state_ ? state_->install_cancel_hook(std::move(fn)) : false;
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

#include "root_tasks.inc"
