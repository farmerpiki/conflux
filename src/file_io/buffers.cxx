module;
#include <cstdlib>
#include <liburing.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

export module conflux.file_io.buffers;

import std;
import conflux.types;

namespace conflux::file_io {

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
	std::span<std::byte> view_{};
	FixedBuffer(
		FixedBufferPool *pool,
		unsigned local_slot,
		std::span<std::byte> view) noexcept
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
		: pool_{std::exchange(o.pool_, nullptr)}
		, local_slot_{o.local_slot_}
		, view_{o.view_} {}
	FixedBuffer &operator =(FixedBuffer &&o) noexcept;
	~FixedBuffer();
	[[nodiscard]] bool valid() const noexcept { return pool_ != nullptr; }
	[[nodiscard]] unsigned slot() const noexcept;
	[[nodiscard]] std::byte *data() noexcept { return view_.data(); }
	[[nodiscard]] std::byte const *data() const noexcept { return view_.data(); }
	[[nodiscard]] std::span<std::byte> view() const noexcept { return view_; }
	[[nodiscard]] std::size_t size() const noexcept { return view_.size(); }
};
export class FixedBufferPool {
	RegisteredBufferTable *table_;
	unsigned base_;
	std::size_t slab_bytes_;
	std::vector<std::unique_ptr<std::byte[], void (*)(void *)>> slabs_{};
	std::vector<unsigned> free_{};
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
		std::size_t slab_count,
		std::size_t slab_bytes)
		: table_{table}
		, base_{base}
		, slab_bytes_{slab_bytes} {
		if (slab_count == 0 || slab_bytes == 0 || !table_->ok()) {
			return;
		}
		long const page = ::sysconf(_SC_PAGESIZE);
		std::size_t const align = page > 0 ? static_cast<std::size_t>(page) : std::size_t{4096};
		std::size_t const aligned_bytes = ((slab_bytes + align - 1) / align) * align;
		slab_bytes_ = aligned_bytes;
		slabs_.reserve(slab_count);
		free_.reserve(slab_count);
		for (std::size_t i = 0; i < slab_count; ++i) {
			void *raw = ::aligned_alloc(align, aligned_bytes);
			if (raw == nullptr) {
				break;
			}
			::madvise(raw, aligned_bytes, MADV_DONTFORK);
			::madvise(raw, aligned_bytes, MADV_HUGEPAGE);
			slabs_.emplace_back(static_cast<std::byte *>(raw), +[](void *p) { ::free(p); });
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
	[[nodiscard]] std::size_t capacity() const noexcept { return slabs_.size(); }
	[[nodiscard]] std::size_t available() const noexcept { return free_.size(); }
	[[nodiscard]] std::size_t slab_bytes() const noexcept { return slab_bytes_; }
	[[nodiscard]] std::optional<FixedBuffer> try_acquire() {
		if (free_.empty()) {
			return std::nullopt;
		}
		unsigned const local = free_.back();
		free_.pop_back();
		return FixedBuffer{
			this,
			local,
			std::span{slabs_[local].get(), slab_bytes_}
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
		pool_ = std::exchange(o.pool_, nullptr);
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

} // namespace conflux::file_io
