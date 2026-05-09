#pragma once
// Internal — do not include from public headers.
// Requires: u8, u32, V, expected, unexpected, eprintln, format in scope.

enum class DirectSlotState : u8 {
	free_slot,
	populated,
	closing,
	poisoned,
};
enum class DirectSlotError : u8 {
	not_registered,
	exhausted,
	out_of_range,
	bad_state,
	install_failed,
};
struct DirectSlotPool {
	explicit DirectSlotPool(
		u32 capacity)
		: capacity_{capacity} {
		state_.assign(capacity_, static_cast<u8>(DirectSlotState::free_slot));
		free_stack_.reserve(capacity_);
		free_pos_.assign(capacity_, ~u32{});
		for (u32 i = 0; i < capacity_; ++i) {
			free_pos_[i] = i;
			free_stack_.push_back(i);
		}
	}
	[[nodiscard]] expected<void, DirectSlotError> install_os_fd(
		u32 slot,
		int) noexcept {
		if (slot >= capacity_) {
			return unexpected(DirectSlotError::out_of_range);
		}
		if (slot_state(slot) != DirectSlotState::free_slot) {
			return unexpected(DirectSlotError::bad_state);
		}
		remove_from_free(slot);
		set_state(slot, DirectSlotState::populated);
		return {};
	}
	[[nodiscard]] expected<void, DirectSlotError> adopt_kernel_allocated(
		u32 slot) noexcept {
		if (slot >= capacity_) {
			return unexpected(DirectSlotError::out_of_range);
		}
		if (slot_state(slot) != DirectSlotState::free_slot) {
			return unexpected(DirectSlotError::bad_state);
		}
		remove_from_free(slot);
		set_state(slot, DirectSlotState::populated);
		return {};
	}
	[[nodiscard]] expected<void, DirectSlotError> mark_closing(
		u32 slot) noexcept {
		if (slot >= capacity_) {
			return unexpected(DirectSlotError::out_of_range);
		}
		if (slot_state(slot) != DirectSlotState::populated) {
			return unexpected(DirectSlotError::bad_state);
		}
		set_state(slot, DirectSlotState::closing);
		return {};
	}
	[[nodiscard]] expected<void, DirectSlotError> release_closed(
		u32 slot) {
		if (slot >= capacity_) {
			return unexpected(DirectSlotError::out_of_range);
		}
		if (slot_state(slot) != DirectSlotState::closing) {
			return unexpected(DirectSlotError::bad_state);
		}
		push_to_free(slot);
		set_state(slot, DirectSlotState::free_slot);
		return {};
	}
	void poison(
		u32 slot,
		int close_res) {
		if (slot >= capacity_) {
			eprintln(format("DirectSlotPool::poison: slot={} out of range close_res={}", slot, close_res));
			return;
		}
		auto const prev = slot_state(slot);
		if (prev == DirectSlotState::free_slot) {
			eprintln(format("DirectSlotPool::poison: slot={} state=free — corruption close_res={}", slot, close_res));
			return;
		}
		set_state(slot, DirectSlotState::poisoned);
		++poisoned_count_;
		eprintln(format(
			"DirectSlotPool::poison: slot={} close_res={} previous_state={}",
			slot,
			close_res,
			static_cast<u8>(prev)));
	}
	[[nodiscard]] u32 capacity() const noexcept { return capacity_; }
	[[nodiscard]] u32 free_count() const noexcept { return static_cast<u32>(free_stack_.size()); }
	[[nodiscard]] u32 poisoned_count() const noexcept { return poisoned_count_; }
	[[nodiscard]] DirectSlotState slot_state(
		u32 slot) const noexcept {
		return static_cast<DirectSlotState>(state_[slot]);
	}

private:
	void set_state(
		u32 slot,
		DirectSlotState s) noexcept {
		state_[slot] = static_cast<u8>(s);
	}
	void remove_from_free(
		u32 slot) noexcept {
		u32 const pos = free_pos_[slot];
		u32 const last = free_stack_.back();
		if (last != slot) {
			free_stack_[pos] = last;
			free_pos_[last] = pos;
		}
		free_stack_.pop_back();
		free_pos_[slot] = ~u32{};
	}
	void push_to_free(
		u32 slot) {
		free_pos_[slot] = static_cast<u32>(free_stack_.size());
		free_stack_.push_back(slot);
	}
	u32 capacity_{};
	u32 poisoned_count_{};
	V<u8> state_{};
	V<u32> free_stack_{};
	V<u32> free_pos_{};
};
