module;
#include <memory>

export module conflux.net.io_buffer;

import std;
import conflux.types;
import conflux.work.root;
export struct IoBuffer {
	std::span<std::byte const> bytes{};
	std::shared_ptr<void const> owner{};

	IoBuffer() = default;
	explicit IoBuffer(
		std::span<std::byte const> view)
		: bytes{view} {}
	IoBuffer(
		std::shared_ptr<std::byte const[]> owned_bytes,
		std::size_t size)
		: bytes{owned_bytes.get(), size}
		, owner{owned_bytes, static_cast<void const *>(owned_bytes.get())} {}
	[[nodiscard]] static IoBuffer from_string(
		std::string value) {
		auto owned = std::make_shared<std::string const>(std::move(value));
		auto view = std::span{
			reinterpret_cast<std::byte const *>(owned->data()),
			owned->size()}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
		return IoBuffer{view, std::move(owned)};
	}

private:
	IoBuffer(
		std::span<std::byte const> view,
		std::shared_ptr<void const> keep_alive)
		: bytes{view}
		, owner{std::move(keep_alive)} {}
};
export struct BufferList {
	std::vector<std::span<std::byte const>> segments{};
};
export struct IoPlan {
	enum class Kind : std::uint8_t {
		callback,
	};

	Kind kind = Kind::callback;
	conflux::work::root::detail::small_move_only_function<void()> callback{};
	[[nodiscard]] static IoPlan call(
		conflux::work::root::detail::small_move_only_function<void()> fn) {
		return IoPlan{.kind = Kind::callback, .callback = std::move(fn)};
	}
};
