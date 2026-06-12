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

namespace conflux::file_io {

namespace root = conflux::work::root;
using conflux::uring::CompletionFn;
using conflux::uring::CompletionTable;
using conflux::uring::FileHandle;
using conflux::uring::IoResult;
using conflux::uring::OsFd;
using conflux::uring::release_fd_tag;
using conflux::uring::RingFd;
using conflux::uring::UserDataFn;
using conflux::uring::visit_fd;
// ---------------------------------------------------------------------------
// FileReader: all async file operations. The caller provides (a) the ring to
// submit on, (b) the CompletionTable that owns library slots, (c) an encoder
// that packs (slot, gen) into the full 64-bit user_data — the caller is free
// to multiplex several subsystems through its own opcode space.
//
// None of these methods call io_uring_submit(). The caller's run_loop is
// responsible for flushing SQEs. On SQ-full, the returned Task completes
// immediately with ENOSPC.
// ---------------------------------------------------------------------------

export class FileReader {
	io_uring *ring_;
	CompletionTable *completions_;
	UserDataFn encode_ud_;
	template<typename T>
	struct PreparedSqe {
		root::Task<T> task;
		std::shared_ptr<root::TaskSource<T>> src;
		conflux::uring::Sqe sqe;
	};
	template<typename T>
	struct PreparedSqeDirect {
		root::Task<T> task;
		root::TaskSource<T> src;
		conflux::uring::Sqe sqe;
	};
	template<typename T>
	[[nodiscard]] PreparedSqe<T> prepare_sqe() const {
		auto [task, raw_src] = root::make_task_source<T>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = std::make_shared<root::TaskSource<T>>(std::move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (!sqe) {
			auto _ = shared_src->try_set_exception(std::make_exception_ptr(IoError{ENOSPC, "file_io: SQ full"}));
		}
		return PreparedSqe<T>{.task = std::move(task), .src = std::move(shared_src), .sqe = conflux::uring::Sqe{sqe}};
	}
	template<typename T>
	[[nodiscard]] PreparedSqeDirect<T> prepare_sqe_direct() const {
		auto [task, src] = root::make_task_source<T>(root::SubmitOptions{.enable_cancellation = false});
		auto *sqe = io_uring_get_sqe(ring_);
		if (!sqe) {
			auto _ = src.try_set_exception(std::make_exception_ptr(IoError{ENOSPC, "file_io: SQ full"}));
		}
		return PreparedSqeDirect<T>{.task = std::move(task), .src = std::move(src), .sqe = conflux::uring::Sqe{sqe}};
	}
	// Reserve a completion slot with a callback that bridges an IoResult into
	// a root::TaskSource<T>. `decode` turns a non-negative res into a T; negative
	// res flows through as IoError automatically.
	template<typename T, typename Decode>
	std::pair<std::uint32_t, std::uint32_t> reserve_bridge(
		std::shared_ptr<root::TaskSource<T>> const &src,
		Decode &&decode) {
		return completions_->reserve([src, decode = std::forward<Decode>(decode)](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: cqe error"}));
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
					auto _ = src.try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: cqe error"}));
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
					auto _ = src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: cqe error"}));
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
		conflux::uring::Sqe{sqe}.prep_poll_multishot(
			conflux::uring::SqeFd{fd},
			conflux::uring::PollFlags{static_cast<unsigned>(poll_mask)});
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
		conflux::uring::Sqe{sqe}.prep_poll_add(
			conflux::uring::SqeFd{fd},
			conflux::uring::PollFlags{static_cast<unsigned>(poll_mask)});
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
		unsigned file_index);

public:
	// Open a path relative to dir_fd. Result is a raw-fd FileHandle.
	// Pass AT_FDCWD for absolute paths / cwd-relative.
	// `path` must be a null-terminated std::string owned by the caller until the
	// CQE fires; if unsure, pass a std::string and we copy.
	[[nodiscard]] root::Task<FileHandle> async_open(int dir_fd, std::string path, int flags, mode_t mode = 0);
	// Open a path directly into the ring's fixed-file table. The owner must
	// have registered a sparse file table first.
	[[nodiscard]] root::Task<FileHandle>
	async_open_direct(int dir_fd, std::string path, int flags, mode_t mode, unsigned file_index);
	// statx on a path. `mask` follows statx(2) — STATX_BASIC_STATS by default.
	[[nodiscard]] root::Task<conflux::file_io_sync::FileStat> async_statx(
		int dir_fd,
		std::string path,
		int flags = 0,
		unsigned mask = STATX_BASIC_STATS) {
		return async_statx(OsFd::from_os(dir_fd), std::move(path), flags, mask);
	}
	[[nodiscard]] root::Task<conflux::file_io_sync::FileStat> async_statx(
		RingFd auto const &dir_fd,
		std::string path,
		int flags = 0,
		unsigned mask = STATX_BASIC_STATS) {
		auto [task, shared_src, sqe] = prepare_sqe<conflux::file_io_sync::FileStat>();
		if (!sqe) {
			return std::move(task);
		}
		auto path_owner = std::make_shared<std::string>(std::move(path));
		auto stx_owner = std::make_shared<struct statx>();
		sqe.prep_statx(dir_fd, path_owner->c_str(), flags, mask, stx_owner.get());
		auto [slot, gen] = completions_->reserve([shared_src, path_owner, stx_owner](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: statx"}));
					return;
				}
				auto const &s = *stx_owner;
				conflux::file_io_sync::FileStat const out{
					.size = s.stx_size,
					.mtime_ns = static_cast<std::uint64_t>(s.stx_mtime.tv_sec) * 1000000000ULL + s.stx_mtime.tv_nsec,
					.ctime_ns = static_cast<std::uint64_t>(s.stx_ctime.tv_sec) * 1000000000ULL + s.stx_ctime.tv_nsec,
					.dev = (static_cast<std::uint64_t>(s.stx_dev_major) << 32U) | s.stx_dev_minor,
					.ino = s.stx_ino,
					.mode = s.stx_mode};
				auto _ = shared_src->try_set_value({out});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// fstat-equivalent via statx with AT_EMPTY_PATH — avoids a path lookup.
	[[nodiscard]] root::Task<conflux::file_io_sync::FileStat> async_stat(
		FileHandle const &fh) {
		return visit_fd(fh, [this](RingFd auto fd) { return async_statx(fd, std::string{}, AT_EMPTY_PATH); });
	}
	// Read into a caller-owned std::span. The caller must keep `dst` alive until the
	// Flow resolves.
	[[nodiscard]] root::JoinTask<std::size_t>
	read_into(FileHandle const &fh, std::uint64_t offset, std::span<std::byte> dst);
	// Scatter-gather read: fills `iovecs` segments in sequence. The V is
	// moved into shared state and kept alive until the CQE fires.
	// Returns total bytes read across all segments.
	[[nodiscard]] root::Task<std::size_t>
	readv_into(FileHandle const &fh, std::uint64_t offset, std::vector<iovec> iovecs);
	// Read into a pre-registered fixed buffer. The pool slot is held by the
	// buffer and returned on destruction — by placing the buffer inside the
	// completion callback we keep it alive until the CQE fires, then move it
	// into the resolved value so the caller decides when to release.
	struct ReadFixedResult {
		FixedBuffer buffer;
		std::size_t bytes{};
	};
	[[nodiscard]] root::Task<ReadFixedResult> read_fixed(
		FileHandle const &fh,
		std::uint64_t offset,
		FixedBuffer buf,
		std::size_t max_bytes = std::numeric_limits<std::size_t>::max());
	// Read into a pre-registered fixed buffer, bypassing the kernel page cache.
	// The file must have been opened with O_DIRECT. `offset` must be a multiple
	// of `block_size`. `max_bytes` is the caller's true limit (e.g. remaining
	// file bytes); it is rounded up to the nearest `block_size` multiple before
	// submission to satisfy O_DIRECT alignment. The resolved std::byte count is
	// capped back to the original `max_bytes`, trimming any alignment padding.
	// If the underlying filesystem does not support O_DIRECT, the kernel returns
	// EINVAL, which surfaces as IoError{EINVAL, ...}.
	[[nodiscard]] root::Task<ReadFixedResult> read_nocache_fixed(
		FileHandle const &fh,
		std::uint64_t offset,
		FixedBuffer buf,
		std::size_t max_bytes = std::numeric_limits<std::size_t>::max(),
		std::size_t block_size = 4096);
	// Write from a pre-registered fixed buffer. Symmetric to read_fixed.
	// The buffer is held by the completion callback until the CQE fires, then returned
	// to the caller (who decides when to release the slot back to the pool).
	struct WriteFixedResult {
		FixedBuffer buffer;
		std::size_t bytes{};
	};
	[[nodiscard]] root::Task<WriteFixedResult> write_fixed(
		FileHandle const &fh,
		std::uint64_t offset,
		FixedBuffer buf,
		std::size_t max_bytes = std::numeric_limits<std::size_t>::max());
	[[nodiscard]] root::JoinTask<std::size_t>
	write_into(FileHandle const &fh, std::uint64_t offset, std::span<std::byte const> src_view);
	// Scatter-gather write: sends `iovecs` segments to the file in sequence.
	// The V is moved into shared state and kept alive until the CQE fires.
	// Returns total bytes written across all segments.
	[[nodiscard]] root::Task<std::size_t>
	writev_into(FileHandle const &fh, std::uint64_t offset, std::vector<iovec> iovecs);
	// readv2_into: like readv_into but with RWF flags (e.g. RWF_NOWAIT, RWF_DSYNC).
	[[nodiscard]] root::Task<std::size_t>
	readv2_into(FileHandle const &fh, std::uint64_t offset, std::vector<iovec> iovecs, int rwf_flags = 0);
	// writev2_into: like writev_into but with RWF flags.
	[[nodiscard]] root::Task<std::size_t>
	writev2_into(FileHandle const &fh, std::uint64_t offset, std::vector<iovec> iovecs, int rwf_flags = 0);
	// No-op SQE — useful for latency measurement, wakeup, or pipeline flushing.
	[[nodiscard]] root::Task<void> async_nop() {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_nop();
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_fsync(FileHandle const &fh, bool data_only = false);
	[[nodiscard]] root::Task<void>
	async_fallocate(FileHandle const &fh, int mode, std::uint64_t offset, std::uint64_t len);
	// Consumes the handle; the ring closes the fd via io_uring.
	[[nodiscard]] root::Task<void>
	async_fadvise(FileHandle const &fh, std::uint64_t offset, std::uint32_t len, int advice);
	[[nodiscard]] root::Task<void> async_madvise(void *addr, std::uint32_t length, int advice);
	[[nodiscard]] root::Task<void> async_unlink(int dir_fd, std::string path, int flags = 0);
	[[nodiscard]] root::Task<void>
	async_rename(int old_dir_fd, std::string old_path, int new_dir_fd, std::string new_path, unsigned flags = 0);
	[[nodiscard]] root::Task<void> async_mkdirat(int dir_fd, std::string path, mode_t mode = 0755);
	[[nodiscard]] root::Task<void> async_symlinkat(std::string target, int new_dir_fd, std::string link_path);
	[[nodiscard]] root::Task<void> async_ftruncate(FileHandle const &fh, std::uint64_t length);
	[[nodiscard]] root::Task<void>
	async_linkat(int old_dir_fd, std::string old_path, int new_dir_fd, std::string new_path, int flags = 0);
	[[nodiscard]] root::Task<void>
	async_sync_file_range(FileHandle const &fh, std::uint64_t offset, unsigned len, int flags = 0);
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]] [[nodiscard]] root::Task<FileHandle>
	async_socket(
		int domain,
		int type,
		int protocol) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_socket(domain, type, protocol, 0);
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]] [[nodiscard]] root::Task<FileHandle>
	async_socket_direct(
		int domain,
		int type,
		int protocol,
		unsigned file_index) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_socket_direct(domain, type, protocol, conflux::uring::DirectSlot{file_index}, 0);
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [file_index](IoResult) {
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]]
	// Create a pipe asynchronously. Returns (read_fd, write_fd) on success.
	[[nodiscard]] root::Task<std::pair<int, int>> async_pipe(
		int pipe_flags = O_CLOEXEC | O_NONBLOCK) {
		auto [task, shared_src, sqe] = prepare_sqe<std::pair<int, int>>();
		if (!sqe) {
			return std::move(task);
		}
		auto fds = std::make_shared<std::array<int, 2>>(std::array<int, 2>{-1, -1});
		sqe.prep_pipe(fds->data(), pipe_flags);
		auto [slot, gen] = completions_->reserve([shared_src, fds](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: pipe"}));
					return;
				}
				auto _ = shared_src->try_set_value({std::make_pair((*fds)[0], (*fds)[1])});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Async bind. `addr` is copied and kept alive until CQE.
	[[nodiscard]] root::Task<void> async_bind(
		FileHandle const &fh,
		sockaddr_storage addr,
		socklen_t addrlen) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		auto addr_owner = std::make_shared<sockaddr_storage>(addr);
		visit_fd(fh, [&](RingFd auto fd) {
			sqe.prep_bind(fd, reinterpret_cast<sockaddr *>(addr_owner.get()), addrlen);
		});
		auto [slot, gen] = reserve_bridge<void>(shared_src, [addr_owner](IoResult) mutable {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Async listen.
	[[nodiscard]] root::Task<void> async_listen(
		FileHandle const &fh,
		int backlog = SOMAXCONN) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) { sqe.prep_listen(fd, backlog); });
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_shutdown(
		FileHandle const &fh,
		int how) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) { sqe.prep_shutdown(fd, how); });
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<std::size_t> async_tee(
		int fd_in,
		int fd_out,
		std::size_t len,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_tee(
			conflux::uring::SqeFd{fd_in},
			conflux::uring::SqeFd{fd_out},
			static_cast<std::uint32_t>(len),
			conflux::uring::SpliceFlags{flags});
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
				std::make_exception_ptr(IoError{EINVAL, "file_io: fixed_fd_install requires direct slot"}));
			return std::move(task);
		}
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(std::make_exception_ptr(IoError{ENOSPC, "file_io: SQ full"}));
			return std::move(task);
		}
		conflux::uring::Sqe sqe_view{sqe};
		sqe_view.prep_fixed_fd_install(fh.direct_fd(), conflux::uring::InstallFdFlags{flags});
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
	// Get extended attribute. `name` is moved and kept alive until CQE.
	// `value` std::span must remain valid until the returned Task resolves.
	// Returns the actual attribute size (may exceed value.size() — ERANGE).
	[[nodiscard]] root::Task<std::size_t> async_fgetxattr(FileHandle const &fh, std::string name, std::span<char> buf);
	// Set extended attribute. Both `name` and `data` are moved/kept alive until CQE.
	[[nodiscard]] root::Task<void>
	async_fsetxattr(FileHandle const &fh, std::string name, std::string data, int flags = 0);
	// Path-based get extended attribute. `name`, `path`, and `buf` must
	// remain valid until the Flow resolves.
	[[nodiscard]] root::Task<std::size_t> async_getxattr(std::string path, std::string name, std::span<char> buf);
	// Path-based set extended attribute. `name`, `data`, and `path` are moved
	// and kept alive until CQE.
	[[nodiscard]] root::Task<void> async_setxattr(std::string path, std::string name, std::string data, int flags = 0);
	// Wait for process state change (IORING_OP_WAITID). `infop` must stay valid
	// until the Flow resolves; on success it is filled with signal info.
	[[nodiscard]] root::Task<void> async_waitid(
		idtype_t idtype,
		id_t id,
		siginfo_t *infop,
		int options = WEXITED,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_waitid(idtype, id, infop, options, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_futex_wait(futex, val, mask, futex_flags, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_futex_wake(futex, val, mask, futex_flags, flags);
		auto [slot, gen] =
			reserve_bridge<std::uint32_t>(shared_src, [](IoResult r) { return static_cast<std::uint32_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_msg_ring(
			conflux::uring::SqeFd{target_ring_fd},
			len,
			conflux::uring::UserData{data},
			conflux::uring::MsgRingFlags{flags});
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		auto ts = std::make_shared<__kernel_timespec>();
		auto const sec = std::chrono::duration_cast<std::chrono::seconds>(ms);
		ts->tv_sec = sec.count();
		ts->tv_nsec = (ms - sec).count() * 1000000LL;
		sqe.prep_timeout_update(ts.get(), conflux::uring::UserData{user_data}, conflux::uring::TimeoutFlags{flags});
		auto [slot, gen] = completions_->reserve([shared_src, ts](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT && r.res != -EALREADY) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(IoError{-r.res, "file_io: timeout_update"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Register a one-shot poll on `fd` for `events` (POLLIN, POLLOUT, …).
	// Resolves with the triggered poll mask when any event fires.
	// -ENOENT on poll_remove before the event: treated as ECANCELED by caller.
	[[nodiscard]] root::Task<std::uint32_t> async_poll_add(
		int fd,
		std::uint32_t events) {
		auto [task, shared_src, sqe] = prepare_sqe<std::uint32_t>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_poll_add(conflux::uring::SqeFd{fd}, conflux::uring::PollFlags{events});
		auto [slot, gen] =
			reserve_bridge<std::uint32_t>(shared_src, [](IoResult r) { return static_cast<std::uint32_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Cancel a pending poll_add identified by `user_data`.
	// -ENOENT means the poll already fired — treated as success.
	[[nodiscard]] root::Task<void> async_poll_remove(
		std::uint64_t user_data) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_poll_remove(conflux::uring::UserData{user_data});
		auto [slot, gen] = completions_->reserve([shared_src](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT && r.res != -EALREADY) {
					auto _ =
						shared_src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: poll_remove"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Update an armed multishot poll's event mask in-place.
	// `user_data` is the encoded user_data of the original poll SQE.
	[[nodiscard]] root::Task<void> async_poll_update(
		std::uint64_t user_data,
		std::uint32_t new_events,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_poll_update(
			conflux::uring::UserData{user_data},
			conflux::uring::UserData{0},
			conflux::uring::PollFlags{new_events},
			conflux::uring::PollAddFlags{flags});
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) { sqe.prep_accept(fd, addr, addrlen, flags); });
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) {
			sqe.prep_accept_direct(fd, addr, addrlen, flags, conflux::uring::DirectSlot{file_index});
		});
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [file_index](IoResult) {
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_msg_ring_fd(
			conflux::uring::SqeFd{target_ring_fd},
			conflux::uring::SqeFd{source_fd},
			conflux::uring::SqeFd{target_fd},
			conflux::uring::UserData{data},
			conflux::uring::MsgRingFlags{flags});
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Wait on multiple futexes simultaneously. Resolves when any waiter
	// condition is met. `waiters` is moved and kept alive until CQE.
	[[nodiscard]] root::Task<void> async_futex_waitv(
		std::vector<futex_waitv> waiters,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		auto wv = std::make_shared<std::vector<futex_waitv>>(std::move(waiters));
		sqe.prep_futex_waitv(wv->data(), static_cast<std::uint32_t>(wv->size()), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [wv](IoResult) mutable {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Cancel a pending op by its user_data tag. Resolves when the cancel
	// was submitted; the target op's CQE will still arrive (with -ECANCELED).
	// -ENOENT means the target already completed — treated as success here.
	[[nodiscard]] root::Task<void> async_cancel(
		std::uint64_t user_data,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_cancel64(
			conflux::uring::UserData{user_data},
			conflux::uring::CancelFlags{static_cast<unsigned>(flags)});
		auto [slot, gen] = completions_->reserve([shared_src](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT) {
					auto _ = shared_src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: cancel"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Cancel all pending ops for `fd`. -ENOENT treated as success.
	[[nodiscard]] root::Task<void> async_cancel_fd(
		int fd,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_cancel_fd(conflux::uring::SqeFd{fd}, conflux::uring::CancelFlags{flags}).fixed_file(false);
		auto [slot, gen] = completions_->reserve([shared_src](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT) {
					auto _ =
						shared_src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: cancel_fd"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Async connect. `addr` is copied into a shared buffer kept alive until CQE.
	[[nodiscard]] root::Task<void> async_connect(
		FileHandle const &fh,
		sockaddr_storage addr,
		socklen_t addrlen) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		auto addr_owner = std::make_shared<sockaddr_storage>(addr);
		visit_fd(fh, [&](RingFd auto fd) {
			sqe.prep_connect(fd, reinterpret_cast<sockaddr *>(addr_owner.get()), addrlen);
		});
		auto [slot, gen] = reserve_bridge<void>(shared_src, [addr_owner](IoResult) mutable {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	[[nodiscard]] root::Task<void> async_close(
		FileHandle fh) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		release_fd_tag(fh, [&](RingFd auto fd) { sqe.prep_close(fd); });
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) {
			sqe.prep_send(fd, buf, len, conflux::uring::MsgFlags{static_cast<unsigned>(flags)});
		});
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Receive up to `len` bytes into `buf` from `fh`. Returns bytes received.
	[[nodiscard]] root::Task<std::size_t> async_recv(
		FileHandle const &fh,
		void *buf,
		std::size_t len,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) {
			sqe.prep_recv(fd, buf, len, conflux::uring::MsgFlags{static_cast<unsigned>(flags)});
		});
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Vectored send via sendmsg(2). `msg` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<std::size_t> async_sendmsg(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) { sqe.prep_sendmsg(fd, msg, conflux::uring::MsgFlags{flags}); });
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Vectored recv via recvmsg(2). `msg` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<std::size_t> async_recvmsg(
		FileHandle const &fh,
		msghdr *msg,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) { sqe.prep_recvmsg(fd, msg, conflux::uring::MsgFlags{flags}); });
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_provide_buffers(addr, len, nr, conflux::uring::BufGroupId{static_cast<std::uint16_t>(bgid)}, bid);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Remove `nr` buffers from buffer group `bgid`.
	[[nodiscard]] root::Task<void> async_remove_buffers(
		int nr,
		int bgid) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_remove_buffers(nr, conflux::uring::BufGroupId{static_cast<std::uint16_t>(bgid)});
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_files_update(fds, nr_fds, offset);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Modify an epoll interest list entry. `ev` may be null for EPOLL_CTL_DEL.
	[[nodiscard]] root::Task<void> async_epoll_ctl(
		int epfd,
		int fd,
		int op,
		epoll_event const *ev = nullptr) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_epoll_ctl(conflux::uring::SqeFd{epfd}, conflux::uring::SqeFd{fd}, op, ev);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_epoll_wait(conflux::uring::SqeFd{epfd}, events, static_cast<std::uint32_t>(maxevents), flags);
		auto [slot, gen] = reserve_bridge<int>(shared_src, [](IoResult r) { return r.res; });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
	[[nodiscard]] root::Task<FileHandle> async_openat2(int dir_fd, std::string path, open_how how);
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
		if (!sqe) {
			return std::move(task);
		}
		auto sa = std::make_shared<sockaddr_storage>(addr);
		visit_fd(fh, [&](RingFd auto fd) {
			sqe.prep_sendto(
				fd,
				buf,
				len,
				conflux::uring::MsgFlags{static_cast<unsigned>(flags)},
				reinterpret_cast<sockaddr *>(sa.get()),
				addrlen);
		});
		auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [sa](IoResult r) mutable {
			return static_cast<std::size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Unsafe zero-copy helper resolves on first send CQE only; this API does not expose buffer-release notification.
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
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) {
			sqe.prep_send_zc(fd, buf, len, conflux::uring::MsgFlags{static_cast<unsigned>(flags)}, zc_flags);
		});
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) {
			sqe.prep_send_zc(fd, buf, len, conflux::uring::MsgFlags{static_cast<unsigned>(flags)}, zc_flags);
		});
		auto [slot, gen] =
			reserve_zc_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Write using a pre-registered fixed buffer (IORING_OP_WRITE_FIXED).
	// `buf` pointer and `buf_index` must refer to the registered buffer in the pool.
	[[nodiscard]] root::Task<std::size_t>
	async_write_fixed(FileHandle const &fh, std::uint64_t offset, void const *buf, unsigned nbytes, int buf_index);
	// Remove a file by name relative to `dir_fd`.
	// `flags` = 0 for file; AT_REMOVEDIR for directory.
	[[nodiscard]] root::Task<void> async_unlinkat(
		int dir_fd,
		std::string path,
		int flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		auto p = std::make_shared<std::string>(std::move(path));
		sqe.prep_unlinkat(conflux::uring::SqeFd{dir_fd}, p->c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [p](IoResult) mutable {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		auto paths = std::make_shared<std::pair<std::string, std::string>>(std::move(old_path), std::move(new_path));
		sqe.prep_renameat(
			conflux::uring::SqeFd{old_dir_fd},
			paths->first.c_str(),
			conflux::uring::SqeFd{new_dir_fd},
			paths->second.c_str(),
			flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Create a directory at `path` (relative to AT_FDCWD).
	[[nodiscard]] root::Task<void> async_mkdir(
		std::string path,
		mode_t mode = 0755) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		auto p = std::make_shared<std::string>(std::move(path));
		sqe.prep_mkdir(p->c_str(), mode);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [p](IoResult) mutable {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Open directly into the registered file table with full openat2 semantics.
	// IORING_FILE_INDEX_ALLOC for `file_index` auto-allocates.
	[[nodiscard]] root::Task<FileHandle>
	async_openat2_direct(int dir_fd, std::string path, open_how how, unsigned file_index);
	// Create a socket directly into the registered file table, with the kernel
	// choosing the slot (IORING_FILE_INDEX_ALLOC). Returns the allocated slot.
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]] [[nodiscard]] root::Task<FileHandle>
	async_socket_direct_alloc(
		int domain,
		int type,
		int protocol,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_socket_direct_alloc(domain, type, protocol, flags);
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_direct_slot(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Open a file directly into the fixed file table (openat semantics).
	// Use IORING_FILE_INDEX_ALLOC for `file_index` to let the kernel pick a slot.
	[[nodiscard]] root::Task<FileHandle> async_openat_direct(
		int dir_fd,
		std::string path,
		int flags,
		mode_t mode = 0,
		unsigned file_index = IORING_FILE_INDEX_ALLOC);
	// Send a source fd to another ring, letting the kernel auto-allocate the slot.
	// Returns the allocated slot index via the target ring's CQE.
	[[nodiscard]] root::Task<void> async_msg_ring_fd_alloc(
		int target_ring_fd,
		int source_fd,
		std::uint64_t data = 0,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<void>();
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_msg_ring_fd_alloc(
			conflux::uring::SqeFd{target_ring_fd},
			conflux::uring::SqeFd{source_fd},
			conflux::uring::UserData{data},
			conflux::uring::MsgRingFlags{flags});
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Create a pipe P directly into fixed file table slots.
	// Use IORING_FILE_INDEX_ALLOC for `file_index` to let the kernel choose.
	// The two slots are allocated consecutively.
	[[nodiscard]] root::Task<std::pair<int, int>> async_pipe_direct(
		unsigned file_index = IORING_FILE_INDEX_ALLOC,
		int pipe_flags = O_CLOEXEC | O_NONBLOCK) {
		auto [task, shared_src, sqe] = prepare_sqe<std::pair<int, int>>();
		if (!sqe) {
			return std::move(task);
		}
		auto fds = std::make_shared<std::array<int, 2>>(std::array<int, 2>{-1, -1});
		sqe.prep_pipe_direct(fds->data(), pipe_flags, conflux::uring::DirectSlot{file_index});
		auto [slot, gen] = completions_->reserve([shared_src, fds](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ =
						shared_src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: pipe_direct"}));
					return;
				}
				auto _ = shared_src->try_set_value({std::make_pair((*fds)[0], (*fds)[1])});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
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
		if (!sqe) {
			return std::move(task);
		}
		sqe.prep_msg_ring_cqe_flags(
			conflux::uring::SqeFd{target_ring_fd},
			len,
			conflux::uring::UserData{data},
			conflux::uring::MsgRingFlags{flags},
			cqe_flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// Unsafe zero-copy helper resolves on first send CQE only; this API does not expose buffer-release notification.
	// Caller must guarantee msg, iovec array, and all pointed buffers remain live by other means.
	[[nodiscard]] root::Task<std::size_t> async_unsafe_sendmsg_zc_sent(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) { sqe.prep_sendmsg_zc(fd, msg, conflux::uring::MsgFlags{flags}); });
		auto [slot, gen] =
			reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}
	// msg, iovec array, and all pointed buffers must remain live until co_return.
	[[nodiscard]] root::Task<std::size_t> async_sendmsg_zc(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
		if (!sqe) {
			return std::move(task);
		}
		visit_fd(fh, [&](RingFd auto fd) { sqe.prep_sendmsg_zc(fd, msg, conflux::uring::MsgFlags{flags}); });
		auto [slot, gen] =
			reserve_zc_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
		io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
		return std::move(task);
	}

private:
	[[nodiscard]] root::Task<FileHandle> async_open_atomic_parent_dir(int root_dir_fd, std::string parent_dir);
	[[nodiscard]] root::Task<FileHandle>
	async_open_atomic_payload(int parent_dir_fd, std::string staging_name, mode_t mode, bool &staging_entry_exists);
	[[nodiscard]] root::Task<void>
	async_link_atomic_payload(FileHandle const &fh, int parent_dir_fd, std::string staging_name);
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
			auto _ = st->src->try_set_exception(std::make_exception_ptr(IoError{ENOSPC, "file_io: splice SQ full"}));
			return;
		}

		auto sqe_in_view = conflux::uring::Sqe{sqe_in};
		sqe_in_view.prep_splice(
			conflux::uring::SqeFd{
				st->file_fd,
			},
			static_cast<std::int64_t>(st->file_off),
			conflux::uring::SqeFd{
				st->pipe.write_fd(),
			},
			-1,
			static_cast<unsigned>(chunk),
			conflux::uring::SpliceFlags{SPLICE_F_MOVE | SPLICE_F_MORE});
		sqe_in_view.add_flags(conflux::uring::sqe_flags::io_link);
		auto [slot_in, gen_in] = st->completions->reserve([st](IoResult r) mutable {
			if (r.res < 0 && r.res != -ECANCELED) {
				auto _ = st->src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: splice in"}));
			}
		});
		io_uring_sqe_set_data64(sqe_in, st->encode_ud(slot_in, gen_in));

		auto sqe_out_view = conflux::uring::Sqe{sqe_out};
		sqe_out_view.prep_splice(
			conflux::uring::SqeFd{
				st->pipe.read_fd(),
			},
			-1,
			conflux::uring::SqeFd{
				st->dst_fd,
			},
			-1,
			static_cast<unsigned>(chunk),
			conflux::uring::SpliceFlags{SPLICE_F_MOVE | SPLICE_F_MORE});
		if (st->dst_fixed) {
			sqe_out_view.fixed_file(true);
		}
		auto [slot_out, gen_out] = st->completions->reserve([st](IoResult r) mutable {
			if (r.res < 0) {
				auto _ = st->src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: splice out"}));
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
		conflux::file_io_sync::TempPublishMode pub_mode = conflux::file_io_sync::TempPublishMode::replace_existing,
		conflux::file_io_sync::TempDurability durability = conflux::file_io_sync::TempDurability::file_and_directory);
};

} // namespace conflux::file_io
