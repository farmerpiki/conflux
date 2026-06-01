module;

export module conflux.work.race;

import std;
import conflux.types;
import conflux.small_function;
import conflux.work.root;
import conflux.work.carrier;

export namespace conflux::work::race {

namespace root = conflux::work::root;
namespace carrier = conflux::work::carrier;

enum class winner_policy : std::uint8_t {
	first_completion,
	first_success,
};

enum class loser_policy : std::uint8_t {
	leave_running,
	request_cancel,
	request_cancel_and_wait,
};

enum class loser_cleanup_policy : std::uint8_t {
	wait_unbounded,
	wait_until_cleanup_deadline,
	detach_after_cleanup_deadline,
	fail_after_cleanup_deadline,
};

struct race_options {
	winner_policy winner = winner_policy::first_completion;
	loser_policy losers = loser_policy::request_cancel_and_wait;
	loser_cleanup_policy cleanup = loser_cleanup_policy::wait_unbounded;
	std::chrono::steady_clock::duration loser_cleanup_budget{};
	root::CancelReason default_loser_reason = root::CancelReason::requested;
	bool preserve_winner_latency = false;
	bool collect_loser_outcomes = false;
};

enum class race_winner_kind : std::uint8_t {
	value_candidate,
	trigger,
};

struct race_winner_info {
	std::size_t index{};
	std::string_view label{};
	race_winner_kind kind{};
	root::CancelReason reason = root::CancelReason::requested;
	std::chrono::nanoseconds latency{};
};

struct race_observation {
	std::size_t participant_count{};
	std::size_t loser_cancel_requested{};
	std::size_t cleanup_timeout_count{};
	bool trigger_won = false;
	bool all_failed = false;
};

template<root::work_value T>
struct race_loser_result {
	std::size_t index{};
	std::string_view label{};
	root::Outcome<T> outcome;
};

template<root::work_value T>
struct race_result {
	race_winner_info winner;
	root::Outcome<T> outcome;
	race_observation observation{};
	std::vector<race_loser_result<T>> loser_outcomes{};
};

struct race_aggregate_error_entry {
	std::size_t index{};
	std::string_view label{};
	std::exception_ptr error{};
};

class race_aggregate_error final : public root::WorkError {
	std::vector<race_aggregate_error_entry> entries_;

public:
	explicit race_aggregate_error(
		std::vector<race_aggregate_error_entry> entries)
		: WorkError{"race: all value candidates failed"}
		, entries_{std::move(entries)} {}
	[[nodiscard]] std::span<race_aggregate_error_entry const> entries() const noexcept { return entries_; }
};

class owned_race_aggregate_error final : public root::WorkError {
	std::vector<std::string> labels_;
	std::vector<race_aggregate_error_entry> entries_;

