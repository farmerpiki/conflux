module;
#include <cerrno>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

export module conflux.file_map;

import std;
import conflux.types;
export import conflux.file_map.types;
export import conflux.file_io_sync;
using FileMapError = IoError;
export std::expected<MappedFileLease, FileMapError> blocking_map_fd_readonly(
	int fd,
	FileStat const &st) noexcept {
	if (st.size == 0) {
		return MappedFileLease{};
	}
	auto *ptr = ::mmap(nullptr, static_cast<std::size_t>(st.size), PROT_READ, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED) {
		return std::unexpected{
			FileMapError{errno, "file_map: mmap"}
        };
	}
	return make_mapped_file_lease(ptr, st.size, st.size);
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
		return std::unexpected{
			FileMapError{errno, "file_map: openat2"}
        };
	}
	UniqueFd guard{fd};

	auto st = blocking_fstat(fd);
	if (!st) {
		return std::unexpected{
			FileMapError{st.error().code().value(), "file_map: fstat"}
        };
	}
	if (!S_ISREG(st->mode)) {
		return std::unexpected{
			FileMapError{EISDIR, "file_map: not a regular file"}
        };
	}
	if (st->size > max_bytes) {
		return std::unexpected{
			FileMapError{EFBIG, "file_map: file exceeds max_bytes"}
        };
	}
	return blocking_map_fd_readonly(fd, *st);
}
