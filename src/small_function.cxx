export module conflux.small_function;

import std;

export namespace conflux::detail {

template<typename Signature, std::size_t InlineBytes = 31>
class small_move_only_function;

template<typename R, typename... Args, std::size_t InlineBytes>
class small_move_only_function<R(Args...), InlineBytes> {
	using invoke_fn = R (*)(void *, Args &&...);
	using destroy_fn = void (*)(void *) noexcept;
	using move_fn = void (*)(void *, void *) noexcept;

	alignas(std::max_align_t) std::byte inline_storage_[InlineBytes]{};
	bool inlined_ = false;
	void *object_ = nullptr;
	invoke_fn invoke_ = nullptr;
	destroy_fn destroy_ = nullptr;
	move_fn move_ = nullptr;

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
			object_ = inline_storage_;
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
	small_move_only_function &operator =(
		std::nullptr_t) noexcept {
		reset();
		return *this;
	}
	template<typename F>
		requires(
			!std::same_as<std::remove_cvref_t<F>, small_move_only_function>
			&& !std::same_as<std::remove_cvref_t<F>, std::nullptr_t>)
	small_move_only_function &operator =(
		F &&fn) {
		auto next = small_move_only_function{std::forward<F>(fn)};
		swap(next);
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
			&& alignof(fn_t) <= alignof(std::max_align_t)
			&& std::is_nothrow_move_constructible_v<fn_t>) {
			object_ = inline_storage_;
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
	void swap(
		small_move_only_function &other) noexcept {
		if (this == &other) {
			return;
		}
		auto tmp = std::move(other);
		other = std::move(*this);
		*this = std::move(tmp);
	}
	R operator ()(
		Args... args) const { // NOLINT(performance-unnecessary-value-param): pack must be forwarded
		return invoke_(object_, std::forward<Args>(args)...);
	}
};

static_assert(sizeof(small_move_only_function<void()>) == 64);

template<typename Signature, std::size_t InlineBytes>
void swap(
	small_move_only_function<Signature, InlineBytes> &lhs,
	small_move_only_function<Signature, InlineBytes> &rhs) noexcept {
	lhs.swap(rhs);
}

template<typename Signature, std::size_t InlineBytes>
[[nodiscard]] bool operator ==(
	small_move_only_function<Signature, InlineBytes> const &fn,
	std::nullptr_t) noexcept {
	return !fn;
}

template<typename Signature, std::size_t InlineBytes>
[[nodiscard]] bool operator ==(
	std::nullptr_t,
	small_move_only_function<Signature, InlineBytes> const &fn) noexcept {
	return !fn;
}

} // namespace conflux::detail