	void rebase() noexcept {
		for (auto &entry: entries_) {
			entry.label = entry.index < labels_.size() ? std::string_view{labels_[entry.index]} : std::string_view{};
		}
	}

public:
	owned_race_aggregate_error(
		std::vector<std::string> labels,
		std::vector<race_aggregate_error_entry> entries)
		: WorkError{"race: all value candidates failed"}
		, labels_{std::move(labels)}
		, entries_{std::move(entries)} {
		rebase();
	}
	owned_race_aggregate_error(
		owned_race_aggregate_error const &other)
		: WorkError{"race: all value candidates failed"}
		, labels_{other.labels_}
		, entries_{other.entries_} {
		rebase();
	}
	owned_race_aggregate_error &operator =(owned_race_aggregate_error const &) = delete;
	[[nodiscard]] std::span<race_aggregate_error_entry const> entries() const noexcept { return entries_; }
};

class race_setup_error final : public root::WorkError {
public:
	explicit race_setup_error(
		char const *msg)
		: WorkError{msg} {}
};

class race_cleanup_error final : public root::WorkError {
	race_observation observation_{};

public:
	explicit race_cleanup_error(
		char const *msg)
		: WorkError{msg} {}
	race_cleanup_error(
		char const *msg,
		race_observation observation)
		: WorkError{msg}
		, observation_{observation} {}
	[[nodiscard]] race_observation observation() const noexcept { return observation_; }
};

template<root::work_value T>
struct race_candidate {
	std::string_view label{};
	std::variant<root::TaskJoinHandle<T>, root::Outcome<T>> payload;
};

struct race_trigger {
	std::string_view label{};
	root::TaskJoinHandle<void> handle{};
	root::CancelReason reason = root::CancelReason::requested;
};

template<root::work_value T>
[[nodiscard]] race_candidate<T> candidate(
	root::Task<T> task) noexcept {
	return race_candidate<T>{.payload = root::into_join_handle(std::move(task))};
}

template<root::work_value T>
[[nodiscard]] race_candidate<T> candidate(
	std::string_view label,
	root::Task<T> task) noexcept {
	return race_candidate<T>{.label = label, .payload = root::into_join_handle(std::move(task))};
}

template<root::work_value T>
[[nodiscard]] race_candidate<T> candidate(
	root::TaskJoinHandle<T> handle) noexcept {
	return race_candidate<T>{.payload = std::move(handle)};
}

template<root::work_value T>
[[nodiscard]] race_candidate<T> candidate(
	std::string_view label,
	root::TaskJoinHandle<T> handle) noexcept {
	return race_candidate<T>{.label = label, .payload = std::move(handle)};
}

template<root::work_value T>
[[nodiscard]] race_candidate<T> candidate(
	carrier::Chain<T> chain) noexcept {
	return race_candidate<T>{.payload = std::move(chain).release_outcome()};
}

template<root::work_value T>
[[nodiscard]] race_candidate<T> candidate(
	std::string_view label,
	carrier::Chain<T> chain) noexcept {
	return race_candidate<T>{.label = label, .payload = std::move(chain).release_outcome()};
}

template<class Fn>
[[nodiscard]] auto task(
	std::string_view label,
	Fn &&fn) {
	return candidate(label, root::make_cancellable_task(std::forward<Fn>(fn)));
}

[[nodiscard]] race_trigger trigger(
	root::Task<void> task,
	root::CancelReason reason) noexcept {
	return race_trigger{.handle = root::into_join_handle(std::move(task)), .reason = reason};
}

[[nodiscard]] race_trigger trigger(
	std::string_view label,
	root::Task<void> task,
	root::CancelReason reason) noexcept {
	return race_trigger{.label = label, .handle = root::into_join_handle(std::move(task)), .reason = reason};
}

[[nodiscard]] race_trigger trigger(
	root::TaskJoinHandle<void> handle,
	root::CancelReason reason) noexcept {
	return race_trigger{.handle = std::move(handle), .reason = reason};
}

[[nodiscard]] race_trigger trigger(
	std::string_view label,
	root::TaskJoinHandle<void> handle,
	root::CancelReason reason) noexcept {
	return race_trigger{.label = label, .handle = std::move(handle), .reason = reason};
}

namespace detail {

struct blocking_fallback_trigger_state {
	std::mutex mu;
	std::condition_variable cv;
	bool cancelled = false;
	root::CancelReason cancel_reason = root::CancelReason::requested;
};

inline void complete_blocking_fallback_trigger(
	root::TaskSource<void> &src,
	bool cancelled,
	root::CancelReason cancel_reason) noexcept {
	if (cancelled) {
		(void)src.try_set_cancelled(cancel_reason);
	} else {
		(void)src.try_set_value();
	}
}

template<class Wait>
[[nodiscard]] root::Task<void> make_blocking_fallback_trigger_task(
	Wait wait) {
	auto [task, src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = true});
	auto state = std::make_shared<blocking_fallback_trigger_state>();
	auto shared_src = std::make_shared<root::TaskSource<void>>(std::move(src));
	(void)shared_src->install_cancel_hook([state](root::CancelReason cancel_reason) noexcept {
		{
			std::scoped_lock lk{state->mu};
			state->cancelled = true;
			state->cancel_reason = cancel_reason;
		}
		state->cv.notify_one();
	});
	try {
		std::thread{[state, shared_src, wait = std::move(wait)]() mutable { wait(state, shared_src); }}.detach();
	} catch (...) { (void)shared_src->try_set_exception(std::current_exception()); }
	return std::move(task);
}

} // namespace detail

[[nodiscard]] race_trigger until_stop_token(
	std::string_view label,
	std::stop_token token,
	root::CancelReason reason = root::CancelReason::shutdown) {
	auto [task, src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = true});
	if (token.stop_requested()) {
		(void)src.try_set_value();
		return trigger(label, std::move(task), reason);
	}
	return trigger(
		label,
		detail::make_blocking_fallback_trigger_task(
			[token = std::move(token)](
				std::shared_ptr<detail::blocking_fallback_trigger_state> const &state,
				std::shared_ptr<root::TaskSource<void>> const &src) mutable {
				root::CancelReason cancel_reason = root::CancelReason::requested;
				bool cancelled = false;
				{
					std::unique_lock lk{state->mu};
					while (!state->cancelled && !token.stop_requested()) {
						state->cv.wait_for(lk, std::chrono::milliseconds{1});
					}
					cancelled = state->cancelled;
					cancel_reason = state->cancel_reason;
				}
				detail::complete_blocking_fallback_trigger(*src, cancelled, cancel_reason);
			}),
		reason);
}

[[nodiscard]] race_trigger until_stop_token(
	std::stop_token token,
	root::CancelReason reason = root::CancelReason::shutdown) {
	return until_stop_token("stop_token", std::move(token), reason);
}

[[nodiscard]] race_trigger timeout_after(
	std::string_view label,
	std::chrono::steady_clock::duration duration,
	root::CancelReason reason = root::CancelReason::deadline) {
	auto [task, src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = true});
	if (duration <= std::chrono::steady_clock::duration{}) {
		(void)src.try_set_value();
		return trigger(label, std::move(task), reason);
	}
	return trigger(
		label,
		detail::make_blocking_fallback_trigger_task(
			[duration](
				std::shared_ptr<detail::blocking_fallback_trigger_state> const &state,
				std::shared_ptr<root::TaskSource<void>> const &src) {
				root::CancelReason cancel_reason = root::CancelReason::requested;
				bool cancelled = false;
				{
					std::unique_lock lk{state->mu};
					cancelled = state->cv.wait_for(lk, duration, [&] { return state->cancelled; });
					cancel_reason = state->cancel_reason;
				}
				detail::complete_blocking_fallback_trigger(*src, cancelled, cancel_reason);
			}),
		reason);
}

