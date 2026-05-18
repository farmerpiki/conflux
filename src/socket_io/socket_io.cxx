module;
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

struct io_uring;
struct io_uring_sqe;
struct __kernel_timespec;

export module conflux.socket_io;
import std;
import conflux.types;
import conflux.uring;
import conflux.uring.completion;
import conflux.uring.handle;
// ─── handle types ────────────────────────────────────────────────────────────

export using OwnedSocketHandle = IoHandle;
export using SocketHandle = RingFd;
// ─── SocketRawRing ───────────────────────────────────────────────────────────
// Non-owning wrapper around io_uring* for raw SQE submission.
// Does NOT own CompletionTable — raw callers dispatch CQEs themselves.

export class SocketRawRing {
	conflux::uring::RingRef ring_;

public:
	explicit SocketRawRing(
		io_uring *ring) noexcept
		: SocketRawRing(conflux::uring::RingRef{ring}) {}
	explicit SocketRawRing(
		io_uring &ring) noexcept
		: SocketRawRing(conflux::uring::RingRef{ring}) {}
	explicit SocketRawRing(
		conflux::uring::RingRef ring) noexcept
		: ring_{ring} {}
	[[nodiscard]] conflux::uring::RingRef ring() const noexcept { return ring_; }
	[[nodiscard]] conflux::uring::Sqe try_get_sqe() const noexcept { return ring_.try_get_sqe(); }
	[[nodiscard]] io_uring_sqe *get_sqe() const noexcept {
		auto sqe = try_get_sqe();
		return sqe ? sqe.raw() : nullptr;
	}
	[[nodiscard]] unsigned sq_space_left() const noexcept { return ring_.sq_space_left(); }
	[[nodiscard]] int submit() const noexcept { return ring_.submit(); }
	[[nodiscard]] bool reserve_sqe_slots(
		std::uint32_t n) const noexcept {
		return sq_space_left() >= n;
	}
	[[nodiscard]] conflux::uring::Sqe get_reserved_sqe() const noexcept {
		auto sqe = try_get_sqe();
		assert(sqe);
		return sqe;
	}
};
// ─── GenerationTable ─────────────────────────────────────────────────────────
// Per-slot generation counters. Rejects stale CQEs from closed/reused sockets.

export class GenerationTable {
	std::vector<std::uint32_t> gens_;

public:
	explicit GenerationTable(
		std::uint32_t capacity)
		: gens_(capacity, 0) {}
	void ensure_capacity(
		std::uint32_t id) {
		if (id >= gens_.size()) {
			gens_.resize(id + 1, 0);
		}
	}
	[[nodiscard]] std::uint32_t current(
		std::uint32_t id) const noexcept {
		return id < static_cast<std::uint32_t>(gens_.size()) ? gens_[id] : 0;
	}
	std::uint32_t advance(
		std::uint32_t id) noexcept {
		ensure_capacity(id);
		return ++gens_[id];
	}
	[[nodiscard]] bool alive(
		std::uint32_t id,
		std::uint32_t gen) const noexcept {
		return id < static_cast<std::uint32_t>(gens_.size()) && gens_[id] == gen;
	}
};
// ─── BufferRing ──────────────────────────────────────────────────────────────
// Owns a kernel buffer ring group. Manages slab allocation and recycling.

export class IncrementalRecvSlice;

export enum class BufferRingMode : std::uint8_t {
	classic_one_cqe_per_buffer,
	recv_bundle,
	incremental,
};
export struct BufferRingOptions {
	std::uint32_t count{4096};
	std::size_t buf_size{8192};
	std::uint16_t group_id{0};
	bool huge_pages{true};
	BufferRingMode mode{BufferRingMode::classic_one_cqe_per_buffer};
};

