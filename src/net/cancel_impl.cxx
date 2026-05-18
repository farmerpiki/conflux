module;
#include <atomic>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

module conflux.net.cancel;

using std::memory_order_acquire;
using std::memory_order_release;
using std::move;

namespace {

struct ActiveTaskCancelRelayState {
	std::mutex m;
	std::optional<wroot::TaskControl> active;
	std::atomic<bool> cancelled{false};
};

[[nodiscard]] ActiveTaskCancelRelayState *relay_state(void *p) noexcept {
	return static_cast<ActiveTaskCancelRelayState *>(p);
}


} // namespace

ActiveTaskCancelRelay::ActiveTaskCancelRelay()
	: state_{new ActiveTaskCancelRelayState{}} {}

ActiveTaskCancelRelay::~ActiveTaskCancelRelay() {
	delete relay_state(state_);
}

ActiveTaskCancelRelay::ActiveTaskCancelRelay(ActiveTaskCancelRelay &&other) noexcept
	: state_{std::exchange(other.state_, nullptr)} {}

ActiveTaskCancelRelay &ActiveTaskCancelRelay::operator =(ActiveTaskCancelRelay &&other) noexcept {
	if (this != &other) {
		delete relay_state(state_);
		state_ = std::exchange(other.state_, nullptr);
	}
	return *this;
}

void ActiveTaskCancelRelay::set_active(wroot::TaskControl c) {
	auto *st = relay_state(state_);
	if (st == nullptr) {
		throw wroot::CancelledError{wroot::CancelReason::requested};
	}

	std::optional<wroot::TaskControl> to_cancel;
	{
		std::lock_guard lk{st->m};
		st->active.emplace(move(c));
		if (st->cancelled.load(memory_order_acquire)) {
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
	} catch (...) {}
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
			st->cancelled.store(true, memory_order_release);
			to_cancel = st->active;
		}
	} catch (...) {
		return;
	}
	if (to_cancel) {
		auto _ = to_cancel->request_cancel();
	}
}

[[nodiscard]] bool ActiveTaskCancelRelay::is_cancelled() const noexcept {
	auto const *st = relay_state(state_);
	return st == nullptr || st->cancelled.load(memory_order_acquire);
}

void ActiveTaskCancelRelay::throw_if_cancelled() const {
	if (is_cancelled()) {
		throw wroot::CancelledError{wroot::CancelReason::requested};
	}
}

[[nodiscard]] wroot::Task<std::size_t> ActiveTaskCancelRelay::await_child(wroot::Task<std::size_t> child) {
	set_active(child.control());
	try {
		auto out = co_await move(child);
		clear_active();
		throw_if_cancelled();
		co_return out;
	} catch (...) {
		clear_active();
		throw;
	}
}

[[nodiscard]] wroot::Task<void> ActiveTaskCancelRelay::await_child(wroot::Task<void> child) {
	set_active(child.control());
	try {
		co_await move(child);
		clear_active();
		throw_if_cancelled();
		co_return;
	} catch (...) {
		clear_active();
		throw;
	}
}