[[nodiscard]] race_trigger timeout_after(
	std::chrono::steady_clock::duration duration,
	root::CancelReason reason = root::CancelReason::deadline) {
	return timeout_after("deadline", duration, reason);
}

[[nodiscard]] race_trigger timeout_at(
	std::string_view label,
	std::chrono::steady_clock::time_point deadline,
	root::CancelReason reason = root::CancelReason::deadline) {
	return timeout_after(label, deadline - std::chrono::steady_clock::now(), reason);
}

[[nodiscard]] race_trigger timeout_at(
	std::chrono::steady_clock::time_point deadline,
	root::CancelReason reason = root::CancelReason::deadline) {
	return timeout_at("deadline", deadline, reason);
}

namespace detail {

template<root::work_value T>
void complete_source_from_outcome(
	root::TaskSource<T> &src,
	root::Outcome<T> out) noexcept {
	if (out.is_success()) {
		(void)src.try_set_value(std::move(out).success());
	} else if (out.is_cancelled()) {
		(void)src.try_set_cancelled(std::move(out).cancelled().reason);
	} else {
		(void)src.try_set_exception(std::move(out).failure().error);
	}
}

template<root::progress_capability Cap, root::work_value T, class Handle>
[[nodiscard]] root::Task<T> adapt_bound_handle(
	Cap &cap,
	Handle handle) {
	struct State {
		Cap *cap{};
		Handle handle{};
		std::shared_ptr<root::TaskSource<T>> src{};

		void complete() noexcept {
			try {
				complete_source_from_outcome(*src, root::join_ready(*cap, std::move(handle)));
			} catch (...) {
				if (handle) {
					root::abandon_to(std::move(handle), root::drop_on_abandon{});
				}
				(void)src->try_set_exception(std::current_exception());
			}
		}
	};

	auto [task, src] = root::make_task_source<T>(root::SubmitOptions{.enable_cancellation = true});
	auto shared_src = std::make_shared<root::TaskSource<T>>(std::move(src));
	auto state = std::make_shared<State>(State{.cap = &cap, .handle = std::move(handle), .src = shared_src});
	std::weak_ptr<State> weak_state{state};
	(void)shared_src->install_cancel_hook([weak_state](root::CancelReason reason) noexcept {
		if (auto state = weak_state.lock()) {
			(void)state->handle.control().request_cancel(reason);
		}
	});
	auto ready = [state]() noexcept { state->complete(); };
	auto result = state->handle.control().try_set_on_ready(::conflux::detail::small_move_only_function<void()>{ready});
	switch (result.status) {
	case root::ReadyRegistration::installed: break;
	case root::ReadyRegistration::already_ready:
		if (result.rejected_fn) {
			result.rejected_fn();
		}
		break;
	case root::ReadyRegistration::already_installed:
		(void)shared_src->try_set_exception(
			std::make_exception_ptr(race_setup_error{"race: participant already has a ready callback"}));
		break;
	case root::ReadyRegistration::empty:
		(void)shared_src->try_set_exception(std::make_exception_ptr(race_setup_error{"race: empty participant"}));
		break;
	}
	return std::move(task);
}

} // namespace detail

template<root::progress_capability Owner, root::work_value T>
[[nodiscard]] race_candidate<T> candidate_on(
	Owner &owner,
	root::Posted<T> posted) {
	return candidate(detail::adapt_bound_handle<Owner, T>(owner, root::into_join_handle(std::move(posted))));
}

template<root::progress_capability Owner, root::work_value T>
[[nodiscard]] race_candidate<T> candidate_on(
	Owner &owner,
	std::string_view label,
	root::Posted<T> posted) {
	return candidate(label, detail::adapt_bound_handle<Owner, T>(owner, root::into_join_handle(std::move(posted))));
}

template<root::progress_capability Owner, root::work_value T>
[[nodiscard]] race_candidate<T> candidate_on(
	Owner &owner,
	root::PostedJoinHandle<T> handle) {
	return candidate(detail::adapt_bound_handle<Owner, T>(owner, std::move(handle)));
}

template<root::progress_capability Owner, root::work_value T>
[[nodiscard]] race_candidate<T> candidate_on(
	Owner &owner,
	std::string_view label,
	root::PostedJoinHandle<T> handle) {
	return candidate(label, detail::adapt_bound_handle<Owner, T>(owner, std::move(handle)));
}

template<root::progress_capability Driver, root::work_value T>
[[nodiscard]] race_candidate<T> candidate_on(
	Driver &driver,
	root::Operation<T> op) {
	return candidate(detail::adapt_bound_handle<Driver, T>(driver, root::into_join_handle(std::move(op))));
}

