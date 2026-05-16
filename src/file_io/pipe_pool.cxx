module;
#include <fcntl.h>
#include <unistd.h>

export module conflux.file_io.pipe_pool;

import std;
import conflux.types;

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
	std::uint32_t slot_{0};
	int read_fd_{-1};
	int write_fd_{-1};
	std::size_t capacity_{0};
	PipePair(
		PipePool *pool,
		std::uint32_t slot,
		int read_fd,
		int write_fd,
		std::size_t capacity) noexcept
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
	[[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
};
export class PipePool {
	struct Pair {
		int read_fd;
		int write_fd;
		std::size_t capacity;
	};
	std::vector<Pair> pairs_{};
	std::vector<std::uint32_t> free_{};

	friend class PipePair;
	void release(
		// NOLINT(bugprone-exception-escape) — free_ is pre-sized; push_back never reallocates
		std::uint32_t slot) noexcept {
		free_.push_back(slot);
	}

public:
	explicit PipePool(
		std::size_t pair_count) {
		pairs_.reserve(pair_count);
		free_.reserve(pair_count);
		for (std::size_t i = 0; i < pair_count; ++i) {
			std::array<int, 2> fds{-1, -1};
			// O_DIRECT = packet-mode pipes: each write yields a distinct read,
			// exactly what splice with SPLICE_F_MOVE expects. Falls back to std::byte
			// stream if kernel rejects (rare).
			if (::pipe2(fds.data(), O_DIRECT | O_CLOEXEC) < 0 && ::pipe2(fds.data(), O_CLOEXEC) < 0) {
				break;
			}
			std::size_t cap = 0;
			if (int const c = ::fcntl(fds[0], F_GETPIPE_SZ); c > 0) {
				cap = static_cast<std::size_t>(c);
			}
			if (cap == 0) {
				cap = 64UL * 1024;
			}
			pairs_.push_back(Pair{.read_fd = fds[0], .write_fd = fds[1], .capacity = cap});
			free_.push_back(static_cast<std::uint32_t>(pairs_.size() - 1));
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
	[[nodiscard]] std::size_t capacity() const noexcept { return pairs_.size(); }
	[[nodiscard]] std::size_t available() const noexcept { return free_.size(); }
	[[nodiscard]] std::optional<PipePair> try_acquire() {
		if (free_.empty()) {
			return nullopt;
		}
		std::uint32_t const idx = free_.back();
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
