module;

export module conflux.work.root;

import std;
import conflux.types;

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
	using std::runtime_error::runtime_error;
};

namespace detail {

[[nodiscard]] inline std::exception_ptr normalize_failure_ptr(
	std::exception_ptr const &ep) {
	if (ep) {
		return ep;
	}
	return std::make_exception_ptr(std::runtime_error{"conflux.work.root: normalized null EP"});
}

} // namespace detail

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

	[[nodiscard]] friend bool operator ==(
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
	[[nodiscard]] friend CapabilityId tag_invoke(
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
	};

	explicit JoinError(
		reason r)
		: std::logic_error{make_msg(r)}
		, reason_{r} {}

	explicit JoinError(
		reason r,
		std::optional<CapabilityId> expected,
		CapabilityId actual)
		: std::logic_error{make_msg(r)}
		, reason_{r}
		, expected_{expected}
		, actual_{actual} {}

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
template<typename Signature, SZ InlineBytes = 32>
class small_move_only_function;

template<typename R, typename... Args, SZ InlineBytes>
class small_move_only_function<R(Args...), InlineBytes> {
	struct storage_t {
		alignas(std::max_align_t) std::byte bytes[InlineBytes];
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
		return std::invoke(*reinterpret_cast<F *>(obj), std::forward<Args>(args)...);
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
		new (dst) F(std::move(*src_fn));
		src_fn->~F();
	}

	template<typename F>
	static R invoke_heap(
		void *obj,
		Args &&...args) {
		return std::invoke(*reinterpret_cast<F *>(obj), std::forward<Args>(args)...);
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

		object_ = std::exchange(other.object_, nullptr);
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
		move_from(std::move(other));
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
		requires(!std::same_as<std::remove_cvref_t<F>, small_move_only_function>)
	small_move_only_function(
		F &&fn) {
		using fn_t = std::remove_cvref_t<F>;
		static_assert(std::is_move_constructible_v<fn_t>);

		if constexpr (
			sizeof(fn_t) <= InlineBytes
			&& alignof(fn_t) <= alignof(storage_t)
			&& std::is_nothrow_move_constructible_v<fn_t>) {
			object_ = &inline_storage_;
			new (object_) fn_t(std::forward<F>(fn));
			invoke_ = &invoke_inline<fn_t>;
			destroy_ = &destroy_inline<fn_t>;
			move_ = &move_inline<fn_t>;
			inlined_ = true;
		} else {
			object_ = new fn_t(std::forward<F>(fn));
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
		return invoke_(object_, std::forward<Args>(args)...);
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
	std::mutex mtx;
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

class ControlBlockBase {
public:
	virtual ~ControlBlockBase() = default;
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
		return try_set_exception(std::make_exception_ptr(std::system_error(ec)));
	}
	[[nodiscard]] virtual bool try_set_cancelled(CancelReason reason, bool allow_abandoned) noexcept = 0;
	[[nodiscard]] virtual Outcome<T> wait_and_take_outcome() = 0;
	virtual void install_abandon_sink(small_move_only_function<void(Outcome<T> const &)> sink) noexcept = 0;
	[[nodiscard]] virtual AbandonStatus
	try_install_abandon_sink(small_move_only_function<void(Outcome<T> const &)> sink) noexcept = 0;
};

template<work_value T, bool EnableCancellation>
class ControlBlockModel final : public ControlBlockInterface<T> {
	std::atomic<bool> cancel_requested_{false};
	std::atomic<bool> terminal_claimed_{false};
	std::atomic<TerminalState> terminal_state_{TerminalState::none};
	std::atomic<CancelReason> terminal_cancel_reason_{CancelReason::requested};
	std::conditional_t<EnableCancellation, std::stop_source, std::monostate> stop_source_{};
	std::atomic<bool> requires_capability_{false};
	std::atomic<void const *> required_capability_address_{nullptr};
	std::atomic<void const *> required_capability_type_tag_{nullptr};

	mutable std::mutex ready_mtx_{};
	mutable std::condition_variable ready_cv_{};

	mutable std::mutex hook_mtx_{};
	small_move_only_function<void(CancelReason)> hook_fn_{};
	bool hook_installed_ = false;
	bool hook_claimed_ = false;

	mutable std::mutex outcome_mtx_{};
	Opt<Outcome<T>> outcome_{};

	mutable std::mutex abandon_mtx_{};
	bool abandoned_ = false;
	small_move_only_function<void(Outcome<T> const &)> abandon_sink_{};

	mutable std::mutex ready_hook_mtx_{};
	std::atomic<ReadyHookState> ready_hook_state_{ReadyHookState::open};
	small_move_only_function<void()> on_ready_fn_{};

	[[nodiscard]] small_move_only_function<void(CancelReason)> claim_requested_hook_if_present() noexcept {
		std::scoped_lock const lk{hook_mtx_};
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
			small_move_only_function<void()> fn{};
			{
				std::unique_lock lk{ready_hook_mtx_};
				fn = std::move(on_ready_fn_);
				ready_hook_state_.store(ReadyHookState::terminal, std::memory_order_release);
			}
			if (fn) {
				fn();
			}
		} else if (prev == ReadyHookState::disarmed) {
			std::unique_lock lk{ready_hook_mtx_};
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
		small_move_only_function<void(Outcome<T> const &)> sink{};
		Opt<Outcome<T>> local{};
		{
			std::scoped_lock const lk{abandon_mtx_, outcome_mtx_};
			if (!abandoned_ || !abandon_sink_ || !outcome_) {
				return;
			}
			sink = std::move(abandon_sink_);
			abandon_sink_ = nullptr;
			local.emplace(std::move(*outcome_));
			outcome_.reset();
		}

		if (local->is_failure() || local->is_cancelled()) {
			try {
				sink(*local);
			} catch (...) { std::terminate(); }
		}
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
		return required_capability_address_.load(std::memory_order_relaxed) == id.address
			&& required_capability_type_tag_.load(std::memory_order_relaxed) == id.type_tag;
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
			(void)stop_source_.request_stop();
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
		small_move_only_function<void(CancelReason)> fn) noexcept override {
		if (!fn) {
			return false;
		}

		small_move_only_function<void(CancelReason)> invoke_now{};
		{
			std::scoped_lock const lk{hook_mtx_};
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
			std::scoped_lock const lk{outcome_mtx_};
			outcome_.emplace(Outcome<T>{std::move(success)});
		}
		{
			std::scoped_lock const ready_lk{ready_mtx_};
			terminal_state_.store(TerminalState::success, std::memory_order_release);
		}
		fire_ready_hook_if_armed_();
		ready_cv_.notify_all();
		run_abandon_path_if_present();
		return true;
	}

	[[nodiscard]] bool try_set_exception(
		std::exception_ptr error) override {
		if (!try_claim_terminal()) {
			return false;
		}
		{
			std::scoped_lock const lk{outcome_mtx_};
			outcome_.emplace(Outcome<T>{Failure{error}});
		}
		{
			std::scoped_lock const ready_lk{ready_mtx_};
			terminal_state_.store(TerminalState::failure, std::memory_order_release);
		}
		fire_ready_hook_if_armed_();
		ready_cv_.notify_all();
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
		terminal_cancel_reason_.store(reason, std::memory_order_relaxed);
		{
			std::scoped_lock const lk{outcome_mtx_};
			outcome_.emplace(Outcome<T>{Cancelled{reason}});
		}
		{
			std::scoped_lock const ready_lk{ready_mtx_};
			terminal_state_.store(TerminalState::cancelled, std::memory_order_release);
		}
		fire_ready_hook_if_armed_();
		ready_cv_.notify_all();
		run_abandon_path_if_present();
		return true;
	}

	[[nodiscard]] ReadyRegistrationResult try_set_on_ready(
		small_move_only_function<void()> fn) noexcept override {
		if (!fn) {
			return {ReadyRegistration::empty, std::move(fn)};
		}
		if (ready_hook_state_.load(std::memory_order_acquire) == ReadyHookState::terminal) {
			return {ReadyRegistration::already_ready, std::move(fn)};
		}
		std::unique_lock lk{ready_hook_mtx_};
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
		std::unique_lock lk{ready_hook_mtx_};
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

	[[nodiscard]] Outcome<T> wait_and_take_outcome() override {
		std::unique_lock lk{ready_mtx_};
		ready_cv_.wait(lk, [&] { return terminal_state_.load(std::memory_order_acquire) != TerminalState::none; });
		lk.unlock();

		std::scoped_lock const out_lk{outcome_mtx_};
		if (!outcome_) {
			throw std::logic_error{"conflux.work.root: missing terminal outcome"};
		}
		Outcome<T> out = std::move(*outcome_);
		outcome_.reset();
		return out;
	}

	void install_abandon_sink(
		small_move_only_function<void(Outcome<T> const &)> sink) noexcept override {
		if (!sink) {
			std::terminate();
		}
		{
			std::scoped_lock const lk{abandon_mtx_};
			if (abandoned_) {
				std::terminate();
			}
			abandoned_ = true;
			abandon_sink_ = std::move(sink);
		}
		run_abandon_path_if_present();
	}

	[[nodiscard]] AbandonStatus try_install_abandon_sink(
		small_move_only_function<void(Outcome<T> const &)> sink) noexcept override {
		if (!sink) {
			return AbandonStatus::empty;
		}
		{
			std::scoped_lock const lk{abandon_mtx_};
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
	std::atomic<bool> cancel_requested_{false};
	std::atomic<bool> terminal_claimed_{false};
	std::atomic<TerminalState> terminal_state_{TerminalState::none};
	std::atomic<CancelReason> terminal_cancel_reason_{CancelReason::requested};
	std::conditional_t<EnableCancellation, std::stop_source, std::monostate> stop_source_{};
	std::atomic<bool> requires_capability_{false};
	std::atomic<void const *> required_capability_address_{nullptr};
	std::atomic<void const *> required_capability_type_tag_{nullptr};

	mutable std::mutex ready_mtx_{};
	mutable std::condition_variable ready_cv_{};

	mutable std::mutex hook_mtx_{};
	small_move_only_function<void(CancelReason)> hook_fn_{};
	bool hook_installed_ = false;
	bool hook_claimed_ = false;

	mutable std::mutex outcome_mtx_{};
	Opt<Outcome<void>> outcome_{};

	mutable std::mutex abandon_mtx_{};
	bool abandoned_ = false;
	small_move_only_function<void(Outcome<void> const &)> abandon_sink_{};

	mutable std::mutex ready_hook_mtx_{};
	std::atomic<ReadyHookState> ready_hook_state_{ReadyHookState::open};
	small_move_only_function<void()> on_ready_fn_{};

	[[nodiscard]] small_move_only_function<void(CancelReason)> claim_requested_hook_if_present() noexcept {
		std::scoped_lock const lk{hook_mtx_};
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
			small_move_only_function<void()> fn{};
			{
				std::unique_lock lk{ready_hook_mtx_};
				fn = std::move(on_ready_fn_);
				ready_hook_state_.store(ReadyHookState::terminal, std::memory_order_release);
			}
			if (fn) {
				fn();
			}
		} else if (prev == ReadyHookState::disarmed) {
			std::unique_lock lk{ready_hook_mtx_};
			ready_hook_state_.store(ReadyHookState::terminal, std::memory_order_release);
		}
	}

	void run_abandon_path_if_present() noexcept {
		small_move_only_function<void(Outcome<void> const &)> sink{};
		Opt<Outcome<void>> local{};
		{
			std::scoped_lock const lk{abandon_mtx_, outcome_mtx_};
			if (!abandoned_ || !abandon_sink_ || !outcome_) {
				return;
			}
			sink = std::move(abandon_sink_);
			abandon_sink_ = nullptr;
			local.emplace(std::move(*outcome_));
			outcome_.reset();
		}

		if (local->is_failure() || local->is_cancelled()) {
			try {
				sink(*local);
			} catch (...) { std::terminate(); }
		}
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
		return required_capability_address_.load(std::memory_order_relaxed) == id.address
			&& required_capability_type_tag_.load(std::memory_order_relaxed) == id.type_tag;
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
			(void)stop_source_.request_stop();
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
		small_move_only_function<void(CancelReason)> fn) noexcept override {
		if (!fn) {
			return false;
		}

		small_move_only_function<void(CancelReason)> invoke_now{};
		{
			std::scoped_lock const lk{hook_mtx_};
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
			std::scoped_lock const lk{outcome_mtx_};
			outcome_.emplace(Outcome<void>{success});
		}
		{
			std::scoped_lock const ready_lk{ready_mtx_};
			terminal_state_.store(TerminalState::success, std::memory_order_release);
		}
		fire_ready_hook_if_armed_();
		ready_cv_.notify_all();
		run_abandon_path_if_present();
		return true;
	}

	[[nodiscard]] bool try_set_exception(
		std::exception_ptr error) override {
		if (!try_claim_terminal()) {
			return false;
		}
		{
			std::scoped_lock const lk{outcome_mtx_};
			outcome_.emplace(Outcome<void>{Failure{error}});
		}
		{
			std::scoped_lock const ready_lk{ready_mtx_};
			terminal_state_.store(TerminalState::failure, std::memory_order_release);
		}
		fire_ready_hook_if_armed_();
		ready_cv_.notify_all();
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
		terminal_cancel_reason_.store(reason, std::memory_order_relaxed);
		{
			std::scoped_lock const lk{outcome_mtx_};
			outcome_.emplace(Outcome<void>{Cancelled{reason}});
		}
		{
			std::scoped_lock const ready_lk{ready_mtx_};
			terminal_state_.store(TerminalState::cancelled, std::memory_order_release);
		}
		fire_ready_hook_if_armed_();
		ready_cv_.notify_all();
		run_abandon_path_if_present();
		return true;
	}

	[[nodiscard]] ReadyRegistrationResult try_set_on_ready(
		small_move_only_function<void()> fn) noexcept override {
		if (!fn) {
			return {ReadyRegistration::empty, std::move(fn)};
		}
		if (ready_hook_state_.load(std::memory_order_acquire) == ReadyHookState::terminal) {
			return {ReadyRegistration::already_ready, std::move(fn)};
		}
		std::unique_lock lk{ready_hook_mtx_};
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
		std::unique_lock lk{ready_hook_mtx_};
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

	[[nodiscard]] Outcome<void> wait_and_take_outcome() override {
		std::unique_lock lk{ready_mtx_};
		ready_cv_.wait(lk, [&] { return terminal_state_.load(std::memory_order_acquire) != TerminalState::none; });
		lk.unlock();

		std::scoped_lock const out_lk{outcome_mtx_};
		if (!outcome_) {
			throw std::logic_error{"conflux.work.root: missing terminal outcome"};
		}
		Outcome<void> out = std::move(*outcome_);
		outcome_.reset();
		return out;
	}

	void install_abandon_sink(
		small_move_only_function<void(Outcome<void> const &)> sink) noexcept override {
		if (!sink) {
			std::terminate();
		}
		{
			std::scoped_lock const lk{abandon_mtx_};
			if (abandoned_) {
				std::terminate();
			}
			abandoned_ = true;
			abandon_sink_ = std::move(sink);
		}
		run_abandon_path_if_present();
	}

	[[nodiscard]] AbandonStatus try_install_abandon_sink(
		small_move_only_function<void(Outcome<void> const &)> sink) noexcept override {
		if (!sink) {
			return AbandonStatus::empty;
		}
		{
			std::scoped_lock const lk{abandon_mtx_};
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

template<ControlCategory Category>
class BasicControl {
	SP<ControlBlockBase> core_{};

	template<work_value, ControlCategory>
	friend class BasicSource;
	template<work_value U>
	friend P<BasicControl<ControlCategory::task>, BasicSource<U, ControlCategory::task>> make_task_control_source();
	template<work_value U>
	friend P<BasicControl<ControlCategory::posted>, BasicSource<U, ControlCategory::posted>>
	make_posted_control_source();
	template<work_value U>
	friend P<BasicControl<ControlCategory::operation>, BasicSource<U, ControlCategory::operation>>
	make_operation_control_source();

public:
	BasicControl() = default;
	explicit BasicControl(
		SP<ControlBlockBase> core) noexcept
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
		return core_ ? core_->can_join_with(id) : false;
	}

	[[nodiscard]] static constexpr ControlCategory category() noexcept { return Category; }

	[[nodiscard]] ReadyRegistrationResult try_set_on_ready(
		small_move_only_function<void()> fn) noexcept {
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
		auto materialised = small_move_only_function<void()>{std::forward<F>(fn)};
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
	SP<detail::ControlBlockInterface<T>> state_{};

	explicit BasicSource(
		SP<detail::ControlBlockInterface<T>> state) noexcept
		: state_{std::move(state)} {}

	template<work_value U>
	friend P<TaskControl, BasicSource<U, ControlCategory::task>> make_task_control_source();
	template<work_value U>
	friend P<PostedControl, BasicSource<U, ControlCategory::posted>> make_posted_control_source();
	template<work_value U>
	friend P<OperationControl, BasicSource<U, ControlCategory::operation>> make_operation_control_source();
	template<work_value U>
	friend P<class BasicResult<U, ControlCategory::task>, BasicSource<U, ControlCategory::task>>
		make_task_source(struct SubmitOptions, std::source_location);
	template<work_value U, progress_capability Owner>
	friend P<class BasicResult<U, ControlCategory::posted>, BasicSource<U, ControlCategory::posted>>
	make_posted_source(Owner &, struct PostOptions, std::source_location);
	template<work_value U, progress_capability Driver>
	friend P<class BasicResult<U, ControlCategory::operation>, BasicSource<U, ControlCategory::operation>>
	make_operation_source(Driver &, struct OperationOptions, std::source_location);

public:
	BasicSource() = default;
	BasicSource(BasicSource &&) noexcept = default;
	BasicSource &operator =(BasicSource &&) noexcept = default;
	BasicSource(BasicSource const &) = delete;
	BasicSource &operator =(BasicSource const &) = delete;
	[[nodiscard]] static BasicSource from_state(
		SP<detail::ControlBlockInterface<T>> state) noexcept {
		return BasicSource{std::move(state)};
	}

	~BasicSource() noexcept {
		if (state_) {
			(void)state_->try_set_cancelled(CancelReason::abandoned, true);
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
		CancelReason reason) noexcept {
		return state_ ? state_->try_set_cancelled(reason, false) : false;
	}

	[[nodiscard]] bool try_set_error(
		std::error_code ec) {
		return state_ ? state_->try_set_error(ec) : false;
	}

	[[nodiscard]] bool install_cancel_hook(
		detail::small_move_only_function<void(CancelReason)> fn) noexcept {
		return state_ ? state_->install_cancel_hook(std::move(fn)) : false;
	}

	[[nodiscard]] std::stop_token stop_token() const noexcept {
		return state_ ? state_->stop_token() : std::stop_token{};
	}
};

template<ControlCategory Category>
class BasicSource<void, Category> {
	SP<detail::ControlBlockInterface<void>> state_{};

	explicit BasicSource(
		SP<detail::ControlBlockInterface<void>> state) noexcept
		: state_{std::move(state)} {}

	template<work_value U>
	friend P<TaskControl, BasicSource<U, ControlCategory::task>> make_task_control_source();
	template<work_value U>
	friend P<PostedControl, BasicSource<U, ControlCategory::posted>> make_posted_control_source();
	template<work_value U>
	friend P<OperationControl, BasicSource<U, ControlCategory::operation>> make_operation_control_source();
	template<work_value U>
	friend P<class BasicResult<U, ControlCategory::task>, BasicSource<U, ControlCategory::task>>
		make_task_source(struct SubmitOptions, std::source_location);
	template<work_value U, progress_capability Owner>
	friend P<class BasicResult<U, ControlCategory::posted>, BasicSource<U, ControlCategory::posted>>
	make_posted_source(Owner &, struct PostOptions, std::source_location);
	template<work_value U, progress_capability Driver>
	friend P<class BasicResult<U, ControlCategory::operation>, BasicSource<U, ControlCategory::operation>>
	make_operation_source(Driver &, struct OperationOptions, std::source_location);

public:
	BasicSource() = default;
	BasicSource(BasicSource &&) noexcept = default;
	BasicSource &operator =(BasicSource &&) noexcept = default;
	BasicSource(BasicSource const &) = delete;
	BasicSource &operator =(BasicSource const &) = delete;
	[[nodiscard]] static BasicSource from_state(
		SP<detail::ControlBlockInterface<void>> state) noexcept {
		return BasicSource{std::move(state)};
	}

	~BasicSource() noexcept {
		if (state_) {
			(void)state_->try_set_cancelled(CancelReason::abandoned, true);
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
		CancelReason reason) noexcept {
		return state_ ? state_->try_set_cancelled(reason, false) : false;
	}

	[[nodiscard]] bool try_set_error(
		std::error_code ec) {
		return state_ ? state_->try_set_error(ec) : false;
	}

	[[nodiscard]] bool install_cancel_hook(
		detail::small_move_only_function<void(CancelReason)> fn) noexcept {
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

namespace detail {

template<work_value T>
struct TaskAwaiter {
	SP<ControlBlockInterface<T>> state_;

	[[nodiscard]] bool await_ready() const noexcept { return !state_ || state_->ready(); }

	[[nodiscard]] bool await_suspend(
		std::coroutine_handle<> h) noexcept {
		auto result = state_->try_set_on_ready(small_move_only_function<void()>{[h]() noexcept { h.resume(); }});
		switch (result.status) {
		case ReadyRegistration::installed        : return true;
		case ReadyRegistration::already_ready    :
		case ReadyRegistration::already_installed:
		case ReadyRegistration::empty            : return false;
		}
		std::unreachable();
	}

	decltype(auto) await_resume() {
		if (!state_) {
			throw JoinError{JoinError::reason::consumed_handle};
		}
		// Rethrow original exception on Failure (not FailureError wrapper) so
		// existing catch sites work. E2b.2 migrates to FailureError uniformly.
		return std::move(state_->wait_and_take_outcome())
			.visit([](auto &&arm) -> std::conditional_t<std::is_void_v<T>, void, T> {
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
	SP<ControlBlockInterface<T>> state_;

	[[nodiscard]] bool await_ready() const noexcept { return !state_ || state_->ready(); }

	[[nodiscard]] bool await_suspend(
		std::coroutine_handle<> h) noexcept {
		auto result = state_->try_set_on_ready(small_move_only_function<void()>{[h]() noexcept { h.resume(); }});
		switch (result.status) {
		case ReadyRegistration::installed        : return true;
		case ReadyRegistration::already_ready    :
		case ReadyRegistration::already_installed:
		case ReadyRegistration::empty            : return false;
		}
		std::unreachable();
	}

	Outcome<T> await_resume() {
		if (!state_) {
			throw JoinError{JoinError::reason::consumed_handle};
		}
		return state_->wait_and_take_outcome();
	}
};

} // namespace detail

template<work_value T, ControlCategory Category>
class BasicResult {
	SP<detail::ControlBlockInterface<T>> state_{};
	join_state state_js_ = join_state::empty;
	std::source_location spawn_loc_{};

	explicit BasicResult(
		SP<detail::ControlBlockInterface<T>> state,
		std::source_location loc) noexcept
		: state_{std::move(state)}
		, state_js_{state_ ? join_state::joinable : join_state::empty}
		, spawn_loc_{loc} {}

	[[nodiscard]] SP<detail::ControlBlockInterface<T>> consume(
		join_state target) noexcept {
		if (state_js_ != join_state::joinable) {
			return {};
		}
		state_js_ = target;
		return std::move(state_);
	}

	void detach_noexcept() noexcept { abandon_impl(std::move(*this), detail::detach_outcome_sink<T>{spawn_loc_}); }

	template<work_value U>
	friend P<BasicResult<U, ControlCategory::task>, TaskSource<U>>
		make_task_source(SubmitOptions, std::source_location);
	template<work_value U, progress_capability Owner>
	friend P<BasicResult<U, ControlCategory::posted>, PostedSource<U>>
	make_posted_source(Owner &, PostOptions, std::source_location);
	template<work_value U, progress_capability Driver>
	friend P<BasicResult<U, ControlCategory::operation>, OperationSource<U>>
	make_operation_source(Driver &, OperationOptions, std::source_location);
	template<work_value U, ControlCategory C, class Sink>
	friend void abandon_impl(BasicResult<U, C> &&, Sink &&) noexcept;
	template<work_value U, ControlCategory C, class Sink>
	friend void abandon_impl(BasicJoinHandle<U, C> &&, Sink &&) noexcept;
	template<work_value U>
	friend Outcome<U> join(BasicResult<U, ControlCategory::task> &&);
	template<progress_capability Owner, work_value U>
	friend Outcome<U> join(Owner &, BasicResult<U, ControlCategory::posted> &&);
	template<progress_capability Driver, work_value U>
	friend Outcome<U> join(Driver &, BasicResult<U, ControlCategory::operation> &&);
	friend class BasicJoinHandle<T, Category>;
	template<class Fn>
	friend auto spawn(Fn &&, std::source_location) -> std::invoke_result_t<Fn>;

public:
	using value_type = T;

	BasicResult() = default;

	[[nodiscard]] static BasicResult from_state(
		SP<detail::ControlBlockInterface<T>> state,
		std::source_location loc = std::source_location::current()) noexcept {
		return BasicResult{std::move(state), loc};
	}

	BasicResult(
		BasicResult &&other) noexcept
		: state_{std::move(other.state_)}
		, state_js_{std::exchange(other.state_js_, join_state::empty)}
		, spawn_loc_{other.spawn_loc_} {}

	BasicResult &operator =(
		BasicResult &&other) noexcept {
		if (this != &other) {
			if (state_js_ == join_state::joinable) {
				detach_noexcept();
			}
			state_ = std::move(other.state_);
			state_js_ = std::exchange(other.state_js_, join_state::empty);
			spawn_loc_ = other.spawn_loc_;
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

	void cancel() noexcept {
		if (state_js_ != join_state::empty) {
			(void)control().request_cancel();
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
};

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
	// Update the spawn location so the dropped-outcome sink reports the
	// caller's site, not wherever make_task_source was called inside fn.
	task.spawn_loc_ = loc;
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
	SP<detail::ControlBlockInterface<T>> state_{};
	bool live_ = false;

	explicit BasicJoinHandle(
		SP<detail::ControlBlockInterface<T>> state) noexcept
		: state_{std::move(state)}
		, live_{static_cast<bool>(state_)} {}

	[[nodiscard]] SP<detail::ControlBlockInterface<T>> consume() noexcept {
		if (!live_) {
			return {};
		}
		live_ = false;
		return std::move(state_);
	}

	template<work_value U, ControlCategory C, class Sink>
	friend void abandon_impl(BasicJoinHandle<U, C> &&, Sink &&) noexcept;
	template<work_value U, class Sink>
		requires abandon_sink<Sink, U>
	friend AbandonStatus try_abandon_to(BasicJoinHandle<U, ControlCategory::task> &&, Sink &&) noexcept;
	template<work_value U, class Sink>
		requires abandon_sink<Sink, U>
	friend AbandonStatus try_abandon_to(BasicJoinHandle<U, ControlCategory::posted> &&, Sink &&) noexcept;
	template<work_value U, class Sink>
		requires abandon_sink<Sink, U>
	friend AbandonStatus try_abandon_to(BasicJoinHandle<U, ControlCategory::operation> &&, Sink &&) noexcept;
	template<work_value U>
	friend Outcome<U> join(BasicJoinHandle<U, ControlCategory::task> &&);
	template<progress_capability Owner, work_value U>
	friend Outcome<U> join(Owner &, BasicJoinHandle<U, ControlCategory::posted> &&);
	template<progress_capability Driver, work_value U>
	friend Outcome<U> join(Driver &, BasicJoinHandle<U, ControlCategory::operation> &&);

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
};

template<work_value T>
using TaskJoinHandle = BasicJoinHandle<T, ControlCategory::task>;

template<work_value T>
using PostedJoinHandle = BasicJoinHandle<T, ControlCategory::posted>;

template<work_value T>
using OperationJoinHandle = BasicJoinHandle<T, ControlCategory::operation>;

template<class H>
concept work_handle = work_value<typename H::value_type> && requires(H const &h) { h.control(); };

template<class Sink, work_value T>
[[nodiscard]] detail::small_move_only_function<void(Outcome<T> const &)> make_abandon_dispatch_sink(
	Sink &&sink) noexcept {
	using sink_t = std::remove_cvref_t<Sink>;
	if constexpr (std::is_nothrow_invocable_v<sink_t &, Outcome<T> const &>) {
		return detail::small_move_only_function<void(Outcome<T> const &)>{std::forward<Sink>(sink)};
	} else {
		return detail::small_move_only_function<void(Outcome<T> const &)>{
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
	auto state = r.consume(join_state::detached);
	if (!state) {
		return; // empty or already-detached/joined — no-op
	}
	state->install_abandon_sink(make_abandon_dispatch_sink<Sink, T>(std::forward<Sink>(sink)));
}

template<work_value T, ControlCategory Category, class Sink>
void abandon_impl(
	BasicJoinHandle<T, Category> &&h,
	Sink &&sink) noexcept {
	auto state = h.consume();
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
	s.fn = detail::small_move_only_function<void(std::source_location, OutcomeKind, std::exception_ptr)>{
		std::forward<Fn>(fn)};
	s.installed.store(true, std::memory_order_release);
}

template<class R, class Sink = drop_on_abandon>
class scoped_abandon {
	Opt<R> value_{};
	Opt<Sink> sink_{};
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
[[nodiscard]] P<Task<T>, TaskSource<T>> make_task_source(
	SubmitOptions opts = {},
	std::source_location loc = std::source_location::current()) {
	SP<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = std::make_shared<detail::ControlBlockModel<T, true>>();
	} else {
		state = std::make_shared<detail::ControlBlockModel<T, false>>();
	}
	return {Task<T>::from_state(state, loc), TaskSource<T>::from_state(std::move(state))};
}

template<work_value T, progress_capability Owner>
[[nodiscard]] P<Posted<T>, PostedSource<T>> make_posted_source(
	Owner &owner,
	PostOptions opts = {},
	std::source_location loc = std::source_location::current()) {
	SP<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = std::make_shared<detail::ControlBlockModel<T, true>>();
	} else {
		state = std::make_shared<detail::ControlBlockModel<T, false>>();
	}
	state->set_required_capability(capability_id(owner));
	return {Posted<T>::from_state(state, loc), PostedSource<T>::from_state(std::move(state))};
}

template<work_value T, progress_capability Driver>
[[nodiscard]] P<Operation<T>, OperationSource<T>> make_operation_source(
	Driver &driver,
	OperationOptions opts = {},
	std::source_location loc = std::source_location::current()) {
	SP<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = std::make_shared<detail::ControlBlockModel<T, true>>();
	} else {
		state = std::make_shared<detail::ControlBlockModel<T, false>>();
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

template<work_value T>
[[nodiscard]] Outcome<T> join(
	Task<T> &&task) {
	auto state = task.consume(join_state::joined);
	if (!state) {
		throw JoinError{JoinError::reason::lifetime_violation};
	}
	return state->wait_and_take_outcome();
}

template<progress_capability Owner, work_value T>
[[nodiscard]] Outcome<T> join(
	Owner &owner,
	Posted<T> &&posted) {
	auto state = posted.consume(join_state::joined);
	if (!state) {
		throw JoinError{JoinError::reason::lifetime_violation};
	}
	if (!state->can_join_with(capability_id(owner))) {
		throw JoinError{JoinError::reason::capability_mismatch, state->required_capability(), capability_id(owner)};
	}
	return state->wait_and_take_outcome();
}

template<progress_capability Driver, work_value T>
[[nodiscard]] Outcome<T> join(
	Driver &driver,
	Operation<T> &&op) {
	auto state = op.consume(join_state::joined);
	if (!state) {
		throw JoinError{JoinError::reason::lifetime_violation};
	}
	if (!state->can_join_with(capability_id(driver))) {
		throw JoinError{JoinError::reason::capability_mismatch, state->required_capability(), capability_id(driver)};
	}
	return state->wait_and_take_outcome();
}

template<work_value T>
[[nodiscard]] Outcome<T> join(
	TaskJoinHandle<T> &&h) {
	auto state = h.consume();
	if (!state) {
		throw JoinError{JoinError::reason::lifetime_violation};
	}
	return state->wait_and_take_outcome();
}

template<progress_capability Owner, work_value T>
[[nodiscard]] Outcome<T> join(
	Owner &owner,
	PostedJoinHandle<T> &&h) {
	auto state = h.consume();
	if (!state) {
		throw JoinError{JoinError::reason::lifetime_violation};
	}
	if (!state->can_join_with(capability_id(owner))) {
		throw JoinError{JoinError::reason::capability_mismatch, state->required_capability(), capability_id(owner)};
	}
	return state->wait_and_take_outcome();
}

template<progress_capability Driver, work_value T>
[[nodiscard]] Outcome<T> join(
	Driver &driver,
	OperationJoinHandle<T> &&h) {
	auto state = h.consume();
	if (!state) {
		throw JoinError{JoinError::reason::lifetime_violation};
	}
	if (!state->can_join_with(capability_id(driver))) {
		throw JoinError{JoinError::reason::capability_mismatch, state->required_capability(), capability_id(driver)};
	}
	return state->wait_and_take_outcome();
}

template<work_value T>
[[nodiscard]] T value(
	Task<T> &&task) {
	return root::value(join(std::move(task)));
}

template<progress_capability Owner, work_value T>
[[nodiscard]] T value(
	Owner &owner,
	Posted<T> &&posted) {
	return root::value(join(owner, std::move(posted)));
}

template<progress_capability Driver, work_value T>
[[nodiscard]] T value(
	Driver &driver,
	Operation<T> &&op) {
	return root::value(join(driver, std::move(op)));
}

template<work_value T>
[[nodiscard]] T value(
	TaskJoinHandle<T> &&h) {
	return root::value(join(std::move(h)));
}

template<progress_capability Owner, work_value T>
[[nodiscard]] T value(
	Owner &owner,
	PostedJoinHandle<T> &&h) {
	return root::value(join(owner, std::move(h)));
}

template<progress_capability Driver, work_value T>
[[nodiscard]] T value(
	Driver &driver,
	OperationJoinHandle<T> &&h) {
	return root::value(join(driver, std::move(h)));
}

inline void value(
	Task<void> &&task) {
	root::value(join(std::move(task)));
}

template<progress_capability Owner>
inline void value(
	Owner &owner,
	Posted<void> &&posted) {
	root::value(join(owner, std::move(posted)));
}

template<progress_capability Driver>
inline void value(
	Driver &driver,
	Operation<void> &&op) {
	root::value(join(driver, std::move(op)));
}

inline void value(
	TaskJoinHandle<void> &&h) {
	root::value(join(std::move(h)));
}

template<progress_capability Owner>
inline void value(
	Owner &owner,
	PostedJoinHandle<void> &&h) {
	root::value(join(owner, std::move(h)));
}

template<progress_capability Driver>
inline void value(
	Driver &driver,
	OperationJoinHandle<void> &&h) {
	root::value(join(driver, std::move(h)));
}

template<work_value T>
[[nodiscard]] P<TaskControl, TaskSource<T>> make_task_control_source() {
	auto state = std::make_shared<detail::ControlBlockModel<T, true>>();
	return {TaskControl{state}, TaskSource<T>::from_state(std::move(state))};
}

template<work_value T>
[[nodiscard]] P<TaskControl, TaskSource<T>> make_task_control_source(
	SubmitOptions opts) {
	SP<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = std::make_shared<detail::ControlBlockModel<T, true>>();
	} else {
		state = std::make_shared<detail::ControlBlockModel<T, false>>();
	}
	return {TaskControl{state}, TaskSource<T>::from_state(std::move(state))};
}

template<work_value T>
[[nodiscard]] P<PostedControl, PostedSource<T>> make_posted_control_source() {
	auto state = std::make_shared<detail::ControlBlockModel<T, true>>();
	return {PostedControl{state}, PostedSource<T>::from_state(std::move(state))};
}

template<work_value T>
[[nodiscard]] P<PostedControl, PostedSource<T>> make_posted_control_source(
	PostOptions opts) {
	SP<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = std::make_shared<detail::ControlBlockModel<T, true>>();
	} else {
		state = std::make_shared<detail::ControlBlockModel<T, false>>();
	}
	return {PostedControl{state}, PostedSource<T>::from_state(std::move(state))};
}

template<work_value T>
[[nodiscard]] P<OperationControl, OperationSource<T>> make_operation_control_source() {
	auto state = std::make_shared<detail::ControlBlockModel<T, true>>();
	return {OperationControl{state}, OperationSource<T>::from_state(std::move(state))};
}

template<work_value T>
[[nodiscard]] P<OperationControl, OperationSource<T>> make_operation_control_source(
	OperationOptions opts) {
	SP<detail::ControlBlockInterface<T>> state{};
	if (opts.enable_cancellation) {
		state = std::make_shared<detail::ControlBlockModel<T, true>>();
	} else {
		state = std::make_shared<detail::ControlBlockModel<T, false>>();
	}
	return {OperationControl{state}, OperationSource<T>::from_state(std::move(state))};
}

} // namespace conflux::work::root