template<root::progress_capability Driver, root::work_value T>
[[nodiscard]] race_candidate<T> candidate_on(
	Driver &driver,
	std::string_view label,
	root::Operation<T> op) {
	return candidate(label, detail::adapt_bound_handle<Driver, T>(driver, root::into_join_handle(std::move(op))));
}

template<root::progress_capability Driver, root::work_value T>
[[nodiscard]] race_candidate<T> candidate_on(
	Driver &driver,
	root::OperationJoinHandle<T> handle) {
	return candidate(detail::adapt_bound_handle<Driver, T>(driver, std::move(handle)));
}

template<root::progress_capability Driver, root::work_value T>
[[nodiscard]] race_candidate<T> candidate_on(
	Driver &driver,
	std::string_view label,
	root::OperationJoinHandle<T> handle) {
	return candidate(label, detail::adapt_bound_handle<Driver, T>(driver, std::move(handle)));
}

template<root::progress_capability Owner>
[[nodiscard]] race_trigger trigger_on(
	Owner &owner,
	root::Posted<void> posted,
	root::CancelReason reason) {
	return trigger(detail::adapt_bound_handle<Owner, void>(owner, root::into_join_handle(std::move(posted))), reason);
}

template<root::progress_capability Owner>
[[nodiscard]] race_trigger trigger_on(
	Owner &owner,
	std::string_view label,
	root::Posted<void> posted,
	root::CancelReason reason) {
	return trigger(
		label,
		detail::adapt_bound_handle<Owner, void>(owner, root::into_join_handle(std::move(posted))),
		reason);
}

template<root::progress_capability Owner>
[[nodiscard]] race_trigger trigger_on(
	Owner &owner,
	root::PostedJoinHandle<void> handle,
	root::CancelReason reason) {
	return trigger(detail::adapt_bound_handle<Owner, void>(owner, std::move(handle)), reason);
}

template<root::progress_capability Owner>
[[nodiscard]] race_trigger trigger_on(
	Owner &owner,
	std::string_view label,
	root::PostedJoinHandle<void> handle,
	root::CancelReason reason) {
	return trigger(label, detail::adapt_bound_handle<Owner, void>(owner, std::move(handle)), reason);
}

template<root::progress_capability Driver>
[[nodiscard]] race_trigger trigger_on(
	Driver &driver,
	root::Operation<void> op,
	root::CancelReason reason) {
	return trigger(detail::adapt_bound_handle<Driver, void>(driver, root::into_join_handle(std::move(op))), reason);
}

template<root::progress_capability Driver>
[[nodiscard]] race_trigger trigger_on(
	Driver &driver,
	std::string_view label,
	root::Operation<void> op,
	root::CancelReason reason) {
	return trigger(
		label,
		detail::adapt_bound_handle<Driver, void>(driver, root::into_join_handle(std::move(op))),
		reason);
}

template<root::progress_capability Driver>
[[nodiscard]] race_trigger trigger_on(
	Driver &driver,
	root::OperationJoinHandle<void> handle,
	root::CancelReason reason) {
	return trigger(detail::adapt_bound_handle<Driver, void>(driver, std::move(handle)), reason);
}

template<root::progress_capability Driver>
[[nodiscard]] race_trigger trigger_on(
	Driver &driver,
	std::string_view label,
	root::OperationJoinHandle<void> handle,
	root::CancelReason reason) {
	return trigger(label, detail::adapt_bound_handle<Driver, void>(driver, std::move(handle)), reason);
}

namespace detail {

template<root::work_value T>
struct participant {
	bool trigger = false;
	std::string_view label{};
	root::CancelReason trigger_reason = root::CancelReason::requested;
	std::variant<std::monostate, root::TaskJoinHandle<T>, root::TaskJoinHandle<void>> handle{};
	std::optional<root::Outcome<T>> ready_value{};
	std::chrono::steady_clock::time_point started{};
	bool live = false;
	bool terminal = false;
	bool winner = false;
};

template<root::work_value T, std::size_t ParticipantCount>
class race_state final : public std::enable_shared_from_this<race_state<T, ParticipantCount>> {
	using result_t = race_result<T>;
	struct cancel_proxy {
		std::mutex mu{};
		race_state *state{};
	};

	race_options opts_;
	root::TaskSource<result_t> out_;
	std::array<participant<T>, ParticipantCount> ps_{};
	std::size_t ps_size_ = 0;
	std::mutex mu_;
	std::size_t live_remaining_ = 0;
	std::size_t value_remaining_ = 0;
	bool winner_selected_ = false;
	bool output_committed_ = false;
	std::size_t winner_index_ = 0;
	std::optional<root::Outcome<T>> winner_outcome_{};
	race_winner_info winner_info_{};
	race_observation observation_{};
	std::vector<race_loser_result<T>> loser_outcomes_{};
	std::vector<race_aggregate_error_entry> failures_{};
	std::optional<root::Cancelled> first_cancel_{};
	std::size_t first_cancel_index_ = 0;
	bool cleanup_timer_started_ = false;
	std::shared_ptr<cancel_proxy> output_cancel_proxy_{std::make_shared<cancel_proxy>()};

public:
	race_state(
		race_options opts,
		root::TaskSource<result_t> out)
		: opts_{opts}
		, out_{std::move(out)} {
		std::scoped_lock lk{output_cancel_proxy_->mu};
		output_cancel_proxy_->state = this;
	}

