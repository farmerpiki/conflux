export module conflux.net.io_buffer;

import std;
import conflux.types;
import conflux.work.root;

export struct IoBuffer {
	span<byte const> bytes{};
	SP<void const> owner{};

	IoBuffer() = default;
	explicit IoBuffer(
		span<byte const> view)
		: bytes{view} {}
	IoBuffer(
		std::shared_ptr<std::byte const[]> owned_bytes,
		std::size_t size)
		: bytes{owned_bytes.get(), size}
		, owner{owned_bytes, static_cast<void const *>(owned_bytes.get())} {}

	[[nodiscard]] static IoBuffer from_string(
		S value) {
		auto owned = make_shared<S const>(move(value));
		auto view = span{
			reinterpret_cast<byte const *>(owned->data()),
			owned->size()}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
		return IoBuffer{view, move(owned)};
	}

private:
	IoBuffer(
		span<byte const> view,
		SP<void const> keep_alive)
		: bytes{view}
		, owner{move(keep_alive)} {}
};

export struct BufferList {
	V<span<byte const>> segments{};
};

export struct IoPlan {
	enum class Kind : u8 {
		callback,
	};

	Kind kind = Kind::callback;
	conflux::work::root::detail::small_move_only_function<void()> callback{};

	[[nodiscard]] static IoPlan call(
		conflux::work::root::detail::small_move_only_function<void()> fn) {
		return IoPlan{.kind = Kind::callback, .callback = move(fn)};
	}
};
