module;

export module conflux.net.io_buffer;

import std;
import conflux.types;
import conflux.small_function;
import conflux.work.root;

export namespace conflux::net {

struct IoBuffer {
	std::span<std::byte const> bytes{};
	std::shared_ptr<void const> owner{};

	IoBuffer() = default;
	explicit IoBuffer(
		std::span<std::byte const> view)
		: bytes{view} {}
	IoBuffer(
		std::shared_ptr<std::byte const[]> const &owned_bytes,
		std::size_t size)
		: bytes{owned_bytes.get(), size}
		, owner{owned_bytes, static_cast<void const *>(owned_bytes.get())} {}
	[[nodiscard]] static IoBuffer from_string(
		std::string value) {
		auto owned = std::make_shared<std::string const>(std::move(value));
		auto view = std::as_bytes(std::span{owned->data(), owned->size()});
		return IoBuffer{view, std::move(owned)};
	}

private:
	IoBuffer(
		std::span<std::byte const> view,
		std::shared_ptr<void const> keep_alive)
		: bytes{view}
		, owner{std::move(keep_alive)} {}
};
struct BufferList {
	std::vector<std::span<std::byte const>> segments{};
};
struct IoPlan {
	enum class Kind : std::uint8_t {
		callback,
	};

	Kind kind = Kind::callback;
	conflux::detail::small_move_only_function<void()> callback{};
	[[nodiscard]] static IoPlan call(
		conflux::detail::small_move_only_function<void()> fn) {
		return IoPlan{.kind = Kind::callback, .callback = std::move(fn)};
	}
};

} // namespace conflux::net