	~race_state() noexcept {
		std::scoped_lock lk{output_cancel_proxy_->mu};
		output_cancel_proxy_->state = nullptr;
	}

	void add(
		race_candidate<T> c) {
		if (ps_size_ >= ps_.size()) {
			fail_setup("race: too many participants");
			return;
		}
		participant<T> p{
			.trigger = false,
			.label = c.label,
			.started = std::chrono::steady_clock::now(),
		};
		if (std::holds_alternative<root::Outcome<T>>(c.payload)) {
			p.ready_value.emplace(std::move(std::get<root::Outcome<T>>(c.payload)));
			p.terminal = true;
		} else {
			p.handle = std::move(std::get<root::TaskJoinHandle<T>>(c.payload));
			p.live = true;
			++live_remaining_;
		}
		++value_remaining_;
		ps_[ps_size_++] = std::move(p);
	}

	void configure_storage(
		std::size_t n) {
		failures_.reserve(n);
		observation_.participant_count = n;
		if (opts_.collect_loser_outcomes) {
			loser_outcomes_.reserve(n > 0 ? n - 1 : 0);
		}
	}

	void add(
		race_trigger t) {
		if (ps_size_ >= ps_.size()) {
			fail_setup("race: too many participants");
			return;
		}
		participant<T> p{
			.trigger = true,
			.label = t.label,
			.trigger_reason = t.reason,
			.handle = std::move(t.handle),
			.started = std::chrono::steady_clock::now(),
			.live = true,
		};
		++live_remaining_;
		ps_[ps_size_++] = std::move(p);
	}

	[[nodiscard]] bool register_all() {
		for (std::size_t i = 0; i < ps_size_; ++i) {
			if (!ps_[i].live) {
				continue;
			}
			auto cb = [self = this->shared_from_this(), i]() noexcept { self->on_ready(i); };
			auto result =
				control(i).try_set_on_ready(::conflux::detail::small_move_only_function<void()>{std::move(cb)});
			switch (result.status) {
			case root::ReadyRegistration::installed: break;
			case root::ReadyRegistration::already_ready:
				if (result.rejected_fn) {
					result.rejected_fn();
				}
				break;
			case root::ReadyRegistration::already_installed:
				fail_setup("race: participant already has a ready callback");
				return false;
			case root::ReadyRegistration::empty: fail_setup("race: empty participant"); return false;
			}
		}
		for (std::size_t i = 0; i < ps_size_; ++i) {
			if (ps_[i].ready_value) {
				on_value_outcome(i, std::move(*ps_[i].ready_value));
				ps_[i].ready_value.reset();
			}
		}
		maybe_commit_after_registration();
		return true;
	}

	void install_output_cancel_hook() noexcept {
		(void)out_.install_cancel_hook([proxy = output_cancel_proxy_](root::CancelReason reason) noexcept {
			std::scoped_lock lk{proxy->mu};
			if (proxy->state != nullptr) {
				proxy->state->request_cancel(reason);
			}
		});
	}

	void request_cancel(
		root::CancelReason reason) noexcept {
		std::vector<root::TaskControl> to_cancel;
		{
			std::scoped_lock lk{mu_};
			if (output_committed_) {
				return;
			}
			output_committed_ = true;
			for (std::size_t i = 0; i < ps_size_; ++i) {
				if (ps_[i].live && !ps_[i].terminal) {
					to_cancel.push_back(control(i));
				}
			}
		}
		for (auto &ctrl: to_cancel) {
			(void)ctrl.request_cancel(reason);
		}
		(void)out_.try_set_cancelled(reason);
	}

	void reject_unsupported_cleanup_options() noexcept {
		{
			std::scoped_lock lk{mu_};
			if (output_committed_) {
				return;
			}
			output_committed_ = true;
			for (std::size_t i = 0; i < ps_size_; ++i) {
				if (ps_[i].live && !ps_[i].terminal) {
					(void)control(i).request_cancel(opts_.default_loser_reason);
					abandon_participant_locked(i);
					ps_[i].live = false;
					ps_[i].terminal = true;
				}
			}
			live_remaining_ = 0;
		}
		try {
			(void)out_.try_set_exception(
				std::make_exception_ptr(race_setup_error{"race: unsupported cleanup budget policy"}));
		} catch (...) { (void)out_.try_set_exception(std::current_exception()); }
	}

private:
	[[nodiscard]] auto control(
		std::size_t i) noexcept {
		if (ps_[i].trigger) {
			return std::get<root::TaskJoinHandle<void>>(ps_[i].handle).control();
		}
		return std::get<root::TaskJoinHandle<T>>(ps_[i].handle).control();
	}

	void abandon_participant_locked(
		std::size_t i) noexcept {
		if (ps_[i].trigger) {
			(void)root::try_abandon_to(
				std::move(std::get<root::TaskJoinHandle<void>>(ps_[i].handle)),
				root::drop_on_abandon{});
			return;
		}
		(void)root::try_abandon_to(
			std::move(std::get<root::TaskJoinHandle<T>>(ps_[i].handle)),
			root::drop_on_abandon{});
	}

