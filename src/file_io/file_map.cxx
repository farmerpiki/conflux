module;
#include <cerrno>
#include <fcntl.h>
#include <linux/openat2.h>
#include <memory>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

export module conflux.file_map;

import std;
import conflux.types;
export import conflux.file_io_sync;
struct MappedRegionEntry {
	void const *ptr{};
	std::uint64_t mmap_size{};
	std::uint64_t file_size{};
	~MappedRegionEntry() noexcept {
		if (ptr != nullptr) {
			::munmap(const_cast<void *>(ptr), static_cast<std::size_t>(mmap_size));
		}
	}
	MappedRegionEntry() noexcept = default;
	MappedRegionEntry(
		void const *p,
		std::uint64_t msz,
		std::uint64_t fsz) noexcept
		: ptr{p}
		, mmap_size{msz}
		, file_size{fsz} {}
	MappedRegionEntry(MappedRegionEntry const &) = delete;
	MappedRegionEntry &operator =(MappedRegionEntry const &) = delete;
	MappedRegionEntry(MappedRegionEntry &&) = delete;
	MappedRegionEntry &operator =(MappedRegionEntry &&) = delete;
};
export class MappedFileLease {
	std::shared_ptr<MappedRegionEntry> region_{};

public:
	MappedFileLease() noexcept = default;
	explicit MappedFileLease(
		std::shared_ptr<MappedRegionEntry> r) noexcept
		: region_{move(r)} {}
	[[nodiscard]] std::span<std::byte const> bytes() const noexcept {
		if (!region_ || region_->ptr == nullptr) {
			return {};
		}
		return {static_cast<std::byte const *>(region_->ptr), static_cast<std::size_t>(region_->file_size)};
	}
	[[nodiscard]] bool empty() const noexcept { return !region_ || region_->ptr == nullptr || region_->file_size == 0; }
	[[nodiscard]] std::uint64_t size() const noexcept { return region_ ? region_->file_size : 0; }
};
export struct MappedBody {
	MappedFileLease lease;
	std::uint64_t offset{};
	std::uint64_t size{};
	[[nodiscard]] std::span<std::byte const> window() const noexcept {
		auto const full = lease.bytes();
		if (offset >= full.size()) {
			return {};
		}
		auto const avail = full.size() - static_cast<std::size_t>(offset);
		auto const len = min(static_cast<std::size_t>(size), avail);
		return full.subspan(static_cast<std::size_t>(offset), len);
	}
};
using FileMapError = IoError;
export std::expected<MappedFileLease, FileMapError> blocking_map_fd_readonly(
	int fd,
	FileStat const &st) noexcept {
	if (st.size == 0) {
		return MappedFileLease{};
	}
	auto *ptr = ::mmap(nullptr, static_cast<std::size_t>(st.size), PROT_READ, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) {
		return unexpected{
			FileMapError{errno, "file_map: mmap"}
        };
	}
	return MappedFileLease{make_shared<MappedRegionEntry>(ptr, st.size, st.size)};
}
export std::expected<MappedFileLease, FileMapError> blocking_map_file_readonly(
	int dir_fd,
	std::string_view relative,
	std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) noexcept {
	open_how how{};
	how.flags = O_RDONLY | O_CLOEXEC;
	how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
	std::string rel{relative};
	int const fd = static_cast<int>(::syscall(SYS_openat2, dir_fd, rel.c_str(), &how, sizeof(how)));
	if (fd < 0) {
		return unexpected{
			FileMapError{errno, "file_map: openat2"}
        };
	}
	UniqueFd guard{fd};

	auto st = blocking_fstat(fd);
	if (!st) {
		return unexpected{
			FileMapError{st.error().code().value(), "file_map: fstat"}
        };
	}
	if (st->size > max_bytes) {
		return unexpected{
			FileMapError{EFBIG, "file_map: file exceeds max_bytes"}
        };
	}
	return blocking_map_fd_readonly(fd, *st);
}
