module;
#include <cerrno>
#include <liburing.h>

export module conflux.uring.completion;

import std;
import conflux.types;

export using UserDataFn = std::function<std::uint64_t(std::uint32_t slot, std::uint32_t gen)>;
export struct IoResult {
	std::int32_t res{};
	std::uint32_t flags{};
};
template<typename Signature, std::size_t InlineBytes = 64>
class MoveOnlyInlineFunction;
template<typename R, typename... Args, std::size_t InlineBytes>
class MoveOnlyInlineFunction<R(Args...), InlineBytes> {
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
		MoveOnlyInlineFunction &&other) noexcept {
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
		} else {
			object_ = std::exchange(other.object_, nullptr);
		}
		other.invoke_ = nullptr;
		other.destroy_ = nullptr;
		other.move_ = nullptr;
		other.inlined_ = false;
	}

public:
	MoveOnlyInlineFunction() noexcept = default;
	MoveOnlyInlineFunction(
		std::nullptr_t) noexcept {}
	MoveOnlyInlineFunction(MoveOnlyInlineFunction const &) = delete;
	MoveOnlyInlineFunction &operator =(MoveOnlyInlineFunction const &) = delete;
	MoveOnlyInlineFunction(
		MoveOnlyInlineFunction &&other) noexcept {
		move_from(std::move(other));
	}
	MoveOnlyInlineFunction &operator =(
		MoveOnlyInlineFunction &&other) noexcept {
		if (this != &other) {
			reset();
			move_from(std::move(other));
		}
		return *this;
	}
	template<typename F>
		requires(!std::same_as<std::remove_cvref_t<F>, MoveOnlyInlineFunction>)
	MoveOnlyInlineFunction(
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
	~MoveOnlyInlineFunction() noexcept { reset(); }
	[[nodiscard]] explicit operator bool() const noexcept { return invoke_ != nullptr; }
	R operator ()(
		Args... args) { // NOLINT(performance-unnecessary-value-param): pack must be forwarded
		return invoke_(object_, std::forward<Args>(args)...);
	}
};
export using CompletionFn = MoveOnlyInlineFunction<void(IoResult)>;
export class CompletionTable {
	enum class SlotMode : std::uint8_t {
		single,
		multishot,
		zc_send,
	};
	struct Slot {
		std::uint32_t gen{0};
		bool in_use{false};
		SlotMode mode{SlotMode::single};
		std::int32_t zc_bytes{0};
		bool zc_seen_send{false};
		CompletionFn fn{};
	};
	std::vector<Slot> slots_{};
	std::vector<std::uint32_t> free_{};

public:
	explicit CompletionTable(
		std::size_t initial_capacity = 64) {
		slots_.reserve(initial_capacity);
	}
	CompletionTable(CompletionTable const &) = delete;
	CompletionTable &operator =(CompletionTable const &) = delete;
	CompletionTable(CompletionTable &&) = delete;
	CompletionTable &operator =(CompletionTable &&) = delete;
	~CompletionTable() {} // NOLINT(modernize-use-equals-default) — GCC module bug
	[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> reserve(
		CompletionFn fn) {
		std::uint32_t slot = 0;
		if (!free_.empty()) {
			slot = free_.back();
			free_.pop_back();
		} else {
			slot = static_cast<std::uint32_t>(slots_.size());
			slots_.emplace_back();
		}
		auto &s = slots_[slot];
		s.in_use = true;
		s.mode = SlotMode::single;
		s.zc_bytes = 0;
		s.zc_seen_send = false;
		s.fn = std::move(fn);
		return {slot, s.gen};
	}
	[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> reserve_multishot(
		CompletionFn fn) {
		auto [slot, gen] = reserve(std::move(fn));
		slots_[slot].mode = SlotMode::multishot;
		return {slot, gen};
	}
	[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> reserve_zc(
		CompletionFn fn) {
		auto [slot, gen] = reserve(std::move(fn));
		slots_[slot].mode = SlotMode::zc_send;
		return {slot, gen};
	}
	void dispatch(
		// NOLINT(bugprone-exception-escape) — callbacks are noexcept by contract
		std::uint32_t slot,
		std::uint32_t gen,
		int res,
		std::uint32_t flags) noexcept {
		if (slot >= slots_.size()) {
			return;
		}
		auto &s = slots_[slot];
		if (!s.in_use || s.gen != gen) {
			return;
		}
		if (s.mode == SlotMode::multishot && res >= 0 && (flags & IORING_CQE_F_MORE) != 0U) {
			if (s.fn) {
				s.fn(IoResult{.res = res, .flags = flags});
			}
			return;
		}
		if (s.mode == SlotMode::zc_send) {
			if ((flags & IORING_CQE_F_NOTIF) == 0U) {
				if (res >= 0 && (flags & IORING_CQE_F_MORE) != 0U) {
					s.zc_bytes = res;
					s.zc_seen_send = true;
					return;
				}
			} else {
				res = s.zc_seen_send ? s.zc_bytes : -EIO;
			}
		}
		auto fn = std::move(s.fn);
		s.fn = {};
		s.in_use = false;
		s.mode = SlotMode::single;
		s.zc_bytes = 0;
		s.zc_seen_send = false;
		++s.gen;
		free_.push_back(slot);
		if (fn) {
			fn(IoResult{.res = res, .flags = flags});
		}
	}
	[[nodiscard]] bool has_pending_zc_notifications() const noexcept {
		for (auto const &s: slots_) {
			if (s.in_use && s.mode == SlotMode::zc_send && s.zc_seen_send) {
				return true;
			}
		}
		return false;
	}
	// Returns false (and cancels nothing) if ZC notification slots are pending.
	// Caller must drain the CQ until has_pending_zc_notifications() returns false, then retry.
	[[nodiscard]] bool cancel_all() noexcept { // NOLINT(bugprone-exception-escape) — callbacks are noexcept by contract
		if (has_pending_zc_notifications()) {
			return false;
		}
		std::uint32_t const n = static_cast<std::uint32_t>(slots_.size()); // cache before callbacks can grow slots_
		for (std::uint32_t slot = 0; slot < n; ++slot) {
			auto &s = slots_[slot];
			if (!s.in_use) {
				continue;
			}
			auto fn = std::move(s.fn);
			s.fn = {};
			s.in_use = false;
			s.mode = SlotMode::single;
			s.zc_bytes = 0;
			s.zc_seen_send = false;
			++s.gen;
			free_.push_back(slot);
			if (fn) {
				fn(IoResult{.res = -ECANCELED, .flags = 0});
			}
		}
		return true;
	}
	[[nodiscard]] std::size_t pending() const noexcept { return slots_.size() - free_.size(); }
};
