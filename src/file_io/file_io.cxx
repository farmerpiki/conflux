module;

#include <cstdio>
#include <fcntl.h>
#include <liburing.h>
#include <linux/futex.h>
#include <linux/stat.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/mman.h>
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

using namespace std;

// ---------------------------------------------------------------------------
// User-data encoder.
//
// The caller controls the full io_uring user_data layout; the library only
// knows how to address its own CompletionTable slots. The encoder packs a
// (slot, gen) pair into the 64 bits the caller owns — e.g. an HTTP server
// packs pack(Op::FileIo, gen, slot) using its existing 8+24+32 layout.
// ---------------------------------------------------------------------------

export using UserDataFn = function<u64(u32 slot, u32 gen)>;

// ---------------------------------------------------------------------------
// Outcome of a single io_uring completion as seen by the library.
// ---------------------------------------------------------------------------

export struct IoResult {
	i32 res{};
	u32 flags{};
};

export using FileCompletionFn = function<void(IoResult)>;

// ---------------------------------------------------------------------------
// CompletionTable: slot-indexed store of pending completion callbacks with
// per-slot gen counter for stale-CQE rejection. Not thread-safe — pinned to
// the ring's owner thread (SINGLE_ISSUER).
// ---------------------------------------------------------------------------

export class CompletionTable {
	struct Slot {
		u32 gen{0};
		bool in_use{false};
		bool multishot{false};
		FileCompletionFn fn{};
	};

	vector<Slot> slots_{};
	vector<u32> free_{};

public:
	explicit CompletionTable(
		size_t initial_capacity = 64) {
		slots_.reserve(initial_capacity);
	}

	CompletionTable(CompletionTable const &) = delete;
	CompletionTable &operator =(CompletionTable const &) = delete;
	CompletionTable(CompletionTable &&) = delete;
	CompletionTable &operator =(CompletionTable &&) = delete;
	~CompletionTable() {} // NOLINT(modernize-use-equals-default) — GCC module bug

	[[nodiscard]] pair<u32, u32> reserve(
		FileCompletionFn fn) {
		u32 slot = 0;
		if (!free_.empty()) {
			slot = free_.back();
			free_.pop_back();
		} else {
			slot = static_cast<u32>(slots_.size());
			slots_.emplace_back();
		}
		auto &s = slots_[slot];
		s.in_use = true;
		s.multishot = false;
		s.fn = move(fn);
		return {slot, s.gen};
	}

	[[nodiscard]] pair<u32, u32> reserve_multishot(
		FileCompletionFn fn) {
		auto [slot, gen] = reserve(move(fn));
		slots_[slot].multishot = true;
		return {slot, gen};
	}

	void dispatch(
		// NOLINT(bugprone-exception-escape) — callbacks are noexcept by contract
		u32 slot,
		u32 gen,
		int res,
		u32 flags) noexcept {
		if (slot >= slots_.size()) {
			return;
		}
		auto &s = slots_[slot];
		if (!s.in_use || s.gen != gen) {
			return;
		}
		if (s.multishot && res >= 0 && (flags & IORING_CQE_F_MORE) != 0U) {
			if (s.fn) {
				s.fn(IoResult{.res = res, .flags = flags});
			}
			return;
		}
		auto fn = move(s.fn);
		s.fn = {};
		s.in_use = false;
		s.multishot = false;
		++s.gen;
		free_.push_back(slot);
		if (fn) {
			fn(IoResult{.res = res, .flags = flags});
		}
	}

	void cancel_all() noexcept { // NOLINT(bugprone-exception-escape) — callbacks are noexcept by contract
		for (u32 slot = 0; slot < slots_.size(); ++slot) {
			auto &s = slots_[slot];
			if (!s.in_use) {
				continue;
			}
			auto fn = move(s.fn);
			s.fn = {};
			s.in_use = false;
			s.multishot = false;
			++s.gen;
			free_.push_back(slot);
			if (fn) {
				fn(IoResult{.res = -ECANCELED, .flags = 0});
			}
		}
	}

	[[nodiscard]] size_t pending() const noexcept { return slots_.size() - free_.size(); }
};

// ---------------------------------------------------------------------------
// FileHandle: RAII owner of a direct-descriptor slot or a plain fd.
// Direct slots MUST be released via FileReader::close_async before dropping
// the handle, otherwise the slot leaks for the lifetime of the ring.
// ---------------------------------------------------------------------------

export class FileHandle {
	int fd_{-1};
	int direct_slot_{-1};

public:
	FileHandle() noexcept {} // NOLINT(modernize-use-equals-default) — GCC module bug

	static FileHandle from_fd(
		int fd) noexcept {
		FileHandle h;
		h.fd_ = fd;
		return h;
	}

	static FileHandle from_direct_slot(
		int slot) noexcept {
		FileHandle h;
		h.direct_slot_ = slot;
		return h;
	}

	FileHandle(FileHandle const &) = delete;
	FileHandle &operator =(FileHandle const &) = delete;

	FileHandle(
		FileHandle &&o) noexcept
		: fd_{exchange(o.fd_, -1)}
		, direct_slot_{exchange(o.direct_slot_, -1)} {}

	FileHandle &operator =(
		FileHandle &&o) noexcept {
		if (this != &o) {
			close_on_drop();
			fd_ = exchange(o.fd_, -1);
			direct_slot_ = exchange(o.direct_slot_, -1);
		}
		return *this;
	}

	~FileHandle() { close_on_drop(); }

	[[nodiscard]] int raw_fd() const noexcept { return fd_; }
	[[nodiscard]] int direct_slot() const noexcept { return direct_slot_; }
	[[nodiscard]] bool is_direct() const noexcept { return direct_slot_ >= 0; }
	[[nodiscard]] bool valid() const noexcept { return fd_ >= 0 || direct_slot_ >= 0; }

	int release_fd() noexcept { return exchange(fd_, -1); }
	int release_direct_slot() noexcept { return exchange(direct_slot_, -1); }

private:
	void close_on_drop() noexcept {
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
		// Direct slot: can only be released via io_uring_prep_close_direct, which
		// requires a ring reference. If still set at drop, caller forgot to call
		// FileReader::close_async — we cannot recover the slot here.
#ifndef NDEBUG
		if (direct_slot_ >= 0) {
			std::fputs(
				"FileHandle dropped with live direct slot — "
				"FileReader::close_async was never called; slot will leak\n",
				stderr);
		}
#endif
		direct_slot_ = -1;
	}
};

// ---------------------------------------------------------------------------
// FileStat: subset of struct statx fields the HTTP/file serving code needs.
// ---------------------------------------------------------------------------

export struct FileStat {
	u64 size{};
	u64 mtime_ns{};
	u64 dev{};
	u64 ino{};
	u32 mode{};
};

// ---------------------------------------------------------------------------
// FixedBuffer / FixedBufferPool: pre-registered page-aligned slabs for
// IORING_OP_{READ,WRITE}_FIXED. A FixedBuffer is a RAII lease on one slot;
// returning the lease pushes the slot back to the pool's free-list.
// ---------------------------------------------------------------------------

export class FixedBufferPool;

export class FixedBuffer {
	FixedBufferPool *pool_{nullptr};
	unsigned slot_{0};
	span<byte> view_{};

	FixedBuffer(
		FixedBufferPool *pool,
		unsigned slot,
		span<byte> view) noexcept
		: pool_{pool}
		, slot_{slot}
		, view_{view} {}

