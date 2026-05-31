module;
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

export module conflux.file_io_sync;

import std;
import conflux.types;

namespace conflux::file_io_sync {

// ───────────────────────────────────────────────────────────────────────
// FileStat — portable stat result for cache revalidation.
// ───────────────────────────────────────────────────────────────────────

export struct FileStat {
	std::uint64_t size{};
	std::uint64_t mtime_ns{};
	std::uint64_t ctime_ns{};
	std::uint64_t dev{};
	std::uint64_t ino{};
	std::uint32_t mode{};
};
// ───────────────────────────────────────────────────────────────────────
// UniqueFd — RAII wrapper for raw POSIX fds (no io_uring coupling).
// ───────────────────────────────────────────────────────────────────────

export class UniqueFd {
	int fd_{-1};

public:
	constexpr UniqueFd() noexcept = default;
	constexpr explicit UniqueFd(
		int fd) noexcept
		: fd_{fd} {}
	UniqueFd(UniqueFd const &) = delete;
	UniqueFd &operator =(UniqueFd const &) = delete;
	constexpr UniqueFd(
		UniqueFd &&o) noexcept
		: fd_{std::exchange(o.fd_, -1)} {}
	constexpr UniqueFd &operator =(
		UniqueFd &&o) noexcept {
		if (this != &o) {
			reset();
			fd_ = std::exchange(o.fd_, -1);
		}
		return *this;
	}
	~UniqueFd() noexcept { reset(); }
	void reset() noexcept {
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}
	[[nodiscard]] constexpr int fd() const noexcept { return fd_; }
	[[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }
	[[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
	[[nodiscard]] constexpr explicit operator bool() const noexcept { return fd_ >= 0; }
};

// ───────────────────────────────────────────────────────────────────────
// Enums + options
// ───────────────────────────────────────────────────────────────────────

export enum class TempPublishMode : std::uint8_t {
	replace_existing,
	create_new,
};

export enum class TempDurability : std::uint8_t {
	none,
	file,
	file_and_directory,
};
export struct TempFileOptions {
	mode_t mode = 0644;
	bool prefer_otmpfile = true;
	TempDurability durability = TempDurability::file_and_directory;
};
// ───────────────────────────────────────────────────────────────────────
// TemporaryFileSync — owns an open temp fd (unnamed or named).
// ───────────────────────────────────────────────────────────────────────

export class TemporaryFileSync {
	UniqueFd fd_{};
	bool unnamed_{false};
	std::string staging_name_{};
	int parent_fd_{-1};

public:
	TemporaryFileSync() noexcept = default;
	TemporaryFileSync(
		UniqueFd fd,
		bool unnamed,
		int parent_fd) noexcept
		: fd_{std::move(fd)}
		, unnamed_{unnamed}
		, parent_fd_{parent_fd} {}
	TemporaryFileSync(
		UniqueFd fd,
		std::string staging_name,
		int parent_fd) noexcept
		: fd_{std::move(fd)}
		, unnamed_{false}
		, staging_name_{std::move(staging_name)}
		, parent_fd_{parent_fd} {}
	TemporaryFileSync(TemporaryFileSync &&) noexcept = default;
	TemporaryFileSync &operator =(TemporaryFileSync &&) noexcept = default;
	~TemporaryFileSync() noexcept {
		if (!staging_name_.empty() && parent_fd_ >= 0) {
			::unlinkat(parent_fd_, staging_name_.c_str(), 0);
		}
	}
	[[nodiscard]] int fd() const noexcept { return fd_.fd(); }
	[[nodiscard]] bool unnamed() const noexcept { return unnamed_; }
	[[nodiscard]] std::string_view staging_name() const noexcept { return staging_name_; }
	[[nodiscard]] int parent_fd() const noexcept { return parent_fd_; }
	UniqueFd take_fd() noexcept { return std::move(fd_); }
	void disarm_staging() noexcept { staging_name_.clear(); }
};
export using FileIoSyncError = IoError;
// ───────────────────────────────────────────────────────────────────────
// Internals
// ───────────────────────────────────────────────────────────────────────

namespace {

std::atomic<std::uint64_t> g_staging_counter{0};
inline std::string make_staging_name() noexcept {
	auto const pid = static_cast<std::uint32_t>(::getpid());
	auto const seq = g_staging_counter.fetch_add(1, std::memory_order_relaxed);
	std::uint32_t rnd{};
	auto _ = ::getrandom(&rnd, sizeof(rnd), 0);
	return std::format(".conflux.tmp.{}.{}.{:08x}", pid, seq, rnd);
}
inline std::expected<void, FileIoSyncError> do_fsync(
	int fd,
	TempDurability d) noexcept {
	if (d < TempDurability::file) {
		return {};
	}
	int const rc = ::fdatasync(fd);
	if (rc < 0) {
		return std::unexpected{
			FileIoSyncError{errno, "file_io_sync: fdatasync"}
        };
	}
	return {};
}
inline std::expected<void, FileIoSyncError> do_dir_fsync(
	int dir_fd,
	TempDurability d) noexcept {
	if (d < TempDurability::file_and_directory) {
		return {};
	}
	int const rc = ::fsync(dir_fd);
	if (rc < 0) {
		return std::unexpected{
			FileIoSyncError{errno, "file_io_sync: dir fsync"}
        };
	}
	return {};
}
constexpr bool is_otmpfile_unsupported_errno(
	int e) noexcept {
	return e == EOPNOTSUPP || e == EISDIR || e == EINVAL || e == ENOSYS || e == EPERM;
}
inline std::expected<void, FileIoSyncError> link_unnamed_fd(
	int tmp_fd,
	int parent_fd,
	std::string_view staging_name) noexcept {
	// AT_EMPTY_PATH — requires CAP_DAC_READ_SEARCH on most kernels
	int rc = static_cast<int>(::syscall(SYS_linkat, tmp_fd, "", parent_fd, staging_name.data(), AT_EMPTY_PATH));
	if (rc == 0) {
		return {};
	}
	int const e1 = errno;
	if (e1 != EPERM && e1 != EINVAL && e1 != ENOENT) {
		return std::unexpected{
			FileIoSyncError{e1, "file_io_sync: linkat AT_EMPTY_PATH"}
        };
	}

	// /proc/self/fd fallback
	auto proc = std::format("/proc/self/fd/{}", tmp_fd);
	rc = ::linkat(AT_FDCWD, proc.c_str(), parent_fd, staging_name.data(), AT_SYMLINK_FOLLOW);
	if (rc == 0) {
		return {};
	}
	return std::unexpected{
		FileIoSyncError{errno, "file_io_sync: linkat /proc/self/fd"}
    };
}
inline int openat2_sync(
	int dir_fd,
	char const *path,
	std::uint64_t flags,
	std::uint64_t mode,
	std::uint64_t resolve) noexcept {
	open_how how{};
	how.flags = flags;
	how.mode = mode;
	how.resolve = resolve;
	return static_cast<int>(::syscall(SYS_openat2, dir_fd, path, &how, sizeof(how)));
}
inline std::expected<UniqueFd, FileIoSyncError> open_parent_dir_contained(
	int root_fd,
	std::string_view relative_dir) noexcept {
	if (relative_dir.empty() || relative_dir == ".") {
		int fd = openat2_sync(
			root_fd,
			".",
			O_RDONLY | O_DIRECTORY | O_CLOEXEC,
			0,
			RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS);
		if (fd < 0) {
			return std::unexpected{
				FileIoSyncError{errno, "file_io_sync: open parent dir"}
            };
		}
		return UniqueFd{fd};
	}
	std::string dir_str{relative_dir};
	int fd = openat2_sync(
		root_fd,
		dir_str.c_str(),
		O_RDONLY | O_DIRECTORY | O_CLOEXEC,
		0,
		RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS);
	if (fd < 0) {
		return std::unexpected{
			FileIoSyncError{errno, "file_io_sync: open parent dir"}
        };
	}
	return UniqueFd{fd};
}

inline std::expected<void, FileIoSyncError> validate_publish_basename(
	std::string_view name) noexcept {
	if (name.empty()) {
		return std::unexpected{
			FileIoSyncError{EINVAL, "file_io_sync: empty publish name"}
        };
	}
	if (name == "." || name == "..") {
		return std::unexpected{
			FileIoSyncError{EINVAL, "file_io_sync: invalid publish name"}
        };
	}
	if (name.contains('/') || name.contains('\0')) {
		return std::unexpected{
			FileIoSyncError{EINVAL, "file_io_sync: publish name must be a basename"}
        };
	}
	return {};
}

} // namespace
struct PathParts {
	std::string_view parent_dir;
	std::string_view basename;
};
inline std::expected<PathParts, FileIoSyncError> split_contained_path(
	std::string_view path) noexcept {
	if (path.empty()) {
		return std::unexpected{
			FileIoSyncError{EINVAL, "file_io_sync: empty path"}
        };
	}
	if (path.starts_with('/')) {
		return std::unexpected{
			FileIoSyncError{EINVAL, "file_io_sync: absolute path"}
        };
	}
	if (path.contains('\0')) {
		return std::unexpected{
			FileIoSyncError{EINVAL, "file_io_sync: NUL in path"}
        };
	}

	// reject pure . or ..
	if (path == "." || path == "..") {
		return std::unexpected{
			FileIoSyncError{EINVAL, "file_io_sync: invalid path component"}
        };
	}

	// reject path components that are ..
	std::string_view remaining = path;
	while (!remaining.empty()) {
		auto const slash = remaining.find('/');
		auto const component = remaining.substr(0, slash);
		if (component == ".." || component.empty()) {
			if (component == "..") {
				return std::unexpected{
					FileIoSyncError{EINVAL, "file_io_sync: .. in path"}
                };
			}
		}
		if (slash == std::string_view::npos) {
			break;
		}
		remaining = remaining.substr(slash + 1);
	}

	auto const last_slash = path.rfind('/');
	if (last_slash == std::string_view::npos) {
		return PathParts{.parent_dir = ".", .basename = path};
	}
	return PathParts{.parent_dir = path.substr(0, last_slash), .basename = path.substr(last_slash + 1)};
}
// ───────────────────────────────────────────────────────────────────────
// Low-level: blocking_openat_contained
// ───────────────────────────────────────────────────────────────────────

export std::expected<UniqueFd, FileIoSyncError> blocking_openat_contained(
	int root_fd,
	std::string_view contained_relative_path,
	int flags,
	mode_t mode = 0) noexcept {
	auto parts = split_contained_path(contained_relative_path);
	if (!parts) {
		return std::unexpected{parts.error()};
	}
	std::string path{contained_relative_path};
	int const fd = openat2_sync(
		root_fd,
		path.c_str(),
		static_cast<std::uint64_t>(flags | O_CLOEXEC),
		mode,
		RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS);
	if (fd < 0) {
		return std::unexpected{
			FileIoSyncError{errno, "file_io_sync: open contained file"}
        };
	}
	return UniqueFd{fd};
}

export std::expected<UniqueFd, FileIoSyncError> blocking_open_file(
	std::string_view path,
	int flags,
	mode_t mode = 0) noexcept {
	if (path.empty() || path.contains('\0')) {
		return std::unexpected{
			FileIoSyncError{EINVAL, "file_io_sync: invalid file path"}
        };
	}
	std::string native{path};
	int const fd = ::open(native.c_str(), flags | O_CLOEXEC, mode);
	if (fd < 0) {
		return std::unexpected{
			FileIoSyncError{errno, "file_io_sync: open file"}
        };
	}
	return UniqueFd{fd};
}

export std::expected<UniqueFd, FileIoSyncError> blocking_open_directory(
	std::string_view path) noexcept {
	if (path.empty() || path.contains('\0')) {
		return std::unexpected{
			FileIoSyncError{EINVAL, "file_io_sync: invalid directory path"}
        };
	}
	std::string native{path};
	int const fd = ::open(native.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		return std::unexpected{
			FileIoSyncError{errno, "file_io_sync: open directory"}
        };
	}
	return UniqueFd{fd};
}

// ───────────────────────────────────────────────────────────────────────
// Low-level: blocking_open_tmpfile
// ───────────────────────────────────────────────────────────────────────

export std::expected<TemporaryFileSync, FileIoSyncError> blocking_open_tmpfile(
	int parent_dir_fd,
	TempFileOptions opts = {}) noexcept {
	if (opts.prefer_otmpfile) {
		int fd = ::openat(parent_dir_fd, ".", O_TMPFILE | O_WRONLY | O_CLOEXEC, opts.mode);
		if (fd >= 0) {
			return TemporaryFileSync{UniqueFd{fd}, true, parent_dir_fd};
		}
		if (!is_otmpfile_unsupported_errno(errno)) {
			return std::unexpected{
				FileIoSyncError{errno, "file_io_sync: O_TMPFILE"}
            };
		}
	}
	// named-temp fallback
	auto staging = make_staging_name();
	int fd = ::openat(parent_dir_fd, staging.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, opts.mode);
	if (fd < 0) {
		return std::unexpected{
			FileIoSyncError{errno, "file_io_sync: named temp create"}
        };
	}
	return TemporaryFileSync{UniqueFd{fd}, std::move(staging), parent_dir_fd};
}
// ───────────────────────────────────────────────────────────────────────
// Low-level: write_all_fd
// ───────────────────────────────────────────────────────────────────────

export std::expected<void, FileIoSyncError> write_all_fd(
	int fd,
	std::span<std::byte const> bytes) noexcept {
	std::size_t off = 0;
	while (off < bytes.size()) {
		auto const n = ::write(fd, bytes.data() + off, bytes.size() - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return std::unexpected{
				FileIoSyncError{errno, "file_io_sync: write"}
            };
		}
		if (n == 0) {
			return std::unexpected{
				FileIoSyncError{EIO, "file_io_sync: zero-length write"}
            };
		}
		off += static_cast<std::size_t>(n);
	}
	return {};
}
// ───────────────────────────────────────────────────────────────────────
// Low-level: blocking_publish_tmpfile
// ───────────────────────────────────────────────────────────────────────

export std::expected<void, FileIoSyncError> blocking_publish_tmpfile(
	TemporaryFileSync &&tmp,
	int parent_dir_fd,
	std::string_view final_name,
	TempPublishMode mode = TempPublishMode::replace_existing,
	TempDurability durability = TempDurability::file_and_directory) noexcept {
	if (auto valid = validate_publish_basename(final_name); !valid) {
		return std::unexpected{valid.error()};
	}
	std::string final_str;
	try {
		final_str = std::string{final_name};
	} catch (std::bad_alloc const &) {
		return std::unexpected{
			FileIoSyncError{ENOMEM, "file_io_sync: publish name allocation"}
        };
	}
	if (auto r = do_fsync(tmp.fd(), durability); !r) {
		return r;
	}

	auto staging = std::string{tmp.staging_name()};
	bool need_unlink_staging = false;

	if (tmp.unnamed()) {
		staging = make_staging_name();
		auto r = link_unnamed_fd(tmp.fd(), parent_dir_fd, staging);
		if (!r) {
			// O_TMPFILE linkat failed entirely — fall back to error
			return std::unexpected{r.error()};
		}
		need_unlink_staging = true;
		tmp.disarm_staging();
	} else {
		need_unlink_staging = true;
		tmp.disarm_staging();
	}

	if (mode == TempPublishMode::replace_existing) {
		int const rc = ::renameat(parent_dir_fd, staging.c_str(), parent_dir_fd, final_str.c_str());
		if (rc < 0) {
			if (need_unlink_staging) {
				::unlinkat(parent_dir_fd, staging.c_str(), 0);
			}
			return std::unexpected{
				FileIoSyncError{errno, "file_io_sync: renameat"}
            };
		}
	} else {
		// create_new — use renameat2 RENAME_NOREPLACE
		int const rc = static_cast<int>(::syscall(
			SYS_renameat2,
			parent_dir_fd,
			staging.c_str(),
			parent_dir_fd,
			final_str.c_str(),
			RENAME_NOREPLACE));
		if (rc < 0) {
			int const e = errno;
			if (need_unlink_staging) {
				::unlinkat(parent_dir_fd, staging.c_str(), 0);
			}
			if (e == ENOSYS) {
				return std::unexpected{
					FileIoSyncError{ENOTSUP, "file_io_sync: RENAME_NOREPLACE unsupported"}
                };
			}
			return std::unexpected{
				FileIoSyncError{e, "file_io_sync: renameat2 RENAME_NOREPLACE"}
            };
		}
	}

	auto r = do_dir_fsync(parent_dir_fd, durability);
	// fd cleanup happens via TemporaryFileSync destructor
	return r;
}
// ───────────────────────────────────────────────────────────────────────
// High-level: blocking_write_file_atomic_at
// ───────────────────────────────────────────────────────────────────────

export std::expected<void, FileIoSyncError> blocking_write_file_atomic_at(
	int root_fd,
	std::string_view contained_relative_path,
	std::span<std::byte const> bytes,
	TempFileOptions opts = {},
	TempPublishMode mode = TempPublishMode::replace_existing) noexcept {
	auto parts = split_contained_path(contained_relative_path);
	if (!parts) {
		return std::unexpected{parts.error()};
	}

	auto parent = open_parent_dir_contained(root_fd, parts->parent_dir);
	if (!parent) {
		return std::unexpected{parent.error()};
	}

	auto durability = opts.durability;
	auto tmp = blocking_open_tmpfile(parent->fd(), opts);
	if (!tmp) {
		return std::unexpected{tmp.error()};
	}

	auto wr = write_all_fd(tmp->fd(), bytes);
	if (!wr) {
		return std::unexpected{wr.error()};
	}

	return blocking_publish_tmpfile(std::move(*tmp), parent->fd(), parts->basename, mode, durability);
}

export std::expected<void, FileIoSyncError> blocking_unlink_file_at(
	int root_fd,
	std::string_view contained_relative_path) noexcept {
	auto parts = split_contained_path(contained_relative_path);
	if (!parts) {
		return std::unexpected{parts.error()};
	}

	auto parent = open_parent_dir_contained(root_fd, parts->parent_dir);
	if (!parent) {
		return std::unexpected{parent.error()};
	}

	std::string basename{parts->basename};
	if (::unlinkat(parent->fd(), basename.c_str(), 0) != 0) {
		return std::unexpected{
			FileIoSyncError{errno, "file_io_sync: unlink contained file"}
        };
	}
	return {};
}

// ───────────────────────────────────────────────────────────────────────
// High-level: blocking_write_text_file_atomic_at
// ───────────────────────────────────────────────────────────────────────

export std::expected<void, FileIoSyncError> blocking_write_text_file_atomic_at(
	int root_fd,
	std::string_view contained_relative_path,
	std::string_view text,
	TempFileOptions opts = {},
	TempPublishMode mode = TempPublishMode::replace_existing) noexcept {
	return blocking_write_file_atomic_at(
		root_fd,
		contained_relative_path,
		std::as_bytes(std::span{text.data(), text.size()}),
		opts,
		mode);
}
// ───────────────────────────────────────────────────────────────────────
// blocking_fstat / blocking_stat_at — populate FileStat from kernel statx.
// ───────────────────────────────────────────────────────────────────────

export std::expected<FileStat, FileIoSyncError> blocking_fstat(
	int fd) noexcept {
	struct statx stx{};
	int const rc = ::statx(fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS | STATX_MTIME | STATX_CTIME, &stx);
	if (rc < 0) {
		return std::unexpected{
			FileIoSyncError{errno, "file_io_sync: statx"}
        };
	}
	return FileStat{
		.size = stx.stx_size,
		.mtime_ns = static_cast<std::uint64_t>(stx.stx_mtime.tv_sec) * 1000000000ULL + stx.stx_mtime.tv_nsec,
		.ctime_ns = static_cast<std::uint64_t>(stx.stx_ctime.tv_sec) * 1000000000ULL + stx.stx_ctime.tv_nsec,
		.dev = static_cast<std::uint64_t>(stx.stx_dev_major) << 32U | stx.stx_dev_minor,
		.ino = stx.stx_ino,
		.mode = stx.stx_mode};
}
export std::expected<FileStat, FileIoSyncError> blocking_stat_at(
	int dir_fd,
	std::string_view path,
	int flags = 0,
	unsigned mask = STATX_BASIC_STATS | STATX_MTIME | STATX_CTIME) noexcept {
	std::string p{path};
	struct statx stx{};
	int const rc = ::statx(dir_fd, p.c_str(), flags, mask, &stx);
	if (rc < 0) {
		return std::unexpected{
			FileIoSyncError{errno, "file_io_sync: statx"}
        };
	}
	return FileStat{
		.size = stx.stx_size,
		.mtime_ns = static_cast<std::uint64_t>(stx.stx_mtime.tv_sec) * 1000000000ULL + stx.stx_mtime.tv_nsec,
		.ctime_ns = static_cast<std::uint64_t>(stx.stx_ctime.tv_sec) * 1000000000ULL + stx.stx_ctime.tv_nsec,
		.dev = static_cast<std::uint64_t>(stx.stx_dev_major) << 32U | stx.stx_dev_minor,
		.ino = stx.stx_ino,
		.mode = stx.stx_mode};
}
// ───────────────────────────────────────────────────────────────────────
// Low-level: read_all_fd
// ───────────────────────────────────────────────────────────────────────

export std::expected<std::string, FileIoSyncError> read_all_fd(
	int fd,
	std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
	auto st = blocking_fstat(fd);
	if (!st) {
		return std::unexpected{st.error()};
	}
	if (st->size > max_bytes) {
		return std::unexpected{
			FileIoSyncError{EFBIG, "file_io_sync: file exceeds read limit"}
        };
	}

	std::string out;
	if (st->size > 0 && st->size <= out.max_size()) {
		out.reserve(static_cast<std::size_t>(st->size));
	}

	std::array<char, 16 * 1024> buf{};
	for (;;) {
		auto const n = ::read(fd, buf.data(), buf.size());
		if (n == 0) {
			return out;
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return std::unexpected{
				FileIoSyncError{errno, "file_io_sync: read"}
            };
		}
		auto const count = static_cast<std::size_t>(n);
		if (count > max_bytes - out.size()) {
			return std::unexpected{
				FileIoSyncError{EFBIG, "file_io_sync: file exceeds read limit"}
            };
		}
		out.append(buf.data(), count);
	}
}
// ───────────────────────────────────────────────────────────────────────
// High-level: blocking_read_text_file / read_text_file_nothrow
// ───────────────────────────────────────────────────────────────────────

export std::expected<std::string, FileIoSyncError> blocking_read_text_file(
	std::string_view path,
	std::size_t max_bytes = std::size_t{16} * 1024 * 1024) {
	auto file = blocking_open_file(path, O_RDONLY);
	if (!file) {
		return std::unexpected{file.error()};
	}
	auto bytes = read_all_fd(file->fd(), max_bytes);
	if (!bytes) {
		return std::unexpected{bytes.error()};
	}
	return std::string{std::move(*bytes)};
}
export std::optional<std::string> read_text_file_nothrow(
	std::string_view path,
	std::size_t max_bytes = std::size_t{16} * 1024 * 1024) noexcept {
	try {
		auto bytes = blocking_read_text_file(path, max_bytes);
		if (!bytes) {
			return std::nullopt;
		}
		return std::string{std::move(*bytes)};
	} catch (...) { return std::nullopt; }
}

// ───────────────────────────────────────────────────────────────────────
// High-level: blocking_read_file_at
// ───────────────────────────────────────────────────────────────────────

export std::expected<std::string, FileIoSyncError> blocking_read_file_at(
	int root_fd,
	std::string_view contained_relative_path,
	std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
	auto parts = split_contained_path(contained_relative_path);
	if (!parts) {
		return std::unexpected{parts.error()};
	}

	auto file = blocking_openat_contained(root_fd, contained_relative_path, O_RDONLY);
	if (!file) {
		return std::unexpected{file.error()};
	}
	return read_all_fd(file->fd(), max_bytes);
}

export inline std::expected<void, FileIoSyncError> blocking_write_all_fd(
	int fd,
	std::span<std::byte const> bytes) noexcept {
	return write_all_fd(fd, bytes);
}

export inline std::expected<std::string, FileIoSyncError> blocking_read_all_fd(
	int fd,
	std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
	return read_all_fd(fd, max_bytes);
}

export inline std::optional<std::string> blocking_read_text_file_nothrow(
	std::string_view path,
	std::size_t max_bytes = std::size_t{16} * 1024 * 1024) noexcept {
	return read_text_file_nothrow(path, max_bytes);
}

} // namespace conflux::file_io_sync
