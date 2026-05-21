module;
#include <cstdio>
#include <fcntl.h>
#include <liburing.h>
#include <linux/futex.h>
#include <linux/openat2.h>
#include <linux/stat.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

export module conflux.file_io.reader;

import std;
import conflux.types;
import std.compat;
import conflux.work;
import conflux.uring;
export import conflux.uring.completion;
export import conflux.uring.handle;
import conflux.uring.timeout;
export import conflux.file_io_sync;
export import conflux.file_io.buffers;
export import conflux.file_io.pipe_pool;

export using FileIoError = IoError;

namespace root = conflux::work::root;
namespace {

std::atomic<std::uint64_t> g_async_staging_counter{0};
inline std::string make_staging_name_async() {
	auto const pid = static_cast<std::uint32_t>(::getpid());
	auto const seq = g_async_staging_counter.fetch_add(1, std::memory_order_relaxed);
	std::uint32_t rnd{};
	auto _ = ::getrandom(&rnd, sizeof(rnd), 0);
	return std::format(".conflux.tmp.{}.{}.{:08x}", pid, seq, rnd);
}
constexpr bool is_otmpfile_unsupported_errno_async(
	int e) noexcept {
	return e == EOPNOTSUPP || e == EISDIR || e == EINVAL || e == ENOSYS || e == EPERM;
}

} // namespace
struct AsyncAtomicPathParts {
	std::string parent_dir;
	std::string basename;
};
[[nodiscard]] std::expected<AsyncAtomicPathParts, FileIoError> split_contained_atomic_path_async(
	std::string_view path) noexcept {
	if (path.empty()) {
		return std::unexpected{
			FileIoError{EINVAL, "file_io: empty atomic-write path"}
        };
	}
	if (path.starts_with('/')) {
		return std::unexpected{
			FileIoError{EINVAL, "file_io: absolute atomic-write path"}
        };
	}
	if (path.contains('\0')) {
		return std::unexpected{
			FileIoError{EINVAL, "file_io: NUL in atomic-write path"}
        };
	}
	if (path == "." || path == ".." || path.ends_with('/')) {
		return std::unexpected{
			FileIoError{EINVAL, "file_io: invalid atomic-write path"}
        };
	}

	std::string_view remaining = path;
	while (!remaining.empty()) {
		auto const slash = remaining.find('/');
		auto const component = remaining.substr(0, slash);
		if (component.empty() || component == "..") {
			return std::unexpected{
				FileIoError{EINVAL, "file_io: invalid atomic-write path component"}
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
// ---------------------------------------------------------------------------
// FileReader: all async file operations. The caller provides (a) the ring to
// submit on, (b) the CompletionTable that owns library slots, (c) an encoder
// that packs (slot, gen) into the full 64-bit user_data — the caller is free
// to multiplex several subsystems through its own opcode space.
//
// None of these methods call io_uring_submit(). The caller's run_loop is
// responsible for flushing SQEs. On SQ-full, the returned Flow rejects
// immediately with ENOSPC.
// ---------------------------------------------------------------------------

export class FileReader {
	io_uring *ring_;
	CompletionTable *completions_;
	UserDataFn encode_ud_;
	[[nodiscard]] static int fd_for_io(
		FileHandle const &fh) noexcept {
		return fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
	}
	static void set_fixed_file_if_direct(
		io_uring_sqe *sqe,
		FileHandle const &fh) noexcept {
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
	}
	template<typename T>
	struct PreparedSqe {
		root::Task<T> task;
		std::shared_ptr<root::TaskSource<T>> src;
		io_uring_sqe *sqe{};
	};
	template<typename T>
	struct PreparedSqeDirect {
		root::Task<T> task;
		root::TaskSource<T> src;
		io_uring_sqe *sqe{};
	};
	template<typename T>
	[[nodiscard]] PreparedSqe<T> prepare_sqe() const {
		auto [task, raw_src] = root::make_task_source<T>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = std::make_shared<root::TaskSource<T>>(std::move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(std::make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
		}
		return PreparedSqe<T>{.task = std::move(task), .src = std::move(shared_src), .sqe = sqe};
	}
	template<typename T>
	[[nodiscard]] PreparedSqeDirect<T> prepare_sqe_direct() const {
		auto [task, src] = root::make_task_source<T>(root::SubmitOptions{.enable_cancellation = false});
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = src.try_set_exception(std::make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
		}
		return PreparedSqeDirect<T>{.task = std::move(task), .src = std::move(src), .sqe = sqe};
	}
	// Reserve a completion slot with a callback that bridges an IoResult into
	// a root::TaskSource<T>. `decode` turns a non-negative res into a T; negative
	// res flows through as FileIoError automatically.
	template<typename T, typename Decode>
	std::pair<std::uint32_t, std::uint32_t> reserve_bridge(
		std::shared_ptr<root::TaskSource<T>> const &src,
		Decode &&decode) {
		return completions_->reserve([src, decode = std::forward<Decode>(decode)](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = src->try_set_exception(std::make_exception_ptr(FileIoError{-r.res, "file_io: cqe error"}));
					return;
				}
				if constexpr (std::is_void_v<T>) {
					decode(r);
					auto _ = src->try_set_value(root::Success<void>{});
				} else {
					auto _ = src->try_set_value(root::Success<T>{decode(r)});
				}
			} catch (...) { auto _ = src->try_set_exception(std::current_exception()); }
		});
	}
	template<typename T, typename Decode>
	std::pair<std::uint32_t, std::uint32_t> reserve_bridge_direct(
		root::TaskSource<T> src,
		Decode &&decode) {
		return completions_->reserve([src = std::move(src), decode = std::forward<Decode>(decode)](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = src.try_set_exception(std::make_exception_ptr(FileIoError{-r.res, "file_io: cqe error"}));
					return;
				}
				if constexpr (std::is_void_v<T>) {
					decode(r);
					auto _ = src.try_set_value(root::Success<void>{});
				} else {
					auto _ = src.try_set_value(root::Success<T>{decode(r)});
				}
			} catch (...) { auto _ = src.try_set_exception(std::current_exception()); }
		});
	}
	template<typename T, typename Decode>
	std::pair<std::uint32_t, std::uint32_t> reserve_zc_bridge(
		std::shared_ptr<root::TaskSource<T>> const &src,
		Decode &&decode) {
		return completions_->reserve_zc([src, decode = std::forward<Decode>(decode)](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = src->try_set_exception(std::make_exception_ptr(FileIoError{-r.res, "file_io: cqe error"}));
					return;
				}
				if constexpr (std::is_void_v<T>) {
					decode(r);
					auto _ = src->try_set_value(root::Success<void>{});
				} else {
					auto _ = src->try_set_value(root::Success<T>{decode(r)});
				}
			} catch (...) { auto _ = src->try_set_exception(std::current_exception()); }
		});
	}