	void fail_setup(
		char const *msg) noexcept {
		{
			std::scoped_lock lk{mu_};
			if (output_committed_) {
				return;
			}
			output_committed_ = true;
			for (std::size_t i = 0; i < ps_size_; ++i) {
				if (ps_[i].live && !ps_[i].terminal) {
					(void)control(i).request_cancel(opts_.default_loser_reason);
					abandon_participant_locked(i);
					ps_[i].live = false;
					ps_[i].terminal = true;
				}
			}
			live_remaining_ = 0;
		}
		try {
			(void)out_.try_set_exception(std::make_exception_ptr(race_setup_error{msg}));
		} catch (...) { (void)out_.try_set_exception(std::current_exception()); }
	}

	void on_ready(
		std::size_t i) noexcept {
		std::vector<root::TaskControl> losers_to_cancel;
		bool commit_now = false;
		bool start_cleanup_timer = false;
		try {
			std::scoped_lock lk{mu_};
			if (ps_[i].terminal && !ps_[i].ready_value) {
				return;
			}
			auto out = join_participant_ready_locked(i);
			ps_[i].terminal = true;
			if (ps_[i].live && live_remaining_ > 0) {
				--live_remaining_;
			}
			if (!ps_[i].trigger && value_remaining_ > 0) {
				--value_remaining_;
			}
			if (output_committed_) {
				collect_loser_outcome_locked(i, std::move(out));
				return;
			}
			process_outcome_locked(i, std::move(out), losers_to_cancel);
			commit_now = should_commit_locked();
			if (commit_now) {
				output_committed_ = true;
			}
			start_cleanup_timer = should_start_cleanup_timer_locked();
		} catch (...) { on_value_outcome(i, root::Outcome<T>{root::Failure{std::current_exception()}}); }
		for (auto &loser: losers_to_cancel) {
			(void)loser.request_cancel(opts_.default_loser_reason);
		}
		if (start_cleanup_timer) {
			start_cleanup_timer_thread();
		}
		if (commit_now) {
			commit_result();
		}
	}

	[[nodiscard]] root::Outcome<T> join_participant_ready_locked(
		std::size_t i) {
		if (!ps_[i].trigger) {
			return root::join_ready(std::move(std::get<root::TaskJoinHandle<T>>(ps_[i].handle)));
		}
		auto out = root::join_ready(std::move(std::get<root::TaskJoinHandle<void>>(ps_[i].handle)));
		if (out.is_failure()) {
			return root::Outcome<T>{std::move(out).failure()};
		}
		if (out.is_cancelled()) {
			return root::Outcome<T>{std::move(out).cancelled()};
		}
		return root::Outcome<T>{root::Cancelled{ps_[i].trigger_reason}};
	}

	void on_value_outcome(
		std::size_t i,
		root::Outcome<T> out) noexcept {
		std::vector<root::TaskControl> losers_to_cancel;
		bool commit_now = false;
		bool start_cleanup_timer = false;
		{
			std::scoped_lock lk{mu_};
			if (ps_[i].terminal && !ps_[i].ready_value) {
				return;
			}
			ps_[i].terminal = true;
			if (ps_[i].live && live_remaining_ > 0) {
				--live_remaining_;
			}
			if (!ps_[i].trigger && value_remaining_ > 0) {
				--value_remaining_;
			}
			process_outcome_locked(i, std::move(out), losers_to_cancel);
			commit_now = should_commit_locked();
			if (commit_now) {
				output_committed_ = true;
			}
			start_cleanup_timer = should_start_cleanup_timer_locked();
		}
		for (auto &loser: losers_to_cancel) {
			(void)loser.request_cancel(opts_.default_loser_reason);
		}
		if (start_cleanup_timer) {
			start_cleanup_timer_thread();
		}
		if (commit_now) {
			commit_result();
		}
	}

	void process_outcome_locked(
		std::size_t i,
		root::Outcome<T> out,
		std::vector<root::TaskControl> &losers_to_cancel) {
		if (winner_selected_) {
			collect_loser_outcome_locked(i, std::move(out));
			return;
		}
		if (ps_[i].trigger || opts_.winner == winner_policy::first_completion || out.is_success()) {
			select_winner_locked(i, std::move(out), losers_to_cancel);
			return;
		}
		if (out.is_failure()) {
			failures_.push_back(
				race_aggregate_error_entry{
					.index = i,
					.label = ps_[i].label,
					.error = std::move(out).failure().error,
				});
		} else if (out.is_cancelled() && !first_cancel_) {
			first_cancel_index_ = i;
			first_cancel_.emplace(std::move(out).cancelled());
		}
		if (value_remaining_ == 0) {
			observation_.all_failed = true;
			if (!failures_.empty()) {
				if (failures_.size() == 1) {
					auto const winner_index = failures_[0].index;
					auto error = failures_[0].error;
					select_winner_locked(
						winner_index,
						root::Outcome<T>{root::Failure{std::move(error)}},
						losers_to_cancel);
				} else {
					auto const winner_index = failures_[0].index;
					select_winner_locked(
						winner_index,
						root::Outcome<T>{
							root::Failure{std::make_exception_ptr(race_aggregate_error{std::move(failures_)})}},
						losers_to_cancel);
				}
			} else {
				select_winner_locked(
					first_cancel_ ? first_cancel_index_ : i,
					root::Outcome<T>{
						root::Cancelled{first_cancel_ ? first_cancel_->reason : opts_.default_loser_reason}},
					losers_to_cancel);
			}
		}
	}