export enum class RecvDecodeError : std::uint8_t {
	bad_cqe,
	bad_id,
	bad_bounds,
	bad_window,
};
export enum class RecvPayloadStorage : std::uint8_t {
	none,
	provided_buffer_ring,
	recv_zc_reserved,
};
export enum class RecvPayloadPinning : std::uint8_t {
	none,
	kernel_buffer_ring_slot,
	user_dma_pinned_buffer,
};
export struct RecvPayloadDescriptor {
	RecvPayloadStorage storage{RecvPayloadStorage::none};
	RecvPayloadPinning pinning{RecvPayloadPinning::none};
	bool incremental{};
	bool multi_buffer{};
};
export [[nodiscard]] constexpr RecvPayloadDescriptor recv_payload_descriptor(
	BufferRingMode mode,
	bool bundle) noexcept {
	return {
		.storage = RecvPayloadStorage::provided_buffer_ring,
		.pinning = RecvPayloadPinning::kernel_buffer_ring_slot,
		.incremental = mode == BufferRingMode::incremental,
		.multi_buffer = bundle && mode == BufferRingMode::recv_bundle,
	};
}
export class RecvBuffer;
export class BufferRing {
	struct SlabDeleter {
		void operator ()(
			std::byte *p) const noexcept {
			::free(p);
		}
	};
	conflux::uring::BufRing ring_{};
	conflux::uring::RingRef uring_{static_cast<io_uring *>(nullptr)};
	std::unique_ptr<std::byte[], SlabDeleter> slab_;
	std::size_t buf_size_{};
	std::uint32_t count_{};
	std::uint16_t group_id_{};
	std::size_t slab_sz_{};
	std::vector<std::uint16_t> ring_order_;
	std::vector<std::uint32_t> id_pos_;
	std::vector<std::uint32_t> bundle_saved_pos_;
	std::vector<std::uint8_t> bundle_has_saved_pos_;
	std::vector<std::uint32_t> bundle_saved_keys_;
	std::vector<std::uint16_t> bundle_saved_ids_;
	std::vector<std::uint8_t> bundle_saved_used_;
	std::uint32_t bundle_saved_mask_{};
	std::uint32_t bundle_preserved_pos_{};
	std::vector<std::uint8_t> decoded_pos_;
	std::vector<std::uint8_t> observed_pos_;
	std::vector<std::uint8_t> recycle_ready_pos_;
	std::uint32_t head_pos_{}; // first logical ring position not yet observed from a CQE
	std::uint32_t recycle_head_pos_{}; // first logical ring position not yet returned to kernel
	std::uint32_t tail_pos_{};
	BufferRingMode mode_{BufferRingMode::classic_one_cqe_per_buffer};
	std::vector<std::size_t> incremental_offsets_{};
	friend class IncrementalRecvSlice;
	IncrementalRecvSlice friend buffer_slice_from_incremental_cqe(BufferRing &, int, std::uint32_t) noexcept;
	std::expected<IncrementalRecvSlice, RecvDecodeError> friend try_buffer_slice_from_incremental_cqe(
		BufferRing &,
		int,
		std::uint32_t) noexcept;
	[[nodiscard]] std::size_t &incremental_offset_ref(
		std::uint16_t id) noexcept {
		assert(mode_ == BufferRingMode::incremental);
		assert(id < count_);
		return incremental_offsets_[id];
	}
	[[nodiscard]] std::span<std::byte const> buffer_view_at_offset(
		std::uint16_t id,
		std::size_t offset,
		std::size_t len) const noexcept {
		return {slab_.get() + static_cast<std::size_t>(id) * buf_size_ + offset, len};
	}

public:
	BufferRing(
		io_uring *uring,
		BufferRingOptions opts,
		conflux::uring::IoUringCaps const &caps)
		: BufferRing(conflux::uring::RingRef{uring}, opts, caps) {}
	BufferRing(
		conflux::uring::RingRef uring,
		BufferRingOptions opts,
		conflux::uring::IoUringCaps const &caps)
		: ring_{}
		, uring_{uring}
		, buf_size_{opts.buf_size}
		, count_{opts.count}
		, group_id_{opts.group_id}
		, mode_{opts.mode} {
		if (opts.mode == BufferRingMode::incremental && !caps.feat_pbuf_ring_inc) {
			throw std::runtime_error{"BufferRingMode::incremental requires IORING_FEAT_PBUF_RING_INC (kernel 6.12+)"};
		}
		if (count_ == 0
			|| count_ > 32768U
			|| (count_ & (count_ - 1)) != 0
			|| buf_size_ == 0
			|| buf_size_ > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())
			|| count_ > std::numeric_limits<std::size_t>::max() / buf_size_) {
			throw std::runtime_error{"BufferRing invalid options"};
		}
		static_assert(conflux::uring::buf_ring_flags::has_inc, "IOU_PBUF_RING_INC required");
		slab_sz_ = static_cast<std::size_t>(count_) * buf_size_;
		std::size_t const aligned_sz = (slab_sz_ + 4095) & ~std::size_t{4095};
		if (aligned_sz < slab_sz_) {
			throw std::runtime_error{"BufferRing allocation overflow"};
		}
		auto *raw = static_cast<std::byte *>(::aligned_alloc(4096, aligned_sz));
		if (raw == nullptr) {
			throw std::bad_alloc{};
		}
		slab_.reset(raw);
		if (opts.huge_pages) {
			::madvise(raw, slab_sz_, MADV_HUGEPAGE);
			::madvise(raw, slab_sz_, MADV_DONTFORK);
		}
		unsigned const ring_flags = mode_ == BufferRingMode::incremental ? conflux::uring::buf_ring_flags::inc : 0u;
		auto built = conflux::uring::BufRing::setup(
			uring_,
			static_cast<unsigned>(count_),
			conflux::uring::BufGroupId{group_id_},
			ring_flags);
		if (!built) {
			slab_.reset();
			if (built.error() == EINVAL && mode_ == BufferRingMode::incremental) {
				throw std::runtime_error{"io_uring_setup_buf_ring: incremental mode requires kernel 6.12+ (IORING_FEAT_PBUF_RING_INC)"};
			}
			throw std::runtime_error{format("io_uring_setup_buf_ring failed: {}", built.error())};
		}
		ring_ = move(*built);
		for (std::uint32_t i = 0; i < count_; ++i) {
			ring_.add(
				raw + i * buf_size_,
				static_cast<std::uint32_t>(buf_size_),
				conflux::uring::BufId{static_cast<std::uint16_t>(i)},
				static_cast<int>(i));
		}
		ring_.advance(static_cast<int>(count_));
		ring_order_.resize(count_);
		id_pos_.resize(count_);
		decoded_pos_.assign(count_, 0);
		observed_pos_.assign(count_, 0);
		recycle_ready_pos_.assign(count_, 0);
		for (std::uint32_t i = 0; i < count_; ++i) {
			ring_order_[i] = static_cast<std::uint16_t>(i);
			id_pos_[i] = i;
		}
		head_pos_ = 0;
		recycle_head_pos_ = 0;
		tail_pos_ = count_;
		if (mode_ == BufferRingMode::incremental) {
			incremental_offsets_.assign(count_, std::size_t{0});
		}
		if (mode_ == BufferRingMode::recv_bundle) {
			bundle_saved_pos_.resize(count_);
			bundle_has_saved_pos_.assign(count_, 0);
			std::uint32_t const saved_slots = count_ * 8;
			bundle_saved_keys_.assign(saved_slots, 0);
			bundle_saved_ids_.assign(saved_slots, 0);
			bundle_saved_used_.assign(saved_slots, 0);
			bundle_saved_mask_ = saved_slots - 1;
			bundle_preserved_pos_ = 0;
		}
	}
	~BufferRing() = default;
	BufferRing(BufferRing const &) = delete;
	BufferRing &operator =(BufferRing const &) = delete;
	BufferRing(BufferRing &&) = delete;
	BufferRing &operator =(BufferRing &&) = delete;
	[[nodiscard]] std::span<std::byte const> buffer_view_checked(
		std::uint16_t id,
		std::size_t len) const noexcept {
		if (id >= count_) {
			return {};
		}
		return {slab_.get() + static_cast<std::size_t>(id) * buf_size_, min(len, buf_size_)};
	}
	[[nodiscard]] std::span<std::byte const> buffer_view_unchecked(
		std::uint16_t id,
		std::size_t len) const noexcept {
		return {slab_.get() + static_cast<std::size_t>(id) * buf_size_, min(len, buf_size_)};
	}
	[[nodiscard]] std::span<std::byte const> buffer_view(
		std::uint16_t id,
		std::size_t len) const noexcept {
		return buffer_view_checked(id, len);
	}
	[[nodiscard]] std::span<std::byte> buffer_mut_checked(
		std::uint16_t id) noexcept {
		if (id >= count_) {
			return {};
		}
		return {slab_.get() + static_cast<std::size_t>(id) * buf_size_, buf_size_};
	}
	[[nodiscard]] std::span<std::byte> buffer_mut_unchecked(
		std::uint16_t id) noexcept {
		return {slab_.get() + static_cast<std::size_t>(id) * buf_size_, buf_size_};
	}
	[[nodiscard]] std::span<std::byte> buffer_mut(
		std::uint16_t id) noexcept {
		return buffer_mut_checked(id);
	}
	void recycle(
		std::uint16_t id) noexcept {
		if (mode_ == BufferRingMode::recv_bundle) {
			std::uint32_t const idx = tail_pos_ % count_;
			ring_order_[idx] = id;
			id_pos_[id] = tail_pos_;
			decoded_pos_[idx] = 0;
			observed_pos_[idx] = 0;
			recycle_ready_pos_[idx] = 0;
			ring_.add(
				slab_.get() + static_cast<std::size_t>(id) * buf_size_,
				static_cast<std::uint32_t>(buf_size_),
				conflux::uring::BufId{id},
				0);
			ring_.advance(1);
			++tail_pos_;
			return;
		}

		for (std::uint32_t pos = recycle_head_pos_; pos != tail_pos_; ++pos) {
			std::uint32_t const idx = pos % count_;
			if (observed_pos_[idx] != 0 && recycle_ready_pos_[idx] == 0 && ring_id_at(pos) == id) {
				recycle_ready_pos_[idx] = 1;
				flush_recycle_ready();
				return;
			}
		}

		std::uint32_t const idx = tail_pos_ % count_;
		ring_order_[idx] = id;
		id_pos_[id] = tail_pos_;
		decoded_pos_[idx] = 0;
		observed_pos_[idx] = 0;
		recycle_ready_pos_[idx] = 0;
		ring_.add(
			slab_.get() + static_cast<std::size_t>(id) * buf_size_,
			static_cast<std::uint32_t>(buf_size_),
			conflux::uring::BufId{id},
			0);
		ring_.advance(1);
		++tail_pos_;
	}
	void recycle_batch(
		std::span<std::uint16_t const> ids) noexcept {
		std::uint32_t i = 0;
		if (mode_ == BufferRingMode::recv_bundle) {
			for (auto id: ids) {
				std::uint32_t const idx = (tail_pos_ + i) % count_;
				ring_order_[idx] = id;
				id_pos_[id] = tail_pos_ + i;
				decoded_pos_[idx] = 0;
				observed_pos_[idx] = 0;
				recycle_ready_pos_[idx] = 0;
				ring_.add(
					slab_.get() + static_cast<std::size_t>(id) * buf_size_,
					static_cast<std::uint32_t>(buf_size_),
					conflux::uring::BufId{id},
					static_cast<int>(i));
				++i;
			}
			ring_.advance(static_cast<int>(ids.size()));
			tail_pos_ += static_cast<std::uint32_t>(ids.size());
			return;
		}
		for (auto id: ids) {
			std::uint32_t const idx = (tail_pos_ + i) % count_;
			ring_order_[idx] = id;
			id_pos_[id] = tail_pos_ + i;
			decoded_pos_[idx] = 0;
			observed_pos_[idx] = 0;
			recycle_ready_pos_[idx] = 0;
			ring_.add(
				slab_.get() + static_cast<std::size_t>(id) * buf_size_,
				static_cast<std::uint32_t>(buf_size_),
				conflux::uring::BufId{id},
				static_cast<int>(i));
			++i;
		}
		ring_.advance(static_cast<int>(ids.size()));
		tail_pos_ += static_cast<std::uint32_t>(ids.size());
	}
	void flush_recycle_ready() noexcept {
		std::uint32_t batch = 0;
		while (recycle_head_pos_ != tail_pos_ && recycle_ready_pos_[recycle_head_pos_ % count_] != 0) {
			std::uint16_t const id = ring_id_at(recycle_head_pos_);
			std::uint32_t const old_idx = recycle_head_pos_ % count_;
			std::uint32_t const idx = tail_pos_ % count_;
			recycle_ready_pos_[old_idx] = 0;
			observed_pos_[old_idx] = 0;
			ring_order_[idx] = id;
			id_pos_[id] = tail_pos_;
			decoded_pos_[idx] = 0;
			observed_pos_[idx] = 0;
			recycle_ready_pos_[idx] = 0;
			ring_.add(
				slab_.get() + static_cast<std::size_t>(id) * buf_size_,
				static_cast<std::uint32_t>(buf_size_),
				conflux::uring::BufId{id},
				static_cast<int>(batch));
			++batch;
			++recycle_head_pos_;
			++tail_pos_;
		}
		if (batch != 0) {
			ring_.advance(static_cast<int>(batch));
		}
	}
	[[nodiscard]] bool recycle_selected_buffer(
		std::uint16_t id) noexcept {
		if (id >= count_ || mode_ == BufferRingMode::incremental) {
			return false;
		}
		auto const start = find_start_pos(id, 1, mode_ == BufferRingMode::recv_bundle);
		if (!start) [[unlikely]] {
			return false;
		}
		consume_at(*start, 1);
		recycle_range(*start, 1);
		return true;
	}
	void recycle_range(
		std::uint32_t start_pos,
		std::uint32_t cnt) noexcept {
		if (cnt == 0) {
			return;
		}
		if (mode_ == BufferRingMode::recv_bundle) {
			assert(start_pos + cnt >= start_pos);
			assert(start_pos + cnt <= tail_pos_);
			for (std::uint32_t i = 0; i < cnt; ++i) {
				std::uint32_t const old_pos = start_pos + i;
				std::uint16_t const id = ring_id_at(old_pos);
				std::uint32_t const idx = (tail_pos_ + i) % count_;
				ring_order_[idx] = id;
				id_pos_[id] = tail_pos_ + i;
				decoded_pos_[idx] = 0;
				observed_pos_[idx] = 0;
				recycle_ready_pos_[idx] = 0;
				if (bundle_has_saved_pos_[id] != 0 && bundle_saved_pos_[id] == old_pos) {
					bundle_has_saved_pos_[id] = 0;
				}
				bundle_saved_erase(old_pos);
				ring_.add(
					slab_.get() + static_cast<std::size_t>(id) * buf_size_,
					static_cast<std::uint32_t>(buf_size_),
					conflux::uring::BufId{id},
					static_cast<int>(i));
			}
			ring_.advance(static_cast<int>(cnt));
			tail_pos_ += cnt;
			return;
		}
		assert(start_pos >= recycle_head_pos_);
		assert(start_pos + cnt <= tail_pos_);
		for (std::uint32_t i = 0; i < cnt; ++i) {
			recycle_ready_pos_[(start_pos + i) % count_] = 1;
		}
		flush_recycle_ready();
	}
	void preserve_bundle_positions_until(
		std::uint32_t end_pos) noexcept {
		assert(mode_ == BufferRingMode::recv_bundle);
		for (; bundle_preserved_pos_ < end_pos; ++bundle_preserved_pos_) {
			std::uint32_t const pos = bundle_preserved_pos_;
			std::uint16_t const id = ring_id_at(pos);
			bundle_saved_insert_or_assign(pos, id);
			bundle_saved_pos_[id] = pos;
			bundle_has_saved_pos_[id] = 1;
		}
	}
	[[nodiscard]] static std::uint32_t bundle_hash(
		std::uint32_t pos) noexcept {
		return pos * 2654435761u;
	}
	[[nodiscard]] std::optional<std::uint16_t> bundle_saved_find(
		std::uint32_t pos) const noexcept {
		if (bundle_saved_used_.empty()) {
			return std::nullopt;
		}
		std::uint32_t i = bundle_hash(pos) & bundle_saved_mask_;
		for (std::uint32_t n = 0, limit = static_cast<std::uint32_t>(bundle_saved_used_.size()); n < limit; ++n) {
			if (bundle_saved_used_[i] == 0) {
				return std::nullopt;
			}
			if (bundle_saved_keys_[i] == pos) {
				return bundle_saved_ids_[i];
			}
			i = (i + 1) & bundle_saved_mask_;
		}
		return std::nullopt;
	}
	void bundle_saved_insert_or_assign(
		std::uint32_t pos,
		std::uint16_t id) noexcept {
		assert(!bundle_saved_used_.empty());
		std::uint32_t i = bundle_hash(pos) & bundle_saved_mask_;
		for (std::uint32_t n = 0, limit = static_cast<std::uint32_t>(bundle_saved_used_.size()); n < limit; ++n) {
			if (bundle_saved_used_[i] == 0 || bundle_saved_keys_[i] == pos) {
				bundle_saved_used_[i] = 1;
				bundle_saved_keys_[i] = pos;
				bundle_saved_ids_[i] = id;
				return;
			}
			i = (i + 1) & bundle_saved_mask_;
		}
		assert(false && "recv-bundle saved-order table exhausted");
		std::abort();
	}
	void bundle_saved_erase(
		std::uint32_t pos) noexcept {
		if (bundle_saved_used_.empty()) {
			return;
		}
		std::uint32_t i = bundle_hash(pos) & bundle_saved_mask_;
		for (std::uint32_t n = 0, limit = static_cast<std::uint32_t>(bundle_saved_used_.size()); n < limit; ++n) {
			if (bundle_saved_used_[i] == 0) {
				return;
			}
			if (bundle_saved_keys_[i] == pos) {
				bundle_saved_used_[i] = 0;
				for (std::uint32_t j = (i + 1) & bundle_saved_mask_; bundle_saved_used_[j] != 0;
					j = (j + 1) & bundle_saved_mask_) {
					std::uint32_t const key = bundle_saved_keys_[j];
					std::uint16_t const val = bundle_saved_ids_[j];
					bundle_saved_used_[j] = 0;
					bundle_saved_insert_or_assign(key, val);
				}
				return;
			}
			i = (i + 1) & bundle_saved_mask_;
		}
	}

	void consume_at(
		std::uint32_t start_pos,
		std::uint32_t cnt) noexcept {
		if (mode_ == BufferRingMode::recv_bundle) {
			assert(start_pos + cnt >= start_pos);
			assert(start_pos + cnt <= tail_pos_);
			for (std::uint32_t i = 0; i < cnt; ++i) {
				decoded_pos_[(start_pos + i) % count_] = 1;
			}
			while (head_pos_ != tail_pos_ && decoded_pos_[head_pos_ % count_] != 0) {
				decoded_pos_[head_pos_ % count_] = 0;
				++head_pos_;
			}
			return;
		}
		assert(start_pos >= recycle_head_pos_);
		assert(start_pos + cnt <= tail_pos_);
		for (std::uint32_t i = 0; i < cnt; ++i) {
			std::uint32_t const idx = (start_pos + i) % count_;
			decoded_pos_[idx] = 1;
			observed_pos_[idx] = 1;
		}
		while (head_pos_ != tail_pos_ && decoded_pos_[head_pos_ % count_] != 0) {
			decoded_pos_[head_pos_ % count_] = 0;
			++head_pos_;
		}
	}
	std::uint32_t consume(
		std::uint32_t cnt) noexcept {
		std::uint32_t const old = head_pos_;
		consume_at(old, cnt);
		return old;
	}
	[[nodiscard]] std::optional<std::uint32_t> find_start_pos(
		std::uint16_t first_id,
		std::uint32_t cnt,
		bool bundle) noexcept {
		if (cnt == 0 || cnt > count_ || first_id >= count_) {
			return std::nullopt;
		}
		std::uint32_t const pos = mode_ == BufferRingMode::recv_bundle && bundle_has_saved_pos_[first_id] != 0
			? bundle_saved_pos_[first_id]
			: (bundle ? id_pos_[first_id] : head_pos_);
		if (pos + cnt < pos || pos + cnt > tail_pos_) {
			return std::nullopt;
		}
		if (mode_ != BufferRingMode::recv_bundle && pos < recycle_head_pos_) {
			return std::nullopt;
		}
		if (mode_ == BufferRingMode::recv_bundle) {
			preserve_bundle_positions_until(pos + cnt);
		}
		if (ring_id_at(pos) != first_id) {
			return std::nullopt;
		}
		return pos;
	}
	bool reclaim_incremental_partial(
		std::uint16_t id) noexcept {
		if (mode_ != BufferRingMode::incremental || id >= count_) {
			return false;
		}
		auto &off = incremental_offsets_[id];
		if (off == 0) {
			return false;
		}
		off = 0;
		consume(1);
		recycle(id);
		return true;
	}
	[[nodiscard]] std::uint16_t ring_id_at(
		std::uint32_t pos) const noexcept {
		if (mode_ == BufferRingMode::recv_bundle) {
			auto const id = bundle_saved_find(pos);
			if (id) {
				return *id;
			}
		}
		return ring_order_[pos % count_];
	}
	[[nodiscard]] BufferRingMode mode() const noexcept { return mode_; }
	[[nodiscard]] RecvBuffer lease(std::uint16_t id, std::size_t len) noexcept;
	[[nodiscard]] std::uint16_t group_id() const noexcept { return group_id_; }
	[[nodiscard]] std::size_t buf_size() const noexcept { return buf_size_; }
	[[nodiscard]] std::uint32_t count() const noexcept { return count_; }
	[[nodiscard]] std::uint32_t debug_head_pos() const noexcept { return head_pos_; }
};
// ─── RecvBuffer ──────────────────────────────────────────────────────────────
// RAII lease on a single buffer ring slot. Auto-recycles unless detached.

