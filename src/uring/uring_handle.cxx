module;
#include <liburing.h>
#include <unistd.h>

export module conflux.uring.handle;

import std;
import conflux.types;
import conflux.uring;

export struct RingFd {
	std::uint32_t id{std::numeric_limits<std::uint32_t>::max()}; // std::max = invalid sentinel; avoids aliasing fd 0
	bool fixed{false};
	[[nodiscard]] constexpr bool valid() const noexcept { return id != std::numeric_limits<std::uint32_t>::max(); }
	[[nodiscard]] static constexpr RingFd from_os(
		int fd) noexcept {
		return {.id = static_cast<std::uint32_t>(fd), .fixed = false};
	}
	[[nodiscard]] static constexpr RingFd from_direct(
		std::uint32_t slot) noexcept {
		return {.id = slot, .fixed = true};
	}
	[[nodiscard]] constexpr bool is_direct() const noexcept { return fixed && valid(); }
	[[nodiscard]] constexpr bool is_os_fd() const noexcept { return !fixed && valid(); }
	[[nodiscard]] constexpr int sqe_fd_value() const noexcept { return static_cast<int>(id); }
	[[nodiscard]] constexpr conflux::uring::Fd sqe_fd() const noexcept { return conflux::uring::Fd{sqe_fd_value()}; }
	[[nodiscard]] constexpr conflux::uring::DirectSlot direct_slot() const noexcept {
		return conflux::uring::DirectSlot{id};
	}
	[[nodiscard]] constexpr conflux::uring::SqeFlags sqe_fd_flags() const noexcept {
		return fixed ? conflux::uring::sqe_flags::fixed_file : conflux::uring::SqeFlags{};
	}
	[[nodiscard]] constexpr int as_fd() const noexcept { return sqe_fd_value(); }
};
export template<typename T>
concept RingFdLike = requires(T const &fd) {
	{ fd.sqe_fd_value() } -> std::convertible_to<int>;
	{ fd.sqe_fd_flags() } -> std::same_as<conflux::uring::SqeFlags>;
};

export inline void apply_sqe_fd_flags(
	io_uring_sqe *sqe,
	RingFdLike auto const &fd) noexcept {
	sqe->flags = static_cast<decltype(sqe->flags)>(sqe->flags | fd.sqe_fd_flags().raw());
}

export class IoHandle {
	RingFd h_{};
	void close_on_drop() noexcept {
		if (h_.is_os_fd()) {
			::close(static_cast<int>(h_.id));
		}
#ifndef NDEBUG
		if (h_.is_direct()) {
			static constexpr char message[] =
				"IoHandle dropped with live direct slot - close_async was never called; slot will leak\n";
			auto remaining = std::string_view{message, sizeof(message) - 1};
			while (!remaining.empty()) {
				auto const n = ::write(STDERR_FILENO, remaining.data(), remaining.size());
				if (n <= 0) {
					break;
				}
				remaining.remove_prefix(static_cast<std::size_t>(n));
			}
		}
#endif
		h_ = RingFd{};
	}

public:
	IoHandle() noexcept {} // NOLINT(modernize-use-equals-default) — GCC module bug
	IoHandle(IoHandle const &) = delete;
	IoHandle &operator =(IoHandle const &) = delete;
	IoHandle(
		IoHandle &&o) noexcept
		: h_{std::exchange(o.h_, RingFd{})} {}
	IoHandle &operator =(
		IoHandle &&o) noexcept {
		if (this != &o) {
			close_on_drop();
			h_ = std::exchange(o.h_, RingFd{});
		}
		return *this;
	}
	~IoHandle() { close_on_drop(); }
	[[nodiscard]] static IoHandle from_fd(
		int fd) noexcept {
		IoHandle h;
		h.h_ = RingFd::from_os(fd);
		return h;
	}
	[[nodiscard]] static IoHandle from_direct_slot(
		int slot) noexcept {
		IoHandle h;
		h.h_ = RingFd::from_direct(static_cast<std::uint32_t>(slot));
		return h;
	}
	[[nodiscard]] RingFd get() const noexcept { return h_; }
	[[nodiscard]] int sqe_fd_value() const noexcept { return h_.sqe_fd_value(); }
	[[nodiscard]] conflux::uring::SqeFlags sqe_fd_flags() const noexcept { return h_.sqe_fd_flags(); }
	[[nodiscard]] conflux::uring::Fd sqe_fd() const noexcept { return h_.sqe_fd(); }
	[[nodiscard]] RingFd release() noexcept { return std::exchange(h_, RingFd{}); }
	[[nodiscard]] bool valid() const noexcept { return h_.valid(); }
	[[nodiscard]] bool is_direct() const noexcept { return h_.is_direct(); }
	[[nodiscard]] bool is_os_fd() const noexcept { return h_.is_os_fd(); }
	[[nodiscard]] int raw_fd() const noexcept { return h_.is_os_fd() ? static_cast<int>(h_.id) : -1; }
	[[nodiscard]] int direct_slot() const noexcept { return h_.is_direct() ? static_cast<int>(h_.id) : -1; }
	[[nodiscard]] int release_fd() noexcept {
		if (!h_.is_os_fd()) {
			return -1;
		}
		int const fd = static_cast<int>(h_.id);
		h_ = RingFd{};
		return fd;
	}
	[[nodiscard]] int release_direct_slot() noexcept {
		if (!h_.is_direct()) {
			return -1;
		}
		int const slot = static_cast<int>(h_.id);
		h_ = RingFd{};
		return slot;
	}
};
export using FileHandle = IoHandle;