public:
	FileReader(
		io_uring *ring,
		CompletionTable *completions,
		UserDataFn encoder)
		: ring_{ring}
		, completions_{completions}
		, encode_ud_{std::move(encoder)} {}
	FileReader(FileReader const &) = delete;
	FileReader &operator =(FileReader const &) = delete;
	FileReader(FileReader &&) = delete;
	FileReader &operator =(FileReader &&) = delete;
	~FileReader() {} // NOLINT(modernize-use-equals-default) — GCC module bug
	[[nodiscard]] io_uring *ring() const noexcept { return ring_; }
	[[nodiscard]] CompletionTable *completions() const noexcept { return completions_; }
	[[nodiscard]] std::uint64_t encode_ud(
		std::uint32_t slot,
		std::uint32_t gen) const {
		return encode_ud_(slot, gen);
	}
	[[nodiscard]] bool poll_add_multi(
		int fd,
		short poll_mask,
		CompletionFn on_event) {
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			return false;
		}
		io_uring_prep_poll_multishot(sqe, fd, static_cast<unsigned>(poll_mask));
		auto [slot, gen] = completions_->reserve_multishot(std::move(on_event));
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return true;
	}
	[[nodiscard]] bool poll_add_oneshot(
		int fd,
		short poll_mask,
		CompletionFn on_event) {
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			return false;
		}
		io_uring_prep_poll_add(sqe, fd, static_cast<unsigned>(poll_mask));
		auto [slot, gen] = completions_->reserve(std::move(on_event));
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return true;
	}