export class RecvBuffer {
	BufferRing *ring_{nullptr};
	std::uint16_t id_{};
	std::size_t len_{};
	bool armed_{true};

public:
	RecvBuffer(
		BufferRing *ring,
		std::uint16_t id,
		std::size_t len) noexcept
		: ring_{ring}
		, id_{id}
		, len_{len} {}
	RecvBuffer(RecvBuffer const &) = delete;
	RecvBuffer &operator =(RecvBuffer const &) = delete;
	RecvBuffer(
		RecvBuffer &&o) noexcept
		: ring_{exchange(o.ring_, nullptr)}
		, id_{o.id_}
		, len_{o.len_}
		, armed_{exchange(o.armed_, false)} {}
	RecvBuffer &operator =(
		RecvBuffer &&o) noexcept {
		if (this != &o) {
			if ((ring_ != nullptr) && armed_) {
				ring_->recycle(id_);
			}
			ring_ = exchange(o.ring_, nullptr);
			id_ = o.id_;
			len_ = o.len_;
			armed_ = exchange(o.armed_, false);
		}
		return *this;
	}
	~RecvBuffer() {
		if ((ring_ != nullptr) && armed_) {
			ring_->recycle(id_);
		}
	}
	[[nodiscard]] std::span<std::byte const> view() const noexcept {
		return (ring_ != nullptr) ? ring_->buffer_view_checked(id_, len_) : std::span<std::byte const>{};
	}
	[[nodiscard]] std::uint16_t id() const noexcept { return id_; }
	[[nodiscard]] std::size_t size() const noexcept { return len_; }
	void release() noexcept {
		if ((ring_ != nullptr) && armed_) {
			ring_->recycle(id_);
			armed_ = false;
		}
	}
	void detach() noexcept { armed_ = false; }
};
inline RecvBuffer BufferRing::lease(
	std::uint16_t id,
	std::size_t len) noexcept {
	return RecvBuffer{this, id, len};
}
// ─── RecvSlice / RecvSlices ──────────────────────────────────────────────────
// Zero-allocation view over one or more buffer-ring slots from a single CQE.
// No auto-recycle — caller must call recycle_all() or detach().

