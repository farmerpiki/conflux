export module conflux.uring.fd;
import std;
export namespace conflux::uring {

struct OsFd {
	std::uint32_t value{std::numeric_limits<std::uint32_t>::max()};
	[[nodiscard]] constexpr bool valid() const noexcept { return value != std::numeric_limits<std::uint32_t>::max(); }
	[[nodiscard]] static constexpr OsFd from_os(
		int fd) noexcept {
		return OsFd{static_cast<std::uint32_t>(fd)};
	}
	[[nodiscard]] constexpr std::uint32_t fd() const noexcept { return value; }
	[[nodiscard]] static constexpr bool is_direct() noexcept { return false; }
	[[nodiscard]] static constexpr bool is_os_fd() noexcept { return true; }
};
struct DirectFd {
	std::uint32_t value{std::numeric_limits<std::uint32_t>::max()};
	[[nodiscard]] constexpr bool valid() const noexcept { return value != std::numeric_limits<std::uint32_t>::max(); }
	[[nodiscard]] static constexpr DirectFd from_direct(
		std::uint32_t fd) noexcept {
		return DirectFd{fd};
	}
	[[nodiscard]] constexpr std::uint32_t fd() const noexcept { return value; }
	[[nodiscard]] static constexpr bool is_direct() noexcept { return true; }
	[[nodiscard]] static constexpr bool is_os_fd() noexcept { return false; }
};
template<typename T>
concept ClassicFd = std::same_as<std::remove_cvref_t<T>, OsFd>;
template<typename T>
concept DirectFdLike = std::same_as<std::remove_cvref_t<T>, DirectFd>;
template<typename T>
concept RingFd = ClassicFd<T> || DirectFdLike<T>;
struct DirectSlot {
	std::uint32_t value{};
};

} // namespace conflux::uring