	friend class FixedBufferPool;

public:
	FixedBuffer() noexcept {} // NOLINT(modernize-use-equals-default) — GCC module bug
	FixedBuffer(FixedBuffer const &) = delete;
	FixedBuffer &operator =(FixedBuffer const &) = delete;

	FixedBuffer(
		FixedBuffer &&o) noexcept
		: pool_{exchange(o.pool_, nullptr)}
		, slot_{o.slot_}
		, view_{o.view_} {}

	FixedBuffer &operator =(FixedBuffer &&o) noexcept;
	~FixedBuffer();

	[[nodiscard]] bool valid() const noexcept { return pool_ != nullptr; }
	[[nodiscard]] unsigned slot() const noexcept { return slot_; }
	[[nodiscard]] span<byte> view() const noexcept { return view_; }
	[[nodiscard]] size_t size() const noexcept { return view_.size(); }
};

export class FixedBufferPool {
	io_uring *ring_;
	size_t slab_bytes_;
	vector<unique_ptr<byte[], void (*)(void *)>> slabs_{}; // aligned_alloc'd
	vector<unsigned> free_{};
	bool registered_{false};

	friend class FixedBuffer;

	void release(
		// NOLINT(bugprone-exception-escape) — free_ is pre-sized; push_back never reallocates
		unsigned slot) noexcept {
		free_.push_back(slot);
	}

public:
	FixedBufferPool(
		io_uring *ring,
		size_t slab_count,
		size_t slab_bytes)
		: ring_{ring}
		, slab_bytes_{slab_bytes} {
		if (slab_count == 0 || slab_bytes == 0) {
			return;
		}
		long const page = ::sysconf(_SC_PAGESIZE);
		size_t const align = page > 0 ? static_cast<size_t>(page) : size_t{4096};
		size_t const aligned_bytes = ((slab_bytes + align - 1) / align) * align;
		slab_bytes_ = aligned_bytes;
		slabs_.reserve(slab_count);
		free_.reserve(slab_count);
		if (io_uring_register_buffers_sparse(ring_, static_cast<unsigned>(slab_count)) < 0) {
			return;
		}
		registered_ = true;
		for (size_t i = 0; i < slab_count; ++i) {
			void *raw = ::aligned_alloc(align, aligned_bytes);
			if (raw == nullptr) {
				break;
			}
			slabs_.emplace_back(static_cast<byte *>(raw), +[](void *p) { ::free(p); });
			iovec const iov{.iov_base = raw, .iov_len = aligned_bytes};
			if (io_uring_register_buffers_update_tag(ring_, static_cast<unsigned>(i), &iov, nullptr, 1) < 0) {
				break;
			}
			free_.push_back(static_cast<unsigned>(i));
		}
	}

	FixedBufferPool(FixedBufferPool const &) = delete;
	FixedBufferPool &operator =(FixedBufferPool const &) = delete;
	FixedBufferPool(FixedBufferPool &&) = delete;
	FixedBufferPool &operator =(FixedBufferPool &&) = delete;

	~FixedBufferPool() {
		if (registered_) {
			io_uring_unregister_buffers(ring_);
		}
	}

	[[nodiscard]] bool ok() const noexcept { return registered_; }
	[[nodiscard]] size_t capacity() const noexcept { return slabs_.size(); }
	[[nodiscard]] size_t available() const noexcept { return free_.size(); }
	[[nodiscard]] size_t slab_bytes() const noexcept { return slab_bytes_; }

	[[nodiscard]] optional<FixedBuffer> try_acquire() {
		if (free_.empty()) {
			return nullopt;
		}
		unsigned const slot = free_.back();
		free_.pop_back();
		return FixedBuffer{
			this,
			slot,
			span{slabs_[slot].get(), slab_bytes_}
        };
	}
};

inline FixedBuffer &FixedBuffer::operator =(
	FixedBuffer &&o) noexcept {
	if (this != &o) {
		if (pool_ != nullptr) {
			pool_->release(slot_);
		}
		pool_ = exchange(o.pool_, nullptr);
		slot_ = o.slot_;
		view_ = o.view_;
	}
	return *this;
}