export struct RecvSlice {
	std::uint16_t id;
	std::span<std::byte const> bytes;
};
export class RecvSlices {
	BufferRing *ring_{};
	std::uint32_t start_pos_{};
	std::uint32_t count_{};
	std::size_t total_{};
	bool detached_{false};

public:
	RecvSlices() noexcept = default;
	RecvSlices(
		BufferRing *ring,
		std::uint32_t start,
		std::uint32_t cnt,
		std::size_t total) noexcept
		: ring_{ring}
		, start_pos_{start}
		, count_{cnt}
		, total_{total} {}
	RecvSlices(RecvSlices const &) = delete;
	RecvSlices &operator =(RecvSlices const &) = delete;
	RecvSlices(
		RecvSlices &&o) noexcept
		: ring_{exchange(o.ring_, nullptr)}
		, start_pos_{o.start_pos_}
		, count_{o.count_}
		, total_{o.total_}
		, detached_{o.detached_} {}
	RecvSlices &operator =(
		RecvSlices &&o) noexcept {
		if (this != &o) {
			ring_ = exchange(o.ring_, nullptr);
			start_pos_ = o.start_pos_;
			count_ = o.count_;
			total_ = o.total_;
			detached_ = o.detached_;
		}
		return *this;
	}
	[[nodiscard]] bool valid() const noexcept { return ring_ != nullptr && count_ > 0; }
	[[nodiscard]] std::size_t total_size() const noexcept { return total_; }
	[[nodiscard]] std::uint32_t count() const noexcept { return count_; }
	struct iterator {
		RecvSlices const *slices_;
		std::uint32_t idx_;
		[[nodiscard]] RecvSlice operator *() const noexcept {
			std::uint16_t const id = slices_->ring_->ring_id_at(slices_->start_pos_ + idx_);
			std::size_t const off = static_cast<std::size_t>(idx_) * slices_->ring_->buf_size();
			std::size_t const len = (idx_ + 1 < slices_->count_) ? slices_->ring_->buf_size() : slices_->total_ - off;
			return {id, slices_->ring_->buffer_view_unchecked(id, len)};
		}
		iterator &operator ++() noexcept {
			++idx_;
			return *this;
		}
		bool operator ==(
			iterator const &o) const noexcept {
			return idx_ == o.idx_;
		}
		bool operator !=(
			iterator const &o) const noexcept {
			return idx_ != o.idx_;
		}
	};
	[[nodiscard]] iterator begin() const noexcept { return {this, 0}; }
	[[nodiscard]] iterator end() const noexcept { return {this, count_}; }
	void recycle_all() noexcept {
		if ((ring_ == nullptr) || detached_) {
			return;
		}
		ring_->recycle_range(start_pos_, count_);
		ring_ = nullptr;
	}
	void detach() noexcept { detached_ = true; }
};
// ─── IncrementalRecvSlice ────────────────────────────────────────────────────
// One CQE's worth of incremental buffer data. Recycles only on final CQE.

export class IncrementalRecvSlice {
	BufferRing *ring_{};
	std::uint16_t id_{};
	std::size_t offset_{};
	std::size_t len_{};
	bool more_{};
	bool detached_{false};

public:
	IncrementalRecvSlice() noexcept = default;
	IncrementalRecvSlice(
		BufferRing *ring,
		std::uint16_t id,
		std::size_t offset,
		std::size_t len,
		bool more) noexcept
		: ring_{ring}
		, id_{id}
		, offset_{offset}
		, len_{len}
		, more_{more} {}
	IncrementalRecvSlice(IncrementalRecvSlice const &) = delete;
	IncrementalRecvSlice &operator =(IncrementalRecvSlice const &) = delete;
	IncrementalRecvSlice(
		IncrementalRecvSlice &&o) noexcept
		: ring_{exchange(o.ring_, nullptr)}
		, id_{o.id_}
		, offset_{o.offset_}
		, len_{o.len_}
		, more_{o.more_}
		, detached_{o.detached_} {}
	~IncrementalRecvSlice() noexcept { recycle_if_final(); }
	IncrementalRecvSlice &operator =(
		IncrementalRecvSlice &&o) noexcept {
		if (this != &o) {
			recycle_if_final();
			ring_ = exchange(o.ring_, nullptr);
			id_ = o.id_;
			offset_ = o.offset_;
			len_ = o.len_;
			more_ = o.more_;
			detached_ = o.detached_;
		}
		return *this;
	}
	[[nodiscard]] bool valid() const noexcept { return ring_ != nullptr && len_ > 0; }
	[[nodiscard]] std::uint16_t id() const noexcept { return id_; }
	[[nodiscard]] std::size_t offset() const noexcept { return offset_; }
	[[nodiscard]] std::size_t size() const noexcept { return len_; }
	[[nodiscard]] bool more() const noexcept { return more_; }
	[[nodiscard]] std::span<std::byte const> bytes() const noexcept {
		if (ring_ == nullptr) {
			return {};
		}
		return ring_->buffer_view_at_offset(id_, offset_, len_);
	}
	void recycle_if_final() noexcept {
		if (ring_ == nullptr || detached_ || more_) {
			return;
		}
		ring_->recycle(id_);
		ring_ = nullptr;
	}
	void detach() noexcept { detached_ = true; }
};
// ─── RecvPayload ─────────────────────────────────────────────────────────────
// RAII owner for bytes reported by one recv CQE.  Today this wraps provided
// buffer-ring slots; the storage/pinning descriptor reserves the shape for a
// later RECV_ZC backend without changing HTTP recv ownership again.

export struct RecvPayloadChunk {
	std::span<std::byte const> bytes;
};
export class RecvPayload {
	enum class Variant : std::uint8_t {
		none,
		slices,
		incremental,
	};
	Variant variant_{Variant::none};
	RecvPayloadDescriptor descriptor_{};
	RecvSlices slices_{};
	IncrementalRecvSlice incremental_{};

public:
	RecvPayload() noexcept = default;
	static RecvPayload from_slices(
		RecvPayloadDescriptor descriptor,
		RecvSlices slices) noexcept {
		RecvPayload payload;
		payload.variant_ = Variant::slices;
		payload.descriptor_ = descriptor;
		payload.slices_ = move(slices);
		return payload;
	}
	static RecvPayload from_incremental(
		RecvPayloadDescriptor descriptor,
		IncrementalRecvSlice slice) noexcept {
		RecvPayload payload;
		payload.variant_ = Variant::incremental;
		payload.descriptor_ = descriptor;
		payload.incremental_ = move(slice);
		return payload;
	}
	RecvPayload(RecvPayload const &) = delete;
	RecvPayload &operator =(RecvPayload const &) = delete;
	RecvPayload(
		RecvPayload &&o) noexcept
		: variant_{exchange(o.variant_, Variant::none)}
		, descriptor_{o.descriptor_}
		, slices_{move(o.slices_)}
		, incremental_{move(o.incremental_)} {}
	RecvPayload &operator =(
		RecvPayload &&o) noexcept {
		if (this != &o) {
			recycle_all();
			variant_ = exchange(o.variant_, Variant::none);
			descriptor_ = o.descriptor_;
			slices_ = move(o.slices_);
			incremental_ = move(o.incremental_);
		}
		return *this;
	}
	~RecvPayload() noexcept { recycle_all(); }
	[[nodiscard]] bool valid() const noexcept { return variant_ != Variant::none; }
	[[nodiscard]] RecvPayloadDescriptor descriptor() const noexcept { return descriptor_; }
	[[nodiscard]] RecvPayloadStorage storage() const noexcept { return descriptor_.storage; }
	[[nodiscard]] RecvPayloadPinning pinning() const noexcept { return descriptor_.pinning; }
	[[nodiscard]] bool incremental() const noexcept { return descriptor_.incremental; }
	[[nodiscard]] bool multi_buffer() const noexcept { return descriptor_.multi_buffer; }
	[[nodiscard]] std::size_t total_size() const noexcept {
		switch (variant_) {
		case Variant::slices     : return slices_.total_size();
		case Variant::incremental: return incremental_.size();
		case Variant::none       : return 0;
		}
		return 0;
	}
	[[nodiscard]] std::uint32_t chunk_count() const noexcept {
		switch (variant_) {
		case Variant::slices     : return slices_.count();
		case Variant::incremental: return incremental_.valid() ? 1u : 0u;
		case Variant::none       : return 0u;
		}
		return 0u;
	}
	struct iterator {
		RecvPayload const *payload{};
		std::uint32_t idx{};
		[[nodiscard]] RecvPayloadChunk operator *() const noexcept {
			if (payload == nullptr || payload->variant_ == Variant::none) {
				return {};
			}
			if (payload->variant_ == Variant::incremental) {
				return idx == 0 ? RecvPayloadChunk{payload->incremental_.bytes()} : RecvPayloadChunk{};
			}
			auto it = payload->slices_.begin();
			for (std::uint32_t i = 0; i < idx; ++i) {
				++it;
			}
			return {(*it).bytes};
		}
		iterator &operator ++() noexcept {
			++idx;
			return *this;
		}
		bool operator ==(
			iterator const &o) const noexcept {
			return payload == o.payload && idx == o.idx;
		}
		bool operator !=(
			iterator const &o) const noexcept {
			return !(*this == o);
		}
	};
	[[nodiscard]] iterator begin() const noexcept { return {this, 0}; }
	[[nodiscard]] iterator end() const noexcept { return {this, chunk_count()}; }
	void recycle_all() noexcept {
		if (variant_ == Variant::slices) {
			slices_.recycle_all();
		}
		if (variant_ == Variant::incremental) {
			incremental_.recycle_if_final();
		}
		variant_ = Variant::none;
	}
	void detach() noexcept {
		if (variant_ == Variant::slices) {
			slices_.detach();
		}
		if (variant_ == Variant::incremental) {
			incremental_.detach();
		}
	}
};
// ─── CQE helpers ─────────────────────────────────────────────────────────────

