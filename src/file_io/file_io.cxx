module;
#include <cstdio>
#include <fcntl.h>
#include <liburing.h>
#include <linux/futex.h>
#include <linux/openat2.h>
#include <linux/stat.h>
#include <memory>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

export module conflux.file_io;

import std;
import conflux.types;
import std.compat;
import conflux.work;
export import conflux.uring.completion;
export import conflux.uring.handle;
import conflux.uring.timeout;
export import conflux.file_io_sync;

export using FileIoError = IoError;

namespace root = conflux::work::root;
namespace {

Atom<u64> g_async_staging_counter{0};
inline S make_staging_name_async() {
	auto const pid = static_cast<u32>(::getpid());
	auto const seq = g_async_staging_counter.fetch_add(1, memory_order_relaxed);
	u32 rnd{};
	auto _ = ::getrandom(&rnd, sizeof(rnd), 0);
	return format(".conflux.tmp.{}.{}.{:08x}", pid, seq, rnd);
}
constexpr bool is_otmpfile_unsupported_errno_async(
	int e) noexcept {
	return e == EOPNOTSUPP || e == EISDIR || e == EINVAL || e == ENOSYS || e == EPERM;
}
struct AsyncAtomicPathParts {
	S parent_dir;
	S basename;
};
[[nodiscard]] expected<AsyncAtomicPathParts, FileIoError> split_contained_atomic_path_async(
	SV path) noexcept {
	if (path.empty()) {
		return unexpected{
			FileIoError{EINVAL, "file_io: empty atomic-write path"}
        };
	}
	if (path.starts_with('/')) {
		return unexpected{
			FileIoError{EINVAL, "file_io: absolute atomic-write path"}
        };
	}
	if (path.contains('\0')) {
		return unexpected{
			FileIoError{EINVAL, "file_io: NUL in atomic-write path"}
        };
	}
	if (path == "." || path == ".." || path.ends_with('/')) {
		return unexpected{
			FileIoError{EINVAL, "file_io: invalid atomic-write path"}
        };
	}

	SV remaining = path;
	while (!remaining.empty()) {
		auto const slash = remaining.find('/');
		auto const component = remaining.substr(0, slash);
		if (component.empty() || component == "..") {
			return unexpected{
				FileIoError{EINVAL, "file_io: invalid atomic-write path component"}
            };
		}
		if (slash == SV::npos) {
			break;
		}
		remaining = remaining.substr(slash + 1);
	}

	auto const last_slash = path.rfind('/');
	if (last_slash == SV::npos) {
		return AsyncAtomicPathParts{.parent_dir = S{"."}, .basename = S{path}};
	}
	return AsyncAtomicPathParts{
		.parent_dir = S{path.substr(0, last_slash)},
		.basename = S{path.substr(last_slash + 1)},
	};
}

} // namespace
// ---------------------------------------------------------------------------
// RegisteredBufferTable: owns the io_uring registered-buffer table lifecycle.
// One instance per ring. Pools carve slices from it.
// ---------------------------------------------------------------------------

export class RegisteredBufferTable {
	io_uring *ring_;
	bool registered_{false};

public:
	RegisteredBufferTable(
		io_uring *ring,
		unsigned total_slots)
		: ring_{ring} {
		if (total_slots == 0) {
			return;
		}
		if (io_uring_register_buffers_sparse(ring_, total_slots) < 0) {
			return;
		}
		registered_ = true;
	}
	RegisteredBufferTable(RegisteredBufferTable const &) = delete;
	RegisteredBufferTable &operator =(RegisteredBufferTable const &) = delete;
	RegisteredBufferTable(RegisteredBufferTable &&) = delete;
	RegisteredBufferTable &operator =(RegisteredBufferTable &&) = delete;
	~RegisteredBufferTable() {
		if (registered_) {
			io_uring_unregister_buffers(ring_);
		}
	}
	[[nodiscard]] bool ok() const noexcept { return registered_; }
	[[nodiscard]] int update(
		unsigned idx,
		iovec const &iov) noexcept {
		return io_uring_register_buffers_update_tag(ring_, idx, &iov, nullptr, 1);
	}
};

// ---------------------------------------------------------------------------
// FixedBuffer / FixedBufferPool: pre-registered page-aligned slabs for
// io_uring fixed-buffer ops. A FixedBuffer is a RAII lease on one slot;
// returning the lease pushes the slot back to the pool's free-list.
//
// Each pool owns a contiguous slice [base, base+count) of a shared
// RegisteredBufferTable. slot() returns the absolute index for io_uring.
// ---------------------------------------------------------------------------

export class FixedBufferPool;
export class FixedBuffer {
	FixedBufferPool *pool_{nullptr};
	unsigned local_slot_{0};
	span<byte> view_{};
	FixedBuffer(
		FixedBufferPool *pool,
		unsigned local_slot,
		span<byte> view) noexcept
		: pool_{pool}
		, local_slot_{local_slot}
		, view_{view} {}

	friend class FixedBufferPool;

public:
	FixedBuffer() noexcept {} // NOLINT(modernize-use-equals-default) — GCC module bug
	FixedBuffer(FixedBuffer const &) = delete;
	FixedBuffer &operator =(FixedBuffer const &) = delete;
	FixedBuffer(
		FixedBuffer &&o) noexcept
		: pool_{exchange(o.pool_, nullptr)}
		, local_slot_{o.local_slot_}
		, view_{o.view_} {}
	FixedBuffer &operator =(FixedBuffer &&o) noexcept;
	~FixedBuffer();
	[[nodiscard]] bool valid() const noexcept { return pool_ != nullptr; }
	[[nodiscard]] unsigned slot() const noexcept;
	[[nodiscard]] byte *data() noexcept { return view_.data(); }
	[[nodiscard]] byte const *data() const noexcept { return view_.data(); }
	[[nodiscard]] span<byte> view() const noexcept { return view_; }
	[[nodiscard]] SZ size() const noexcept { return view_.size(); }
};
export class FixedBufferPool {
	RegisteredBufferTable *table_;
	unsigned base_;
	SZ slab_bytes_;
	V<UPD<byte[], void (*)(void *)>> slabs_{};
	V<unsigned> free_{};
	bool ok_{false};

	friend class FixedBuffer;
	void release(
		// NOLINT(bugprone-exception-escape) — free_ is pre-sized; push_back never reallocates
		unsigned local_slot) noexcept {
		free_.push_back(local_slot);
	}

public:
	FixedBufferPool(
		RegisteredBufferTable *table,
		unsigned base,
		SZ slab_count,
		SZ slab_bytes)
		: table_{table}
		, base_{base}
		, slab_bytes_{slab_bytes} {
		if (slab_count == 0 || slab_bytes == 0 || !table_->ok()) {
			return;
		}
		long const page = ::sysconf(_SC_PAGESIZE);
		SZ const align = page > 0 ? static_cast<SZ>(page) : SZ{4096};
		SZ const aligned_bytes = ((slab_bytes + align - 1) / align) * align;
		slab_bytes_ = aligned_bytes;
		slabs_.reserve(slab_count);
		free_.reserve(slab_count);
		for (SZ i = 0; i < slab_count; ++i) {
			void *raw = ::aligned_alloc(align, aligned_bytes);
			if (raw == nullptr) {
				break;
			}
			::madvise(raw, aligned_bytes, MADV_DONTFORK);
			::madvise(raw, aligned_bytes, MADV_HUGEPAGE);
			slabs_.emplace_back(static_cast<byte *>(raw), +[](void *p) { ::free(p); });
			iovec const iov{.iov_base = raw, .iov_len = aligned_bytes};
			if (table_->update(base_ + static_cast<unsigned>(i), iov) < 0) {
				break;
			}
			free_.push_back(static_cast<unsigned>(i));
		}
		ok_ = !slabs_.empty();
	}
	FixedBufferPool(FixedBufferPool const &) = delete;
	FixedBufferPool &operator =(FixedBufferPool const &) = delete;
	FixedBufferPool(FixedBufferPool &&) = delete;
	FixedBufferPool &operator =(FixedBufferPool &&) = delete;
	~FixedBufferPool() {}
	[[nodiscard]] bool ok() const noexcept { return ok_; }
	[[nodiscard]] unsigned base() const noexcept { return base_; }
	[[nodiscard]] SZ capacity() const noexcept { return slabs_.size(); }
	[[nodiscard]] SZ available() const noexcept { return free_.size(); }
	[[nodiscard]] SZ slab_bytes() const noexcept { return slab_bytes_; }
	[[nodiscard]] Opt<FixedBuffer> try_acquire() {
		if (free_.empty()) {
			return nullopt;
		}
		unsigned const local = free_.back();
		free_.pop_back();
		return FixedBuffer{
			this,
			local,
			span{slabs_[local].get(), slab_bytes_}
        };
	}
};
inline unsigned FixedBuffer::slot() const noexcept {
	return pool_->base_ + local_slot_;
}
inline FixedBuffer &FixedBuffer::operator =(
	FixedBuffer &&o) noexcept {
	if (this != &o) {
		if (pool_ != nullptr) {
			pool_->release(local_slot_);
		}
		pool_ = exchange(o.pool_, nullptr);
		local_slot_ = o.local_slot_;
		view_ = o.view_;
	}
	return *this;
}
inline FixedBuffer::~FixedBuffer() {
	if (pool_ != nullptr) {
		pool_->release(local_slot_);
	}
}