inline FixedBuffer::~FixedBuffer() {
	if (pool_ != nullptr) {
		pool_->release(slot_);
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
	size_t capacity_{0};

	PipePair(
		PipePool *pool,
		u32 slot,
		int read_fd,
		int write_fd,
		size_t capacity) noexcept
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
	[[nodiscard]] size_t capacity() const noexcept { return capacity_; }
};

export class PipePool {
	struct Pair {
		int read_fd;
		int write_fd;
		size_t capacity;
	};

	vector<Pair> pairs_{};
	vector<u32> free_{};

	friend class PipePair;

	void release(
		// NOLINT(bugprone-exception-escape) — free_ is pre-sized; push_back never reallocates
		u32 slot) noexcept {
		free_.push_back(slot);
	}

public:
	explicit PipePool(
		size_t pair_count) {
		pairs_.reserve(pair_count);
		free_.reserve(pair_count);
		for (size_t i = 0; i < pair_count; ++i) {
			array<int, 2> fds{-1, -1};
			// O_DIRECT = packet-mode pipes: each write yields a distinct read,
			// exactly what splice with SPLICE_F_MOVE expects. Falls back to byte
			// stream if kernel rejects (rare).
			if (::pipe2(fds.data(), O_DIRECT | O_CLOEXEC) < 0 && ::pipe2(fds.data(), O_CLOEXEC) < 0) {
				break;
			}
			size_t cap = 0;
			if (int const c = ::fcntl(fds[0], F_GETPIPE_SZ); c > 0) {
				cap = static_cast<size_t>(c);
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

	[[nodiscard]] size_t capacity() const noexcept { return pairs_.size(); }
	[[nodiscard]] size_t available() const noexcept { return free_.size(); }

	[[nodiscard]] optional<PipePair> try_acquire() {
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
// FileIoError: thrown inside Flow<T> pipelines on negative io_uring res.
// ---------------------------------------------------------------------------

export struct FileIoError final : system_error {
	FileIoError(
		int err,
		string const &what)
		: system_error{err, generic_category(), what} {}
};

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
	Flow<T> fail_sq_full() const {
		FlowSource<T> src;
		auto flow = src.flow();
		src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
		return flow;
	}

	// Reserve a completion slot with a callback that bridges an IoResult into
	// a FlowSource<T>. `decode` turns a non-negative res into a T; negative
	// res flows through as FileIoError automatically.
	template<typename T, typename Decode>
	pair<u32, u32> reserve_bridge(
		FlowSource<T> &&src,
		Decode &&decode) {
		return completions_->reserve([src = move(src), decode = forward<Decode>(decode)](IoResult r) mutable {
			try {
				if (r.res < 0) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: cqe error"}));
					return;
				}
				if constexpr (is_void_v<T>) {
					decode(r);
					src.resolve();
				} else {
					src.resolve(decode(r));
				}
			} catch (...) { src.reject(current_exception()); }
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

	[[nodiscard]] bool poll_add_multi(
		int fd,
		short poll_mask,
		FileCompletionFn on_event) {
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
		FileCompletionFn on_event) {
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
		FlowSource<FileHandle> src,
		shared_ptr<string> const &path_owner,
		int dir_fd,
		int flags,
		mode_t mode,
		unsigned file_index) {
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return false;
		}
		io_uring_prep_openat(sqe, dir_fd, path_owner->c_str(), flags, mode);
		auto [slot, gen] = completions_->reserve([this, src = move(src), path_owner, file_index](IoResult r) mutable {
			(void)path_owner; // keep-alive until CQE
			try {
				if (r.res < 0) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: open_direct"}));
					return;
				}
				int const fd = r.res;
				int const update_rc = ::io_uring_register_files_update(ring_, file_index, &fd, 1);
				::close(fd);
				if (update_rc < 0) {
					int const sparse = -1;
					::io_uring_register_files_update(ring_, file_index, &sparse, 1);
					src.reject(make_exception_ptr(FileIoError{-update_rc, "file_io: open_direct"}));
					return;
				}
				src.resolve(FileHandle::from_direct_slot(static_cast<int>(file_index)));
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return true;
	}

public:
	// Open a path relative to dir_fd. Result is a raw-fd FileHandle.
	// Pass AT_FDCWD for absolute paths / cwd-relative.
	// `path` must be a null-terminated string owned by the caller until the
	// CQE fires; if unsure, pass a std::string and we copy.
	[[nodiscard]] Flow<FileHandle> open_async(
		int dir_fd,
		string path,
		int flags,
		mode_t mode = 0) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto path_owner = make_shared<string>(move(path));
		io_uring_prep_openat(sqe, dir_fd, path_owner->c_str(), flags, mode);
		auto [slot, gen] = completions_->reserve([src = move(src), path_owner](IoResult r) mutable {
			(void)path_owner; // keep-alive until CQE
			try {
				if (r.res < 0) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: open"}));
					return;
				}
				src.resolve(FileHandle::from_fd(r.res));
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Open a path directly into the ring's fixed-file table. The owner must
	// have registered a sparse file table first.
	[[nodiscard]] Flow<FileHandle> open_direct_async(
		int dir_fd,
		string path,
		int flags,
		mode_t mode,
		unsigned file_index) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto path_owner = make_shared<string>(move(path));
		io_uring_prep_openat_direct(sqe, dir_fd, path_owner->c_str(), flags, mode, file_index);
		auto [slot, gen] = completions_->reserve(
			[this, src = move(src), path_owner, dir_fd, flags, mode, file_index](IoResult r) mutable {
				(void)path_owner; // keep-alive until CQE
				try {
					if (r.res < 0) {
						int const err = -r.res;
						if (err == EINVAL || err == EOPNOTSUPP || err == ENOSYS) {
							(void)submit_open_direct_fallback(move(src), path_owner, dir_fd, flags, mode, file_index);
							return;
						}
						src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: open_direct"}));
						return;
					}
					src.resolve(FileHandle::from_direct_slot(r.res == 0 ? static_cast<int>(file_index) : r.res));
				} catch (...) { src.reject(current_exception()); }
			});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// statx on a path. `mask` follows statx(2) — STATX_BASIC_STATS by default.
	[[nodiscard]] Flow<FileStat> statx_async(
		int dir_fd,
		string path,
		int flags = 0,
		unsigned mask = STATX_BASIC_STATS,
		bool fixed_file = false) {
		FlowSource<FileStat> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto path_owner = make_shared<string>(move(path));
		auto stx_owner = make_shared<struct statx>();
		io_uring_prep_statx(sqe, dir_fd, path_owner->c_str(), flags, mask, stx_owner.get());
		if (fixed_file) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = completions_->reserve([src = move(src), path_owner, stx_owner](IoResult r) mutable {
			(void)path_owner;
			try {
				if (r.res < 0) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: statx"}));
					return;
				}
				auto const &s = *stx_owner;
				FileStat const out{
					.size = s.stx_size,
					.mtime_ns = static_cast<u64>(s.stx_mtime.tv_sec) * 1'000'000'000ULL + s.stx_mtime.tv_nsec,
					.dev = (static_cast<u64>(s.stx_dev_major) << 32U) | s.stx_dev_minor,
					.ino = s.stx_ino,
					.mode = s.stx_mode};
				src.resolve(out);
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// fstat-equivalent via statx with AT_EMPTY_PATH — avoids a path lookup.
	[[nodiscard]] Flow<FileStat> stat_async(
		FileHandle const &fh) {
		if (fh.is_direct()) {
			return statx_async(fh.direct_slot(), string{}, AT_EMPTY_PATH, STATX_BASIC_STATS, true);
		}
		return statx_async(fh.raw_fd(), string{}, AT_EMPTY_PATH);
	}

	// Read into a caller-owned span. The caller must keep `dst` alive until the
	// Flow resolves.
	[[nodiscard]] Flow<size_t> read_into(
		FileHandle const &fh,
		u64 offset,
		span<byte> dst) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
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
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [](IoResult r) { return static_cast<size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Scatter-gather read: fills `iovecs` segments in sequence. The vector is
	// moved into shared state and kept alive until the CQE fires.
	// Returns total bytes read across all segments.
	[[nodiscard]] Flow<size_t> readv_into(
		FileHandle const &fh,
		u64 offset,
		vector<iovec> iovecs) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto iov_owner = make_shared<vector<iovec>>(move(iovecs));
		io_uring_prep_readv(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			iov_owner->data(),
			static_cast<unsigned>(iov_owner->size()),
			offset);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [iov_owner](IoResult r) mutable {
			(void)iov_owner; // keep-alive until CQE
			return static_cast<size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Read into a pre-registered fixed buffer. The pool slot is held by the
	// buffer and returned on destruction — by placing the buffer inside the
	// shared-state closure we keep it alive until the CQE fires, then move it
	// into the resolved value so the caller decides when to release.
	struct ReadFixedResult {
		FixedBuffer buffer;
		size_t bytes{};
	};

	[[nodiscard]] Flow<ReadFixedResult> read_fixed(
		FileHandle const &fh,
		u64 offset,
		FixedBuffer buf,
		size_t max_bytes = numeric_limits<size_t>::max()) {
		FlowSource<ReadFixedResult> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		unsigned const slot_idx = buf.slot();
		auto holder = make_shared<FixedBuffer>(move(buf));
		size_t const bytes = min(holder->view().size(), max_bytes);
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
		auto [slot, gen] = completions_->reserve([src = move(src), holder](IoResult r) mutable {
			try {
				if (r.res < 0) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: read_fixed"}));
					return;
				}
				src.resolve(ReadFixedResult{.buffer = move(*holder), .bytes = static_cast<size_t>(r.res)});
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Read into a pre-registered fixed buffer, bypassing the kernel page cache.
	// The file must have been opened with O_DIRECT. `offset` must be a multiple
	// of `block_size`. `max_bytes` is the caller's true limit (e.g. remaining
	// file bytes); it is rounded up to the nearest `block_size` multiple before
	// submission to satisfy O_DIRECT alignment. The resolved byte count is
	// capped back to the original `max_bytes`, trimming any alignment padding.
	// If the underlying filesystem does not support O_DIRECT, the kernel returns
	// EINVAL, which surfaces as FileIoError{EINVAL, ...}.
	[[nodiscard]] Flow<ReadFixedResult> read_nocache_fixed(
		FileHandle const &fh,
		u64 offset,
		FixedBuffer buf,
		size_t max_bytes = numeric_limits<size_t>::max(),
		size_t block_size = 4096) {
		size_t const actual_cap = min(max_bytes, buf.size());
		size_t aligned_bytes = actual_cap;
		if (block_size > 1 && actual_cap > 0) {
			aligned_bytes = ((actual_cap + block_size - 1) / block_size) * block_size;
			aligned_bytes = min(aligned_bytes, buf.size());
		}
		FlowSource<ReadFixedResult> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
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
		auto [slot, gen] = completions_->reserve([src = move(src), holder, actual_cap](IoResult r) mutable {
			try {
				if (r.res < 0) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: read_nocache_fixed"}));
					return;
				}
				size_t const bytes = min(static_cast<size_t>(r.res), actual_cap);
				src.resolve(ReadFixedResult{.buffer = move(*holder), .bytes = bytes});
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Write from a pre-registered fixed buffer. Symmetric to read_fixed.
	// The buffer is held in shared state until the CQE fires, then returned
	// to the caller (who decides when to release the slot back to the pool).
	struct WriteFixedResult {
		FixedBuffer buffer;
		size_t bytes{};
	};

	[[nodiscard]] Flow<WriteFixedResult> write_fixed(
		FileHandle const &fh,
		u64 offset,
		FixedBuffer buf,
		size_t max_bytes = numeric_limits<size_t>::max()) {
		FlowSource<WriteFixedResult> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		unsigned const slot_idx = buf.slot();
		auto holder = make_shared<FixedBuffer>(move(buf));
		size_t const bytes = min(holder->view().size(), max_bytes);
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
		auto [slot, gen] = completions_->reserve([src = move(src), holder](IoResult r) mutable {
			try {
				if (r.res < 0) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: write_fixed"}));
					return;
				}
				src.resolve(WriteFixedResult{.buffer = move(*holder), .bytes = static_cast<size_t>(r.res)});
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<size_t> write_into(
		FileHandle const &fh,
		u64 offset,
		span<byte const> src_view) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
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
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [](IoResult r) { return static_cast<size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Scatter-gather write: sends `iovecs` segments to the file in sequence.
	// The vector is moved into shared state and kept alive until the CQE fires.
	// Returns total bytes written across all segments.
	[[nodiscard]] Flow<size_t> writev_into(
		FileHandle const &fh,
		u64 offset,
		vector<iovec> iovecs) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto iov_owner = make_shared<vector<iovec>>(move(iovecs));
		io_uring_prep_writev(
			sqe,
			fh.is_direct() ? fh.direct_slot() : fh.raw_fd(),
			iov_owner->data(),
			static_cast<unsigned>(iov_owner->size()),
			offset);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [iov_owner](IoResult r) mutable {
			(void)iov_owner; // keep-alive until CQE
			return static_cast<size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// readv2_into: like readv_into but with RWF flags (e.g. RWF_NOWAIT, RWF_DSYNC).
	[[nodiscard]] Flow<size_t> readv2_into(
		FileHandle const &fh,
		u64 offset,
		vector<iovec> iovecs,
		int rwf_flags = 0) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto iov_owner = make_shared<vector<iovec>>(move(iovecs));
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
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [iov_owner](IoResult r) mutable {
			(void)iov_owner;
			return static_cast<size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// writev2_into: like writev_into but with RWF flags.
	[[nodiscard]] Flow<size_t> writev2_into(
		FileHandle const &fh,
		u64 offset,
		vector<iovec> iovecs,
		int rwf_flags = 0) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto iov_owner = make_shared<vector<iovec>>(move(iovecs));
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
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [iov_owner](IoResult r) mutable {
			(void)iov_owner;
			return static_cast<size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// No-op SQE — useful for latency measurement, wakeup, or pipeline flushing.
	[[nodiscard]] Flow<void> nop_async() {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_nop(sqe);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> fsync_async(
		FileHandle const &fh,
		bool data_only = false) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_fsync(sqe, fh.raw_fd(), data_only ? IORING_FSYNC_DATASYNC : 0U);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> fallocate_async(
		FileHandle const &fh,
		int mode,
		u64 offset,
		u64 len) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_fallocate(sqe, fh.raw_fd(), mode, offset, len);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Consumes the handle; the ring closes the fd via io_uring.
	[[nodiscard]] Flow<void> fadvise_async(
		FileHandle const &fh,
		u64 offset,
		u32 len,
		int advice) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_fadvise(sqe, fd, offset, len, advice);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> madvise_async(
		void *addr,
		u32 length,
		int advice) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_madvise(sqe, addr, length, advice);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> unlink_async(
		int dir_fd,
		string path,
		int flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto path_owner = make_shared<string>(move(path));
		io_uring_prep_unlinkat(sqe, dir_fd, path_owner->c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [path_owner](IoResult) mutable { (void)path_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> rename_async(
		int old_dir_fd,
		string old_path,
		int new_dir_fd,
		string new_path,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto paths = make_shared<pair<string, string>>(move(old_path), move(new_path));
		io_uring_prep_renameat(sqe, old_dir_fd, paths->first.c_str(), new_dir_fd, paths->second.c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [paths](IoResult) mutable { (void)paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> mkdirat_async(
		int dir_fd,
		string path,
		mode_t mode = 0755) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto path_owner = make_shared<string>(move(path));
		io_uring_prep_mkdirat(sqe, dir_fd, path_owner->c_str(), mode);
		auto [slot, gen] = reserve_bridge<void>(move(src), [path_owner](IoResult) mutable { (void)path_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> symlinkat_async(
		string target,
		int new_dir_fd,
		string link_path) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto paths = make_shared<pair<string, string>>(move(target), move(link_path));
		io_uring_prep_symlinkat(sqe, paths->first.c_str(), new_dir_fd, paths->second.c_str());
		auto [slot, gen] = reserve_bridge<void>(move(src), [paths](IoResult) mutable { (void)paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> ftruncate_async(
		FileHandle const &fh,
		u64 length) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_ftruncate(sqe, fd, static_cast<loff_t>(length));
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> linkat_async(
		int old_dir_fd,
		string old_path,
		int new_dir_fd,
		string new_path,
		int flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto paths = make_shared<pair<string, string>>(move(old_path), move(new_path));
		io_uring_prep_linkat(sqe, old_dir_fd, paths->first.c_str(), new_dir_fd, paths->second.c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [paths](IoResult) mutable { (void)paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> sync_file_range_async(
		FileHandle const &fh,
		u64 offset,
		unsigned len,
		int flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_sync_file_range(sqe, fd, len, offset, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<FileHandle> socket_async(
		int domain,
		int type,
		int protocol) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_socket(sqe, domain, type, protocol, 0);
		auto [slot, gen] = reserve_bridge<FileHandle>(move(src), [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<FileHandle> socket_direct_async(
		int domain,
		int type,
		int protocol,
		unsigned file_index) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_socket_direct(sqe, domain, type, protocol, file_index, 0);
		auto [slot, gen] = reserve_bridge<FileHandle>(move(src), [file_index](IoResult) {
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Create a pipe asynchronously. Returns (read_fd, write_fd) on success.
	[[nodiscard]] Flow<pair<int, int>> pipe_async(
		int pipe_flags = O_CLOEXEC | O_NONBLOCK) {
		FlowSource<pair<int, int>> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto fds = make_shared<array<int, 2>>(array<int, 2>{-1, -1});
		io_uring_prep_pipe(sqe, fds->data(), pipe_flags);
		auto [slot, gen] = completions_->reserve([src = move(src), fds](IoResult r) mutable {
			try {
				if (r.res < 0) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: pipe"}));
					return;
				}
				src.resolve(make_pair((*fds)[0], (*fds)[1]));
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Async bind. `addr` is copied and kept alive until CQE.
	[[nodiscard]] Flow<void> bind_async(
		FileHandle const &fh,
		sockaddr_storage addr,
		socklen_t addrlen) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		auto addr_owner = make_shared<sockaddr_storage>(addr);
		io_uring_prep_bind(sqe, fd, reinterpret_cast<sockaddr *>(addr_owner.get()), addrlen);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(move(src), [addr_owner](IoResult) mutable { (void)addr_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Async listen.
	[[nodiscard]] Flow<void> listen_async(
		FileHandle const &fh,
		int backlog = SOMAXCONN) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_listen(sqe, fd, backlog);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> shutdown_async(
		FileHandle const &fh,
		int how) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_shutdown(sqe, fd, how);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<size_t> tee_async(
		int fd_in,
		int fd_out,
		size_t len,
		unsigned flags = 0) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_tee(sqe, fd_in, fd_out, static_cast<unsigned int>(len), flags);
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [](IoResult r) { return static_cast<size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Installs a direct-slot fd into the process fd table. Returns a raw-fd
	// FileHandle wrapping the installed fd. Caller must hold a registered-files
	// table (io_uring_register_files) on this ring.
	[[nodiscard]] Flow<FileHandle> fixed_fd_install_async(
		FileHandle const &fh,
		unsigned flags = 0) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		if (!fh.is_direct()) {
			src.reject(make_exception_ptr(FileIoError{EINVAL, "file_io: fixed_fd_install requires direct slot"}));
			return flow;
		}
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_fixed_fd_install(sqe, fh.direct_slot(), flags);
		auto [slot, gen] = reserve_bridge<FileHandle>(move(src), [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Get extended attribute. `name` is moved and kept alive until CQE.
	// `value` span must remain valid until the returned Flow resolves.
	// Returns the actual attribute size (may exceed value.size() — ERANGE).
	[[nodiscard]] Flow<size_t> fgetxattr_async(
		FileHandle const &fh,
		string name,
		span<char> buf) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		auto name_owner = make_shared<string>(move(name));
		io_uring_prep_fgetxattr(sqe, fd, name_owner->c_str(), buf.data(), static_cast<unsigned>(buf.size()));
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [name_owner](IoResult r) mutable {
			(void)name_owner;
			return static_cast<size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Set extended attribute. Both `name` and `data` are moved/kept alive until CQE.
	[[nodiscard]] Flow<void> fsetxattr_async(
		FileHandle const &fh,
		string name,
		string data,
		int flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		auto kv = make_shared<pair<string, string>>(move(name), move(data));
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
		auto [slot, gen] = reserve_bridge<void>(move(src), [kv](IoResult) mutable { (void)kv; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Path-based get extended attribute. `name`, `path`, and `buf` must
	// remain valid until the Flow resolves.
	[[nodiscard]] Flow<size_t> getxattr_async(
		string path,
		string name,
		span<char> buf) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto kp = make_shared<pair<string, string>>(move(path), move(name));
		io_uring_prep_getxattr(
			sqe,
			kp->second.c_str(),
			buf.data(),
			kp->first.c_str(),
			static_cast<unsigned>(buf.size()));
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [kp](IoResult r) mutable {
			(void)kp;
			return static_cast<size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Path-based set extended attribute. `name`, `data`, and `path` are moved
	// and kept alive until CQE.
	[[nodiscard]] Flow<void> setxattr_async(
		string path,
		string name,
		string data,
		int flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		struct XattrState {
			string path;
			string name;
			string data;
		};
		auto st = make_shared<XattrState>(move(path), move(name), move(data));
		io_uring_prep_setxattr(
			sqe,
			st->name.c_str(),
			st->data.c_str(),
			st->path.c_str(),
			flags,
			static_cast<unsigned>(st->data.size()));
		auto [slot, gen] = reserve_bridge<void>(move(src), [st](IoResult) mutable { (void)st; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Wait for process state change (IORING_OP_WAITID). `infop` must stay valid
	// until the Flow resolves; on success it is filled with signal info.
	[[nodiscard]] Flow<void> waitid_async(
		idtype_t idtype,
		id_t id,
		siginfo_t *infop,
		int options = WEXITED,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_waitid(sqe, idtype, id, infop, options, flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Futex wait — waits until *futex != val. The futex pointer must remain
	// valid until the Flow resolves. Returns void on wakeup.
	[[nodiscard]] Flow<void> futex_wait_async(
		u32 *futex,
		u64 val,
		u64 mask = FUTEX_BITSET_MATCH_ANY,
		u32 futex_flags = FUTEX2_SIZE_U32,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_futex_wait(sqe, futex, val, mask, futex_flags, flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Futex wake — wakes up to `val` waiters. Returns the number woken.
	[[nodiscard]] Flow<u32> futex_wake_async(
		u32 *futex,
		u64 val,
		u64 mask = FUTEX_BITSET_MATCH_ANY,
		u32 futex_flags = FUTEX2_SIZE_U32,
		unsigned flags = 0) {
		FlowSource<u32> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_futex_wake(sqe, futex, val, mask, futex_flags, flags);
		auto [slot, gen] = reserve_bridge<u32>(move(src), [](IoResult r) { return static_cast<u32>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Send a synthetic CQE to `target_ring_fd` (the target ring's ring_fd).
	// The CQE on the target will have res=len, user_data=data.
	[[nodiscard]] Flow<void> msg_ring_async(
		int target_ring_fd,
		unsigned len,
		u64 data,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_msg_ring(sqe, target_ring_fd, len, data, flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Arm a one-shot timeout. Resolves (with -ETIME mapped to void) when ms
	// elapses. If count > 0, fires after count CQE completions OR ms, whichever
	// is first (IORING_TIMEOUT_BOOTTIME etc. can be passed in flags).
	[[nodiscard]] Flow<void> timeout_async(
		chrono::milliseconds ms,
		unsigned count = 0,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto ts = make_shared<__kernel_timespec>();
		auto const sec = chrono::duration_cast<chrono::seconds>(ms);
		ts->tv_sec = sec.count();
		ts->tv_nsec = (ms - sec).count() * 1'000'000LL;
		io_uring_prep_timeout(sqe, ts.get(), count, flags);
		auto [slot, gen] = completions_->reserve([src = move(src), ts](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ETIME && r.res != -ECANCELED) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: timeout"}));
					return;
				}
				src.resolve();
			} catch (...) { src.reject(current_exception()); }
			(void)ts;
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Cancel a running timeout by its user_data tag. -ENOENT → already fired.
	[[nodiscard]] Flow<void> timeout_remove_async(
		u64 user_data,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_timeout_remove(sqe, user_data, flags);
		auto [slot, gen] = completions_->reserve([src = move(src)](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT && r.res != -EALREADY) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: timeout_remove"}));
					return;
				}
				src.resolve();
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Update an armed timeout. New deadline `ms` replaces the existing one.
	// `user_data` identifies the timeout SQE to update (its encoded user_data).
	[[nodiscard]] Flow<void> timeout_update_async(
		u64 user_data,
		chrono::milliseconds ms,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto ts = make_shared<__kernel_timespec>();
		auto const sec = chrono::duration_cast<chrono::seconds>(ms);
		ts->tv_sec = sec.count();
		ts->tv_nsec = (ms - sec).count() * 1'000'000LL;
		io_uring_prep_timeout_update(sqe, ts.get(), user_data, flags);
		auto [slot, gen] = completions_->reserve([src = move(src), ts](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT && r.res != -EALREADY) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: timeout_update"}));
					return;
				}
				src.resolve();
			} catch (...) { src.reject(current_exception()); }
			(void)ts;
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Register a one-shot poll on `fd` for `events` (POLLIN, POLLOUT, …).
	// Resolves with the triggered poll mask when any event fires.
	// -ENOENT on poll_remove before the event: treated as ECANCELED by caller.
	[[nodiscard]] Flow<u32> poll_add_async(
		int fd,
		u32 events) {
		FlowSource<u32> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_poll_add(sqe, fd, events);
		auto [slot, gen] = reserve_bridge<u32>(move(src), [](IoResult r) { return static_cast<u32>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Cancel a pending poll_add identified by `user_data`.
	// -ENOENT means the poll already fired — treated as success.
	[[nodiscard]] Flow<void> poll_remove_async(
		u64 user_data) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_poll_remove(sqe, user_data);
		auto [slot, gen] = completions_->reserve([src = move(src)](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT && r.res != -EALREADY) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: poll_remove"}));
					return;
				}
				src.resolve();
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Update an armed multishot poll's event mask in-place.
	// `user_data` is the encoded user_data of the original poll SQE.
	[[nodiscard]] Flow<void> poll_update_async(
		u64 user_data,
		u32 new_events,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_poll_update(sqe, user_data, 0, new_events, flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Accept one connection on a listening socket. Returns the accepted fd.
	// `addr`/`addrlen` are optional out-params for the peer address.
	[[nodiscard]] Flow<FileHandle> accept_async(
		FileHandle const &fh,
		sockaddr *addr = nullptr,
		socklen_t *addrlen = nullptr,
		int flags = SOCK_CLOEXEC) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<FileHandle>(move(src), [](IoResult r) { return FileHandle::from_fd(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Accept one connection into a registered direct slot.
	// `addr`/`addrlen` are optional out-params for the peer address.
	[[nodiscard]] Flow<FileHandle> accept_direct_async(
		FileHandle const &fh,
		unsigned file_index,
		sockaddr *addr = nullptr,
		socklen_t *addrlen = nullptr,
		int flags = SOCK_CLOEXEC) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_accept_direct(sqe, fd, addr, addrlen, flags, file_index);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<FileHandle>(move(src), [file_index](IoResult) {
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Send an fd to another ring's registered file table. `source_fd` is
	// installed at `target_fd` slot in the target ring's file table.
	// Pass IORING_FILE_INDEX_ALLOC for `target_fd` to auto-allocate.
	[[nodiscard]] Flow<void> msg_ring_fd_async(
		int target_ring_fd,
		int source_fd,
		int target_fd,
		u64 data = 0,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_msg_ring_fd(sqe, target_ring_fd, source_fd, target_fd, data, flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Wait on multiple futexes simultaneously. Resolves when any waiter
	// condition is met. `waiters` is moved and kept alive until CQE.
	[[nodiscard]] Flow<void> futex_waitv_async(
		vector<futex_waitv> waiters,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto wv = make_shared<vector<futex_waitv>>(move(waiters));
		io_uring_prep_futex_waitv(sqe, wv->data(), static_cast<u32>(wv->size()), flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [wv](IoResult) mutable { (void)wv; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Cancel a pending op by its user_data tag. Resolves when the cancel
	// was submitted; the target op's CQE will still arrive (with -ECANCELED).
	// -ENOENT means the target already completed — treated as success here.
	[[nodiscard]] Flow<void> cancel_async(
		u64 user_data,
		int flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_cancel64(sqe, user_data, flags);
		auto [slot, gen] = completions_->reserve([src = move(src)](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: cancel"}));
					return;
				}
				src.resolve();
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Cancel all pending ops for `fd`. -ENOENT treated as success.
	[[nodiscard]] Flow<void> cancel_fd_async(
		int fd,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_cancel_fd(sqe, fd, flags);
		auto [slot, gen] = completions_->reserve([src = move(src)](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ENOENT) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: cancel_fd"}));
					return;
				}
				src.resolve();
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Async connect. `addr` is copied into a shared buffer kept alive until CQE.
	[[nodiscard]] Flow<void> connect_async(
		FileHandle const &fh,
		sockaddr_storage addr,
		socklen_t addrlen) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		auto addr_owner = make_shared<sockaddr_storage>(addr);
		io_uring_prep_connect(sqe, fd, reinterpret_cast<sockaddr *>(addr_owner.get()), addrlen);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<void>(move(src), [addr_owner](IoResult) mutable { (void)addr_owner; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	[[nodiscard]] Flow<void> close_async(
		FileHandle fh) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		if (fh.is_direct()) {
			io_uring_prep_close_direct(sqe, static_cast<unsigned>(fh.release_direct_slot()));
		} else {
			io_uring_prep_close(sqe, fh.release_fd());
		}
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
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
	[[nodiscard]] Flow<size_t> splice_to_fd(
		FileHandle const &file,
		u64 off,
		size_t len,
		int dst_fd,
		PipePair pipe) {
		struct State {
			io_uring *ring;
			CompletionTable *completions;
			UserDataFn encode_ud;
			int file_fd;
			int dst_fd;
			PipePair pipe;
			u64 file_off;
			size_t remaining;
			size_t delivered{0};
			FlowSource<size_t> src;
		};
		auto st = make_shared<State>(State{
			.ring = ring_,
			.completions = completions_,
			.encode_ud = encode_ud_,
			.file_fd = file.raw_fd(),
			.dst_fd = dst_fd,
			.pipe = move(pipe),
			.file_off = off,
			.remaining = len,
			.delivered = 0,
			.src = {}});
		auto flow = st->src.flow();
		step_splice(st);
		return flow;
	}

	// Send `len` bytes from `buf` on `fh`. Returns bytes sent.
	[[nodiscard]] Flow<size_t> send_async(
		FileHandle const &fh,
		void const *buf,
		size_t len,
		int flags = 0) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_send(sqe, fd, buf, len, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [](IoResult r) { return static_cast<size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Receive up to `len` bytes into `buf` from `fh`. Returns bytes received.
	[[nodiscard]] Flow<size_t> recv_async(
		FileHandle const &fh,
		void *buf,
		size_t len,
		int flags = 0) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_recv(sqe, fd, buf, len, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [](IoResult r) { return static_cast<size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Vectored send via sendmsg(2). `msg` must remain valid until the Flow resolves.
	[[nodiscard]] Flow<size_t> sendmsg_async(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_sendmsg(sqe, fd, msg, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [](IoResult r) { return static_cast<size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Vectored recv via recvmsg(2). `msg` must remain valid until the Flow resolves.
	[[nodiscard]] Flow<size_t> recvmsg_async(
		FileHandle const &fh,
		msghdr *msg,
		unsigned flags = 0) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_recvmsg(sqe, fd, msg, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [](IoResult r) { return static_cast<size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Register `nr` buffers of `len` bytes starting at `addr` into buffer group `bgid`.
	// Kernel increments `bid` automatically for subsequent provides in the same group.
	[[nodiscard]] Flow<void> provide_buffers_async(
		void *addr,
		int len,
		int nr,
		int bgid,
		int bid = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_provide_buffers(sqe, addr, len, nr, bgid, bid);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Remove `nr` buffers from buffer group `bgid`.
	[[nodiscard]] Flow<void> remove_buffers_async(
		int nr,
		int bgid) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_remove_buffers(sqe, nr, bgid);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Update the registered file table. `fds` is a span of `nr_fds` fds starting
	// at `offset` in the kernel's registered file array. -1 entries remove a slot.
	// `fds` must remain valid until the Flow resolves.
	[[nodiscard]] Flow<void> files_update_async(
		int *fds,
		unsigned nr_fds,
		int offset = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_files_update(sqe, fds, nr_fds, offset);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Modify an epoll interest list entry. `ev` may be null for EPOLL_CTL_DEL.
	[[nodiscard]] Flow<void> epoll_ctl_async(
		int epfd,
		int fd,
		int op,
		epoll_event const *ev = nullptr) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_epoll_ctl(sqe, epfd, fd, op, ev);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Wait for epoll events. Resolves with the number of events returned.
	// `events` must remain valid until the Flow resolves.
	[[nodiscard]] Flow<int> epoll_wait_async(
		int epfd,
		epoll_event *events,
		int maxevents,
		unsigned flags = 0) {
		FlowSource<int> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_epoll_wait(sqe, epfd, events, maxevents, flags);
		auto [slot, gen] = reserve_bridge<int>(move(src), [](IoResult r) { return r.res; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Attach a timeout to the preceding SQE in the ring's submission chain.
	// The preceding SQE must have been submitted with IOSQE_IO_LINK.
	// Resolves when the link fires (either the linked op completed or the
	// timeout expired — -ETIME in the latter case is treated as success).
	[[nodiscard]] Flow<void> link_timeout_async(
		chrono::milliseconds ms,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto ts = make_shared<__kernel_timespec>();
		auto const sec = chrono::duration_cast<chrono::seconds>(ms);
		ts->tv_sec = sec.count();
		ts->tv_nsec = (ms - sec).count() * 1'000'000LL;
		io_uring_prep_link_timeout(sqe, ts.get(), flags);
		auto [slot, gen] = completions_->reserve([src = move(src), ts](IoResult r) mutable {
			try {
				if (r.res < 0 && r.res != -ETIME && r.res != -ECANCELED && r.res != -ENOENT) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: link_timeout"}));
					return;
				}
				src.resolve();
			} catch (...) { src.reject(current_exception()); }
			(void)ts;
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Open a file with full openat2(2) semantics (`open_how` struct).
	// `how` is copied internally so the caller need not keep it alive.
	[[nodiscard]] Flow<FileHandle> openat2_async(
		int dir_fd,
		string path,
		open_how how) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto ctx = make_shared<pair<string, open_how>>(move(path), how);
		io_uring_prep_openat2(sqe, dir_fd, ctx->first.c_str(), &ctx->second);
		auto [slot, gen] = reserve_bridge<FileHandle>(move(src), [ctx](IoResult r) mutable {
			(void)ctx;
			return FileHandle::from_fd(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Send with destination address — for SOCK_DGRAM sockets.
	// `addr` is copied internally; `buf` must remain valid until the Flow resolves.
	[[nodiscard]] Flow<size_t> sendto_async(
		FileHandle const &fh,
		void const *buf,
		size_t len,
		int flags,
		sockaddr_storage addr,
		socklen_t addrlen) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto sa = make_shared<sockaddr_storage>(addr);
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_sendto(sqe, fd, buf, len, flags, reinterpret_cast<sockaddr *>(sa.get()), addrlen);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [sa](IoResult r) mutable {
			(void)sa;
			return static_cast<size_t>(r.res);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Zero-copy send. `buf` must remain valid until the CQE with
	// IORING_CQE_F_NOTIF fires (the caller is notified via a second CQE).
	// For simplicity this Flow resolves when the first CQE arrives;
	// the notification CQE is discarded by the completion dispatch.
	[[nodiscard]] Flow<size_t> send_zc_async(
		FileHandle const &fh,
		void const *buf,
		size_t len,
		int flags = 0,
		unsigned zc_flags = 0) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_send_zc(sqe, fd, buf, len, flags, zc_flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [](IoResult r) { return static_cast<size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Write using a pre-registered fixed buffer (IORING_OP_WRITE_FIXED).
	// `buf` pointer and `buf_index` must refer to the registered buffer in the pool.
	[[nodiscard]] Flow<size_t> write_fixed_async(
		FileHandle const &fh,
		u64 offset,
		void const *buf,
		unsigned nbytes,
		int buf_index) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_write_fixed(sqe, fd, buf, nbytes, offset, buf_index);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [](IoResult r) { return static_cast<size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Remove a file by name relative to `dir_fd`.
	// `flags` = 0 for file; AT_REMOVEDIR for directory.
	[[nodiscard]] Flow<void> unlinkat_async(
		int dir_fd,
		string path,
		int flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto p = make_shared<string>(move(path));
		io_uring_prep_unlinkat(sqe, dir_fd, p->c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [p](IoResult) mutable { (void)p; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Rename with full dirfd control.
	[[nodiscard]] Flow<void> renameat_async(
		int old_dir_fd,
		string old_path,
		int new_dir_fd,
		string new_path,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto paths = make_shared<pair<string, string>>(move(old_path), move(new_path));
		io_uring_prep_renameat(sqe, old_dir_fd, paths->first.c_str(), new_dir_fd, paths->second.c_str(), flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [paths](IoResult) mutable { (void)paths; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Create a directory at `path` (relative to AT_FDCWD).
	[[nodiscard]] Flow<void> mkdir_async(
		string path,
		mode_t mode = 0755) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto p = make_shared<string>(move(path));
		io_uring_prep_mkdir(sqe, p->c_str(), mode);
		auto [slot, gen] = reserve_bridge<void>(move(src), [p](IoResult) mutable { (void)p; });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Open directly into the registered file table with full openat2 semantics.
	// IORING_FILE_INDEX_ALLOC for `file_index` auto-allocates.
	[[nodiscard]] Flow<FileHandle> openat2_direct_async(
		int dir_fd,
		string path,
		open_how how,
		unsigned file_index) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto ctx = make_shared<pair<string, open_how>>(move(path), how);
		io_uring_prep_openat2_direct(sqe, dir_fd, ctx->first.c_str(), &ctx->second, file_index);
		auto [slot, gen] = reserve_bridge<FileHandle>(move(src), [ctx, file_index](IoResult) mutable {
			(void)ctx;
			return FileHandle::from_direct_slot(static_cast<int>(file_index));
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Create a socket directly into the registered file table, with the kernel
	// choosing the slot (IORING_FILE_INDEX_ALLOC). Returns the allocated slot.
	[[nodiscard]] Flow<FileHandle> socket_direct_alloc_async(
		int domain,
		int type,
		int protocol,
		unsigned flags = 0) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_socket_direct_alloc(sqe, domain, type, protocol, flags);
		auto [slot, gen] =
			reserve_bridge<FileHandle>(move(src), [](IoResult r) { return FileHandle::from_direct_slot(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Open a file directly into the fixed file table (openat semantics).
	// Use IORING_FILE_INDEX_ALLOC for `file_index` to let the kernel pick a slot.
	[[nodiscard]] Flow<FileHandle> openat_direct_async(
		int dir_fd,
		string path,
		int flags,
		mode_t mode = 0,
		unsigned file_index = IORING_FILE_INDEX_ALLOC) {
		FlowSource<FileHandle> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto p = make_shared<string>(move(path));
		io_uring_prep_openat_direct(sqe, dir_fd, p->c_str(), flags, mode, file_index);
		auto [slot, gen] = reserve_bridge<FileHandle>(move(src), [p, file_index](IoResult r) mutable {
			(void)p;
			// When IORING_FILE_INDEX_ALLOC: res carries the allocated slot.
			int const s = (file_index == IORING_FILE_INDEX_ALLOC) ? r.res : static_cast<int>(file_index);
			return FileHandle::from_direct_slot(s);
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Send a source fd to another ring, letting the kernel auto-allocate the slot.
	// Returns the allocated slot index via the target ring's CQE.
	[[nodiscard]] Flow<void> msg_ring_fd_alloc_async(
		int target_ring_fd,
		int source_fd,
		u64 data = 0,
		unsigned flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_msg_ring_fd_alloc(sqe, target_ring_fd, source_fd, data, flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Create a pipe pair directly into fixed file table slots.
	// Use IORING_FILE_INDEX_ALLOC for `file_index` to let the kernel choose.
	// The two slots are allocated consecutively.
	[[nodiscard]] Flow<pair<int, int>> pipe_direct_async(
		unsigned file_index = IORING_FILE_INDEX_ALLOC,
		int pipe_flags = O_CLOEXEC | O_NONBLOCK) {
		FlowSource<pair<int, int>> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		auto fds = make_shared<array<int, 2>>(array<int, 2>{-1, -1});
		io_uring_prep_pipe_direct(sqe, fds->data(), pipe_flags, file_index);
		auto [slot, gen] = completions_->reserve([src = move(src), fds](IoResult r) mutable {
			try {
				if (r.res < 0) {
					src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: pipe_direct"}));
					return;
				}
				src.resolve(make_pair((*fds)[0], (*fds)[1]));
			} catch (...) { src.reject(current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Post a message to another ring, forwarding specific CQE flags in the payload.
	// Useful for waking up a consumer ring with custom CQE flags set.
	[[nodiscard]] Flow<void> msg_ring_cqe_flags_async(
		int target_ring_fd,
		unsigned len,
		u64 data,
		unsigned flags = 0,
		unsigned cqe_flags = 0) {
		FlowSource<void> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		io_uring_prep_msg_ring_cqe_flags(sqe, target_ring_fd, len, data, flags, cqe_flags);
		auto [slot, gen] = reserve_bridge<void>(move(src), [](IoResult) {});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

	// Zero-copy vectored send via sendmsg(2). `msg` must remain valid until the
	// notification CQE fires (i.e., the kernel has finished reading the buffers).
	// The Flow resolves on the first CQE (send completion); the notification CQE
	// is a separate event that callers must handle via their completion dispatch.
	[[nodiscard]] Flow<size_t> sendmsg_zc_async(
		FileHandle const &fh,
		msghdr const *msg,
		unsigned flags = 0) {
		FlowSource<size_t> src;
		auto flow = src.flow();
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: SQ full"}));
			return flow;
		}
		int const fd = fh.is_direct() ? fh.direct_slot() : fh.raw_fd();
		io_uring_prep_sendmsg_zc(sqe, fd, msg, flags);
		if (fh.is_direct()) {
			io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		}
		auto [slot, gen] = reserve_bridge<size_t>(move(src), [](IoResult r) { return static_cast<size_t>(r.res); });
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return flow;
	}

private:
	template<typename StatePtr>
	static void step_splice(
		StatePtr const &st) {
		if (st->remaining == 0) {
			st->src.resolve(st->delivered);
			return;
		}
		size_t const chunk = min(st->remaining, st->pipe.capacity());
		auto *sqe_in = io_uring_get_sqe(st->ring);
		auto *sqe_out = io_uring_get_sqe(st->ring);
		if (sqe_in == nullptr || sqe_out == nullptr) {
			st->src.reject(make_exception_ptr(FileIoError{ENOSPC, "file_io: splice SQ full"}));
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
				st->src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: splice in"}));
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
		auto [slot_out, gen_out] = st->completions->reserve([st](IoResult r) mutable {
			if (r.res < 0) {
				if (r.res == -ECANCELED && st->src.armed()) {
					return;
				}
				st->src.reject(make_exception_ptr(FileIoError{-r.res, "file_io: splice out"}));
				return;
			}
			auto const n = static_cast<size_t>(r.res);
			st->delivered += n;
			st->file_off += n;
			st->remaining = st->remaining > n ? st->remaining - n : 0;
			step_splice(st);
		});
		io_uring_sqe_set_data64(sqe_out, st->encode_ud(slot_out, gen_out));
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
	std::pair<u32, u32> operator ()(
		u64 ud) const noexcept {
		return {static_cast<u32>(ud & 0xFFFFFFFFU), static_cast<u32>(ud >> 32U)};
	}
};

export struct PumpTimeout final : std::runtime_error {
	PumpTimeout()
		: std::runtime_error{"conflux.file_io: pump_until budget exhausted"} {}
};

export template<typename Decode = DefaultUdDecoder>
void pump_until(
	FileReader &reader,
	std::atomic_flag const &done,
	std::optional<std::chrono::milliseconds> budget = std::nullopt,
	Decode decode = {}) {
	auto *ring = reader.ring();
	auto *completions = reader.completions();
	auto const deadline = budget ? std::optional{std::chrono::steady_clock::now() + *budget} : std::nullopt;
	while (!done.test(std::memory_order_acquire)) {
		::io_uring_cqe *cqe = nullptr;
		int rc = 0;
		if (deadline) {
			__kernel_timespec ts{.tv_sec = 1, .tv_nsec = 0};
			rc = ::io_uring_submit_and_wait_timeout(ring, &cqe, 1, &ts, nullptr);
			if (rc == -ETIME) {
				if (std::chrono::steady_clock::now() > *deadline) {
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
		if (rc < 0 || cqe == nullptr) {
			throw std::runtime_error{std::format("conflux.file_io: submit_and_wait rc={}", rc)};
		}
		for (;;) {
			auto [slot, gen] = decode(cqe->user_data);
			i32 const res = cqe->res;
			u32 const flags = cqe->flags;
			::io_uring_cqe_seen(ring, cqe);
			completions->dispatch(slot, gen, res, flags);
			if (done.test(std::memory_order_acquire)) {
				break;
			}
			if (::io_uring_peek_cqe(ring, &cqe) != 0 || cqe == nullptr) {
				break;
			}
		}
	}
}

export template<typename T, typename Decode = DefaultUdDecoder>
T block_on(
	FileReader &reader,
	Task<T> task,
	std::optional<std::chrono::milliseconds> budget = std::nullopt,
	Decode decode = {}) {
	if constexpr (std::is_void_v<T>) {
		block_on<void>(reader, std::move(task).flow(), budget, std::move(decode));
	} else {
		return block_on<T>(reader, std::move(task).flow(), budget, std::move(decode));
	}
}

export template<typename T, typename Decode = DefaultUdDecoder>
T block_on(
	FileReader &reader,
	Flow<T> flow,
	std::optional<std::chrono::milliseconds> budget = std::nullopt,
	Decode decode = {}) {
	if constexpr (std::is_void_v<T>) {
		struct Slot {
			std::atomic_flag done{};
			std::exception_ptr err{};
		};
		auto slot = std::make_shared<Slot>();
		auto held = std::move(flow)
				  | then([slot]() { slot->done.test_and_set(std::memory_order_release); })
				  | on_error([slot](std::exception_ptr const &ex) {
						slot->err = ex;
						slot->done.test_and_set(std::memory_order_release);
					});
		(void)held;
		pump_until(reader, slot->done, budget, std::move(decode));
		if (slot->err) {
			std::rethrow_exception(slot->err);
		}
	} else {
		struct Slot {
			std::atomic_flag done{};
			std::exception_ptr err{};
			std::optional<T> value{};
		};
		auto slot = std::make_shared<Slot>();
		auto held = std::move(flow)
				  | then([slot](T v) {
						slot->value.emplace(std::move(v));
						slot->done.test_and_set(std::memory_order_release);
					})
				  | on_error([slot](std::exception_ptr const &ex) {
						slot->err = ex;
						slot->done.test_and_set(std::memory_order_release);
					});
		(void)held;
		pump_until(reader, slot->done, budget, std::move(decode));
		if (slot->err) {
			std::rethrow_exception(slot->err);
		}
		return std::move(*slot->value);
	}
}