export [[nodiscard]] inline std::uint16_t cqe_buffer_id(
	std::uint32_t cqe_flags) noexcept {
	return conflux::uring::cqe_flags::buf_id(conflux::uring::CqeFlags{cqe_flags}).v;
}
export [[nodiscard]] inline bool cqe_has_more(
	std::uint32_t cqe_flags) noexcept {
	return conflux::uring::CqeFlags{cqe_flags}.any(conflux::uring::cqe_flags::more);
}
export [[nodiscard]] inline bool cqe_has_buffer(
	std::uint32_t cqe_flags) noexcept {
	return conflux::uring::CqeFlags{cqe_flags}.any(conflux::uring::cqe_flags::buffer);
}
export [[nodiscard]] inline bool cqe_has_buf_more(
	std::uint32_t cqe_flags) noexcept {
	return conflux::uring::CqeFlags{cqe_flags}.any(conflux::uring::cqe_flags::buf_more);
}
export [[nodiscard]] IncrementalRecvSlice buffer_slice_from_incremental_cqe(
	BufferRing &ring,
	int res,
	std::uint32_t flags) noexcept {
	assert(res > 0);
	assert(cqe_has_buffer(flags));
	assert(ring.mode() == BufferRingMode::incremental);
	std::uint16_t const id = cqe_buffer_id(flags);
	assert(id < ring.count());
	std::size_t &off = ring.incremental_offset_ref(id);
	assert(off <= ring.buf_size());
	assert(static_cast<std::size_t>(res) <= ring.buf_size() - off);
	bool const more = cqe_has_buf_more(flags);
	IncrementalRecvSlice slice{&ring, id, off, static_cast<std::size_t>(res), more};
	off += static_cast<std::size_t>(res);
	if (!more) {
		off = 0;
		ring.consume(1);
	}
	return slice;
}
export [[nodiscard]] std::expected<IncrementalRecvSlice, RecvDecodeError> try_buffer_slice_from_incremental_cqe(
	BufferRing &ring,
	int res,
	std::uint32_t flags) noexcept {
	if (res <= 0 || !cqe_has_buffer(flags) || ring.mode() != BufferRingMode::incremental) {
		return std::unexpected(RecvDecodeError::bad_cqe);
	}
	std::uint16_t const id = cqe_buffer_id(flags);
	if (id >= ring.count()) {
		return std::unexpected(RecvDecodeError::bad_id);
	}
	std::size_t &off = ring.incremental_offset_ref(id);
	std::size_t const len = static_cast<std::size_t>(res);
	if (off > ring.buf_size() || len > ring.buf_size() - off) {
		return std::unexpected(RecvDecodeError::bad_bounds);
	}
	bool const more = cqe_has_buf_more(flags);
	IncrementalRecvSlice slice{&ring, id, off, len, more};
	off += len;
	if (!more) {
		off = 0;
		ring.consume(1);
	}
	return slice;
}
export [[nodiscard]] std::expected<RecvSlices, RecvDecodeError> try_buffer_slices_from_cqe(
	BufferRing &ring,
	int res,
	std::uint32_t flags,
	bool bundle) noexcept {
	if (res <= 0 || !cqe_has_buffer(flags)) {
		return std::unexpected(RecvDecodeError::bad_cqe);
	}
	std::size_t const total = static_cast<std::size_t>(res);
	std::uint32_t const cnt = bundle ? static_cast<std::uint32_t>((total + ring.buf_size() - 1) / ring.buf_size()) : 1u;
	if (cnt == 0 || cnt > ring.count()) {
		return std::unexpected(RecvDecodeError::bad_bounds);
	}
	std::uint16_t const first_id = cqe_buffer_id(flags);
	auto const start = ring.find_start_pos(first_id, cnt, bundle);
	if (!start) [[unlikely]] {
		return std::unexpected(RecvDecodeError::bad_window);
	}
	ring.consume_at(*start, cnt);
	return RecvSlices{&ring, *start, cnt, total};
}
export [[nodiscard]] RecvSlices buffer_slices_from_cqe(
	BufferRing &ring,
	int res,
	std::uint32_t flags,
	bool bundle) noexcept {
	if (res <= 0 || !cqe_has_buffer(flags)) {
		assert(!cqe_has_buffer(flags));
		return {};
	}
	auto slices = try_buffer_slices_from_cqe(ring, res, flags, bundle);
	if (!slices) [[unlikely]] {
		assert(false && "CQE buffer id/range is not present in the userspace buffer-ring window");
		return {};
	}
	return move(*slices);
}
export [[nodiscard]] std::expected<RecvPayload, RecvDecodeError> try_recv_payload_from_cqe(
	BufferRing &ring,
	int res,
	std::uint32_t flags,
	bool bundle) noexcept {
	auto descriptor = recv_payload_descriptor(ring.mode(), bundle);
	if (ring.mode() == BufferRingMode::incremental) {
		auto slice = try_buffer_slice_from_incremental_cqe(ring, res, flags);
		if (!slice) [[unlikely]] {
			return std::unexpected(slice.error());
		}
		return RecvPayload::from_incremental(descriptor, move(*slice));
	}
	auto slices = try_buffer_slices_from_cqe(ring, res, flags, bundle);
	if (!slices) [[unlikely]] {
		return std::unexpected(slices.error());
	}
	return RecvPayload::from_slices(descriptor, move(*slices));
}
export [[nodiscard]] RecvPayload recv_payload_from_cqe(
	BufferRing &ring,
	int res,
	std::uint32_t flags,
	bool bundle) noexcept {
	auto payload = try_recv_payload_from_cqe(ring, res, flags, bundle);
	if (!payload) [[unlikely]] {
		assert(false && "CQE recv payload is not present in the userspace ownership window");
		return {};
	}
	return move(*payload);
}
// ─── DirectFdTable ───────────────────────────────────────────────────────────
// Registers a sparse fixed-file table with io_uring.
// accept_direct auto-allocates slots; close_direct frees them.

export class DirectFdTable {
	conflux::uring::RingRef ring_;
	std::uint32_t capacity_{};
	int err_{0};
	bool registered_{false};

public:
	DirectFdTable(
		io_uring *ring,
		std::uint32_t max_slots)
		: DirectFdTable(conflux::uring::RingRef{ring}, max_slots) {}
	DirectFdTable(
		conflux::uring::RingRef ring,
		std::uint32_t max_slots)
		: ring_{ring}
		, capacity_{max_slots} {
		err_ = ring_.register_files_sparse(capacity_);
		if (err_ == 0) {
			registered_ = true;
		}
	}
	~DirectFdTable() {
		if (registered_) {
			auto _ = ring_.unregister_files();
		}
	}
	DirectFdTable(DirectFdTable const &) = delete;
	DirectFdTable &operator =(DirectFdTable const &) = delete;
	DirectFdTable(DirectFdTable &&) = delete;
	DirectFdTable &operator =(DirectFdTable &&) = delete;
	[[nodiscard]] bool install(
		std::uint32_t slot,
		int fd) {
		if (!registered_ || slot >= capacity_) {
			return false;
		}
		return ring_.register_files_update(slot, std::span<int const>{&fd, 1}) == 1;
	}
	[[nodiscard]] bool registered() const noexcept { return registered_; }
	[[nodiscard]] int error() const noexcept { return err_; }
	[[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
};
// ─── raw submission: accept ──────────────────────────────────────────────────
// All borrowed data (buffers, iovecs) must remain valid until CQE completion.

export bool submit_accept_multishot_borrowed(
	SocketRawRing &ring,
	SocketHandle listen,
	sockaddr *addr,
	socklen_t *addrlen,
	std::uint64_t user_data,
	int accept_flags,
	bool direct = true) noexcept {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	if (direct) {
		sqe.prep_multishot_accept_direct(listen.sqe_fd(), addr, addrlen, accept_flags);
	} else {
		sqe.prep_multishot_accept(listen.sqe_fd(), addr, addrlen, accept_flags);
	}
	sqe.add_flags(listen.sqe_fd_flags());
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_accept_multishot_borrowed(
	SocketRawRing &ring,
	SocketHandle listen,
	sockaddr *addr,
	socklen_t *addrlen,
	std::uint64_t user_data,
	conflux::uring::IoUringCaps const &caps,
	int accept_flags,
	bool direct) noexcept {
	return submit_accept_multishot_borrowed(
		ring,
		listen,
		addr,
		addrlen,
		user_data,
		accept_flags,
		direct && caps.accept_direct_supported);
}
// compat wrappers — zero accept_flags
export bool submit_accept_multishot_borrowed(
	SocketRawRing &ring,
	SocketHandle listen,
	sockaddr *addr,
	socklen_t *addrlen,
	std::uint64_t user_data,
	bool direct = true) noexcept {
	return submit_accept_multishot_borrowed(ring, listen, addr, addrlen, user_data, 0, direct);
}
export bool submit_accept_multishot_borrowed(
	SocketRawRing &ring,
	SocketHandle listen,
	sockaddr *addr,
	socklen_t *addrlen,
	std::uint64_t user_data,
	conflux::uring::IoUringCaps const &caps,
	bool direct) noexcept {
	return submit_accept_multishot_borrowed(ring, listen, addr, addrlen, user_data, caps, 0, direct);
}
// ─── raw submission: recv ────────────────────────────────────────────────────

export enum class RecvArmPolicy : std::uint8_t {
	default_,
	poll_first,
};
export [[nodiscard]] RecvArmPolicy resolve_recv_arm_policy(
	bool auto_enabled,
	bool recv_poll_first,
	bool have_last_flags,
	std::uint32_t last_flags) noexcept {
	if (!auto_enabled || !recv_poll_first || !have_last_flags) {
		return RecvArmPolicy::default_;
	}
	bool const nonempty = conflux::uring::CqeFlags{last_flags}.any(conflux::uring::cqe_flags::sock_nonempty);
	return nonempty ? RecvArmPolicy::default_ : RecvArmPolicy::poll_first;
}
export bool submit_recv_multishot(
	SocketRawRing &ring,
	SocketHandle handle,
	BufferRing &bufs,
	std::uint64_t user_data,
	bool bundle = false,
	RecvArmPolicy arm = RecvArmPolicy::default_) {
	assert(!(bundle && bufs.mode() == BufferRingMode::incremental));
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_recv_multishot(handle.sqe_fd(), nullptr, 0, conflux::uring::MsgFlags{});
	sqe.buf_group(conflux::uring::BufGroupId{bufs.group_id()});
	conflux::uring::IoPrioFlags ioprio{};
#if CONFLUX_ENABLE_RECV_BUNDLE
	if (bundle && bufs.mode() == BufferRingMode::recv_bundle) {
		ioprio = ioprio | conflux::uring::ioprio_flags::recvsend_bundle;
	}
#else
	auto _ = bundle;
#endif
	if (arm == RecvArmPolicy::poll_first) {
		ioprio = ioprio | conflux::uring::ioprio_flags::recvsend_poll_first;
	}
	if (ioprio) {
		sqe.ioprio(ioprio);
	}
	sqe.add_flags(conflux::uring::sqe_flags::buffer_select);
	sqe.add_flags(handle.sqe_fd_flags());
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: send ────────────────────────────────────────────────────

export bool submit_send_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	void const *data,
	std::size_t len,
	std::uint64_t user_data,
	int msg_flags = MSG_NOSIGNAL) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_send(handle.sqe_fd(), data, len, conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)});
	sqe.add_flags(handle.sqe_fd_flags());
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_send_fixed_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	std::uint32_t buf_idx,
	void const *data,
	std::size_t len,
	std::uint64_t user_data,
	int msg_flags = MSG_NOSIGNAL) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_send(handle.sqe_fd(), data, len, conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)});
	sqe.add_flags(handle.sqe_fd_flags());
	sqe.ioprio(conflux::uring::ioprio_flags::recvsend_fixed_buf);
	sqe.buf_index(conflux::uring::FixedBufIdx{static_cast<std::int32_t>(buf_idx)});
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_writev_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	iovec const *iov,
	unsigned nr_vecs,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_writev(handle.sqe_fd(), iov, nr_vecs, 0);
	sqe.add_flags(handle.sqe_fd_flags());
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_send_zc_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	void const *data,
	std::size_t len,
	std::uint64_t user_data,
	bool report_usage = true,
	int msg_flags = MSG_NOSIGNAL) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_send_zc(handle.sqe_fd(), data, len, conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)}, 0);
	if (report_usage) {
		sqe.ioprio(conflux::uring::ioprio_flags::send_zc_report_usage);
	}
	sqe.add_flags(handle.sqe_fd_flags());
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: shutdown + close ────────────────────────────────────────
// Linked HARDLINK: shutdown(WR) then close. Requires 2 SQE slots.
// Returns false if SQ has fewer than 2 slots (caller should submit and retry).