	void select_winner_locked(
		std::size_t i,
		root::Outcome<T> out,
		std::vector<root::TaskControl> &losers_to_cancel) {
		winner_selected_ = true;
		winner_index_ = i;
		ps_[i].winner = true;
		winner_info_ = race_winner_info{
			.index = i,
			.label = ps_[i].label,
			.kind = ps_[i].trigger ? race_winner_kind::trigger : race_winner_kind::value_candidate,
			.reason = out.is_cancelled() ? out.cancelled().reason : root::CancelReason::requested,
			.latency = opts_.preserve_winner_latency ? std::chrono::duration_cast<std::chrono::nanoseconds>(
														   std::chrono::steady_clock::now() - ps_[i].started) :
													   std::chrono::nanoseconds{},
		};
		observation_.trigger_won = ps_[i].trigger;
		winner_outcome_.emplace(std::move(out));
		if (opts_.losers == loser_policy::request_cancel || opts_.losers == loser_policy::request_cancel_and_wait) {
			for (std::size_t idx = 0; idx < ps_size_; ++idx) {
				if (idx != i && ps_[idx].live && !ps_[idx].terminal) {
					losers_to_cancel.push_back(control(idx));
					++observation_.loser_cancel_requested;
				}
			}
		}
	}

	void collect_loser_outcome_locked(
		std::size_t i,
		root::Outcome<T> out) {
		if (!opts_.collect_loser_outcomes || i == winner_index_) {
			return;
		}
		loser_outcomes_.push_back(
			race_loser_result<T>{
				.index = i,
				.label = ps_[i].label,
				.outcome = std::move(out),
			});
	}

	[[nodiscard]] bool should_commit_locked() const noexcept {
		if (output_committed_ || !winner_selected_) {
			return false;
		}
		if (opts_.losers == loser_policy::request_cancel_and_wait) {
			return live_remaining_ == 0;
		}
		return true;
	}

	[[nodiscard]] bool should_start_cleanup_timer_locked() noexcept {
		if (cleanup_timer_started_ || output_committed_ || !winner_selected_ || live_remaining_ == 0) {
			return false;
		}
		if (opts_.losers != loser_policy::request_cancel_and_wait
			|| opts_.cleanup != loser_cleanup_policy::fail_after_cleanup_deadline
			|| opts_.loser_cleanup_budget <= std::chrono::steady_clock::duration{}) {
			return false;
		}
		cleanup_timer_started_ = true;
		return true;
	}

	void start_cleanup_timer_thread() noexcept {
		auto self = this->shared_from_this();
		auto budget = opts_.loser_cleanup_budget;
		try {
			std::thread{[self = std::move(self), budget] {
				std::this_thread::sleep_for(budget);
				self->expire_cleanup_budget();
			}}.detach();
		} catch (...) { expire_cleanup_budget(); }
	}

	void expire_cleanup_budget() noexcept {
		{
			std::scoped_lock lk{mu_};
			if (output_committed_) {
				return;
			}
			output_committed_ = true;
			++observation_.cleanup_timeout_count;
			for (std::size_t i = 0; i < ps_size_; ++i) {
				if (ps_[i].live && !ps_[i].terminal) {
					abandon_participant_locked(i);
					ps_[i].live = false;
					ps_[i].terminal = true;
				}
			}
			live_remaining_ = 0;
		}
		try {
			(void)out_.try_set_exception(
				std::make_exception_ptr(race_cleanup_error{"race: loser cleanup deadline expired", observation_}));
		} catch (...) { (void)out_.try_set_exception(std::current_exception()); }
	}

	void maybe_commit_after_registration() noexcept {
		bool commit_now = false;
		bool start_cleanup_timer = false;
		{
			std::scoped_lock lk{mu_};
			commit_now = should_commit_locked();
			if (commit_now) {
				output_committed_ = true;
			}
			start_cleanup_timer = should_start_cleanup_timer_locked();
		}
		if (start_cleanup_timer) {
			start_cleanup_timer_thread();
		}
		if (commit_now) {
			commit_result();
		}
	}

