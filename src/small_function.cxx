export module conflux.small_function;

import std;

export namespace conflux::detail {

template<typename Signature, std::size_t InlineBytes = 40>
class small_move_only_function;

template<typename R, typename... Args, std::size_t InlineBytes>
class small_move_only_function<R(Args...), InlineBytes> {
	using invoke_fn = R (*)(void *, Args &&...);
	enum class manager_op : std::uint8_t {
		destroy,
		move,
	};
	using manager_fn = void (*)(manager_op, void *, void *) noexcept;

	union storage_t {
		alignas(void *) std::byte inline_storage[InlineBytes];
		void *heap_object;
	};

	storage_t storage_{};
	invoke_fn invoke_ = nullptr;
	manager_fn manager_ = nullptr;
	bool heap_allocated_ = false;

	[[nodiscard]] manager_fn manager() const noexcept { return manager_; }
	[[nodiscard]] bool heap_allocated() const noexcept { return heap_allocated_; }
	[[nodiscard]] void *inline_object(
		this auto &&self) noexcept {
		return const_cast<std::byte *>(self.storage_.inline_storage);
	}
	[[nodiscard]] void *object(
		this auto &&self) noexcept {
		return self.heap_allocated() ? self.storage_.heap_object : self.inline_object();
	}
	void set_manager(
		manager_fn fn,
		bool heap_allocated) noexcept {
		manager_ = fn;
		heap_allocated_ = heap_allocated;
	}

	template<typename F>
	static R invoke_inline(
		void *obj,
		Args &&...args) {
		return std::invoke(*reinterpret_cast<F *>(obj), std::forward<Args>(args)...);
	}
	template<typename F>
	static void manage_inline(
		manager_op op,
		void *dst,
		void *src) noexcept {
		if (op == manager_op::destroy) {
			reinterpret_cast<F *>(src)->~F();
			return;
		}
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
	static void manage_heap(
		manager_op op,
		void *,
		void *src) noexcept {
		if (op == manager_op::destroy) {
			delete reinterpret_cast<F *>(src);
		}
	}
	void reset() noexcept {
		if (invoke_ == nullptr) {
			return;
		}
		manager()(manager_op::destroy, nullptr, object());
		storage_.heap_object = nullptr;
		invoke_ = nullptr;
		manager_ = nullptr;
		heap_allocated_ = false;
	}
	void move_from(
		small_move_only_function &&other) noexcept {
		invoke_ = other.invoke_;
		manager_ = other.manager_;
		heap_allocated_ = other.heap_allocated_;

		if (invoke_ == nullptr) {
			storage_.heap_object = nullptr;
			heap_allocated_ = false;
			return;
		}

		if (!other.heap_allocated()) {
			manager()(manager_op::move, inline_object(), other.inline_object());
			other.storage_.heap_object = nullptr;
			other.invoke_ = nullptr;
			other.manager_ = nullptr;
			other.heap_allocated_ = false;
			return;
		}

		storage_.heap_object = std::exchange(other.storage_.heap_object, nullptr);
		other.invoke_ = nullptr;
		other.manager_ = nullptr;
		other.heap_allocated_ = false;
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
			&& alignof(fn_t) <= alignof(void *)
			&& std::is_nothrow_move_constructible_v<fn_t>) {
			invoke_ = &invoke_inline<fn_t>;
			set_manager(&manage_inline<fn_t>, false);
			new (inline_object()) fn_t(std::forward<F>(fn));
		} else {
			storage_.heap_object = new fn_t(std::forward<F>(fn));
			invoke_ = &invoke_heap<fn_t>;
			set_manager(&manage_heap<fn_t>, true);
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
		return invoke_(object(), std::forward<Args>(args)...);
	}
};

#if defined(__GNUC__) && !defined(__clang__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Winterference-size"
#endif
static_assert(sizeof(small_move_only_function<void()>) <= std::hardware_destructive_interference_size);
#if defined(__GNUC__) && !defined(__clang__)
	#pragma GCC diagnostic pop
#endif

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