export [[nodiscard]] bool submit_shutdown_close(
	SocketRawRing &ring,
	SocketHandle handle,
	std::uint64_t shutdown_ud,
	std::uint64_t close_ud) {
	if (ring.sq_space_left() < 2) {
		return false;
	}
	auto shutdown_sqe = ring.try_get_sqe();
	if (!shutdown_sqe) {
		return false;
	}
	auto close_sqe = ring.try_get_sqe();
	if (!close_sqe) {
		return false;
	}
	shutdown_sqe.prep_shutdown(handle.sqe_fd(), SHUT_WR);
	shutdown_sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);
	shutdown_sqe.add_flags(handle.sqe_fd_flags());
	shutdown_sqe.user_data(conflux::uring::UserData{shutdown_ud});
	if (handle.fixed) {
		close_sqe.prep_close_direct(handle.direct_slot());
	} else {
		close_sqe.prep_close(handle.sqe_fd());
	}
	close_sqe.user_data(conflux::uring::UserData{close_ud});
	return true;
}
// ─── raw submission: close ───────────────────────────────────────────────────

export bool submit_close(
	SocketRawRing &ring,
	SocketHandle handle,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	if (handle.fixed) {
		sqe.prep_close_direct(handle.direct_slot());
	} else {
		sqe.prep_close(handle.sqe_fd());
	}
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export struct SocketCloseOptions {
	bool shutdown_write{true};
	bool skip_shutdown_success_cqe{true};
	bool allow_async_shutdown_for_os_fd{false};
};
export [[nodiscard]] bool submit_close_fast(
	SocketRawRing &ring,
	SocketHandle handle,
	std::uint64_t shutdown_ud,
	std::uint64_t close_ud,
	SocketCloseOptions opts) noexcept {
	bool const needs_shutdown = handle.fixed ? opts.shutdown_write : opts.allow_async_shutdown_for_os_fd;
	unsigned const needed = 1U + (needs_shutdown ? 1U : 0U);
	if (ring.sq_space_left() < needed) {
		return false;
	}
	if (needs_shutdown) {
		auto shut_sqe = ring.try_get_sqe();
		if (!shut_sqe) {
			return false;
		}
		auto close_sqe = ring.try_get_sqe();
		if (!close_sqe) {
			return false;
		}
		shut_sqe.prep_shutdown(handle.sqe_fd(), SHUT_WR);
		shut_sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);
		shut_sqe.add_flags(handle.sqe_fd_flags());
		if (opts.skip_shutdown_success_cqe) {
			shut_sqe.add_flags(conflux::uring::sqe_flags::cqe_skip_success);
		}
		shut_sqe.user_data(conflux::uring::UserData{shutdown_ud});
		if (handle.fixed) {
			close_sqe.prep_close_direct(handle.direct_slot());
		} else {
			close_sqe.prep_close(handle.sqe_fd());
		}
		close_sqe.user_data(conflux::uring::UserData{close_ud});
	} else {
		auto sqe = ring.try_get_sqe();
		if (!sqe) {
			return false;
		}
		if (handle.fixed) {
			sqe.prep_close_direct(handle.direct_slot());
		} else {
			sqe.prep_close(handle.sqe_fd());
		}
		sqe.user_data(conflux::uring::UserData{close_ud});
	}
	return true;
}
// ─── raw submission: setsockopt ──────────────────────────────────────────────
// Async socket option via io_uring cmd_sock. Only works with fixed fds.

export bool submit_setsockopt_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	int level,
	int optname,
	void const *optval,
	socklen_t optlen,
	std::uint64_t user_data) {
	if (!handle.fixed) {
		return false;
	}
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_cmd_sock(
		conflux::uring::uring_cmd_op::setsockopt,
		handle.sqe_fd(),
		level,
		optname,
		const_cast<void *>(optval),
		static_cast<int>(optlen));
	sqe.add_flags(conflux::uring::sqe_flags::fixed_file);
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export struct DirectTcpAcceptSetup {
	bool tcp_nodelay_once{false}; // opt-in; requires caps.cmd_sock_setsockopt
	bool tcp_quickack_once{false}; // opt-in; requires caps.cmd_sock_setsockopt
	bool prefer_busy_poll_once{false}; // opt-in; requires caps.cmd_sock_setsockopt
	int const *busy_poll_us_optval{nullptr}; // non-null+>0 enables SO_BUSY_POLL
	bool recv_bundle{false}; // mirrors normal recv re-arms for direct accepted sockets
	RecvArmPolicy recv_arm_policy{RecvArmPolicy::default_};
	bool skip_sockopt_success_cqes{true};
};
namespace {

static int const k_socket_opt_on = 1;

} // namespace
export struct DirectTcpAcceptRecvTarget {
	std::uint16_t buf_group{};
	BufferRingMode buffer_mode{BufferRingMode::classic_one_cqe_per_buffer};
};

namespace {

[[nodiscard]] unsigned direct_tcp_accept_setup_sqe_count(
	DirectTcpAcceptSetup const &opts) noexcept {
	return 1U
		 + (opts.tcp_nodelay_once ? 1U : 0U)
		 + (opts.tcp_quickack_once ? 1U : 0U)
		 + (opts.prefer_busy_poll_once ? 1U : 0U)
		 + (opts.busy_poll_us_optval && *opts.busy_poll_us_optval > 0 ? 1U : 0U);
}
void prepare_direct_accept_sockopt_sqe(
	conflux::uring::Sqe sqe,
	SocketHandle direct_socket,
	int level,
	int optname,
	void const *optval,
	int optlen,
	std::uint64_t user_data,
	bool skip_success_cqe) noexcept {
	sqe.prep_cmd_sock(
		conflux::uring::uring_cmd_op::setsockopt,
		direct_socket.sqe_fd(),
		level,
		optname,
		const_cast<void *>(optval),
		optlen);
	sqe.add_flags(conflux::uring::sqe_flags::fixed_file);
	sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);
	if (skip_success_cqe) {
		sqe.add_flags(conflux::uring::sqe_flags::cqe_skip_success);
	}
	sqe.user_data(conflux::uring::UserData{user_data});
}

} // namespace