// ---------------------------------------------------------------------------
// PipePair / PipePool: cache of pipe2(O_DIRECT | O_CLOEXEC) pairs for
// zero-copy splice chains (file → pipe → socket).
//
// Per-ring: io_uring expects SQEs to reference fds visible to the thread that
// owns the ring. Sharing a PipePool across rings is an error — each ring must
// hold its own pool. TRICKS.md §pipe-per-ring.
// ---------------------------------------------------------------------------

export class PipePool;
export class PipePair {
	PipePool *pool_{nullptr};
	u32 slot_{0};
	int read_fd_{-1};
	int write_fd_{-1};
	SZ capacity_{0};
	PipePair(
		PipePool *pool,
		u32 slot,
		int read_fd,
		int write_fd,
		SZ capacity) noexcept
		: pool_{pool}
		, slot_{slot}
		, read_fd_{read_fd}
		, write_fd_{write_fd}
		, capacity_{capacity} {}

	friend class PipePool;

public:
	PipePair() noexcept {} // NOLINT(modernize-use-equals-default) — GCC module bug
	PipePair(PipePair const &) = delete;
	PipePair &operator =(PipePair const &) = delete;
	PipePair(
		PipePair &&o) noexcept
		: pool_{exchange(o.pool_, nullptr)}
		, slot_{o.slot_}
		, read_fd_{exchange(o.read_fd_, -1)}
		, write_fd_{exchange(o.write_fd_, -1)}
		, capacity_{o.capacity_} {}
	PipePair &operator =(PipePair &&o) noexcept;
	~PipePair();
	[[nodiscard]] bool valid() const noexcept { return pool_ != nullptr; }
	[[nodiscard]] int read_fd() const noexcept { return read_fd_; }
	[[nodiscard]] int write_fd() const noexcept { return write_fd_; }
	[[nodiscard]] SZ capacity() const noexcept { return capacity_; }
};
export class PipePool {
	struct Pair {
		int read_fd;
		int write_fd;
		SZ capacity;
	};
	V<Pair> pairs_{};
	V<u32> free_{};

	friend class PipePair;
	void release(
		// NOLINT(bugprone-exception-escape) — free_ is pre-sized; push_back never reallocates
		u32 slot) noexcept {
		free_.push_back(slot);
	}

public:
	explicit PipePool(
		SZ pair_count) {
		pairs_.reserve(pair_count);
		free_.reserve(pair_count);
		for (SZ i = 0; i < pair_count; ++i) {
			A<int, 2> fds{-1, -1};
			// O_DIRECT = packet-mode pipes: each write yields a distinct read,
			// exactly what splice with SPLICE_F_MOVE expects. Falls back to byte
			// stream if kernel rejects (rare).
			if (::pipe2(fds.data(), O_DIRECT | O_CLOEXEC) < 0 && ::pipe2(fds.data(), O_CLOEXEC) < 0) {
				break;
			}
			SZ cap = 0;
			if (int const c = ::fcntl(fds[0], F_GETPIPE_SZ); c > 0) {
				cap = static_cast<SZ>(c);
			}
			if (cap == 0) {
				cap = 64UL * 1024;
			}
			pairs_.push_back(Pair{.read_fd = fds[0], .write_fd = fds[1], .capacity = cap});
			free_.push_back(static_cast<u32>(pairs_.size() - 1));
		}
	}
	PipePool(PipePool const &) = delete;
	PipePool &operator =(PipePool const &) = delete;
	PipePool(PipePool &&) = delete;
	PipePool &operator =(PipePool &&) = delete;
	~PipePool() {
		for (auto const &p: pairs_) {
			if (p.read_fd >= 0) {
				::close(p.read_fd);
			}
			if (p.write_fd >= 0) {
				::close(p.write_fd);
			}
		}
	}
	[[nodiscard]] SZ capacity() const noexcept { return pairs_.size(); }
	[[nodiscard]] SZ available() const noexcept { return free_.size(); }
	[[nodiscard]] Opt<PipePair> try_acquire() {
		if (free_.empty()) {
			return nullopt;
		}
		u32 const idx = free_.back();
		free_.pop_back();
		auto const &p = pairs_[idx];
		return PipePair{this, idx, p.read_fd, p.write_fd, p.capacity};
	}
};
inline PipePair &PipePair::operator =(
	PipePair &&o) noexcept {
	if (this != &o) {
		if (pool_ != nullptr) {
			pool_->release(slot_);
		}
		pool_ = exchange(o.pool_, nullptr);
		slot_ = o.slot_;
		read_fd_ = exchange(o.read_fd_, -1);
		write_fd_ = exchange(o.write_fd_, -1);
		capacity_ = o.capacity_;
	}
	return *this;
}
inline PipePair::~PipePair() {
	if (pool_ != nullptr) {
		pool_->release(slot_);
	}
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
	template<typename T>
	root::Task<T> fail_sq_full() const {
		auto [task, raw_src] = root::make_task_source<T>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<T>>(move(raw_src));
		auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
		return move(task);
	}
	// Reserve a completion slot with a callback that bridges an IoResult into
	// a root::TaskSource<T>. `decode` turns a non-negative res into a T; negative
	// res flows through as FileIoError automatically.
	template<typename T, typename Decode>
	P<u32, u32> reserve_bridge(
		SP<root::TaskSource<T>> const &src,
		Decode &&decode) {
		return completions_->reserve([src, decode = forward<Decode>(decode)](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: cqe error"}));
					return;
				}
				if constexpr (std::is_void_v<T>) {
					decode(r);
					auto _ = src->try_set_value(root::Success<void>{});
				} else {
					auto _ = src->try_set_value(root::Success<T>{decode(r)});
				}
			} catch (...) { auto _ = src->try_set_exception(current_exception()); }
		});
	}
	template<typename T, typename Decode>
	P<u32, u32> reserve_zc_bridge(
		SP<root::TaskSource<T>> const &src,
		Decode &&decode) {
		return completions_->reserve_zc([src, decode = forward<Decode>(decode)](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: cqe error"}));
					return;
				}
				if constexpr (std::is_void_v<T>) {
					decode(r);
					auto _ = src->try_set_value(root::Success<void>{});
				} else {
					auto _ = src->try_set_value(root::Success<T>{decode(r)});
				}
			} catch (...) { auto _ = src->try_set_exception(current_exception()); }
		});
	}

public:
	FileReader(
		io_uring *ring,
		CompletionTable *completions,
		UserDataFn encoder)
		: ring_{ring}
		, completions_{completions}
		, encode_ud_{move(encoder)} {}
	FileReader(FileReader const &) = delete;
	FileReader &operator =(FileReader const &) = delete;
	FileReader(FileReader &&) = delete;
	FileReader &operator =(FileReader &&) = delete;
	~FileReader() {} // NOLINT(modernize-use-equals-default) — GCC module bug
	[[nodiscard]] io_uring *ring() const noexcept { return ring_; }
	[[nodiscard]] CompletionTable *completions() const noexcept { return completions_; }
	[[nodiscard]] u64 encode_ud(
		u32 slot,
		u32 gen) const {
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
		auto [slot, gen] = completions_->reserve_multishot(move(on_event));
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
		auto [slot, gen] = completions_->reserve(move(on_event));
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return true;
	}

