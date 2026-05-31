module;

module conflux.net.cancel;

import std;

namespace {

struct ActiveTaskCancelRelayState {
	std::mutex m;
	std::optional<wroot::TaskControl> active;
	std::atomic<bool> cancelled{false};
};

[[nodiscard]] ActiveTaskCancelRelayState *relay_state(
	void *p) noexcept {
	return static_cast<ActiveTaskCancelRelayState *>(p);
}

} // namespace

namespace conflux::net::detail {

ActiveTaskCancelRelay::ActiveTaskCancelRelay()
	: state_{new ActiveTaskCancelRelayState{}} {}

ActiveTaskCancelRelay::~ActiveTaskCancelRelay() {
	delete relay_state(state_);
}

ActiveTaskCancelRelay::ActiveTaskCancelRelay(
	ActiveTaskCancelRelay &&other) noexcept
	: state_{std::exchange(other.state_, nullptr)} {}

ActiveTaskCancelRelay &ActiveTaskCancelRelay::operator =(
	ActiveTaskCancelRelay &&other) noexcept {
	if (this != &other) {
		delete relay_state(state_);
		state_ = std::exchange(other.state_, nullptr);
	}
	return *this;
}

void ActiveTaskCancelRelay::set_active(
	wroot::TaskControl c) {
	auto *st = relay_state(state_);
	if (st == nullptr) {
		throw wroot::CancelledError{wroot::CancelReason::requested};
	}

	std::optional<wroot::TaskControl> to_cancel;
	{
		std::lock_guard lk{st->m};
		st->active.emplace(std::move(c));
		if (st->cancelled.load(std::memory_order_acquire)) {
			to_cancel = st->active;
		}
	}
	if (to_cancel) {
		auto _ = to_cancel->request_cancel();
	}
}

void ActiveTaskCancelRelay::clear_active() noexcept {
	try {
		auto *st = relay_state(state_);
		if (st == nullptr) {
			return;
		}
		std::lock_guard lk{st->m};
		st->active.reset();
	} catch (...) {} // NOLINT(bugprone-empty-catch): noexcept cleanup; lost relay state is already non-actionable.
}

void ActiveTaskCancelRelay::cancel() noexcept {
	std::optional<wroot::TaskControl> to_cancel;
	try {
		auto *st = relay_state(state_);
		if (st == nullptr) {
			return;
		}
		{
			std::lock_guard lk{st->m};
			st->cancelled.store(true, std::memory_order_release);
			to_cancel = st->active;
		}
	} catch (...) {
		return;
	} // NOLINT(bugprone-empty-catch): noexcept cancel relay; failure means no active task can be cancelled safely.
	if (to_cancel) {
		auto _ = to_cancel->request_cancel();
	}
}

[[nodiscard]] bool ActiveTaskCancelRelay::is_cancelled() const noexcept {
	auto const *st = relay_state(state_);
	return st == nullptr || st->cancelled.load(std::memory_order_acquire);
}

void ActiveTaskCancelRelay::throw_if_cancelled() const {
	if (is_cancelled()) {
		throw wroot::CancelledError{wroot::CancelReason::requested};
	}
}

[[nodiscard]] wroot::Task<decltype(sizeof(0))> ActiveTaskCancelRelay::await_child(
	wroot::Task<decltype(sizeof(0))> child) {
	set_active(child.control());
	try {
		auto out = co_await std::move(child);
		clear_active();
		throw_if_cancelled();
		co_return out;
	} catch (wroot::CancelledError const &err) {
		clear_active();
		if (err.reason() == wroot::CancelReason::abandoned && is_cancelled()) {
			throw wroot::CancelledError{wroot::CancelReason::requested};
		}
		throw;
	} catch (...) {
		clear_active();
		throw;
	}
}

[[nodiscard]] wroot::Task<void> ActiveTaskCancelRelay::await_child(
	wroot::Task<void> child) {
	set_active(child.control());
	try {
		co_await std::move(child);
		clear_active();
		throw_if_cancelled();
		co_return;
	} catch (wroot::CancelledError const &err) {
		clear_active();
		if (err.reason() == wroot::CancelReason::abandoned && is_cancelled()) {
			throw wroot::CancelledError{wroot::CancelReason::requested};
		}
		throw;
	} catch (...) {
		clear_active();
		throw;
	}
}

} // namespace conflux::net::detail