export [[nodiscard]] bool submit_direct_tcp_accept_setup_recv_to_group(
	SocketRawRing &ring,
	SocketHandle direct_socket,
	DirectTcpAcceptRecvTarget target,
	std::uint64_t sockopt_ud,
	std::uint64_t recv_ud,
	DirectTcpAcceptSetup opts) noexcept {
	if (!direct_socket.is_direct()) {
		return false;
	}
	if (ring.sq_space_left() < direct_tcp_accept_setup_sqe_count(opts)) {
		return false;
	}
	if (opts.tcp_nodelay_once) {
		auto sqe = ring.try_get_sqe();
		if (!sqe) {
			return false;
		}
		prepare_direct_accept_sockopt_sqe(
			sqe,
			direct_socket,
			IPPROTO_TCP,
			TCP_NODELAY,
			&k_socket_opt_on,
			static_cast<int>(sizeof(k_socket_opt_on)),
			sockopt_ud,
			opts.skip_sockopt_success_cqes);
	}
	if (opts.tcp_quickack_once) {
		auto sqe = ring.try_get_sqe();
		if (!sqe) {
			return false;
		}
		prepare_direct_accept_sockopt_sqe(
			sqe,
			direct_socket,
			IPPROTO_TCP,
			TCP_QUICKACK,
			&k_socket_opt_on,
			static_cast<int>(sizeof(k_socket_opt_on)),
			sockopt_ud,
			opts.skip_sockopt_success_cqes);
	}
	if (opts.prefer_busy_poll_once) {
		auto sqe = ring.try_get_sqe();
		if (!sqe) {
			return false;
		}
		prepare_direct_accept_sockopt_sqe(
			sqe,
			direct_socket,
			SOL_SOCKET,
			SO_PREFER_BUSY_POLL,
			&k_socket_opt_on,
			static_cast<int>(sizeof(k_socket_opt_on)),
			sockopt_ud,
			opts.skip_sockopt_success_cqes);
	}
	if (opts.busy_poll_us_optval && *opts.busy_poll_us_optval > 0) {
		auto sqe = ring.try_get_sqe();
		if (!sqe) {
			return false;
		}
		prepare_direct_accept_sockopt_sqe(
			sqe,
			direct_socket,
			SOL_SOCKET,
			SO_BUSY_POLL,
			opts.busy_poll_us_optval,
			static_cast<int>(sizeof(*opts.busy_poll_us_optval)),
			sockopt_ud,
			opts.skip_sockopt_success_cqes);
	}
	auto recv_sqe = ring.try_get_sqe();
	if (!recv_sqe) {
		return false;
	}
	recv_sqe.prep_recv_multishot(direct_socket.sqe_fd(), nullptr, 0, conflux::uring::MsgFlags{});
	recv_sqe.buf_group(conflux::uring::BufGroupId{target.buf_group});
	conflux::uring::IoPrioFlags recv_ioprio{};
#if CONFLUX_ENABLE_RECV_BUNDLE
	if (opts.recv_bundle && target.buffer_mode == BufferRingMode::recv_bundle) {
		recv_ioprio = recv_ioprio | conflux::uring::ioprio_flags::recvsend_bundle;
	}
#endif
	if (opts.recv_arm_policy == RecvArmPolicy::poll_first) {
		recv_ioprio = recv_ioprio | conflux::uring::ioprio_flags::recvsend_poll_first;
	}
	if (recv_ioprio) {
		recv_sqe.ioprio(recv_ioprio);
	}
	recv_sqe.add_flags(conflux::uring::sqe_flags::buffer_select);
	recv_sqe.add_flags(conflux::uring::sqe_flags::fixed_file);
	recv_sqe.user_data(conflux::uring::UserData{recv_ud});
	return true;
}
export [[nodiscard]] bool submit_direct_tcp_accept_setup_recv(
	SocketRawRing &ring,
	SocketHandle direct_socket,
	BufferRing &buffers,
	std::uint64_t sockopt_ud,
	std::uint64_t recv_ud,
	DirectTcpAcceptSetup opts) noexcept {
	return submit_direct_tcp_accept_setup_recv_to_group(
		ring,
		direct_socket,
		DirectTcpAcceptRecvTarget{.buf_group = buffers.group_id(), .buffer_mode = buffers.mode()},
		sockopt_ud,
		recv_ud,
		opts);
}
export bool submit_accept_borrowed(
	SocketRawRing &ring,
	SocketHandle listen,
	sockaddr *addr,
	socklen_t *addrlen,
	std::uint64_t user_data,
	int accept_flags) noexcept {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_accept(listen.sqe_fd(), addr, addrlen, accept_flags);
	sqe.add_flags(listen.sqe_fd_flags());
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: cancel ──────────────────────────────────────────────────

export bool submit_cancel_fd(
	SocketRawRing &ring,
	SocketHandle handle,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_cancel_fd(
		handle.sqe_fd(),
		handle.fixed ? conflux::uring::cancel_flags::fd_fixed : conflux::uring::CancelFlags{});
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_cancel_by_ud(
	SocketRawRing &ring,
	std::uint64_t target_ud,
	std::uint64_t cancel_ud) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_cancel64(conflux::uring::UserData{target_ud}, conflux::uring::CancelFlags{});
	sqe.user_data(conflux::uring::UserData{cancel_ud});
	return true;
}
export bool submit_cancel_multishot_recv(
	SocketRawRing &ring,
	SocketHandle handle,
	std::uint64_t user_data) {
	return submit_cancel_fd(ring, handle, user_data);
}
export enum class CancelPolicy : std::uint8_t {
	ignore,
	cancel_sqe_by_user_data,
	cancel_fd,
	close_fd,
};
// ─── raw submission: timeout ─────────────────────────────────────────────────

export bool submit_timeout_borrowed(
	SocketRawRing &ring,
	__kernel_timespec *ts,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_timeout(ts, 0, conflux::uring::TimeoutFlags{});
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_link_timeout_borrowed(
	SocketRawRing &ring,
	__kernel_timespec *ts,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_link_timeout(ts, conflux::uring::TimeoutFlags{});
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: socket creation ─────────────────────────────────────────

export bool submit_socket(
	SocketRawRing &ring,
	int domain,
	int type,
	int protocol,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_socket(domain, type, protocol, 0);
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_socket_direct(
	SocketRawRing &ring,
	int domain,
	int type,
	int protocol,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_socket_direct_alloc(domain, type, protocol, 0);
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_socket_direct(
	SocketRawRing &ring,
	int domain,
	int type,
	int protocol,
	std::uint64_t user_data,
	conflux::uring::IoUringCaps const &caps) {
	if (!caps.socket_direct_alloc) {
		return false;
	}
	return submit_socket_direct(ring, domain, type, protocol, user_data);
}
// ─── raw submission: connect ─────────────────────────────────────────────────
// addr must remain valid until CQE. Caller owns lifetime.

export bool submit_connect_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	sockaddr const *addr,
	socklen_t addrlen,
	std::uint64_t user_data,
	bool link_next = false) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_connect(handle.sqe_fd(), addr, addrlen);
	sqe.add_flags(handle.sqe_fd_flags());
	if (link_next) {
		sqe.add_flags(conflux::uring::sqe_flags::io_link);
	}
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: recv (single-shot, into caller buffer) ──────────────────

export bool submit_async_recv_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	void *buf,
	std::size_t len,
	std::uint64_t user_data,
	int msg_flags = 0) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_recv(handle.sqe_fd(), buf, len, conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)});
	sqe.add_flags(handle.sqe_fd_flags());
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: sendmsg / recvmsg (UDP) ─────────────────────────────────

export bool submit_sendmsg_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	msghdr const *msg,
	std::uint64_t user_data,
	unsigned flags = 0) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_sendmsg(handle.sqe_fd(), msg, conflux::uring::MsgFlags{flags});
	sqe.add_flags(handle.sqe_fd_flags());
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_recvmsg_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	msghdr *msg,
	std::uint64_t user_data,
	unsigned flags = 0) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_recvmsg(handle.sqe_fd(), msg, conflux::uring::MsgFlags{flags});
	sqe.add_flags(handle.sqe_fd_flags());
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: linked recvmsg + timeout (UDP pattern) ──────────────────
// Requires 2 SQE slots. recvmsg is IO_LINK'd to a link_timeout.
// On timeout, recvmsg CQE arrives with res=-ECANCELED.
// Returns false if SQ has fewer than 2 slots.

export [[nodiscard]] bool submit_recvmsg_timeout_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	msghdr *msg,
	__kernel_timespec *ts,
	std::uint64_t recv_ud,
	std::uint64_t timeout_ud,
	unsigned recv_flags = 0) {
	if (ring.sq_space_left() < 2) {
		return false;
	}
	auto recv_sqe = ring.try_get_sqe();
	if (!recv_sqe) {
		return false;
	}
	auto timeout_sqe = ring.try_get_sqe();
	if (!timeout_sqe) {
		return false;
	}
	recv_sqe.prep_recvmsg(handle.sqe_fd(), msg, conflux::uring::MsgFlags{recv_flags});
	recv_sqe.add_flags(conflux::uring::sqe_flags::io_link);
	recv_sqe.add_flags(handle.sqe_fd_flags());
	recv_sqe.user_data(conflux::uring::UserData{recv_ud});
	timeout_sqe.prep_link_timeout(ts, conflux::uring::TimeoutFlags{});
	timeout_sqe.user_data(conflux::uring::UserData{timeout_ud});
	return true;
}
// ─── raw submission: linked recv + timeout (TCP flat-buf pattern) ────────────
// Requires 2 SQE slots. recv is IO_LINK'd to a link_timeout.
// On timeout: recv CQE res=-ECANCELED.
export [[nodiscard]] bool submit_recv_timeout_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	void *buf,
	std::size_t len,
	__kernel_timespec *ts,
	std::uint64_t recv_ud,
	std::uint64_t timeout_ud) {
	if (ring.sq_space_left() < 2) {
		return false;
	}
	auto recv_sqe = ring.try_get_sqe();
	if (!recv_sqe) {
		return false;
	}
	auto timeout_sqe = ring.try_get_sqe();
	if (!timeout_sqe) {
		return false;
	}
	recv_sqe.prep_recv(handle.sqe_fd(), buf, len, conflux::uring::MsgFlags{});
	recv_sqe.add_flags(conflux::uring::sqe_flags::io_link);
	recv_sqe.add_flags(handle.sqe_fd_flags());
	recv_sqe.user_data(conflux::uring::UserData{recv_ud});
	timeout_sqe.prep_link_timeout(ts, conflux::uring::TimeoutFlags{});
	timeout_sqe.user_data(conflux::uring::UserData{timeout_ud});
	return true;
}
// ─── raw submission: linked send + timeout ───────────────────────────────────
// Requires 2 SQE slots. send is IO_LINK'd to a link_timeout.
// On timeout: send CQE res=-ECANCELED.

export [[nodiscard]] bool submit_send_timeout_borrowed(
	SocketRawRing &ring,
	SocketHandle handle,
	void const *data,
	std::size_t len,
	__kernel_timespec *ts,
	std::uint64_t send_ud,
	std::uint64_t timeout_ud,
	int msg_flags = MSG_NOSIGNAL) {
	if (ring.sq_space_left() < 2) {
		return false;
	}
	auto send_sqe = ring.try_get_sqe();
	if (!send_sqe) {
		return false;
	}
	auto timeout_sqe = ring.try_get_sqe();
	if (!timeout_sqe) {
		return false;
	}
	send_sqe.prep_send(handle.sqe_fd(), data, len, conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)});
	send_sqe.add_flags(conflux::uring::sqe_flags::io_link);
	send_sqe.add_flags(handle.sqe_fd_flags());
	send_sqe.user_data(conflux::uring::UserData{send_ud});
	timeout_sqe.prep_link_timeout(ts, conflux::uring::TimeoutFlags{});
	timeout_sqe.user_data(conflux::uring::UserData{timeout_ud});
	return true;
}
// ─── raw submission: fixed fd install ────────────────────────────────────────