	void commit_result() noexcept {
		try {
			result_t result{
				.winner = winner_info_,
				.outcome = std::move(*winner_outcome_),
				.observation = observation_,
				.loser_outcomes = std::move(loser_outcomes_),
			};
			(void)out_.try_set_value(root::Success<result_t>{std::move(result)});
		} catch (...) { (void)out_.try_set_exception(std::current_exception()); }
	}
};

template<root::work_value T, class... Participants>
[[nodiscard]] root::Task<race_result<T>> make_race(
	race_options opts,
	Participants &&...participants) {
	auto [task, src] = root::make_task_source<race_result<T>>(root::SubmitOptions{.enable_cancellation = true});
	if constexpr (sizeof...(Participants) == 0) {
		(void)src.try_set_exception(std::make_exception_ptr(race_setup_error{"race: no participants"}));
		return std::move(task);
	}
	auto state = std::make_shared<race_state<T, sizeof...(Participants)>>(opts, std::move(src));
	state->configure_storage(sizeof...(Participants));
	(state->add(std::forward<Participants>(participants)), ...);
	auto out = std::move(task);
	state->install_output_cancel_hook();
	if ((opts.cleanup == loser_cleanup_policy::wait_unbounded
		 && opts.loser_cleanup_budget != std::chrono::steady_clock::duration{})
		|| (opts.cleanup != loser_cleanup_policy::wait_unbounded
			&& (opts.cleanup != loser_cleanup_policy::fail_after_cleanup_deadline
				|| opts.losers != loser_policy::request_cancel_and_wait
				|| opts.loser_cleanup_budget <= std::chrono::steady_clock::duration{}))) {
		state->reject_unsupported_cleanup_options();
		return out;
	}
	(void)state->register_all();
	return out;
}

} // namespace detail

template<root::work_value T, class... Participants>
[[nodiscard]] root::Task<race_result<T>> race(
	race_options opts,
	Participants &&...participants) {
	return detail::make_race<T>(opts, std::forward<Participants>(participants)...);
}

template<root::work_value T, class... Participants>
[[nodiscard]] root::Task<race_result<T>> race(
	Participants &&...participants) {
	return detail::make_race<T>(race_options{}, std::forward<Participants>(participants)...);
}

template<root::work_value T>
[[nodiscard]] root::Task<race_result<T>> with_timeout(
	root::Task<T> work,
	root::Task<void> timeout,
	race_options opts = {}) {
	opts.winner = winner_policy::first_completion;
	return race<T>(
		opts,
		candidate("work", std::move(work)),
		trigger("deadline", std::move(timeout), root::CancelReason::deadline));
}

template<root::work_value T>
[[nodiscard]] root::Task<race_result<T>> with_timeout(
	root::Task<T> work,
	std::chrono::steady_clock::duration duration,
	race_options opts = {}) {
	opts.winner = winner_policy::first_completion;
	return race<T>(
		opts,
		candidate("work", std::move(work)),
		timeout_after("deadline", duration, root::CancelReason::deadline));
}

template<root::work_value T>
struct owned_labeled_race_result {
	race_result<T> result;
	std::vector<std::string> labels;

	owned_labeled_race_result(owned_labeled_race_result const &) = delete;
	owned_labeled_race_result &operator =(owned_labeled_race_result const &) = delete;
	owned_labeled_race_result(
		owned_labeled_race_result &&other) noexcept
		: result{std::move(other.result)}
		, labels{std::move(other.labels)} {
		rebase();
	}
	owned_labeled_race_result &operator =(
		owned_labeled_race_result &&other) noexcept {
		if (this != &other) {
			result = std::move(other.result);
			labels = std::move(other.labels);
			rebase();
		}
		return *this;
	}
	owned_labeled_race_result(
		race_result<T> r,
		std::vector<std::string> owned) noexcept
		: result{std::move(r)}
		, labels{std::move(owned)} {
		rebase();
	}
	[[nodiscard]] std::string_view winner_label() const noexcept {
		if (result.winner.index >= labels.size()) {
			return {};
		}
		return labels[result.winner.index];
	}

private:
	void rebase() noexcept { result.winner.label = winner_label(); }
};

template<root::work_value T, class... Participants>
[[nodiscard]] root::Task<owned_labeled_race_result<T>> race_owned_labels(
	race_options opts,
	Participants... participants) {
	auto rebased_participants = std::tuple<Participants...>{std::move(participants)...};
	std::vector<std::string> labels{};
	labels.reserve(sizeof...(Participants));
	std::apply([&labels](auto const &...ps) { (labels.emplace_back(ps.label), ...); }, rebased_participants);
	[&labels]<std::size_t... I>(std::index_sequence<I...>, auto &tuple) {
		((std::get<I>(tuple).label = labels[I]), ...);
	}(std::make_index_sequence<sizeof...(Participants)>{}, rebased_participants);
	auto result =
		co_await std::apply([&opts](auto &...ps) { return race<T>(opts, std::move(ps)...); }, rebased_participants);
	if (result.outcome.is_failure()) {
		try {
			std::rethrow_exception(result.outcome.failure().error);
		} catch (race_aggregate_error const &err) {
			std::vector<race_aggregate_error_entry> entries{err.entries().begin(), err.entries().end()};
			result.outcome = root::Outcome<T>{
				root::Failure{std::make_exception_ptr(owned_race_aggregate_error{labels, std::move(entries)})}};
		} catch (...) {} // NOLINT(bugprone-empty-catch): preserve the original race outcome if aggregate wrapping fails.
	}
	co_return owned_labeled_race_result<T>{std::move(result), std::move(labels)};
}

} // namespace conflux::work::race