private:
	[[nodiscard]] bool submit_open_direct_fallback(
		std::shared_ptr<root::TaskSource<FileHandle>> const &src,
		std::shared_ptr<std::string> const &path_owner,
		int dir_fd,
		int flags,
		mode_t mode,
		unsigned file_index) {
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = src->try_set_exception(std::make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return false;
		}
		io_uring_prep_openat(sqe, dir_fd, path_owner->c_str(), flags, mode);
		auto [slot, gen] = completions_->reserve([this, src, path_owner, file_index](IoResult r) mutable {
			auto _ = path_owner; // keep-alive until CQE
			try {
				if (r.res < 0) {
					auto _ =
						src->try_set_exception(std::make_exception_ptr(FileIoError{-r.res, "file_io: open_direct"}));
					return;
				}
				int const fd = r.res;
				int const update_rc = ::io_uring_register_files_update(ring_, file_index, &fd, 1);
				::close(fd);
				if (update_rc < 0) {
					int const sparse = -1;
					::io_uring_register_files_update(ring_, file_index, &sparse, 1);
					auto _ = src->try_set_exception(
						std::make_exception_ptr(FileIoError{-update_rc, "file_io: open_direct"}));
					return;
				}
				auto _ = src->try_set_value(
					root::Success<FileHandle>{FileHandle::from_direct_slot(static_cast<int>(file_index))});
			} catch (...) { auto _ = src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return true;
	}

public:
	// Open a path relative to dir_fd. Result is a raw-fd FileHandle.
	// Pass AT_FDCWD for absolute paths / cwd-relative.
	// `path` must be a null-terminated std::string owned by the caller until the
	// CQE fires; if unsure, pass a std::string and we copy.
	[[nodiscard]] root::Task<FileHandle> async_open(
		int dir_fd,
		std::string path,
		int flags,
		mode_t mode = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto path_owner = std::make_shared<std::string>(std::move(path));
		io_uring_prep_openat(sqe, dir_fd, path_owner->c_str(), flags, mode);
		auto [slot, gen] = completions_->reserve([shared_src, path_owner](IoResult r) mutable {
			auto _ = path_owner; // keep-alive until CQE
			try {
				if (r.res < 0) {
					auto _ =
						shared_src->try_set_exception(std::make_exception_ptr(FileIoError{-r.res, "file_io: open"}));
					return;
				}
				auto _ = shared_src->try_set_value({FileHandle::from_fd(r.res)});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Open a path directly into the ring's fixed-file table. The owner must
	// have registered a sparse file table first.
	[[nodiscard]] root::Task<FileHandle> async_open_direct(
		int dir_fd,
		std::string path,
		int flags,
		mode_t mode,
		unsigned file_index) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto path_owner = std::make_shared<std::string>(std::move(path));
		io_uring_prep_openat_direct(sqe, dir_fd, path_owner->c_str(), flags, mode, file_index);
		auto [slot, gen] =
			completions_->reserve([this, shared_src, path_owner, dir_fd, flags, mode, file_index](IoResult r) mutable {
				auto _ = path_owner; // keep-alive until CQE
				try {
					if (r.res < 0) {
						int const err = -r.res;
						if (err == EINVAL || err == EOPNOTSUPP || err == ENOSYS) {
							auto _ =
								submit_open_direct_fallback(shared_src, path_owner, dir_fd, flags, mode, file_index);
							return;
						}
						auto _ = shared_src->try_set_exception(
							std::make_exception_ptr(FileIoError{-r.res, "file_io: open_direct"}));
						return;
					}
					auto _ = shared_src->try_set_value(
						{FileHandle::from_direct_slot(r.res == 0 ? static_cast<int>(file_index) : r.res)});
				} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
			});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// statx on a path. `mask` follows statx(2) — STATX_BASIC_STATS by default.
	[[nodiscard]] root::Task<FileStat> async_statx(
		int dir_fd,
		std::string path,
		int flags = 0,
		unsigned mask = STATX_BASIC_STATS,
		bool fixed_file = false) {
		auto [task, shared_src, sqe] = prepare_sqe<FileStat>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto path_owner = std::make_shared<std::string>(std::move(path));
		auto stx_owner = std::make_shared<struct statx>();
		io_uring_prep_statx(sqe, dir_fd, path_owner->c_str(), flags, mask, stx_owner.get());
		if (fixed_file) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = completions_->reserve([shared_src, path_owner, stx_owner](IoResult r) mutable {
			auto _ = path_owner;
			try {
				if (r.res < 0) {
					auto _ =
						shared_src->try_set_exception(std::make_exception_ptr(FileIoError{-r.res, "file_io: statx"}));
					return;
				}
				auto const &s = *stx_owner;
				FileStat const out{
					.size = s.stx_size,
					.mtime_ns = static_cast<std::uint64_t>(s.stx_mtime.tv_sec) * 1000000000ULL + s.stx_mtime.tv_nsec,
					.ctime_ns = static_cast<std::uint64_t>(s.stx_ctime.tv_sec) * 1000000000ULL + s.stx_ctime.tv_nsec,
					.dev = (static_cast<std::uint64_t>(s.stx_dev_major) << 32U) | s.stx_dev_minor,
					.ino = s.stx_ino,
					.mode = s.stx_mode};
				auto _ = shared_src->try_set_value({out});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// fstat-equivalent via statx with AT_EMPTY_PATH — avoids a path lookup.
	[[nodiscard]] root::Task<FileStat> async_stat(
		FileHandle const &fh) {
		if (fh.is_direct()) {
			return async_statx(fh.direct_slot(), std::string{}, AT_EMPTY_PATH, STATX_BASIC_STATS, true);
		}
		return async_statx(fh.raw_fd(), std::string{}, AT_EMPTY_PATH);
	}
	// Read into a caller-owned std::span. The caller must keep `dst` alive until the
	// Flow resolves.
	[[nodiscard]] root::Task<std::size_t> read_into(
		FileHandle const &fh,
		std::uint64_t offset,
		std::span<std::byte> dst) {
		auto [task, src, sqe] = prepare_sqe_direct<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_read(sqe, fd_for_io(fh), dst.data(), static_cast<unsigned>(dst.size()), offset);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge_direct<std::size_t>(std::move(src), [](IoResult r) {
			return static_cast<std::size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Scatter-gather read: fills `iovecs` segments in sequence. The V is
	// moved into shared state and kept alive until the CQE fires.
	// Returns total bytes read across all segments.
	[[nodiscard]] root::Task<std::size_t> readv_into(
		FileHandle const &fh,
		std::uint64_t offset,
		std::vector<iovec> iovecs) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto iov_owner = std::make_shared<std::vector<iovec>>(std::move(iovecs));
		io_uring_prep_readv(sqe, fd_for_io(fh), iov_owner->data(), static_cast<unsigned>(iov_owner->size()), offset);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [iov_owner](IoResult r) mutable {
			auto _ = iov_owner; // keep-alive until CQE
			return static_cast<std::size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Read into a pre-registered fixed buffer. The pool slot is held by the
	// buffer and returned on destruction — by placing the buffer inside the
	// shared-state closure we keep it alive until the CQE fires, then move it
	// into the resolved value so the caller decides when to release.
	struct ReadFixedResult {
		FixedBuffer buffer;
		std::size_t bytes{};
	};
	[[nodiscard]] root::Task<ReadFixedResult> read_fixed(
		FileHandle const &fh,
		std::uint64_t offset,
		FixedBuffer buf,
		std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
		auto [task, shared_src, sqe] = prepare_sqe<ReadFixedResult>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		unsigned const slot_idx = buf.slot();
		auto holder = std::make_shared<FixedBuffer>(std::move(buf));
		std::size_t const bytes = std::min(holder->view().size(), max_bytes);
		io_uring_prep_read_fixed(
			sqe,
			fd_for_io(fh),
			holder->view().data(),
			static_cast<unsigned>(bytes),
			offset,
			static_cast<int>(slot_idx));
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = completions_->reserve([shared_src, holder](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(FileIoError{-r.res, "file_io: read_fixed"}));
					return;
				}
				auto _ = shared_src->try_set_value({
					ReadFixedResult{.buffer = std::move(*holder), .bytes = static_cast<std::size_t>(r.res)}
                });
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Read into a pre-registered fixed buffer, bypassing the kernel page cache.
	// The file must have been opened with O_DIRECT. `offset` must be a multiple
	// of `block_size`. `max_bytes` is the caller's true limit (e.g. remaining
	// file bytes); it is rounded up to the nearest `block_size` multiple before
	// submission to satisfy O_DIRECT alignment. The resolved std::byte count is
	// capped back to the original `max_bytes`, trimming any alignment padding.
	// If the underlying filesystem does not support O_DIRECT, the kernel returns
	// EINVAL, which surfaces as FileIoError{EINVAL, ...}.
	[[nodiscard]] root::Task<ReadFixedResult> read_nocache_fixed(
		FileHandle const &fh,
		std::uint64_t offset,
		FixedBuffer buf,
		std::size_t max_bytes = std::numeric_limits<std::size_t>::max(),
		std::size_t block_size = 4096) {
		std::size_t const actual_cap = std::min(max_bytes, buf.size());
		std::size_t aligned_bytes = actual_cap;
		if (block_size > 1 && actual_cap > 0) {
			aligned_bytes = ((actual_cap + block_size - 1) / block_size) * block_size;
			aligned_bytes = std::min(aligned_bytes, buf.size());
		}
		auto [task, shared_src, sqe] = prepare_sqe<ReadFixedResult>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		unsigned const slot_idx = buf.slot();
		auto holder = std::make_shared<FixedBuffer>(std::move(buf));
		io_uring_prep_read_fixed(
			sqe,
			fd_for_io(fh),
			holder->view().data(),
			static_cast<unsigned>(aligned_bytes),
			offset,
			static_cast<int>(slot_idx));
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = completions_->reserve([shared_src, holder, actual_cap](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(FileIoError{-r.res, "file_io: read_nocache_fixed"}));
					return;
				}
				std::size_t const bytes = std::min(static_cast<std::size_t>(r.res), actual_cap);
				auto _ = shared_src->try_set_value({
					ReadFixedResult{.buffer = std::move(*holder), .bytes = bytes}
                });
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Write from a pre-registered fixed buffer. Symmetric to read_fixed.
	// The buffer is held in shared state until the CQE fires, then returned
	// to the caller (who decides when to release the slot back to the pool).
	struct WriteFixedResult {
		FixedBuffer buffer;
		std::size_t bytes{};
	};
	[[nodiscard]] root::Task<WriteFixedResult> write_fixed(
		FileHandle const &fh,
		std::uint64_t offset,
		FixedBuffer buf,
		std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
		auto [task, shared_src, sqe] = prepare_sqe<WriteFixedResult>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		unsigned const slot_idx = buf.slot();
		auto holder = std::make_shared<FixedBuffer>(std::move(buf));
		std::size_t const bytes = std::min(holder->view().size(), max_bytes);
		io_uring_prep_write_fixed(
			sqe,
			fd_for_io(fh),
			holder->view().data(),
			static_cast<unsigned>(bytes),
			offset,
			static_cast<int>(slot_idx));
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = completions_->reserve([shared_src, holder](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(FileIoError{-r.res, "file_io: write_fixed"}));
					return;
				}
				auto _ = shared_src->try_set_value({
					WriteFixedResult{.buffer = std::move(*holder), .bytes = static_cast<std::size_t>(r.res)}
                });
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<std::size_t> write_into(
		FileHandle const &fh,
		std::uint64_t offset,
		std::span<std::byte const> src_view) {
		auto [task, src, sqe] = prepare_sqe_direct<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_write(sqe, fd_for_io(fh), src_view.data(), static_cast<unsigned>(src_view.size()), offset);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge_direct<std::size_t>(std::move(src), [](IoResult r) {
			return static_cast<std::size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Scatter-gather write: sends `iovecs` segments to the file in sequence.
	// The V is moved into shared state and kept alive until the CQE fires.
	// Returns total bytes written across all segments.
	[[nodiscard]] root::Task<std::size_t> writev_into(
		FileHandle const &fh,
		std::uint64_t offset,
		std::vector<iovec> iovecs) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto iov_owner = std::make_shared<std::vector<iovec>>(std::move(iovecs));
		io_uring_prep_writev(sqe, fd_for_io(fh), iov_owner->data(), static_cast<unsigned>(iov_owner->size()), offset);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [iov_owner](IoResult r) mutable {
			auto _ = iov_owner; // keep-alive until CQE
			return static_cast<std::size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// readv2_into: like readv_into but with RWF flags (e.g. RWF_NOWAIT, RWF_DSYNC).
	[[nodiscard]] root::Task<std::size_t> readv2_into(
		FileHandle const &fh,
		std::uint64_t offset,
		std::vector<iovec> iovecs,
		int rwf_flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto iov_owner = std::make_shared<std::vector<iovec>>(std::move(iovecs));
		io_uring_prep_readv2(
			sqe,
			fd_for_io(fh),
			iov_owner->data(),
			static_cast<unsigned>(iov_owner->size()),
			offset,
			rwf_flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [iov_owner](IoResult r) mutable {
			auto _ = iov_owner;
			return static_cast<std::size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// writev2_into: like writev_into but with RWF flags.
	[[nodiscard]] root::Task<std::size_t> writev2_into(
		FileHandle const &fh,
		std::uint64_t offset,
		std::vector<iovec> iovecs,
		int rwf_flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto iov_owner = std::make_shared<std::vector<iovec>>(std::move(iovecs));
		io_uring_prep_writev2(
			sqe,
			fd_for_io(fh),
			iov_owner->data(),
			static_cast<unsigned>(iov_owner->size()),
			offset,
			rwf_flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [iov_owner](IoResult r) mutable {
			auto _ = iov_owner;
			return static_cast<std::size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// No-op SQE — useful for latency measurement, wakeup, or pipeline flushing.
	[[nodiscard]] root::Task<void> async_nop() {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_nop(sqe);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_fsync(
		FileHandle const &fh,
		bool data_only = false) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_fsync(sqe, fh.raw_fd(), data_only ? IORING_FSYNC_DATASYNC : 0U);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_fallocate(
		FileHandle const &fh,
		int mode,
		std::uint64_t offset,
		std::uint64_t len) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_fallocate(sqe, fh.raw_fd(), mode, offset, len);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Consumes the handle; the ring closes the fd via io_uring.
	[[nodiscard]] root::Task<void> async_fadvise(
		FileHandle const &fh,
		std::uint64_t offset,
		std::uint32_t len,
		int advice) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_fadvise(sqe, fd, offset, len, advice);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_madvise(
		void *addr,
		std::uint32_t length,
		int advice) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_madvise(sqe, addr, length, advice);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_unlink(
		int dir_fd,
		std::string path,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto path_owner = std::make_shared<std::string>(std::move(path));
		io_uring_prep_unlinkat(sqe, dir_fd, path_owner->c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [path_owner](IoResult) mutable { auto _ = path_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_rename(
		int old_dir_fd,
		std::string old_path,
		int new_dir_fd,
		std::string new_path,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto paths = std::make_shared<std::pair<std::string, std::string>>(std::move(old_path), std::move(new_path));
		io_uring_prep_renameat(sqe, old_dir_fd, paths->first.c_str(), new_dir_fd, paths->second.c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable { auto _ = paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_mkdirat(
		int dir_fd,
		std::string path,
		mode_t mode = 0755) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto path_owner = std::make_shared<std::string>(std::move(path));
		io_uring_prep_mkdirat(sqe, dir_fd, path_owner->c_str(), mode);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [path_owner](IoResult) mutable { auto _ = path_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_symlinkat(
		std::string target,
		int new_dir_fd,
		std::string link_path) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto paths = std::make_shared<std::pair<std::string, std::string>>(std::move(target), std::move(link_path));
		io_uring_prep_symlinkat(sqe, paths->first.c_str(), new_dir_fd, paths->second.c_str());
		auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable { auto _ = paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_ftruncate(
		FileHandle const &fh,
		std::uint64_t length) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_ftruncate(sqe, fd, static_cast<loff_t>(length));
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_linkat(
		int old_dir_fd,
		std::string old_path,
		int new_dir_fd,
		std::string new_path,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto paths = std::make_shared<std::pair<std::string, std::string>>(std::move(old_path), std::move(new_path));
		io_uring_prep_linkat(sqe, old_dir_fd, paths->first.c_str(), new_dir_fd, paths->second.c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable { auto _ = paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_sync_file_range(
		FileHandle const &fh,
		std::uint64_t offset,
		unsigned len,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_sync_file_range(sqe, fd, len, offset, flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]] [[nodiscard]] root::Task<FileHandle>
	async_socket(
		int domain,
		int type,
		int protocol) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_socket(sqe, domain, type, protocol, 0);
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]] [[deprecated(
		"use socket_io::tcp_connect/tcp_accept paths instead")]] [[nodiscard]] root::Task<FileHandle>
	async_socket_direct(
		int domain,
		int type,
		int protocol,
		unsigned file_index) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_socket_direct(sqe, domain, type, protocol, file_index, 0);
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [file_index](IoResult) {
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]]
	// Create a pipe asynchronously. Returns (read_fd, write_fd) on success.
	[[nodiscard]] root::Task<std::pair<int, int>> async_pipe(
		int pipe_flags = O_CLOEXEC | O_NONBLOCK) {
		auto [task, shared_src, sqe] = prepare_sqe<std::pair<int, int>>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto fds = std::make_shared<std::array<int, 2>>(std::array<int, 2>{-1, -1});
		io_uring_prep_pipe(sqe, fds->data(), pipe_flags);
		auto [slot, gen] = completions_->reserve([shared_src, fds](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ =
						shared_src->try_set_exception(std::make_exception_ptr(FileIoError{-r.res, "file_io: pipe"}));
					return;
				}
				auto _ = shared_src->try_set_value({std::make_pair((*fds)[0], (*fds)[1])});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Async bind. `addr` is copied and kept alive until CQE.
	[[nodiscard]] root::Task<void> async_bind(
		FileHandle const &fh,
		sockaddr_storage addr,
		socklen_t addrlen) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		auto addr_owner = std::make_shared<sockaddr_storage>(addr);
		io_uring_prep_bind(sqe, fd, reinterpret_cast<sockaddr *>(addr_owner.get()), addrlen);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [addr_owner](IoResult) mutable { auto _ = addr_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Async listen.
	[[nodiscard]] root::Task<void> async_listen(
		FileHandle const &fh,
		int backlog = SOMAXCONN) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_listen(sqe, fd, backlog);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_shutdown(
		FileHandle const &fh,
		int how) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_shutdown(sqe, fd, how);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<std::size_t> async_tee(
		int fd_in,
		int fd_out,
		std::size_t len,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_tee(sqe, fd_in, fd_out, static_cast<unsigned int>(len), flags);
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Installs a direct-slot fd into the process fd table. Returns a raw-fd
	// FileHandle wrapping the installed fd. Caller must hold a registered-files
	// table (io_uring_register_files) on this ring.
	[[nodiscard]] root::Task<FileHandle> async_fixed_fd_install(
		FileHandle const &fh,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = std::make_shared<root::TaskSource<FileHandle>>(std::move(raw_src));
		if (!fh.is_direct()) {
			auto _ = shared_src->try_set_exception(
				std::make_exception_ptr(FileIoError{EINVAL, "file_io: fixed_fd_install requires direct slot"}));
			return std::move(task);
		}
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(std::make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return std::move(task);
		}
		io_uring_prep_fixed_fd_install(sqe, fh.direct_slot(), flags);
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Get extended attribute. `name` is moved and kept alive until CQE.
	// `value` std::span must remain valid until the returned Flow resolves.
	// Returns the actual attribute size (may exceed value.size() — ERANGE).
	[[nodiscard]] root::Task<std::size_t> async_fgetxattr(
		FileHandle const &fh,
		std::string name,
		std::span<char> buf) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		auto name_owner = std::make_shared<std::string>(std::move(name));
		io_uring_prep_fgetxattr(sqe, fd, name_owner->c_str(), buf.data(), static_cast<unsigned>(buf.size()));
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [name_owner](IoResult r) mutable {
			auto _ = name_owner;
			return static_cast<std::size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Set extended attribute. Both `name` and `data` are moved/kept alive until CQE.
	[[nodiscard]] root::Task<void> async_fsetxattr(
		FileHandle const &fh,
		std::string name,
		std::string data,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		auto kv = std::make_shared<std::pair<std::string, std::string>>(std::move(name), std::move(data));
		io_uring_prep_fsetxattr(
			sqe,
			fd,
			kv->first.c_str(),
			kv->second.c_str(),
			flags,
			static_cast<unsigned>(kv->second.size()));
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [kv](IoResult) mutable { auto _ = kv; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Path-based get extended attribute. `name`, `path`, and `buf` must
	// remain valid until the Flow resolves.
	[[nodiscard]] root::Task<std::size_t> async_getxattr(
		std::string path,
		std::string name,
		std::span<char> buf) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto kp = std::make_shared<std::pair<std::string, std::string>>(std::move(path), std::move(name));
		io_uring_prep_getxattr(
			sqe,
			kp->second.c_str(),
			buf.data(),
			kp->first.c_str(),
			static_cast<unsigned>(buf.size()));
		auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [kp](IoResult r) mutable {
			auto _ = kp;
			return static_cast<std::size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Path-based set extended attribute. `name`, `data`, and `path` are moved
	// and kept alive until CQE.
	[[nodiscard]] root::Task<void> async_setxattr(
		std::string path,
		std::string name,
		std::string data,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		struct XattrState {
			std::string path;
			std::string name;
			std::string data;
		};
		auto st = std::make_shared<XattrState>(std::move(path), std::move(name), std::move(data));
		io_uring_prep_setxattr(
			sqe,
			st->name.c_str(),
			st->data.c_str(),
			st->path.c_str(),
			flags,
			static_cast<unsigned>(st->data.size()));
		auto [slot, gen] = reserve_bridge<void>(shared_src, [st](IoResult) mutable { auto _ = st; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Wait for process state change (IORING_OP_WAITID). `infop` must stay valid
	// until the Flow resolves; on success it is filled with signal info.
	[[nodiscard]] root::Task<void> async_waitid(
		idtype_t idtype,
		id_t id,
		siginfo_t *infop,
		int options = WEXITED,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_waitid(sqe, idtype, id, infop, options, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Futex wait — waits until *futex != val. The futex pointer must remain
	// valid until the Flow resolves. Returns void on wakeup.
	[[nodiscard]] root::Task<void> async_futex_wait(
		std::uint32_t *futex,
		std::uint64_t val,
		std::uint64_t mask = FUTEX_BITSET_MATCH_ANY,
		std::uint32_t futex_flags = FUTEX2_SIZE_U32,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_futex_wait(sqe, futex, val, mask, futex_flags, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Futex wake — wakes up to `val` waiters. Returns the number woken.
	[[nodiscard]] root::Task<std::uint32_t> async_futex_wake(
		std::uint32_t *futex,
		std::uint64_t val,
		std::uint64_t mask = FUTEX_BITSET_MATCH_ANY,
		std::uint32_t futex_flags = FUTEX2_SIZE_U32,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::uint32_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_futex_wake(sqe, futex, val, mask, futex_flags, flags);
		auto [slot, gen] =
			reserve_bridge<std::uint32_t>(shared_src, [](IoResult r) { return static_cast<std::uint32_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Send a synthetic CQE to `target_ring_fd` (the target ring's ring_fd).
	// The CQE on the target will have res=len, user_data=data.
	[[nodiscard]] root::Task<void> async_msg_ring(
		int target_ring_fd,
		unsigned len,
		std::uint64_t data,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_msg_ring(sqe, target_ring_fd, len, data, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Arm a one-shot timeout. Resolves (with -ETIME mapped to void) when ms
	// elapses. If count > 0, fires after count CQE completions OR ms, whichever
	// is first (IORING_TIMEOUT_BOOTTIME etc. can be passed in flags).
	[[nodiscard]] root::Task<void> async_timeout(
		std::chrono::milliseconds ms,
		unsigned count = 0,
		unsigned flags = 0) {
		return conflux::uring::async_timeout(
			ring_,
			*completions_,
			[this](std::uint32_t slot, std::uint32_t gen) noexcept { return encode_ud_(slot, gen); },
			ms,
			count,
			flags);
	}
	// Cancel a running timeout by its user_data tag. -ENOENT → already fired.
	[[nodiscard]] root::Task<void> async_timeout_remove(
		std::uint64_t user_data,
		unsigned flags = 0) {
		return conflux::uring::async_timeout_remove(
			ring_,
			*completions_,
			[this](std::uint32_t slot, std::uint32_t gen) noexcept { return encode_ud_(slot, gen); },
			user_data,
			flags);
	}
	// Update an armed timeout. New deadline `ms` replaces the existing one.
	// `user_data` identifies the timeout SQE to update (its encoded user_data).
	[[nodiscard]] root::Task<void> async_timeout_update(
		std::uint64_t user_data,
		std::chrono::milliseconds ms,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto ts = std::make_shared<__kernel_timespec>();
		auto const sec = std::chrono::duration_cast<std::chrono::seconds>(ms);
		ts->tv_sec = sec.count();
		ts->tv_nsec = (ms - sec).count() * 1000000LL;
		io_uring_prep_timeout_update(sqe, ts.get(), user_data, flags);
		auto [slot, gen] = completions_->reserve([shared_src, ts](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT && r.res != -EALREADY) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(FileIoError{-r.res, "file_io: timeout_update"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
			auto _ = ts;
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Register a one-shot poll on `fd` for `events` (POLLIN, POLLOUT, …).
	// Resolves with the triggered poll mask when any event fires.
	// -ENOENT on poll_remove before the event: treated as ECANCELED by caller.
	[[nodiscard]] root::Task<std::uint32_t> async_poll_add(
		int fd,
		std::uint32_t events) {
		auto [task, shared_src, sqe] = prepare_sqe<std::uint32_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_poll_add(sqe, fd, events);
		auto [slot, gen] =
			reserve_bridge<std::uint32_t>(shared_src, [](IoResult r) { return static_cast<std::uint32_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Cancel a pending poll_add identified by `user_data`.
	// -ENOENT means the poll already fired — treated as success.
	[[nodiscard]] root::Task<void> async_poll_remove(
		std::uint64_t user_data) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_poll_remove(sqe, user_data);
		auto [slot, gen] = completions_->reserve([shared_src](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT && r.res != -EALREADY) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(FileIoError{-r.res, "file_io: poll_remove"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Update an armed multishot poll's event mask in-place.
	// `user_data` is the encoded user_data of the original poll SQE.
	[[nodiscard]] root::Task<void> async_poll_update(
		std::uint64_t user_data,
		std::uint32_t new_events,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_poll_update(sqe, user_data, 0, new_events, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Accept one connection on a listening socket. Returns the accepted fd.
	// `addr`/`addrlen` are Opt out-params for the peer address.
	[[nodiscard]] root::Task<FileHandle> async_accept(
		FileHandle const &fh,
		sockaddr *addr = nullptr,
		socklen_t *addrlen = nullptr,
		int flags = SOCK_CLOEXEC) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Accept one connection into a registered direct slot.
	// `addr`/`addrlen` are Opt out-params for the peer address.
	[[nodiscard]] root::Task<FileHandle> async_accept_direct(
		FileHandle const &fh,
		unsigned file_index,
		sockaddr *addr = nullptr,
		socklen_t *addrlen = nullptr,
		int flags = SOCK_CLOEXEC) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_accept_direct(sqe, fd, addr, addrlen, flags, file_index);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [file_index](IoResult) {
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Send an fd to another ring's registered file table. `source_fd` is
	// installed at `target_fd` slot in the target ring's file table.
	// Pass IORING_FILE_INDEX_ALLOC for `target_fd` to auto-allocate.
	[[nodiscard]] root::Task<void> async_msg_ring_fd(
		int target_ring_fd,
		int source_fd,
		int target_fd,
		std::uint64_t data = 0,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_msg_ring_fd(sqe, target_ring_fd, source_fd, target_fd, data, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Wait on multiple futexes simultaneously. Resolves when any waiter
	// condition is met. `waiters` is moved and kept alive until CQE.
	[[nodiscard]] root::Task<void> async_futex_waitv(
		std::vector<futex_waitv> waiters,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto wv = std::make_shared<std::vector<futex_waitv>>(std::move(waiters));
		io_uring_prep_futex_waitv(sqe, wv->data(), static_cast<std::uint32_t>(wv->size()), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [wv](IoResult) mutable { auto _ = wv; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Cancel a pending op by its user_data tag. Resolves when the cancel
	// was submitted; the target op's CQE will still arrive (with -ECANCELED).
	// -ENOENT means the target already completed — treated as success here.
	[[nodiscard]] root::Task<void> async_cancel(
		std::uint64_t user_data,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_cancel64(sqe, user_data, flags);
		auto [slot, gen] = completions_->reserve([shared_src](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT) {
					auto _ =
						shared_src->try_set_exception(std::make_exception_ptr(FileIoError{-r.res, "file_io: cancel"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Cancel all pending ops for `fd`. -ENOENT treated as success.
	[[nodiscard]] root::Task<void> async_cancel_fd(
		int fd,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_cancel_fd(sqe, fd, flags);
		auto [slot, gen] = completions_->reserve([shared_src](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(FileIoError{-r.res, "file_io: cancel_fd"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Async connect. `addr` is copied into a shared buffer kept alive until CQE.
	[[nodiscard]] root::Task<void> async_connect(
		FileHandle const &fh,
		sockaddr_storage addr,
		socklen_t addrlen) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		auto addr_owner = std::make_shared<sockaddr_storage>(addr);
		io_uring_prep_connect(sqe, fd, reinterpret_cast<sockaddr *>(addr_owner.get()), addrlen);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [addr_owner](IoResult) mutable { auto _ = addr_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_close(
		FileHandle fh) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		if (fh.is_direct()) {
			io_uring_prep_close_direct(sqe, static_cast<unsigned>(fh.release_direct_slot()));
		} else {
			io_uring_prep_close(sqe, fh.release_fd());
		}
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Splice `len` bytes from `file` at `off` into `dst_fd`, using `pipe` as
	// the intermediate kernel buffer. `SPLICE_F_MOVE | SPLICE_F_MORE` — bytes
	// stay in the page cache; no user-space copy.
	//
	// Issues two linked SQEs at a time (file→pipe, pipe→dst) chunked to the
	// pipe capacity. If the first splice returns short or the chain breaks,
	// the shared state tracks remaining bytes and resubmits.
	//
	// The PipePair is held until `len` is drained or an error arrives; it is
	// then dropped (returning to the pool).
	[[nodiscard]] root::Task<std::size_t> splice_to_fd(
		FileHandle const &file,
		std::uint64_t off,
		std::size_t len,
		int dst_fd,
		PipePair pipe,
		bool dst_fixed = false) {
		struct State {
			io_uring *ring;
			CompletionTable *completions;
			UserDataFn encode_ud;
			int file_fd;
			int dst_fd;
			bool dst_fixed;
			PipePair pipe;
			std::uint64_t file_off;
			std::size_t remaining;
			std::size_t delivered{0};
			std::shared_ptr<root::TaskSource<std::size_t>> src;
		};
		auto [task, raw_src] = root::make_task_source<std::size_t>(root::SubmitOptions{.enable_cancellation = false});
		auto st = std::make_shared<State>(State{
			.ring = ring_,
			.completions = completions_,
			.encode_ud = encode_ud_,
			.file_fd = file.raw_fd(),
			.dst_fd = dst_fd,
			.dst_fixed = dst_fixed,
			.pipe = std::move(pipe),
			.file_off = off,
			.remaining = len,
			.delivered = 0,
			.src = std::make_shared<root::TaskSource<std::size_t>>(std::move(raw_src))});
		step_splice(st);
		return std::move(task);
	}
	// Send `len` bytes from `buf` on `fh`. Returns bytes sent.
	[[nodiscard]] root::Task<std::size_t> async_send(
		FileHandle const &fh,
		void const *buf,
		std::size_t len,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_send(sqe, fd, buf, len, flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Receive up to `len` bytes into `buf` from `fh`. Returns bytes received.
	[[nodiscard]] root::Task<std::size_t> async_recv(
		FileHandle const &fh,
		void *buf,
		std::size_t len,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_recv(sqe, fd, buf, len, flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Vectored send via sendmsg(2). `msg` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<std::size_t> async_sendmsg(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_sendmsg(sqe, fd, msg, flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Vectored recv via recvmsg(2). `msg` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<std::size_t> async_recvmsg(
		FileHandle const &fh,
		msghdr *msg,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_recvmsg(sqe, fd, msg, flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Register `nr` buffers of `len` bytes starting at `addr` into buffer group `bgid`.
	// Kernel increments `bid` automatically for subsequent provides in the same group.
	[[nodiscard]] root::Task<void> async_provide_buffers(
		void *addr,
		int len,
		int nr,
		int bgid,
		int bid = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_provide_buffers(sqe, addr, len, nr, bgid, bid);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Remove `nr` buffers from buffer group `bgid`.
	[[nodiscard]] root::Task<void> async_remove_buffers(
		int nr,
		int bgid) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_remove_buffers(sqe, nr, bgid);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Update the registered file table. `fds` is a std::span of `nr_fds` fds starting
	// at `offset` in the kernel's registered file A. -1 entries remove a slot.
	// `fds` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<void> async_files_update(
		int *fds,
		unsigned nr_fds,
		int offset = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_files_update(sqe, fds, nr_fds, offset);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Modify an epoll interest list entry. `ev` may be null for EPOLL_CTL_DEL.
	[[nodiscard]] root::Task<void> async_epoll_ctl(
		int epfd,
		int fd,
		int op,
		epoll_event const *ev = nullptr) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_epoll_ctl(sqe, epfd, fd, op, ev);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Wait for epoll events. Resolves with the number of events returned.
	// `events` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<int> async_epoll_wait(
		int epfd,
		epoll_event *events,
		int maxevents,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<int>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_epoll_wait(sqe, epfd, events, maxevents, flags);
		auto [slot, gen] = reserve_bridge<int>(shared_src, [](IoResult r) { return r.res; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Attach a timeout to the preceding SQE in the ring's submission chain.
	// The preceding SQE must have been submitted with IOSQE_IO_LINK.
	// Resolves when the link fires (either the linked op completed or the
	// timeout expired — -ETIME in the latter case is treated as success).
	[[nodiscard]] root::Task<void> async_link_timeout(
		std::chrono::milliseconds ms,
		unsigned flags = 0) {
		return conflux::uring::async_link_timeout(
			ring_,
			*completions_,
			[this](std::uint32_t slot, std::uint32_t gen) noexcept { return encode_ud_(slot, gen); },
			ms,
			flags);
	}
	// Open a file with full openat2(2) semantics (`open_how` struct).
	// `how` is copied internally so the caller need not keep it alive.
	[[nodiscard]] root::Task<FileHandle> async_openat2(
		int dir_fd,
		std::string path,
		open_how how) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto ctx = std::make_shared<std::pair<std::string, open_how>>(std::move(path), how);
		io_uring_prep_openat2(sqe, dir_fd, ctx->first.c_str(), &ctx->second);
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [ctx](IoResult r) mutable {
			auto _ = ctx;
			return FileHandle::from_fd(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Send with destination address — for SOCK_DGRAM sockets.
	// `addr` is copied internally; `buf` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<std::size_t> async_sendto(
		FileHandle const &fh,
		void const *buf,
		std::size_t len,
		int flags,
		sockaddr_storage addr,
		socklen_t addrlen) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto sa = std::make_shared<sockaddr_storage>(addr);
		int const fd = fd_for_io(fh);
		io_uring_prep_sendto(sqe, fd, buf, len, flags, reinterpret_cast<sockaddr *>(sa.get()), addrlen);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [sa](IoResult r) mutable {
			auto _ = sa;
			return static_cast<std::size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// hack: resolves on first send CQE only; this API does not expose buffer-release notification.
	// Caller must guarantee the buffer remains live by other means.
	[[nodiscard]] root::Task<std::size_t> async_unsafe_send_zc_sent(
		FileHandle const &fh,
		void const *buf,
		std::size_t len,
		int flags = 0,
		unsigned zc_flags = 0) {
#ifdef IORING_SEND_ZC_REPORT_USAGE
		zc_flags &= ~IORING_SEND_ZC_REPORT_USAGE;
#endif
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_send_zc(sqe, fd, buf, len, flags, zc_flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<std::size_t> async_send_zc(
		FileHandle const &fh,
		void const *buf,
		std::size_t len,
		int flags = 0,
		unsigned zc_flags = 0) {
#ifdef IORING_SEND_ZC_REPORT_USAGE
		zc_flags &= ~IORING_SEND_ZC_REPORT_USAGE;
#endif
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_send_zc(sqe, fd, buf, len, flags, zc_flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] =
			reserve_zc_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Write using a pre-registered fixed buffer (IORING_OP_WRITE_FIXED).
	// `buf` pointer and `buf_index` must refer to the registered buffer in the pool.
	[[nodiscard]] root::Task<std::size_t> async_write_fixed(
		FileHandle const &fh,
		std::uint64_t offset,
		void const *buf,
		unsigned nbytes,
		int buf_index) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_write_fixed(sqe, fd, buf, nbytes, offset, buf_index);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Remove a file by name relative to `dir_fd`.
	// `flags` = 0 for file; AT_REMOVEDIR for directory.
	[[nodiscard]] root::Task<void> async_unlinkat(
		int dir_fd,
		std::string path,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto p = std::make_shared<std::string>(std::move(path));
		io_uring_prep_unlinkat(sqe, dir_fd, p->c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [p](IoResult) mutable { auto _ = p; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Rename with full dirfd control.
	[[nodiscard]] root::Task<void> async_renameat(
		int old_dir_fd,
		std::string old_path,
		int new_dir_fd,
		std::string new_path,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto paths = std::make_shared<std::pair<std::string, std::string>>(std::move(old_path), std::move(new_path));
		io_uring_prep_renameat(sqe, old_dir_fd, paths->first.c_str(), new_dir_fd, paths->second.c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable { auto _ = paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Create a directory at `path` (relative to AT_FDCWD).
	[[nodiscard]] root::Task<void> async_mkdir(
		std::string path,
		mode_t mode = 0755) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto p = std::make_shared<std::string>(std::move(path));
		io_uring_prep_mkdir(sqe, p->c_str(), mode);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [p](IoResult) mutable { auto _ = p; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Open directly into the registered file table with full openat2 semantics.
	// IORING_FILE_INDEX_ALLOC for `file_index` auto-allocates.
	[[nodiscard]] root::Task<FileHandle> async_openat2_direct(
		int dir_fd,
		std::string path,
		open_how how,
		unsigned file_index) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto ctx = std::make_shared<std::pair<std::string, open_how>>(std::move(path), how);
		io_uring_prep_openat2_direct(sqe, dir_fd, ctx->first.c_str(), &ctx->second, file_index);
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [ctx, file_index](IoResult) mutable {
			auto _ = ctx;
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Create a socket directly into the registered file table, with the kernel
	// choosing the slot (IORING_FILE_INDEX_ALLOC). Returns the allocated slot.
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]] [[nodiscard]] root::Task<FileHandle>
	async_socket_direct_alloc(
		int domain,
		int type,
		int protocol,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_socket_direct_alloc(sqe, domain, type, protocol, flags);
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_direct_slot(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Open a file directly into the fixed file table (openat semantics).
	// Use IORING_FILE_INDEX_ALLOC for `file_index` to let the kernel pick a slot.
	[[nodiscard]] root::Task<FileHandle> async_openat_direct(
		int dir_fd,
		std::string path,
		int flags,
		mode_t mode = 0,
		unsigned file_index = IORING_FILE_INDEX_ALLOC) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto p = std::make_shared<std::string>(std::move(path));
		io_uring_prep_openat_direct(sqe, dir_fd, p->c_str(), flags, mode, file_index);
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [p, file_index](IoResult r) mutable {
			auto _ = p;
			// When IORING_FILE_INDEX_ALLOC: res carries the allocated slot.
			int const s = (file_index == IORING_FILE_INDEX_ALLOC) ? r.res : static_cast<int>(file_index);
			return FileHandle::from_direct_slot(s);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Send a source fd to another ring, letting the kernel auto-allocate the slot.
	// Returns the allocated slot index via the target ring's CQE.
	[[nodiscard]] root::Task<void> async_msg_ring_fd_alloc(
		int target_ring_fd,
		int source_fd,
		std::uint64_t data = 0,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_msg_ring_fd_alloc(sqe, target_ring_fd, source_fd, data, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Create a pipe P directly into fixed file table slots.
	// Use IORING_FILE_INDEX_ALLOC for `file_index` to let the kernel choose.
	// The two slots are allocated consecutively.
	[[nodiscard]] root::Task<std::pair<int, int>> async_pipe_direct(
		unsigned file_index = IORING_FILE_INDEX_ALLOC,
		int pipe_flags = O_CLOEXEC | O_NONBLOCK) {
		auto [task, shared_src, sqe] = prepare_sqe<std::pair<int, int>>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		auto fds = std::make_shared<std::array<int, 2>>(std::array<int, 2>{-1, -1});
		io_uring_prep_pipe_direct(sqe, fds->data(), pipe_flags, file_index);
		auto [slot, gen] = completions_->reserve([shared_src, fds](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(FileIoError{-r.res, "file_io: pipe_direct"}));
					return;
				}
				auto _ = shared_src->try_set_value({std::make_pair((*fds)[0], (*fds)[1])});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Post a message to another ring, forwarding specific CQE flags in the payload.
	// Useful for waking up a consumer ring with custom CQE flags set.
	[[nodiscard]] root::Task<void> async_msg_ring_cqe_flags(
		int target_ring_fd,
		unsigned len,
		std::uint64_t data,
		unsigned flags = 0,
		unsigned cqe_flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		io_uring_prep_msg_ring_cqe_flags(sqe, target_ring_fd, len, data, flags, cqe_flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// hack: resolves on first send CQE only; this API does not expose buffer-release notification.
	// Caller must guarantee msg, iovec array, and all pointed buffers remain live by other means.
	[[nodiscard]] root::Task<std::size_t> async_unsafe_sendmsg_zc_sent(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_sendmsg_zc(sqe, fd, msg, flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// msg, iovec array, and all pointed buffers must remain live until co_return.
	[[nodiscard]] root::Task<std::size_t> async_sendmsg_zc(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (sqe == nullptr) {
			return std::move(task);
		}
		int const fd = fd_for_io(fh);
		io_uring_prep_sendmsg_zc(sqe, fd, msg, flags);
		set_fixed_file_if_direct(sqe, fh);
		auto [slot, gen] =
			reserve_zc_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}

private:
	[[nodiscard]] root::Task<FileHandle> async_open_atomic_parent_dir(
		int root_dir_fd,
		std::string parent_dir) {
		open_how how{};
		how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
		how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
		co_return co_await async_openat2(root_dir_fd, std::move(parent_dir), how);
	}
	[[nodiscard]] root::Task<FileHandle> async_open_atomic_payload(
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
		} catch (FileIoError const &e) {
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
	[[nodiscard]] root::Task<void> async_link_atomic_payload(
		FileHandle const &fh,
		int parent_dir_fd,
		std::string staging_name) {
		try {
			co_await async_linkat(fh.raw_fd(), std::string{}, parent_dir_fd, std::string{staging_name}, AT_EMPTY_PATH);
			co_return;
		} catch (FileIoError const &e) {
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
	template<typename StatePtr>
	static void step_splice(
		StatePtr const &st) {
		if (st->remaining == 0) {
			auto _ = st->src->try_set_value(root::Success<std::size_t>{st->delivered});
			return;
		}
		std::size_t const chunk = std::min(st->remaining, st->pipe.capacity());
		auto *sqe_in = io_uring_get_sqe(st->ring);
		auto *sqe_out = io_uring_get_sqe(st->ring);
		if (sqe_in == nullptr || sqe_out == nullptr) {
			auto _ =
				st->src->try_set_exception(std::make_exception_ptr(FileIoError{ENOSPC, "file_io: splice SQ full"}));
			return;
		}

		io_uring_prep_splice(
			sqe_in,
			st->file_fd,
			static_cast<std::int64_t>(st->file_off),
			st->pipe.write_fd(),
			-1,
			static_cast<unsigned>(chunk),
			SPLICE_F_MOVE | SPLICE_F_MORE);
		sqe_in->flags |= IOSQE_IO_LINK;
		auto [slot_in, gen_in] = st->completions->reserve([st](IoResult r) mutable {
			if (r.res < 0 && r.res != -ECANCELED) {
				auto _ = st->src->try_set_exception(std::make_exception_ptr(FileIoError{-r.res, "file_io: splice in"}));
			}
		});
		io_uring_sqe_set_data64(sqe_in, st->encode_ud(slot_in, gen_in));

		io_uring_prep_splice(
			sqe_out,
			st->pipe.read_fd(),
			-1,
			st->dst_fd,
			-1,
			static_cast<unsigned>(chunk),
			SPLICE_F_MOVE | SPLICE_F_MORE);
		if (st->dst_fixed) {
			sqe_out->flags |= IOSQE_FIXED_FILE;
		}
		auto [slot_out, gen_out] = st->completions->reserve([st](IoResult r) mutable {
			if (r.res < 0) {
				auto _ =
					st->src->try_set_exception(std::make_exception_ptr(FileIoError{-r.res, "file_io: splice out"}));
				return;
			}
			auto const n = static_cast<std::size_t>(r.res);
			st->delivered += n;
			st->file_off += n;
			st->remaining = st->remaining > n ? st->remaining - n : 0;
			step_splice(st);
		});
		io_uring_sqe_set_data64(sqe_out, st->encode_ud(slot_out, gen_out));
	}

public:
	[[nodiscard]] root::Task<void> async_atomic_write(
		int dir_fd,
		std::string rel_path,
		std::span<std::byte const> data,
		mode_t mode = 0644,
		TempPublishMode pub_mode = TempPublishMode::replace_existing,
		TempDurability durability = TempDurability::file_and_directory) {
		auto parts = split_contained_atomic_path_async(rel_path);
		if (!parts) {
			throw parts.error();
		}

		auto parent_fh = co_await async_open_atomic_parent_dir(dir_fd, std::move(parts->parent_dir));
		int const parent_fd = parent_fh.raw_fd();
		std::string staging = make_staging_name_async();
		bool staging_entry_exists = false;

		std::exception_ptr cleanup_error;
		try {
			auto fh = co_await async_open_atomic_payload(parent_fd, std::string{staging}, mode, staging_entry_exists);

			std::size_t off = 0;
			while (off < data.size()) {
				auto wrote = co_await write_into(fh, off, data.subspan(off));
				if (wrote == 0) {
					throw FileIoError{EIO, "file_io: short write"};
				}
				off += wrote;
			}

			if (durability >= TempDurability::file) {
				co_await async_fsync(fh, true);
			}

			if (!staging_entry_exists) {
				co_await async_link_atomic_payload(fh, parent_fd, std::string{staging});
				staging_entry_exists = true;
			}

			if (pub_mode == TempPublishMode::replace_existing) {
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

			if (durability >= TempDurability::file_and_directory) {
				co_await async_fsync(parent_fh);
			}
		} catch (...) { cleanup_error = std::current_exception(); }
		if (staging_entry_exists) {
			try {
				co_await async_unlinkat(parent_fd, std::string{staging});
			} catch (...) {} // NOLINT(bugprone-empty-catch)
		}
		if (cleanup_error) {
			std::rethrow_exception(cleanup_error);
		}
	}
};