export bool submit_fixed_fd_install(
	SocketRawRing &ring,
	std::uint32_t direct_slot,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_fixed_fd_install(conflux::uring::DirectSlot{direct_slot}, conflux::uring::InstallFdFlags{});
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── TcpListener ─────────────────────────────────────────────────────────────

export enum class TcpBindAddress : std::uint8_t {
	loopback_v4,
	any_v4,
	loopback_v6,
	any_v6_dual,
	any_v6_only,
};
export struct TcpListenerOptions {
	std::uint16_t port{0};
	TcpBindAddress bind{TcpBindAddress::any_v6_dual};
	bool reuse_addr{true};
	bool reuse_port{false};
	int backlog{SOMAXCONN};
	int accept_flags{SOCK_CLOEXEC | SOCK_NONBLOCK};
};
namespace {

struct FdGuard {
	int fd{-1};
	~FdGuard() noexcept {
		if (fd >= 0) {
			::close(fd);
		}
	}
};

} // namespace
export class TcpListener {
	int fd_{-1};
	std::uint16_t port_{};
	int accept_flags_{SOCK_CLOEXEC | SOCK_NONBLOCK};

public:
	explicit TcpListener(
		TcpListenerOptions opts = {}) {
		bool const is_v6 =
			(opts.bind == TcpBindAddress::loopback_v6
			 || opts.bind == TcpBindAddress::any_v6_dual
			 || opts.bind == TcpBindAddress::any_v6_only);
		int const domain = is_v6 ? AF_INET6 : AF_INET;
		int const raw = ::socket(domain, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
		if (raw < 0) {
			throw std::system_error(errno, system_category(), "socket");
		}
		FdGuard guard{raw};
		int const on = 1;
		if (opts.reuse_addr) {
			if (::setsockopt(raw, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
				throw std::system_error(errno, system_category(), "SO_REUSEADDR");
			}
		}
		if (opts.reuse_port) {
			if (::setsockopt(raw, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) < 0) {
				throw std::system_error(errno, std::system_category(), "SO_REUSEPORT");
			}
		}
		if (is_v6) {
			int const v6only =
				(opts.bind == TcpBindAddress::loopback_v6 || opts.bind == TcpBindAddress::any_v6_only) ? 1 : 0;
			if (::setsockopt(raw, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) < 0) {
				throw std::system_error(errno, std::system_category(), "IPV6_V6ONLY");
			}
			sockaddr_in6 addr{};
			addr.sin6_family = AF_INET6;
			addr.sin6_port = htons(opts.port);
			addr.sin6_addr = (opts.bind == TcpBindAddress::loopback_v6) ? in6addr_loopback : in6addr_any;
			if (::bind(raw, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
				throw std::system_error(errno, std::system_category(), "bind");
			}
		} else {
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(opts.port);
			addr.sin_addr.s_addr =
				(opts.bind == TcpBindAddress::loopback_v4) ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
			if (::bind(raw, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
				throw std::system_error(errno, std::system_category(), "bind");
			}
		}
		if (::listen(raw, opts.backlog) < 0) {
			throw std::system_error(errno, system_category(), "listen");
		}
		sockaddr_storage ss{};
		socklen_t sslen = sizeof(ss);
		if (::getsockname(raw, reinterpret_cast<sockaddr *>(&ss), &sslen) < 0) {
			throw std::system_error(errno, system_category(), "getsockname");
		}
		port_ = (ss.ss_family == AF_INET6) ? ntohs(reinterpret_cast<sockaddr_in6 const *>(&ss)->sin6_port) :
											 ntohs(reinterpret_cast<sockaddr_in const *>(&ss)->sin_port);
		accept_flags_ = opts.accept_flags;
		fd_ = exchange(guard.fd, -1);
	}
	~TcpListener() noexcept {
		if (fd_ >= 0) {
			::close(fd_);
		}
	}
	TcpListener(TcpListener const &) = delete;
	TcpListener &operator =(TcpListener const &) = delete;
	TcpListener(
		TcpListener &&o) noexcept
		: fd_{exchange(o.fd_, -1)}
		, port_{exchange(o.port_, std::uint16_t{})}
		, accept_flags_{o.accept_flags_} {}
	TcpListener &operator =(
		TcpListener &&o) noexcept {
		if (this != &o) {
			if (fd_ >= 0) {
				::close(fd_);
			}
			fd_ = exchange(o.fd_, -1);
			port_ = exchange(o.port_, std::uint16_t{});
			accept_flags_ = o.accept_flags_;
		}
		return *this;
	}
	[[nodiscard]] std::uint16_t port() const noexcept { return port_; }
	[[nodiscard]] int raw_fd() const noexcept { return fd_; }
	[[nodiscard]] int accept_flags() const noexcept { return accept_flags_; }
	[[nodiscard]] SocketHandle handle() const noexcept { return SocketHandle::from_os(fd_); }
	[[nodiscard]] bool arm_accept_multishot_borrowed(
		SocketRawRing &ring,
		sockaddr *addr,
		socklen_t *addrlen,
		std::uint64_t user_data,
		conflux::uring::IoUringCaps const &caps,
		bool accept_direct = false) noexcept {
		return submit_accept_multishot_borrowed(
			ring,
			handle(),
			addr,
			addrlen,
			user_data,
			caps,
			accept_flags_,
			accept_direct);
	}
	[[nodiscard]] bool rearm_accept_multishot_borrowed(
		SocketRawRing &ring,
		sockaddr *addr,
		socklen_t *addrlen,
		std::uint64_t user_data,
		conflux::uring::IoUringCaps const &caps,
		bool accept_direct = false) noexcept {
		return submit_accept_multishot_borrowed(
			ring,
			handle(),
			addr,
			addrlen,
			user_data,
			caps,
			accept_flags_,
			accept_direct);
	}
};
// ─── raw submission: standalone shutdown ──────────────────────────────────────

export bool submit_shutdown(
	SocketRawRing &ring,
	SocketHandle handle,
	int how,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_shutdown(handle.sqe_fd(), how);
	sqe.add_flags(handle.sqe_fd_flags());
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── SocketFdMode ─────────────────────────────────────────────────────────────

export enum class SocketFdMode : std::uint8_t {
	os_fd,
	direct_if_available,
	direct_required,
};
// ─── AcceptOptions ────────────────────────────────────────────────────────────

export struct AcceptOptions {
	bool tcp_nodelay{true};
	bool tcp_quickack{false};
};
// ─── ConnectOptions ───────────────────────────────────────────────────────────

export struct ConnectOptions {
	std::chrono::milliseconds timeout{std::chrono::seconds{30}};
	CancelPolicy cancel{CancelPolicy::cancel_fd};
	bool tcp_nodelay{true};
	bool tcp_quickack{false};
};
// ─── SocketTaskRing ───────────────────────────────────────────────────────────
// Thin wrapper: SocketRawRing + CompletionTable& + UserDataFn + options.
// Does NOT own the ring — ring lifetime is managed by the HTTP server.
// Does NOT own CompletionTable — owned by the caller.

export class SocketTaskRing; // forward declare before RingOpFn alias

export using RingOpFn = std::function<void(SocketTaskRing &)>;
export struct SocketTaskRingOptions {
	SocketFdMode fd_mode{SocketFdMode::os_fd}; // P1 safe default; direct_* is explicit opt-in until P1-04
	conflux::uring::IoUringCaps const *caps{};
	// Must enqueue fn on ring-owner thread and return true, or return false.
	// Must NOT invoke fn inline from an arbitrary cancelling thread.
	// If null: ring is treated as single-threaded; submit_on_owner asserts caller==owner
	// and calls fn inline. Cross-thread cancel callers MUST provide submit_on_ring_owner.
	std::function<bool(RingOpFn)> submit_on_ring_owner{};
};
export class SocketTaskRing {
	SocketRawRing raw_;
	CompletionTable *completions_{};
	UserDataFn encode_ud_{};
	SocketTaskRingOptions opts_{};
	thread::id owner_thread_{std::this_thread::get_id()};

public:
	SocketTaskRing(
		SocketRawRing raw,
		CompletionTable &completions,
		UserDataFn encode_ud,
		SocketTaskRingOptions opts = {}) noexcept
		: raw_{raw}
		, completions_{&completions}
		, encode_ud_{std::move(encode_ud)}
		, opts_{std::move(opts)} {}
	SocketTaskRing(SocketTaskRing const &) = delete;
	SocketTaskRing &operator =(SocketTaskRing const &) = delete;
	SocketTaskRing(SocketTaskRing &&) = delete;
	SocketTaskRing &operator =(SocketTaskRing &&) = delete;
	[[nodiscard]] SocketRawRing &raw() noexcept { return raw_; }
	[[nodiscard]] CompletionTable &completions() noexcept { return *completions_; }
	[[nodiscard]] SocketTaskRingOptions const &opts() const noexcept { return opts_; }
	[[nodiscard]] std::uint64_t encode(
		std::uint32_t slot,
		std::uint32_t gen) const {
		return encode_ud_(slot, gen);
	}
	[[nodiscard]] bool submit_on_owner(
		RingOpFn fn) {
		if (opts_.submit_on_ring_owner) {
			return opts_.submit_on_ring_owner(move(fn));
		}
		assert(std::this_thread::get_id() == owner_thread_);
		fn(*this);
		return true;
	}
};
