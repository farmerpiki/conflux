module;
#include <unistd.h>

export module conflux.uring.handle;

import std;
import conflux.uring;
import conflux_uring_fd;
import conflux_uring_sqe;

export using OsFd = conflux::uring::OsFd;
export using DirectFd = conflux::uring::DirectFd;
export template<typename T>
concept ClassicFd = conflux::uring::ClassicFd<T>;
export template<typename T>
concept DirectFdLike = conflux::uring::DirectFdLike<T>;
export template<typename T>
concept RingFd = conflux::uring::RingFd<T>;

export class IoHandle {
	enum class Kind : std::uint8_t {
		invalid,
		os,
		direct,
	};
	std::uint32_t value_{std::numeric_limits<std::uint32_t>::max()};
	Kind kind_{Kind::invalid};
	void close_on_drop() noexcept {
		if (is_os_fd()) {
			::close(static_cast<int>(value_));
		}
#ifndef NDEBUG
		if (is_direct()) {
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
		clear();
	}
	void clear() noexcept {
		value_ = std::numeric_limits<std::uint32_t>::max();
		kind_ = Kind::invalid;
	}

public:
	IoHandle() noexcept {} // NOLINT(modernize-use-equals-default) — GCC module bug
	IoHandle(IoHandle const &) = delete;
	IoHandle &operator =(IoHandle const &) = delete;
	IoHandle(
		IoHandle &&o) noexcept
		: value_{std::exchange(o.value_, std::numeric_limits<std::uint32_t>::max())}
		, kind_{std::exchange(o.kind_, Kind::invalid)} {}
	IoHandle &operator =(
		IoHandle &&o) noexcept {
		if (this != &o) {
			close_on_drop();
			value_ = std::exchange(o.value_, std::numeric_limits<std::uint32_t>::max());
			kind_ = std::exchange(o.kind_, Kind::invalid);
		}
		return *this;
	}
	~IoHandle() { close_on_drop(); }
	[[nodiscard]] static IoHandle from_fd(
		int fd) noexcept {
		IoHandle h;
		h.value_ = static_cast<std::uint32_t>(fd);
		h.kind_ = Kind::os;
		return h;
	}
	[[nodiscard]] static IoHandle from_direct_slot(
		int slot) noexcept {
		IoHandle h;
		h.value_ = static_cast<std::uint32_t>(slot);
		h.kind_ = Kind::direct;
		return h;
	}
	[[nodiscard]] OsFd os_fd() const noexcept { return is_os_fd() ? OsFd{value_} : OsFd{}; }
	[[nodiscard]] DirectFd direct_fd() const noexcept { return is_direct() ? DirectFd{value_} : DirectFd{}; }
	[[nodiscard]] OsFd get() const noexcept { return os_fd(); }
	[[nodiscard]] bool valid() const noexcept { return kind_ != Kind::invalid; }
	[[nodiscard]] bool is_direct() const noexcept { return kind_ == Kind::direct; }
	[[nodiscard]] bool is_os_fd() const noexcept { return kind_ == Kind::os; }
	[[nodiscard]] int raw_fd() const noexcept { return is_os_fd() ? static_cast<int>(value_) : -1; }
	[[nodiscard]] int direct_slot() const noexcept { return is_direct() ? static_cast<int>(value_) : -1; }
	[[nodiscard]] OsFd release() noexcept { return release_os_fd(); }
	[[nodiscard]] OsFd release_os_fd() noexcept {
		if (!is_os_fd()) {
			return {};
		}
		OsFd fd{value_};
		clear();
		return fd;
	}
	[[nodiscard]] DirectFd release_direct_fd() noexcept {
		if (!is_direct()) {
			return {};
		}
		DirectFd fd{value_};
		clear();
		return fd;
	}
	[[nodiscard]] int release_fd() noexcept {
		if (!is_os_fd()) {
			return -1;
		}
		int const fd = static_cast<int>(value_);
		clear();
		return fd;
	}
	[[nodiscard]] int release_direct_slot() noexcept {
		if (!is_direct()) {
			return -1;
		}
		int const slot = static_cast<int>(value_);
		clear();
		return slot;
	}
};
export using FileHandle = IoHandle;

export decltype(auto) visit_fd(
	IoHandle const &h,
	auto &&fn) {
	if (h.is_direct()) {
		return std::forward<decltype(fn)>(fn)(h.direct_fd());
	}
	return std::forward<decltype(fn)>(fn)(h.os_fd());
}

export decltype(auto) release_fd_tag(
	IoHandle &h,
	auto &&fn) {
	if (h.is_direct()) {
		return std::forward<decltype(fn)>(fn)(h.release_direct_fd());
	}
	return std::forward<decltype(fn)>(fn)(h.release_os_fd());
}
