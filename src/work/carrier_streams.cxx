module;

export module conflux.work.carrier.streams;

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier.model_a;

export namespace conflux::work::carrier {

template<root::work_value T>
class DroppableSlotAwaiter;

template<root::work_value T>
class DroppableSlot {
	struct DrainState {
		root::TaskJoinHandle<T> handle;
		Fn<void(root::Outcome<T>)> on_drop_fn;
	};

	UP<DrainState> state_;
	bool consumed_ = false;

	friend class DroppableSlotAwaiter<T>;

	void drain_() noexcept {
		auto ctrl = state_->handle.control();
		if (ctrl.ready()) {
			auto out = root::join(std::move(state_->handle));
			if (state_->on_drop_fn) {
				state_->on_drop_fn(std::move(out));
			}
			return;
		}
		auto result = ctrl.try_set_on_ready([s = std::move(state_)]() mutable noexcept {
			auto out = root::join(std::move(s->handle));
			if (s->on_drop_fn) {
				s->on_drop_fn(std::move(out));
			}
		});
		switch (result.status) {
		case root::ReadyRegistration::installed: return;
		case root::ReadyRegistration::already_ready:
			if (result.rejected_fn) {
				result.rejected_fn();
			}
			return;
		case root::ReadyRegistration::already_installed:
#ifdef CONFLUX_WORK_CHECKED_BUILD
			root::emit_carrier_diagnostic(
				"DroppableSlot: single-consumer rule violated — drain hook could not install");
#endif
			std::terminate();
		case root::ReadyRegistration::empty: return;
		}
	}

public:
	explicit DroppableSlot(
		root::TaskJoinHandle<T> &&h)
		: state_{std::make_unique<DrainState>(DrainState{std::move(h), {}})} {}

	DroppableSlot(DroppableSlot &&) noexcept = default;
	DroppableSlot &operator =(DroppableSlot &&) noexcept = default;
	DroppableSlot(DroppableSlot const &) = delete;
	DroppableSlot &operator =(DroppableSlot const &) = delete;

	~DroppableSlot() noexcept {
		if (consumed_ || !state_) {
			return;
		}
		drain_();
	}

	template<class F>
		requires std::invocable<F, root::Outcome<T>> && std::is_nothrow_invocable_v<F, root::Outcome<T>>
	void on_drop(
		F &&fn) noexcept {
		if (!consumed_ && state_) {
			state_->on_drop_fn = std::forward<F>(fn);
		}
	}

	[[nodiscard]] bool ready() const noexcept { return state_ && state_->handle.control().ready(); }

	[[nodiscard]] Opt<root::Outcome<T>> try_get() && {
		if (!state_ || !state_->handle.control().ready()) {
			return std::nullopt;
		}
		auto out = root::join(std::move(state_->handle));
		consumed_ = true;
		return out;
	}

	[[nodiscard]] model_a::Chain<T> wait() && {
		auto out = root::join(std::move(state_->handle));
		consumed_ = true;
		return model_a::Chain<T>{std::move(out), model_a::CarrierKind::task};
	}

	[[nodiscard]] DroppableSlotAwaiter<T> operator co_await() && noexcept;
};

template<root::work_value T>
class DroppableSlotAwaiter {
	using DrainState = typename DroppableSlot<T>::DrainState;

	UP<DrainState> state_;
	root::BasicControl<root::ControlCategory::task> control_;
	bool consumed_ = false;
	bool callback_installed_ = false;

public:
	explicit DroppableSlotAwaiter(
		UP<DrainState> s) noexcept
		: state_{std::move(s)}
		, control_{state_->handle.control()} {}

	~DroppableSlotAwaiter() noexcept {
		if (consumed_ || !state_) {
			return;
		}
		if (callback_installed_) {
			auto status = control_.clear_on_ready();
			if (status == root::ClearOnReadyStatus::in_flight) {
#ifdef CONFLUX_WORK_CHECKED_BUILD
				root::emit_carrier_diagnostic_fmt(
					"DroppableSlotAwaiter dtor raced commit's in-flight callback "
					"— best-effort abandon (awaiter=%p)",
					static_cast<void *>(this));
#endif
				(void)root::try_abandon_to(std::move(state_->handle), root::drop_on_abandon{});
				return;
			}
		}
		auto result = control_.try_set_on_ready([s = std::move(state_)]() mutable noexcept {
			auto out = root::join(std::move(s->handle));
			if (s->on_drop_fn) {
				s->on_drop_fn(std::move(out));
			}
		});
		switch (result.status) {
		case root::ReadyRegistration::installed: return;
		case root::ReadyRegistration::already_ready:
			if (result.rejected_fn) {
				result.rejected_fn();
			}
			return;
		case root::ReadyRegistration::already_installed:
#ifdef CONFLUX_WORK_CHECKED_BUILD
			root::emit_carrier_diagnostic("DroppableSlotAwaiter: single-consumer rule violated");
#endif
			std::terminate();
		case root::ReadyRegistration::empty: return;
		}
	}

	DroppableSlotAwaiter(DroppableSlotAwaiter &&) noexcept = default;
	DroppableSlotAwaiter &operator =(DroppableSlotAwaiter &&) noexcept = default;
	DroppableSlotAwaiter(DroppableSlotAwaiter const &) = delete;
	DroppableSlotAwaiter &operator =(DroppableSlotAwaiter const &) = delete;

	[[nodiscard]] bool await_ready() const noexcept { return control_.ready(); }

	[[nodiscard]] bool await_suspend(
		std::coroutine_handle<> h) noexcept {
		auto result = control_.try_set_on_ready([h]() mutable noexcept { h.resume(); });
		if (result.status == root::ReadyRegistration::installed) {
			callback_installed_ = true;
			return true;
		}
		return false;
	}

	[[nodiscard]] model_a::Chain<T> await_resume() {
		callback_installed_ = false;
		auto out = root::join(std::move(state_->handle));
		consumed_ = true;
		return model_a::Chain<T>{std::move(out), model_a::CarrierKind::task};
	}
};

template<root::work_value T>
DroppableSlotAwaiter<T> DroppableSlot<T>::operator co_await() && noexcept {
	consumed_ = true;
	return DroppableSlotAwaiter<T>{std::move(state_)};
}

template<root::work_value T>
	requires(!std::same_as<T, void>)
class CoalescingSlot {
	mutable std::mutex mu_;
	Opt<T> slot_;

public:
	CoalescingSlot() noexcept = default;
	~CoalescingSlot() noexcept = default;

	CoalescingSlot(CoalescingSlot &&) noexcept = delete;
	CoalescingSlot &operator =(CoalescingSlot &&) noexcept = delete;
	CoalescingSlot(CoalescingSlot const &) = delete;
	CoalescingSlot &operator =(CoalescingSlot const &) = delete;

	void commit(
		T value) noexcept {
		std::lock_guard const lock{mu_};
		slot_ = std::move(value);
	}

	[[nodiscard]] Opt<T> take() noexcept {
		std::lock_guard const lock{mu_};
		return std::exchange(slot_, std::nullopt);
	}

	[[nodiscard]] bool available() const noexcept {
		std::lock_guard const lock{mu_};
		return slot_.has_value();
	}
};

} // namespace conflux::work::carrier
