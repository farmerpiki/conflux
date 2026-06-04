module;
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

module conflux.file_io.reader;
import std;

namespace conflux::file_io {
namespace {

std::atomic<std::uint64_t> g_async_staging_counter{0};

[[nodiscard]] std::string make_staging_name_async() {
	auto const pid = static_cast<std::uint32_t>(::getpid());
	auto const seq = g_async_staging_counter.fetch_add(1, std::memory_order_relaxed);
	std::uint32_t rnd{};
	auto _ = ::getrandom(&rnd, sizeof(rnd), 0);
	return std::format(".conflux.tmp.{}.{}.{:08x}", pid, seq, rnd);
}

[[nodiscard]] constexpr bool is_otmpfile_unsupported_errno_async(
	int e) noexcept {
	return e == EOPNOTSUPP || e == EISDIR || e == EINVAL || e == ENOSYS || e == EPERM;
}

void ignore_best_effort_cleanup_failure() noexcept {}

struct AsyncAtomicPathParts {
	std::string parent_dir;
	std::string basename;
};

[[nodiscard]] std::expected<AsyncAtomicPathParts, IoError> split_contained_atomic_path_async(
	std::string_view path) noexcept {
	if (path.empty()) {
		return std::unexpected{
			IoError{EINVAL, "file_io: empty atomic-write path"}
        };
	}
	if (path.starts_with('/')) {
		return std::unexpected{
			IoError{EINVAL, "file_io: absolute atomic-write path"}
        };
	}
	if (path.contains('\0')) {
		return std::unexpected{
			IoError{EINVAL, "file_io: NUL in atomic-write path"}
        };
	}
	if (path == "." || path == ".." || path.ends_with('/')) {
		return std::unexpected{
			IoError{EINVAL, "file_io: invalid atomic-write path"}
        };
	}

	std::string_view remaining = path;
	while (!remaining.empty()) {
		auto const slash = remaining.find('/');
		auto const component = remaining.substr(0, slash);
		if (component.empty() || component == "..") {
			return std::unexpected{
				IoError{EINVAL, "file_io: invalid atomic-write path component"}
            };
		}
		if (slash == std::string_view::npos) {
			break;
		}
		remaining = remaining.substr(slash + 1);
	}

	auto const last_slash = path.rfind('/');
	if (last_slash == std::string_view::npos) {
		return AsyncAtomicPathParts{.parent_dir = std::string{"."}, .basename = std::string{path}};
	}
	return AsyncAtomicPathParts{
		.parent_dir = std::string{path.substr(0, last_slash)},
		.basename = std::string{path.substr(last_slash + 1)},
	};
}

} // namespace

root::Task<FileHandle> FileReader::async_open_atomic_parent_dir(
	int root_dir_fd,
	std::string parent_dir) {
	open_how how{};
	how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
	how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
	co_return co_await async_openat2(root_dir_fd, std::move(parent_dir), how);
}

root::Task<FileHandle> FileReader::async_open_atomic_payload(
	int parent_dir_fd,
	std::string staging_name,
	mode_t mode,
	bool &staging_entry_exists) {
	open_how how{};
	how.flags = O_TMPFILE | O_WRONLY | O_CLOEXEC;
	how.mode = static_cast<__u64>(mode);
	how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;

	try {
		co_return co_await async_openat2(parent_dir_fd, std::string{"."}, how);
	} catch (IoError const &e) {
		if (!is_otmpfile_unsupported_errno_async(e.code().value())) {
			throw;
		}
	}

	open_how fallback{};
	fallback.flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
	fallback.mode = static_cast<__u64>(mode);
	fallback.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
	auto fh = co_await async_openat2(parent_dir_fd, std::move(staging_name), fallback);
	staging_entry_exists = true;
	co_return fh;
}

root::Task<void> FileReader::async_link_atomic_payload(
	FileHandle const &fh,
	int parent_dir_fd,
	std::string staging_name) {
	try {
		co_await async_linkat(fh.raw_fd(), std::string{}, parent_dir_fd, std::string{staging_name}, AT_EMPTY_PATH);
		co_return;
	} catch (IoError const &e) {
		int const code = e.code().value();
		if (code != EPERM && code != EINVAL && code != ENOENT) {
			throw;
		}
	}
	co_await async_linkat(
		AT_FDCWD,
		std::format("/proc/self/fd/{}", fh.raw_fd()),
		parent_dir_fd,
		std::move(staging_name),
		AT_SYMLINK_FOLLOW);
}

root::Task<void> FileReader::async_atomic_write(
	int dir_fd,
	std::string rel_path,
	std::span<std::byte const> data,
	mode_t mode,
	conflux::file_io_sync::TempPublishMode pub_mode,
	conflux::file_io_sync::TempDurability durability) {
	auto parts = split_contained_atomic_path_async(rel_path);
	if (!parts) {
		throw parts.error();
	}

	auto parent_fh = co_await async_open_atomic_parent_dir(dir_fd, std::move(parts->parent_dir));
	int const parent_fd = parent_fh.raw_fd();
	std::string const staging = make_staging_name_async();
	bool staging_entry_exists = false;

	std::exception_ptr cleanup_error;
	try {
		auto fh = co_await async_open_atomic_payload(parent_fd, std::string{staging}, mode, staging_entry_exists);

		std::size_t off = 0;
		while (off < data.size()) {
			auto wrote = co_await write_into(fh, off, data.subspan(off));
			if (wrote == 0) {
				throw IoError{EIO, "file_io: short write"};
			}
			off += wrote;
		}

		if (durability >= conflux::file_io_sync::TempDurability::file) {
			co_await async_fsync(fh, true);
		}

		if (!staging_entry_exists) {
			co_await async_link_atomic_payload(fh, parent_fd, std::string{staging});
			staging_entry_exists = true;
		}

		if (pub_mode == conflux::file_io_sync::TempPublishMode::replace_existing) {
			co_await async_renameat(parent_fd, std::string{staging}, parent_fd, std::move(parts->basename));
		} else {
			co_await async_renameat(
				parent_fd,
				std::string{staging},
				parent_fd,
				std::move(parts->basename),
				RENAME_NOREPLACE);
		}
		staging_entry_exists = false;

		if (durability >= conflux::file_io_sync::TempDurability::file_and_directory) {
			co_await async_fsync(parent_fh);
		}
	} catch (...) { cleanup_error = std::current_exception(); }
	if (staging_entry_exists) {
		try {
			co_await async_unlinkat(parent_fd, std::string{staging});
		} catch (...) { ignore_best_effort_cleanup_failure(); }
	}
	if (cleanup_error) {
		std::rethrow_exception(cleanup_error);
	}
}

} // namespace conflux::file_io
