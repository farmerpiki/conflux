module;
#include <cassert>
#include <memory>

export module conflux.net.direct_slot_pool;
import std;
import conflux.types;
import conflux.utils;
// Internal — do not include from public headers.
// Requires: std::uint8_t, std::uint32_t, V, std::expected, std::unexpected, eprintln, std::format in scope.

export enum class DirectSlotState : std::uint8_t {
	free_slot,
	leased_empty,
	populated,
	closing,
	poisoned,
};
export enum class DirectSlotError : std::uint8_t {
	not_registered,
	exhausted,
	out_of_range,
	bad_state,
	install_failed,
};
export struct DirectSlotPool {
	explicit DirectSlotPool(
		std::uint32_t capacity)
		: capacity_{capacity} {
		state_.assign(capacity_, static_cast<std::uint8_t>(DirectSlotState::free_slot));
		free_stack_.reserve(capacity_);
		free_pos_.assign(capacity_, ~std::uint32_t{});
		for (std::uint32_t i = 0; i < capacity_; ++i) {
			free_pos_[i] = i;
			free_stack_.push_back(i);
		}
	}
	[[nodiscard]] std::expected<std::uint32_t, DirectSlotError> acquire() noexcept {
		if (free_stack_.empty()) {
			return std::unexpected(DirectSlotError::exhausted);
		}
		std::uint32_t const slot = free_stack_.back();
		remove_from_free(slot);
		set_state(slot, DirectSlotState::leased_empty);
		return slot;
	}
	[[nodiscard]] std::expected<void, DirectSlotError> release_empty(
		std::uint32_t slot) noexcept {
		if (slot >= capacity_) {
			return std::unexpected(DirectSlotError::out_of_range);
		}
		if (slot_state(slot) != DirectSlotState::leased_empty) {
			return std::unexpected(DirectSlotError::bad_state);
		}
		set_state(slot, DirectSlotState::free_slot);
		push_to_free(slot);
		return {};
	}
	[[nodiscard]] std::expected<void, DirectSlotError> install_os_fd(
		std::uint32_t slot,
		int) noexcept {
		if (slot >= capacity_) {
			return std::unexpected(DirectSlotError::out_of_range);
		}
		if (slot_state(slot) != DirectSlotState::free_slot) {
			return std::unexpected(DirectSlotError::bad_state);
		}
		remove_from_free(slot);
		set_state(slot, DirectSlotState::populated);
		return {};
	}
	[[nodiscard]] std::expected<void, DirectSlotError> adopt_kernel_allocated(
		std::uint32_t slot) noexcept {
		if (slot >= capacity_) {
			return std::unexpected(DirectSlotError::out_of_range);
		}
		if (slot_state(slot) != DirectSlotState::free_slot) {
			return std::unexpected(DirectSlotError::bad_state);
		}
		remove_from_free(slot);
		set_state(slot, DirectSlotState::populated);
		return {};
	}
	[[nodiscard]] std::expected<void, DirectSlotError> mark_closing(
		std::uint32_t slot) noexcept {
		if (slot >= capacity_) {
			return std::unexpected(DirectSlotError::out_of_range);
		}
		if (slot_state(slot) != DirectSlotState::populated) {
			return std::unexpected(DirectSlotError::bad_state);
		}
		set_state(slot, DirectSlotState::closing);
		return {};
	}
	[[nodiscard]] std::expected<void, DirectSlotError> release_closed(
		std::uint32_t slot) noexcept {
		if (slot >= capacity_) {
			return std::unexpected(DirectSlotError::out_of_range);
		}
		if (slot_state(slot) != DirectSlotState::closing) {
			return std::unexpected(DirectSlotError::bad_state);
		}
		push_to_free(slot);
		set_state(slot, DirectSlotState::free_slot);
		return {};
	}
	void poison(
		std::uint32_t slot,
		int close_res) {
		if (slot >= capacity_) {
			eprintln(std::format("DirectSlotPool::poison: slot={} out of range close_res={}", slot, close_res));
			return;
		}
		auto const prev = slot_state(slot);
		if (prev == DirectSlotState::free_slot) {
			eprintln(std::format("DirectSlotPool::poison: slot={} state=free — corruption close_res={}", slot, close_res));
			return;
		}
		assert(free_pos_[slot] == ~std::uint32_t{});
		if (prev != DirectSlotState::poisoned) {
			set_state(slot, DirectSlotState::poisoned);
			++poisoned_count_;
		}
		eprintln(std::format(
			"DirectSlotPool::poison: slot={} close_res={} previous_state={}",
			slot,
			close_res,
			static_cast<std::uint8_t>(prev)));
	}
	[[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
	[[nodiscard]] std::uint32_t free_count() const noexcept { return static_cast<std::uint32_t>(free_stack_.size()); }
	[[nodiscard]] std::uint32_t poisoned_count() const noexcept { return poisoned_count_; }
	[[nodiscard]] DirectSlotState slot_state(
		std::uint32_t slot) const noexcept {
		return static_cast<DirectSlotState>(state_[slot]);
	}

private:
	void set_state(
		std::uint32_t slot,
		DirectSlotState s) noexcept {
		state_[slot] = static_cast<std::uint8_t>(s);
	}
	void remove_from_free(
		std::uint32_t slot) noexcept {
		std::uint32_t const pos = free_pos_[slot];
		assert(pos != ~std::uint32_t{});
		assert(free_stack_[pos] == slot);
		std::uint32_t const last = free_stack_.back();
		if (last != slot) {
			free_stack_[pos] = last;
			free_pos_[last] = pos;
		}
		free_stack_.pop_back();
		free_pos_[slot] = ~std::uint32_t{};
	}
	void push_to_free(
		std::uint32_t slot) noexcept {
		assert(free_pos_[slot] == ~std::uint32_t{});
		free_pos_[slot] = static_cast<std::uint32_t>(free_stack_.size());
		free_stack_.push_back(slot);
	}
	std::uint32_t capacity_{};
	std::uint32_t poisoned_count_{};
	std::vector<std::uint8_t> state_{};
	std::vector<std::uint32_t> free_stack_{};
	std::vector<std::uint32_t> free_pos_{};
};
export struct DirectSlotLease {
	DirectSlotPool *pool_{};
	std::uint32_t slot_{~std::uint32_t{}};
	DirectSlotLease() noexcept = default;
	explicit DirectSlotLease(
		DirectSlotPool &pool,
		std::uint32_t slot) noexcept
		: pool_{&pool}
		, slot_{slot} {}
	~DirectSlotLease() noexcept {
		if (pool_) {
			auto _ = pool_->release_empty(slot_);
		}
	}
	DirectSlotLease(DirectSlotLease const &) = delete;
	DirectSlotLease &operator =(DirectSlotLease const &) = delete;
	DirectSlotLease(
		DirectSlotLease &&o) noexcept
		: pool_{o.pool_}
		, slot_{o.slot_} {
		o.pool_ = nullptr;
	}
	DirectSlotLease &operator =(
		DirectSlotLease &&o) noexcept {
		if (this != &o) {
			if (pool_) {
				auto _ = pool_->release_empty(slot_);
			}
			pool_ = o.pool_;
			slot_ = o.slot_;
			o.pool_ = nullptr;
		}
		return *this;
	}
	[[nodiscard]] std::uint32_t slot() const noexcept { return slot_; }
	void detach() noexcept { pool_ = nullptr; }
};