private:
	[[nodiscard]] bool submit_open_direct_fallback(
		SP<root::TaskSource<FileHandle>> const &src,
		SP<S> const &path_owner,
		int dir_fd,
		int flags,
		mode_t mode,
		unsigned file_index) {
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return false;
		}
		io_uring_prep_openat(sqe, dir_fd, path_owner->c_str(), flags, mode);
		auto [slot, gen] = completions_->reserve([this, src, path_owner, file_index](IoResult r) mutable {
			auto _ = path_owner; // keep-alive until CQE
			try {
				if (r.res < 0) {
					auto _ = src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: open_direct"}));
					return;
				}
				int const fd = r.res;
				int const update_rc = ::io_uring_register_files_update(ring_, file_index, &fd, 1);
				::close(fd);
				if (update_rc < 0) {
					int const sparse = -1;
					::io_uring_register_files_update(ring_, file_index, &sparse, 1);
					auto _ =
						src->try_set_exception(make_exception_ptr(FileIoError{-update_rc, "file_io: open_direct"}));
					return;
				}
				auto _ = src->try_set_value(
					root::Success<FileHandle>{FileHandle::from_direct_slot(static_cast<int>(file_index))});
			} catch (...) { auto _ = src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return true;
	}

public:
	// Open a path relative to dir_fd. Result is a raw-fd FileHandle.
	// Pass AT_FDCWD for absolute paths / cwd-relative.
	// `path` must be a null-terminated S owned by the caller until the
	// CQE fires; if unsure, pass a S and we copy.
	[[nodiscard]] root::Task<FileHandle> open_async(
		int dir_fd,
		S path,
		int flags,
		mode_t mode = 0) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto path_owner = make_shared<S>(move(path));
		io_uring_prep_openat(sqe, dir_fd, path_owner->c_str(), flags, mode);
		auto [slot, gen] = completions_->reserve([shared_src, path_owner](IoResult r) mutable {
			auto _ = path_owner; // keep-alive until CQE
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: open"}));
					return;
				}
				auto _ = shared_src->try_set_value({FileHandle::from_fd(r.res)});
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Open a path directly into the ring's fixed-file table. The owner must
	// have registered a sparse file table first.
	[[nodiscard]] root::Task<FileHandle> open_direct_async(
		int dir_fd,
		S path,
		int flags,
		mode_t mode,
		unsigned file_index) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto path_owner = make_shared<S>(move(path));
		io_uring_prep_openat_direct(sqe, dir_fd, path_owner->c_str(), flags, mode, file_index);
		auto [slot, gen] = completions_->reserve([this, shared_src, path_owner, dir_fd, flags, mode, file_index](
													 IoResult r) mutable {
			auto _ = path_owner; // keep-alive until CQE
			try {
				if (r.res < 0) {
					int const err = -r.res;
					if (err == EINVAL || err == EOPNOTSUPP || err == ENOSYS) {
						auto _ = submit_open_direct_fallback(shared_src, path_owner, dir_fd, flags, mode, file_index);
						return;
					}
					auto _ =
						shared_src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: open_direct"}));
					return;
				}
				auto _ = shared_src->try_set_value(
					{FileHandle::from_direct_slot(r.res == 0 ? static_cast<int>(file_index) : r.res)});
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// statx on a path. `mask` follows statx(2) — STATX_BASIC_STATS by default.
	[[nodiscard]] root::Task<FileStat> statx_async(
		int dir_fd,
		S path,
		int flags = 0,
		unsigned mask = STATX_BASIC_STATS,
		bool fixed_file = false) {
		auto [task, raw_src] = root::make_task_source<FileStat>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileStat>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto path_owner = make_shared<S>(move(path));
		auto stx_owner = make_shared<struct statx>();
		io_uring_prep_statx(sqe, dir_fd, path_owner->c_str(), flags, mask, stx_owner.get());
		if (fixed_file) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = completions_->reserve([shared_src, path_owner, stx_owner](IoResult r) mutable {
			auto _ = path_owner;
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: statx"}));
					return;
				}
				auto const &s = *stx_owner;
				FileStat const out{
					.size = s.stx_size,
					.mtime_ns = static_cast<u64>(s.stx_mtime.tv_sec) * 1000000000ULL + s.stx_mtime.tv_nsec,
					.ctime_ns = static_cast<u64>(s.stx_ctime.tv_sec) * 1000000000ULL + s.stx_ctime.tv_nsec,
					.dev = (static_cast<u64>(s.stx_dev_major) << 32U) | s.stx_dev_minor,
					.ino = s.stx_ino,
					.mode = s.stx_mode};
				auto _ = shared_src->try_set_value({out});
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// fstat-equivalent via statx with AT_EMPTY_PATH — avoids a path lookup.
	[[nodiscard]] root::Task<FileStat> stat_async(
		FileHandle const &fh) {
		if (fh.is_direct()) {
			return statx_async(fh.direct_slot(), S{}, AT_EMPTY_PATH, STATX_BASIC_STATS, true);
		}
		return statx_async(fh.raw_fd(), S{}, AT_EMPTY_PATH);
	}
	// Read into a caller-owned span. The caller must keep `dst` alive until the
	// Flow resolves.
	[[nodiscard]] root::Task<SZ> read_into(
		FileHandle const &fh,
		u64 offset,
		span<byte> dst) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_read(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			dst.data(),
			static_cast<unsigned>(dst.size()),
			offset);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Scatter-gather read: fills `iovecs` segments in sequence. The V is
	// moved into shared state and kept alive until the CQE fires.
	// Returns total bytes read across all segments.
	[[nodiscard]] root::Task<SZ> readv_into(
		FileHandle const &fh,
		u64 offset,
		V<iovec> iovecs) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto iov_owner = make_shared<V<iovec>>(move(iovecs));
		io_uring_prep_readv(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			iov_owner->data(),
			static_cast<unsigned>(iov_owner->size()),
			offset);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [iov_owner](IoResult r) mutable {
			auto _ = iov_owner; // keep-alive until CQE
			return static_cast<SZ>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Read into a pre-registered fixed buffer. The pool slot is held by the
	// buffer and returned on destruction — by placing the buffer inside the
	// shared-state closure we keep it alive until the CQE fires, then move it
	// into the resolved value so the caller decides when to release.
	struct ReadFixedResult {
		FixedBuffer buffer;
		SZ bytes{};
	};
	[[nodiscard]] root::Task<ReadFixedResult> read_fixed(
		FileHandle const &fh,
		u64 offset,
		FixedBuffer buf,
		SZ max_bytes = NL<SZ>::max()) {
		auto [task, raw_src] =
			root::make_task_source<ReadFixedResult>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<ReadFixedResult>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		unsigned const slot_idx = buf.slot();
		auto holder = make_shared<FixedBuffer>(move(buf));
		SZ const bytes = min(holder->view().size(), max_bytes);
		io_uring_prep_read_fixed(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			holder->view().data(),
			static_cast<unsigned>(bytes),
			offset,
			static_cast<int>(slot_idx));
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = completions_->reserve([shared_src, holder](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ =
						shared_src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: read_fixed"}));
					return;
				}
				auto _ = shared_src->try_set_value({
					ReadFixedResult{.buffer = move(*holder), .bytes = static_cast<SZ>(r.res)}
                });
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Read into a pre-registered fixed buffer, bypassing the kernel page cache.
	// The file must have been opened with O_DIRECT. `offset` must be a multiple
	// of `block_size`. `max_bytes` is the caller's true limit (e.g. remaining
	// file bytes); it is rounded up to the nearest `block_size` multiple before
	// submission to satisfy O_DIRECT alignment. The resolved byte count is
	// capped back to the original `max_bytes`, trimming any alignment padding.
	// If the underlying filesystem does not support O_DIRECT, the kernel returns
	// EINVAL, which surfaces as FileIoError{EINVAL, ...}.
	[[nodiscard]] root::Task<ReadFixedResult> read_nocache_fixed(
		FileHandle const &fh,
		u64 offset,
		FixedBuffer buf,
		SZ max_bytes = NL<SZ>::max(),
		SZ block_size = 4096) {
		SZ const actual_cap = min(max_bytes, buf.size());
		SZ aligned_bytes = actual_cap;
		if (block_size > 1 && actual_cap > 0) {
			aligned_bytes = ((actual_cap + block_size - 1) / block_size) * block_size;
			aligned_bytes = min(aligned_bytes, buf.size());
		}
		auto [task, raw_src] =
			root::make_task_source<ReadFixedResult>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<ReadFixedResult>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		unsigned const slot_idx = buf.slot();
		auto holder = make_shared<FixedBuffer>(move(buf));
		io_uring_prep_read_fixed(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			holder->view().data(),
			static_cast<unsigned>(aligned_bytes),
			offset,
			static_cast<int>(slot_idx));
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = completions_->reserve([shared_src, holder, actual_cap](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(
						make_exception_ptr(FileIoError{-r.res, "file_io: read_nocache_fixed"}));
					return;
				}
				SZ const bytes = min(static_cast<SZ>(r.res), actual_cap);
				auto _ = shared_src->try_set_value({
					ReadFixedResult{.buffer = move(*holder), .bytes = bytes}
                });
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Write from a pre-registered fixed buffer. Symmetric to read_fixed.
	// The buffer is held in shared state until the CQE fires, then returned
	// to the caller (who decides when to release the slot back to the pool).
	struct WriteFixedResult {
		FixedBuffer buffer;
		SZ bytes{};
	};
	[[nodiscard]] root::Task<WriteFixedResult> write_fixed(
		FileHandle const &fh,
		u64 offset,
		FixedBuffer buf,
		SZ max_bytes = NL<SZ>::max()) {
		auto [task, raw_src] =
			root::make_task_source<WriteFixedResult>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<WriteFixedResult>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		unsigned const slot_idx = buf.slot();
		auto holder = make_shared<FixedBuffer>(move(buf));
		SZ const bytes = min(holder->view().size(), max_bytes);
		io_uring_prep_write_fixed(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			holder->view().data(),
			static_cast<unsigned>(bytes),
			offset,
			static_cast<int>(slot_idx));
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = completions_->reserve([shared_src, holder](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ =
						shared_src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: write_fixed"}));
					return;
				}
				auto _ = shared_src->try_set_value({
					WriteFixedResult{.buffer = move(*holder), .bytes = static_cast<SZ>(r.res)}
                });
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<SZ> write_into(
		FileHandle const &fh,
		u64 offset,
		span<byte const> src_view) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_write(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			src_view.data(),
			static_cast<unsigned>(src_view.size()),
			offset);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Scatter-gather write: sends `iovecs` segments to the file in sequence.
	// The V is moved into shared state and kept alive until the CQE fires.
	// Returns total bytes written across all segments.
	[[nodiscard]] root::Task<SZ> writev_into(
		FileHandle const &fh,
		u64 offset,
		V<iovec> iovecs) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto iov_owner = make_shared<V<iovec>>(move(iovecs));
		io_uring_prep_writev(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			iov_owner->data(),
			static_cast<unsigned>(iov_owner->size()),
			offset);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [iov_owner](IoResult r) mutable {
			auto _ = iov_owner; // keep-alive until CQE
			return static_cast<SZ>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// readv2_into: like readv_into but with RWF flags (e.g. RWF_NOWAIT, RWF_DSYNC).
	[[nodiscard]] root::Task<SZ> readv2_into(
		FileHandle const &fh,
		u64 offset,
		V<iovec> iovecs,
		int rwf_flags = 0) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto iov_owner = make_shared<V<iovec>>(move(iovecs));
		io_uring_prep_readv2(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			iov_owner->data(),
			static_cast<unsigned>(iov_owner->size()),
			offset,
			rwf_flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [iov_owner](IoResult r) mutable {
			auto _ = iov_owner;
			return static_cast<SZ>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// writev2_into: like writev_into but with RWF flags.
	[[nodiscard]] root::Task<SZ> writev2_into(
		FileHandle const &fh,
		u64 offset,
		V<iovec> iovecs,
		int rwf_flags = 0) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto iov_owner = make_shared<V<iovec>>(move(iovecs));
		io_uring_prep_writev2(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			iov_owner->data(),
			static_cast<unsigned>(iov_owner->size()),
			offset,
			rwf_flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [iov_owner](IoResult r) mutable {
			auto _ = iov_owner;
			return static_cast<SZ>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// No-op SQE — useful for latency measurement, wakeup, or pipeline flushing.
	[[nodiscard]] root::Task<void> nop_async() {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_nop(sqe);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> fsync_async(
		FileHandle const &fh,
		bool data_only = false) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_fsync(sqe, fh.raw_fd(), data_only ? IORING_FSYNC_DATASYNC : 0U);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> fallocate_async(
		FileHandle const &fh,
		int mode,
		u64 offset,
		u64 len) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_fallocate(sqe, fh.raw_fd(), mode, offset, len);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Consumes the handle; the ring closes the fd via io_uring.
	[[nodiscard]] root::Task<void> fadvise_async(
		FileHandle const &fh,
		u64 offset,
		u32 len,
		int advice) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_fadvise(sqe, fd, offset, len, advice);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> madvise_async(
		void *addr,
		u32 length,
		int advice) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_madvise(sqe, addr, length, advice);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> unlink_async(
		int dir_fd,
		S path,
		int flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto path_owner = make_shared<S>(move(path));
		io_uring_prep_unlinkat(sqe, dir_fd, path_owner->c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [path_owner](IoResult) mutable { auto _ = path_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> rename_async(
		int old_dir_fd,
		S old_path,
		int new_dir_fd,
		S new_path,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto paths = make_shared<P<S, S>>(move(old_path), move(new_path));
		io_uring_prep_renameat(sqe, old_dir_fd, paths->first.c_str(), new_dir_fd, paths->second.c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable { auto _ = paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> mkdirat_async(
		int dir_fd,
		S path,
		mode_t mode = 0755) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto path_owner = make_shared<S>(move(path));
		io_uring_prep_mkdirat(sqe, dir_fd, path_owner->c_str(), mode);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [path_owner](IoResult) mutable { auto _ = path_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> symlinkat_async(
		S target,
		int new_dir_fd,
		S link_path) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto paths = make_shared<P<S, S>>(move(target), move(link_path));
		io_uring_prep_symlinkat(sqe, paths->first.c_str(), new_dir_fd, paths->second.c_str());
		auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable { auto _ = paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> ftruncate_async(
		FileHandle const &fh,
		u64 length) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_ftruncate(sqe, fd, static_cast<loff_t>(length));
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> linkat_async(
		int old_dir_fd,
		S old_path,
		int new_dir_fd,
		S new_path,
		int flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto paths = make_shared<P<S, S>>(move(old_path), move(new_path));
		io_uring_prep_linkat(sqe, old_dir_fd, paths->first.c_str(), new_dir_fd, paths->second.c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable { auto _ = paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> sync_file_range_async(
		FileHandle const &fh,
		u64 offset,
		unsigned len,
		int flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_sync_file_range(sqe, fd, len, offset, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]]
	[[nodiscard]] root::Task<FileHandle> socket_async(
		int domain,
		int type,
		int protocol) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_socket(sqe, domain, type, protocol, 0);
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]]
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]]
	[[nodiscard]] root::Task<FileHandle> socket_direct_async(
		int domain,
		int type,
		int protocol,
		unsigned file_index) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_socket_direct(sqe, domain, type, protocol, file_index, 0);
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [file_index](IoResult) {
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]]
	// Create a pipe asynchronously. Returns (read_fd, write_fd) on success.
	[[nodiscard]] root::Task<P<int, int>> pipe_async(
		int pipe_flags = O_CLOEXEC | O_NONBLOCK) {
		auto [task, raw_src] = root::make_task_source<P<int, int>>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<P<int, int>>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto fds = make_shared<A<int, 2>>(A<int, 2>{-1, -1});
		io_uring_prep_pipe(sqe, fds->data(), pipe_flags);
		auto [slot, gen] = completions_->reserve([shared_src, fds](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: pipe"}));
					return;
				}
				auto _ = shared_src->try_set_value({make_pair((*fds)[0], (*fds)[1])});
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Async bind. `addr` is copied and kept alive until CQE.
	[[nodiscard]] root::Task<void> bind_async(
		FileHandle const &fh,
		sockaddr_storage addr,
		socklen_t addrlen) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		auto addr_owner = make_shared<sockaddr_storage>(addr);
		io_uring_prep_bind(sqe, fd, reinterpret_cast<sockaddr *>(addr_owner.get()), addrlen);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(shared_src, [addr_owner](IoResult) mutable { auto _ = addr_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Async listen.
	[[nodiscard]] root::Task<void> listen_async(
		FileHandle const &fh,
		int backlog = SOMAXCONN) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_listen(sqe, fd, backlog);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> shutdown_async(
		FileHandle const &fh,
		int how) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_shutdown(sqe, fd, how);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<SZ> tee_async(
		int fd_in,
		int fd_out,
		SZ len,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_tee(sqe, fd_in, fd_out, static_cast<unsigned int>(len), flags);
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Installs a direct-slot fd into the process fd table. Returns a raw-fd
	// FileHandle wrapping the installed fd. Caller must hold a registered-files
	// table (io_uring_register_files) on this ring.
	[[nodiscard]] root::Task<FileHandle> fixed_fd_install_async(
		FileHandle const &fh,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		if (!fh.is_direct()) {
			auto _ = shared_src->try_set_exception(
				make_exception_ptr(FileIoError{EINVAL, "file_io: fixed_fd_install requires direct slot"}));
			return move(task);
		}
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_fixed_fd_install(sqe, fh.direct_slot(), flags);
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Get extended attribute. `name` is moved and kept alive until CQE.
	// `value` span must remain valid until the returned Flow resolves.
	// Returns the actual attribute size (may exceed value.size() — ERANGE).
	[[nodiscard]] root::Task<SZ> fgetxattr_async(
		FileHandle const &fh,
		S name,
		span<char> buf) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		auto name_owner = make_shared<S>(move(name));
		io_uring_prep_fgetxattr(sqe, fd, name_owner->c_str(), buf.data(), static_cast<unsigned>(buf.size()));
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [name_owner](IoResult r) mutable {
			auto _ = name_owner;
			return static_cast<SZ>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Set extended attribute. Both `name` and `data` are moved/kept alive until CQE.
	[[nodiscard]] root::Task<void> fsetxattr_async(
		FileHandle const &fh,
		S name,
		S data,
		int flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		auto kv = make_shared<P<S, S>>(move(name), move(data));
		io_uring_prep_fsetxattr(
			sqe,
			fd,
			kv->first.c_str(),
			kv->second.c_str(),
			flags,
			static_cast<unsigned>(kv->second.size()));
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(shared_src, [kv](IoResult) mutable { auto _ = kv; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Path-based get extended attribute. `name`, `path`, and `buf` must
	// remain valid until the Flow resolves.
	[[nodiscard]] root::Task<SZ> getxattr_async(
		S path,
		S name,
		span<char> buf) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto kp = make_shared<P<S, S>>(move(path), move(name));
		io_uring_prep_getxattr(
			sqe,
			kp->second.c_str(),
			buf.data(),
			kp->first.c_str(),
			static_cast<unsigned>(buf.size()));
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [kp](IoResult r) mutable {
			auto _ = kp;
			return static_cast<SZ>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Path-based set extended attribute. `name`, `data`, and `path` are moved
	// and kept alive until CQE.
	[[nodiscard]] root::Task<void> setxattr_async(
		S path,
		S name,
		S data,
		int flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		struct XattrState {
			S path;
			S name;
			S data;
		};
		auto st = make_shared<XattrState>(move(path), move(name), move(data));
		io_uring_prep_setxattr(
			sqe,
			st->name.c_str(),
			st->data.c_str(),
			st->path.c_str(),
			flags,
			static_cast<unsigned>(st->data.size()));
		auto [slot, gen] = reserve_bridge<void>(shared_src, [st](IoResult) mutable { auto _ = st; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Wait for process state change (IORING_OP_WAITID). `infop` must stay valid
	// until the Flow resolves; on success it is filled with signal info.
	[[nodiscard]] root::Task<void> waitid_async(
		idtype_t idtype,
		id_t id,
		siginfo_t *infop,
		int options = WEXITED,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_waitid(sqe, idtype, id, infop, options, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Futex wait — waits until *futex != val. The futex pointer must remain
	// valid until the Flow resolves. Returns void on wakeup.
	[[nodiscard]] root::Task<void> futex_wait_async(
		u32 *futex,
		u64 val,
		u64 mask = FUTEX_BITSET_MATCH_ANY,
		u32 futex_flags = FUTEX2_SIZE_U32,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_futex_wait(sqe, futex, val, mask, futex_flags, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Futex wake — wakes up to `val` waiters. Returns the number woken.
	[[nodiscard]] root::Task<u32> futex_wake_async(
		u32 *futex,
		u64 val,
		u64 mask = FUTEX_BITSET_MATCH_ANY,
		u32 futex_flags = FUTEX2_SIZE_U32,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<u32>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<u32>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_futex_wake(sqe, futex, val, mask, futex_flags, flags);
		auto [slot, gen] = reserve_bridge<u32>(shared_src, [](IoResult r) { return static_cast<u32>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Send a synthetic CQE to `target_ring_fd` (the target ring's ring_fd).
	// The CQE on the target will have res=len, user_data=data.
	[[nodiscard]] root::Task<void> msg_ring_async(
		int target_ring_fd,
		unsigned len,
		u64 data,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_msg_ring(sqe, target_ring_fd, len, data, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Arm a one-shot timeout. Resolves (with -ETIME mapped to void) when ms
	// elapses. If count > 0, fires after count CQE completions OR ms, whichever
	// is first (IORING_TIMEOUT_BOOTTIME etc. can be passed in flags).
	[[nodiscard]] root::Task<void> timeout_async(
		chrono::milliseconds ms,
		unsigned count = 0,
		unsigned flags = 0) {
		return conflux::uring::timeout_async(ring_, *completions_, [this](u32 slot, u32 gen) noexcept {
			return encode_ud_(slot, gen);
		}, ms, count, flags);
	}
	// Cancel a running timeout by its user_data tag. -ENOENT → already fired.
	[[nodiscard]] root::Task<void> timeout_remove_async(
		u64 user_data,
		unsigned flags = 0) {
		return conflux::uring::timeout_remove_async(ring_, *completions_, [this](u32 slot, u32 gen) noexcept {
			return encode_ud_(slot, gen);
		}, user_data, flags);
	}
	// Update an armed timeout. New deadline `ms` replaces the existing one.
	// `user_data` identifies the timeout SQE to update (its encoded user_data).
	[[nodiscard]] root::Task<void> timeout_update_async(
		u64 user_data,
		chrono::milliseconds ms,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto ts = make_shared<__kernel_timespec>();
		auto const sec = chrono::duration_cast<chrono::seconds>(ms);
		ts->tv_sec = sec.count();
		ts->tv_nsec = (ms - sec).count() * 1000000LL;
		io_uring_prep_timeout_update(sqe, ts.get(), user_data, flags);
		auto [slot, gen] = completions_->reserve([shared_src, ts](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT && r.res != -EALREADY) {
					auto _ = shared_src->try_set_exception(
						make_exception_ptr(FileIoError{-r.res, "file_io: timeout_update"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
			auto _ = ts;
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Register a one-shot poll on `fd` for `events` (POLLIN, POLLOUT, …).
	// Resolves with the triggered poll mask when any event fires.
	// -ENOENT on poll_remove before the event: treated as ECANCELED by caller.
	[[nodiscard]] root::Task<u32> poll_add_async(
		int fd,
		u32 events) {
		auto [task, raw_src] = root::make_task_source<u32>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<u32>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_poll_add(sqe, fd, events);
		auto [slot, gen] = reserve_bridge<u32>(shared_src, [](IoResult r) { return static_cast<u32>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Cancel a pending poll_add identified by `user_data`.
	// -ENOENT means the poll already fired — treated as success.
	[[nodiscard]] root::Task<void> poll_remove_async(
		u64 user_data) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_poll_remove(sqe, user_data);
		auto [slot, gen] = completions_->reserve([shared_src](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT && r.res != -EALREADY) {
					auto _ =
						shared_src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: poll_remove"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Update an armed multishot poll's event mask in-place.
	// `user_data` is the encoded user_data of the original poll SQE.
	[[nodiscard]] root::Task<void> poll_update_async(
		u64 user_data,
		u32 new_events,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_poll_update(sqe, user_data, 0, new_events, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Accept one connection on a listening socket. Returns the accepted fd.
	// `addr`/`addrlen` are Opt out-params for the peer address.
	[[nodiscard]] root::Task<FileHandle> accept_async(
		FileHandle const &fh,
		sockaddr *addr = nullptr,
		socklen_t *addrlen = nullptr,
		int flags = SOCK_CLOEXEC) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Accept one connection into a registered direct slot.
	// `addr`/`addrlen` are Opt out-params for the peer address.
	[[nodiscard]] root::Task<FileHandle> accept_direct_async(
		FileHandle const &fh,
		unsigned file_index,
		sockaddr *addr = nullptr,
		socklen_t *addrlen = nullptr,
		int flags = SOCK_CLOEXEC) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_accept_direct(sqe, fd, addr, addrlen, flags, file_index);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [file_index](IoResult) {
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Send an fd to another ring's registered file table. `source_fd` is
	// installed at `target_fd` slot in the target ring's file table.
	// Pass IORING_FILE_INDEX_ALLOC for `target_fd` to auto-allocate.
	[[nodiscard]] root::Task<void> msg_ring_fd_async(
		int target_ring_fd,
		int source_fd,
		int target_fd,
		u64 data = 0,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_msg_ring_fd(sqe, target_ring_fd, source_fd, target_fd, data, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Wait on multiple futexes simultaneously. Resolves when any waiter
	// condition is met. `waiters` is moved and kept alive until CQE.
	[[nodiscard]] root::Task<void> futex_waitv_async(
		V<futex_waitv> waiters,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto wv = make_shared<V<futex_waitv>>(move(waiters));
		io_uring_prep_futex_waitv(sqe, wv->data(), static_cast<u32>(wv->size()), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [wv](IoResult) mutable { auto _ = wv; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Cancel a pending op by its user_data tag. Resolves when the cancel
	// was submitted; the target op's CQE will still arrive (with -ECANCELED).
	// -ENOENT means the target already completed — treated as success here.
	[[nodiscard]] root::Task<void> cancel_async(
		u64 user_data,
		int flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_cancel64(sqe, user_data, flags);
		auto [slot, gen] = completions_->reserve([shared_src](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT) {
					auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: cancel"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Cancel all pending ops for `fd`. -ENOENT treated as success.
	[[nodiscard]] root::Task<void> cancel_fd_async(
		int fd,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_cancel_fd(sqe, fd, flags);
		auto [slot, gen] = completions_->reserve([shared_src](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT) {
					auto _ =
						shared_src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: cancel_fd"}));
					return;
				}
				auto _ = shared_src->try_set_value(root::Success<void>{});
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Async connect. `addr` is copied into a shared buffer kept alive until CQE.
	[[nodiscard]] root::Task<void> connect_async(
		FileHandle const &fh,
		sockaddr_storage addr,
		socklen_t addrlen) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		auto addr_owner = make_shared<sockaddr_storage>(addr);
		io_uring_prep_connect(sqe, fd, reinterpret_cast<sockaddr *>(addr_owner.get()), addrlen);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(shared_src, [addr_owner](IoResult) mutable { auto _ = addr_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<void> close_async(
		FileHandle fh) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		if (fh.is_direct()) {
			io_uring_prep_close_direct(sqe, static_cast<unsigned>(fh.release_direct_slot()));
		} else {
			io_uring_prep_close(sqe, fh.release_fd());
		}
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
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
	[[nodiscard]] root::Task<SZ> splice_to_fd(
		FileHandle const &file,
		u64 off,
		SZ len,
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
			u64 file_off;
			SZ remaining;
			SZ delivered{0};
			SP<root::TaskSource<SZ>> src;
		};
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto st = make_shared<State>(State{
			.ring = ring_,
			.completions = completions_,
			.encode_ud = encode_ud_,
			.file_fd = file.raw_fd(),
			.dst_fd = dst_fd,
			.dst_fixed = dst_fixed,
			.pipe = move(pipe),
			.file_off = off,
			.remaining = len,
			.delivered = 0,
			.src = make_shared<root::TaskSource<SZ>>(move(raw_src))});
		step_splice(st);
		return move(task);
	}
	// Send `len` bytes from `buf` on `fh`. Returns bytes sent.
	[[nodiscard]] root::Task<SZ> send_async(
		FileHandle const &fh,
		void const *buf,
		SZ len,
		int flags = 0) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_send(sqe, fd, buf, len, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Receive up to `len` bytes into `buf` from `fh`. Returns bytes received.
	[[nodiscard]] root::Task<SZ> recv_async(
		FileHandle const &fh,
		void *buf,
		SZ len,
		int flags = 0) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_recv(sqe, fd, buf, len, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Vectored send via sendmsg(2). `msg` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<SZ> sendmsg_async(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_sendmsg(sqe, fd, msg, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Vectored recv via recvmsg(2). `msg` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<SZ> recvmsg_async(
		FileHandle const &fh,
		msghdr *msg,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_recvmsg(sqe, fd, msg, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Register `nr` buffers of `len` bytes starting at `addr` into buffer group `bgid`.
	// Kernel increments `bid` automatically for subsequent provides in the same group.
	[[nodiscard]] root::Task<void> provide_buffers_async(
		void *addr,
		int len,
		int nr,
		int bgid,
		int bid = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_provide_buffers(sqe, addr, len, nr, bgid, bid);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Remove `nr` buffers from buffer group `bgid`.
	[[nodiscard]] root::Task<void> remove_buffers_async(
		int nr,
		int bgid) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_remove_buffers(sqe, nr, bgid);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Update the registered file table. `fds` is a span of `nr_fds` fds starting
	// at `offset` in the kernel's registered file A. -1 entries remove a slot.
	// `fds` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<void> files_update_async(
		int *fds,
		unsigned nr_fds,
		int offset = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_files_update(sqe, fds, nr_fds, offset);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Modify an epoll interest list entry. `ev` may be null for EPOLL_CTL_DEL.
	[[nodiscard]] root::Task<void> epoll_ctl_async(
		int epfd,
		int fd,
		int op,
		epoll_event const *ev = nullptr) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_epoll_ctl(sqe, epfd, fd, op, ev);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Wait for epoll events. Resolves with the number of events returned.
	// `events` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<int> epoll_wait_async(
		int epfd,
		epoll_event *events,
		int maxevents,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<int>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<int>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_epoll_wait(sqe, epfd, events, maxevents, flags);
		auto [slot, gen] = reserve_bridge<int>(shared_src, [](IoResult r) { return r.res; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Attach a timeout to the preceding SQE in the ring's submission chain.
	// The preceding SQE must have been submitted with IOSQE_IO_LINK.
	// Resolves when the link fires (either the linked op completed or the
	// timeout expired — -ETIME in the latter case is treated as success).
	[[nodiscard]] root::Task<void> link_timeout_async(
		chrono::milliseconds ms,
		unsigned flags = 0) {
		return conflux::uring::link_timeout_async(ring_, *completions_, [this](u32 slot, u32 gen) noexcept {
			return encode_ud_(slot, gen);
		}, ms, flags);
	}
	// Open a file with full openat2(2) semantics (`open_how` struct).
	// `how` is copied internally so the caller need not keep it alive.
	[[nodiscard]] root::Task<FileHandle> openat2_async(
		int dir_fd,
		S path,
		open_how how) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto ctx = make_shared<P<S, open_how>>(move(path), how);
		io_uring_prep_openat2(sqe, dir_fd, ctx->first.c_str(), &ctx->second);
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [ctx](IoResult r) mutable {
			auto _ = ctx;
			return FileHandle::from_fd(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Send with destination address — for SOCK_DGRAM sockets.
	// `addr` is copied internally; `buf` must remain valid until the Flow resolves.
	[[nodiscard]] root::Task<SZ> sendto_async(
		FileHandle const &fh,
		void const *buf,
		SZ len,
		int flags,
		sockaddr_storage addr,
		socklen_t addrlen) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto sa = make_shared<sockaddr_storage>(addr);
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_sendto(sqe, fd, buf, len, flags, reinterpret_cast<sockaddr *>(sa.get()), addrlen);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [sa](IoResult r) mutable {
			auto _ = sa;
			return static_cast<SZ>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// hack: resolves on first send CQE only; this API does not expose buffer-release notification.
	// Caller must guarantee the buffer remains live by other means.
	[[nodiscard]] root::Task<SZ> unsafe_send_zc_sent_async(
		FileHandle const &fh,
		void const *buf,
		SZ len,
		int flags = 0,
		unsigned zc_flags = 0) {
#ifdef IORING_SEND_ZC_REPORT_USAGE
		zc_flags &= ~IORING_SEND_ZC_REPORT_USAGE;
#endif
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_send_zc(sqe, fd, buf, len, flags, zc_flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	[[nodiscard]] root::Task<SZ> send_zc_async(
		FileHandle const &fh,
		void const *buf,
		SZ len,
		int flags = 0,
		unsigned zc_flags = 0) {
#ifdef IORING_SEND_ZC_REPORT_USAGE
		zc_flags &= ~IORING_SEND_ZC_REPORT_USAGE;
#endif
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_send_zc(sqe, fd, buf, len, flags, zc_flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_zc_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Write using a pre-registered fixed buffer (IORING_OP_WRITE_FIXED).
	// `buf` pointer and `buf_index` must refer to the registered buffer in the pool.
	[[nodiscard]] root::Task<SZ> write_fixed_async(
		FileHandle const &fh,
		u64 offset,
		void const *buf,
		unsigned nbytes,
		int buf_index) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_write_fixed(sqe, fd, buf, nbytes, offset, buf_index);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Remove a file by name relative to `dir_fd`.
	// `flags` = 0 for file; AT_REMOVEDIR for directory.
	[[nodiscard]] root::Task<void> unlinkat_async(
		int dir_fd,
		S path,
		int flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto p = make_shared<S>(move(path));
		io_uring_prep_unlinkat(sqe, dir_fd, p->c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [p](IoResult) mutable { auto _ = p; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Rename with full dirfd control.
	[[nodiscard]] root::Task<void> renameat_async(
		int old_dir_fd,
		S old_path,
		int new_dir_fd,
		S new_path,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto paths = make_shared<P<S, S>>(move(old_path), move(new_path));
		io_uring_prep_renameat(sqe, old_dir_fd, paths->first.c_str(), new_dir_fd, paths->second.c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable { auto _ = paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Create a directory at `path` (relative to AT_FDCWD).
	[[nodiscard]] root::Task<void> mkdir_async(
		S path,
		mode_t mode = 0755) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto p = make_shared<S>(move(path));
		io_uring_prep_mkdir(sqe, p->c_str(), mode);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [p](IoResult) mutable { auto _ = p; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Open directly into the registered file table with full openat2 semantics.
	// IORING_FILE_INDEX_ALLOC for `file_index` auto-allocates.
	[[nodiscard]] root::Task<FileHandle> openat2_direct_async(
		int dir_fd,
		S path,
		open_how how,
		unsigned file_index) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto ctx = make_shared<P<S, open_how>>(move(path), how);
		io_uring_prep_openat2_direct(sqe, dir_fd, ctx->first.c_str(), &ctx->second, file_index);
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [ctx, file_index](IoResult) mutable {
			auto _ = ctx;
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Create a socket directly into the registered file table, with the kernel
	// choosing the slot (IORING_FILE_INDEX_ALLOC). Returns the allocated slot.
	[[deprecated("use socket_io::tcp_connect/tcp_accept paths instead")]]
	[[nodiscard]] root::Task<FileHandle> socket_direct_alloc_async(
		int domain,
		int type,
		int protocol,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_socket_direct_alloc(sqe, domain, type, protocol, flags);
		auto [slot, gen] =
			reserve_bridge<FileHandle>(shared_src, [](IoResult r) { return FileHandle::from_direct_slot(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Open a file directly into the fixed file table (openat semantics).
	// Use IORING_FILE_INDEX_ALLOC for `file_index` to let the kernel pick a slot.
	[[nodiscard]] root::Task<FileHandle> openat_direct_async(
		int dir_fd,
		S path,
		int flags,
		mode_t mode = 0,
		unsigned file_index = IORING_FILE_INDEX_ALLOC) {
		auto [task, raw_src] = root::make_task_source<FileHandle>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<FileHandle>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto p = make_shared<S>(move(path));
		io_uring_prep_openat_direct(sqe, dir_fd, p->c_str(), flags, mode, file_index);
		auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [p, file_index](IoResult r) mutable {
			auto _ = p;
			// When IORING_FILE_INDEX_ALLOC: res carries the allocated slot.
			int const s = (file_index == IORING_FILE_INDEX_ALLOC) ? r.res : static_cast<int>(file_index);
			return FileHandle::from_direct_slot(s);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Send a source fd to another ring, letting the kernel auto-allocate the slot.
	// Returns the allocated slot index via the target ring's CQE.
	[[nodiscard]] root::Task<void> msg_ring_fd_alloc_async(
		int target_ring_fd,
		int source_fd,
		u64 data = 0,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_msg_ring_fd_alloc(sqe, target_ring_fd, source_fd, data, flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Create a pipe P directly into fixed file table slots.
	// Use IORING_FILE_INDEX_ALLOC for `file_index` to let the kernel choose.
	// The two slots are allocated consecutively.
	[[nodiscard]] root::Task<P<int, int>> pipe_direct_async(
		unsigned file_index = IORING_FILE_INDEX_ALLOC,
		int pipe_flags = O_CLOEXEC | O_NONBLOCK) {
		auto [task, raw_src] = root::make_task_source<P<int, int>>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<P<int, int>>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		auto fds = make_shared<A<int, 2>>(A<int, 2>{-1, -1});
		io_uring_prep_pipe_direct(sqe, fds->data(), pipe_flags, file_index);
		auto [slot, gen] = completions_->reserve([shared_src, fds](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ =
						shared_src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: pipe_direct"}));
					return;
				}
				auto _ = shared_src->try_set_value({make_pair((*fds)[0], (*fds)[1])});
			} catch (...) { auto _ = shared_src->try_set_exception(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// Post a message to another ring, forwarding specific CQE flags in the payload.
	// Useful for waking up a consumer ring with custom CQE flags set.
	[[nodiscard]] root::Task<void> msg_ring_cqe_flags_async(
		int target_ring_fd,
		unsigned len,
		u64 data,
		unsigned flags = 0,
		unsigned cqe_flags = 0) {
		auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<void>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		io_uring_prep_msg_ring_cqe_flags(sqe, target_ring_fd, len, data, flags, cqe_flags);
		auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// hack: resolves on first send CQE only; this API does not expose buffer-release notification.
	// Caller must guarantee msg, iovec array, and all pointed buffers remain live by other means.
	[[nodiscard]] root::Task<SZ> unsafe_sendmsg_zc_sent_async(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_sendmsg_zc(sqe, fd, msg, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}
	// msg, iovec array, and all pointed buffers must remain live until co_return.
	[[nodiscard]] root::Task<SZ> sendmsg_zc_async(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		auto [task, raw_src] = root::make_task_source<SZ>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = make_shared<root::TaskSource<SZ>>(move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return move(task);
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_sendmsg_zc(sqe, fd, msg, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_zc_bridge<SZ>(shared_src, [](IoResult r) { return static_cast<SZ>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return move(task);
	}

private:
	[[nodiscard]] root::Task<FileHandle> open_atomic_parent_dir_async(
		int root_dir_fd,
		S parent_dir) {
		open_how how{};
		how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
		how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
		co_return co_await openat2_async(root_dir_fd, move(parent_dir), how);
	}
	[[nodiscard]] root::Task<FileHandle> open_atomic_payload_async(
		int parent_dir_fd,
		S staging_name,
		mode_t mode,
		bool &staging_entry_exists) {
		open_how how{};
		how.flags = O_TMPFILE | O_WRONLY | O_CLOEXEC;
		how.mode = static_cast<__u64>(mode);
		how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;

		try {
			co_return co_await openat2_async(parent_dir_fd, S{"."}, how);
		} catch (FileIoError const &e) {
			if (!is_otmpfile_unsupported_errno_async(e.code().value())) {
				throw;
			}
		}

		open_how fallback{};
		fallback.flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
		fallback.mode = static_cast<__u64>(mode);
		fallback.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
		auto fh = co_await openat2_async(parent_dir_fd, move(staging_name), fallback);
		staging_entry_exists = true;
		co_return fh;
	}
	[[nodiscard]] root::Task<void> link_atomic_payload_async(
		FileHandle const &fh,
		int parent_dir_fd,
		S staging_name) {
		try {
			co_await linkat_async(fh.raw_fd(), S{}, parent_dir_fd, S{staging_name}, AT_EMPTY_PATH);
			co_return;
		} catch (FileIoError const &e) {
			int const code = e.code().value();
			if (code != EPERM && code != EINVAL && code != ENOENT) {
				throw;
			}
		}
		co_await linkat_async(
			AT_FDCWD,
			format("/proc/self/fd/{}", fh.raw_fd()),
			parent_dir_fd,
			move(staging_name),
			AT_SYMLINK_FOLLOW);
	}
	template<typename StatePtr>
	static void step_splice(
		StatePtr const &st) {
		if (st->remaining == 0) {
			auto _ = st->src->try_set_value(root::Success<SZ>{st->delivered});
			return;
		}
		SZ const chunk = min(st->remaining, st->pipe.capacity());
		auto *sqe_in = io_uring_get_sqe(st->ring);
		auto *sqe_out = io_uring_get_sqe(st->ring);
		if (sqe_in == nullptr || sqe_out == nullptr) {
			auto _ = st->src->try_set_exception(make_exception_ptr(FileIoError{ENOSPC, "file_io: splice SQ full"}));
			return;
		}

		io_uring_prep_splice(
			sqe_in,
			st->file_fd,
			static_cast<i64>(st->file_off),
			st->pipe.write_fd(),
			-1,
			static_cast<unsigned>(chunk),
			SPLICE_F_MOVE | SPLICE_F_MORE);
		sqe_in->flags |= IOSQE_IO_LINK;
		auto [slot_in, gen_in] = st->completions->reserve([st](IoResult r) mutable {
			if (r.res < 0 && r.res != -ECANCELED) {
				auto _ = st->src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: splice in"}));
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
				auto _ = st->src->try_set_exception(make_exception_ptr(FileIoError{-r.res, "file_io: splice out"}));
				return;
			}
			auto const n = static_cast<SZ>(r.res);
			st->delivered += n;
			st->file_off += n;
			st->remaining = st->remaining > n ? st->remaining - n : 0;
			step_splice(st);
		});
		io_uring_sqe_set_data64(sqe_out, st->encode_ud(slot_out, gen_out));
	}

public:
	[[nodiscard]] root::Task<void> atomic_write_async(
		int dir_fd,
		S rel_path,
		span<byte const> data,
		mode_t mode = 0644,
		TempPublishMode pub_mode = TempPublishMode::replace_existing,
		TempDurability durability = TempDurability::file_and_directory) {
		auto parts = split_contained_atomic_path_async(rel_path);
		if (!parts) {
			throw parts.error();
		}

		auto parent_fh = co_await open_atomic_parent_dir_async(dir_fd, move(parts->parent_dir));
		int const parent_fd = parent_fh.raw_fd();
		S staging = make_staging_name_async();
		bool staging_entry_exists = false;

		std::exception_ptr cleanup_error;
		try {
			auto fh = co_await open_atomic_payload_async(parent_fd, S{staging}, mode, staging_entry_exists);

			SZ off = 0;
			while (off < data.size()) {
				auto wrote = co_await write_into(fh, off, data.subspan(off));
				if (wrote == 0) {
					throw FileIoError{EIO, "file_io: short write"};
				}
				off += wrote;
			}

			if (durability >= TempDurability::file) {
				co_await fsync_async(fh, true);
			}

			if (!staging_entry_exists) {
				co_await link_atomic_payload_async(fh, parent_fd, S{staging});
				staging_entry_exists = true;
			}

			if (pub_mode == TempPublishMode::replace_existing) {
				co_await renameat_async(parent_fd, S{staging}, parent_fd, move(parts->basename));
			} else {
				co_await renameat_async(parent_fd, S{staging}, parent_fd, move(parts->basename), RENAME_NOREPLACE);
			}
			staging_entry_exists = false;

			if (durability >= TempDurability::file_and_directory) {
				co_await fsync_async(parent_fh);
			}
		} catch (...) {
			cleanup_error = current_exception();
		}
		if (staging_entry_exists) {
			try {
				co_await unlinkat_async(parent_fd, S{staging});
			} catch (...) {} // NOLINT(bugprone-empty-catch)
		}
		if (cleanup_error) {
			rethrow_exception(cleanup_error);
		}
	}
};
// ---------------------------------------------------------------------------
// Thread-local FileReader registration.
//
// Each ring runs on a dedicated thread; the ring's FileReader is installed at
// run_loop entry and cleared at exit. Handlers that live inside the router
// (no ring context of their own) look up the current reader to decide between
// async uring paths and synchronous fallbacks.
// ---------------------------------------------------------------------------

namespace {

thread_local FileReader *tls_current_reader{nullptr};

} // namespace
export FileReader *current_file_reader() noexcept {
	return tls_current_reader;
}
export class CurrentFileReaderScope {
	FileReader *prev_;

public:
	explicit CurrentFileReaderScope(
		FileReader *next) noexcept
		: prev_{tls_current_reader} {
		tls_current_reader = next;
	}
	~CurrentFileReaderScope() { tls_current_reader = prev_; }
	CurrentFileReaderScope(CurrentFileReaderScope const &) = delete;
	CurrentFileReaderScope &operator =(CurrentFileReaderScope const &) = delete;
	CurrentFileReaderScope(CurrentFileReaderScope &&) = delete;
	CurrentFileReaderScope &operator =(CurrentFileReaderScope &&) = delete;
};
// ---------------------------------------------------------------------------
// Single-thread io_uring driver: pump_until + block_on.
//
// Tests and examples all rolled their own submit/wait_cqe/dispatch loop.
// These primitives factor out that loop and the Flow→atomic_flag plumbing.
// HTTP server keeps its own driver because it shares the ring with non-
// file_io ops (Op-tagged user_data); this helper assumes the ring is owned
// solely by FileReader and uses the default 32:32 ud layout unless a
// caller-provided decoder says otherwise.
// ---------------------------------------------------------------------------

export struct DefaultUdDecoder {
	P<u32, u32> operator ()(
		u64 ud) const noexcept {
		return {static_cast<u32>(ud & 0xFFFFFFFFU), static_cast<u32>(ud >> 32U)};
	}
};
export struct PumpTimeout final : RE {
	PumpTimeout()
		: RE{"conflux.file_io: pump_until budget exhausted"} {}
};
export template<typename Decode = DefaultUdDecoder>
void pump_until(
	FileReader &reader,
	atomic_flag const &done,
	Opt<chrono::milliseconds> budget = nullopt,
	Decode decode = {}) {
	auto *ring = reader.ring();
	auto *completions = reader.completions();
	auto const deadline = budget ? std::make_optional(chrono::steady_clock::now() + *budget) : nullopt;
	while (!done.test(memory_order_acquire)) {
		::io_uring_cqe *cqe = nullptr;
		int rc = 0;
		if (deadline) {
			__kernel_timespec ts{.tv_sec = 1, .tv_nsec = 0};
			rc = ::io_uring_submit_and_wait_timeout(ring, &cqe, 1, &ts, nullptr);
			if (rc == -ETIME) {
				if (chrono::steady_clock::now() > *deadline) {
					throw PumpTimeout{};
				}
				continue;
			}
		} else {
			rc = ::io_uring_submit_and_wait(ring, 1);
			if (rc >= 0) {
				rc = ::io_uring_peek_cqe(ring, &cqe);
			}
		}
		if (rc == -EINTR) {
			continue;
		}
		// io_uring_submit_and_wait may report submitted SQEs while no CQE is
		// immediately visible to peek_cqe yet. Treat as transient and keep
		// pumping instead of surfacing a hard failure.
		if (rc >= 0 && cqe == nullptr) {
			continue;
		}
		if (rc < 0 || cqe == nullptr) {
			throw RE{format("conflux.file_io: submit_and_wait rc={}", rc)};
		}
		A<::io_uring_cqe *, 32> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(ring, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				auto const *c = batch[static_cast<SZ>(i)];
				auto [slot, gen] = decode(c->user_data);
				completions->dispatch(slot, gen, c->res, c->flags);
			}
			::io_uring_cq_advance(ring, n);
			if (done.test(memory_order_acquire)) {
				break;
			}
		}
	}
}
export template<typename T, typename Decode = DefaultUdDecoder>
T block_on(
	FileReader &reader,
	conflux::work::root::Task<T> task,
	Opt<chrono::milliseconds> budget = nullopt,
	Decode decode = {}) {
	using namespace conflux::work::root;
	struct Slot {
		atomic_flag done{};
		EP err{};
		[[no_unique_address]] std::conditional_t<std::is_void_v<T>, std::monostate, Opt<T>> value{};
	};
	auto slot = make_shared<Slot>();
	auto jh = make_shared<TaskJoinHandle<T>>(into_join_handle(move(task)));
	jh->control().set_on_ready_or_run([slot, jh]() noexcept {
		try {
			auto outcome = join(move(*jh));
			if (outcome.is_failure()) {
				slot->err = move(outcome).failure().error;
			} else if (outcome.is_cancelled()) {
				slot->err = make_exception_ptr(::Cancelled{});
			} else if constexpr (!std::is_void_v<T>) {
				slot->value.emplace(move(outcome).success().value);
			}
		} catch (...) { slot->err = current_exception(); }
		slot->done.test_and_set(memory_order_release);
	});
	pump_until(reader, slot->done, budget, move(decode));
	if (slot->err) {
		rethrow_exception(slot->err);
	}
	if constexpr (!std::is_void_v<T>) {
		return move(*slot->value);
	}
}
