module;
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <liburing.h>
#include <sys/mman.h>

module conflux.socket_io;
import std;
import conflux.uring;

namespace conflux::socket_io {

void BufferRing::SlabDeleter::operator ()(
	std::byte *p) const noexcept {
	::free(p);
}

std::size_t &BufferRing::incremental_offset_ref(
	std::uint16_t id) noexcept {
	assert(mode_ == BufferRingMode::incremental);
	assert(id < count_);
	return incremental_offsets_[id];
}

std::span<std::byte const> BufferRing::buffer_view_at_offset(
	std::uint16_t id,
	std::size_t offset,
	std::size_t len) const noexcept {
	return {slab_.get() + static_cast<std::size_t>(id) * buf_size_ + offset, len};
}

BufferRing::BufferRing(
	io_uring *uring,
	BufferRingOptions opts,
	conflux::uring::IoUringCaps const &caps)
	: BufferRing(conflux::uring::RingRef{uring}, opts, caps) {}

BufferRing::BufferRing(
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
		throw std::runtime_error{"BufferRingMode::incremental requires IOU_PBUF_RING_INC (kernel 6.12+)"};
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
	std::size_t const slab_sz = static_cast<std::size_t>(count_) * buf_size_;
	std::size_t const aligned_sz = (slab_sz + 4095) & ~std::size_t{4095};
	if (aligned_sz < slab_sz) {
		throw std::runtime_error{"BufferRing allocation overflow"};
	}
	auto *raw = static_cast<std::byte *>(::aligned_alloc(4096, aligned_sz));
	if (raw == nullptr) {
		throw std::bad_alloc{};
	}
	slab_.reset(raw);
	if (opts.huge_pages) {
		::madvise(raw, slab_sz, MADV_HUGEPAGE);
		::madvise(raw, slab_sz, MADV_DONTFORK);
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
			throw std::runtime_error{
				"io_uring_setup_buf_ring: incremental mode requires kernel 6.12+ (IOU_PBUF_RING_INC)"};
		}
		throw std::runtime_error{std::format("io_uring_setup_buf_ring failed: {}", built.error())};
	}
	ring_ = std::move(*built);
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

std::span<std::byte const> BufferRing::buffer_view_checked(
	std::uint16_t id,
	std::size_t len) const noexcept {
	if (id >= count_) {
		return {};
	}
	return {slab_.get() + static_cast<std::size_t>(id) * buf_size_, std::min(len, buf_size_)};
}

std::span<std::byte const> BufferRing::buffer_view_unchecked(
	std::uint16_t id,
	std::size_t len) const noexcept {
	return {slab_.get() + static_cast<std::size_t>(id) * buf_size_, std::min(len, buf_size_)};
}

std::span<std::byte const> BufferRing::buffer_view(
	std::uint16_t id,
	std::size_t len) const noexcept {
	return buffer_view_checked(id, len);
}

std::span<std::byte> BufferRing::buffer_mut_checked(
	std::uint16_t id) noexcept {
	if (id >= count_) {
		return {};
	}
	return {slab_.get() + static_cast<std::size_t>(id) * buf_size_, buf_size_};
}

std::span<std::byte> BufferRing::buffer_mut_unchecked(
	std::uint16_t id) noexcept {
	return {slab_.get() + static_cast<std::size_t>(id) * buf_size_, buf_size_};
}

std::span<std::byte> BufferRing::buffer_mut(
	std::uint16_t id) noexcept {
	return buffer_mut_checked(id);
}

void BufferRing::recycle(
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

void BufferRing::recycle_batch(
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

void BufferRing::flush_recycle_ready() noexcept {
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

bool BufferRing::recycle_selected_buffer(
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

void BufferRing::recycle_range(
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

bool BufferRing::preserve_bundle_positions_until(
	std::uint32_t end_pos) noexcept {
	assert(mode_ == BufferRingMode::recv_bundle);
	for (; bundle_preserved_pos_ < end_pos; ++bundle_preserved_pos_) {
		std::uint32_t const pos = bundle_preserved_pos_;
		std::uint16_t const id = ring_id_at(pos);
		if (!bundle_saved_insert_or_assign(pos, id)) [[unlikely]] {
			return false;
		}
		bundle_saved_pos_[id] = pos;
		bundle_has_saved_pos_[id] = 1;
	}
	return true;
}

std::uint32_t BufferRing::bundle_hash(
	std::uint32_t pos) noexcept {
	return pos * 2654435761u;
}

std::optional<std::uint16_t> BufferRing::bundle_saved_find(
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

bool BufferRing::bundle_saved_insert_or_assign(
	std::uint32_t pos,
	std::uint16_t id) noexcept {
	assert(!bundle_saved_used_.empty());
	std::uint32_t i = bundle_hash(pos) & bundle_saved_mask_;
	for (std::uint32_t n = 0, limit = static_cast<std::uint32_t>(bundle_saved_used_.size()); n < limit; ++n) {
		if (bundle_saved_used_[i] == 0 || bundle_saved_keys_[i] == pos) {
			bundle_saved_used_[i] = 1;
			bundle_saved_keys_[i] = pos;
			bundle_saved_ids_[i] = id;
			return true;
		}
		i = (i + 1) & bundle_saved_mask_;
	}
	return false;
}

void BufferRing::bundle_saved_erase(
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
				[[maybe_unused]] bool const inserted = bundle_saved_insert_or_assign(key, val);
				assert(inserted && "recv-bundle saved-order table lost an entry while rehashing");
			}
			return;
		}
		i = (i + 1) & bundle_saved_mask_;
	}
}

void BufferRing::consume_at(
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

std::uint32_t BufferRing::consume(
	std::uint32_t cnt) noexcept {
	std::uint32_t const old = head_pos_;
	consume_at(old, cnt);
	return old;
}

std::optional<std::uint32_t> BufferRing::find_start_pos(
	std::uint16_t first_id,
	std::uint32_t cnt,
	bool bundle) noexcept {
	if (cnt == 0 || cnt > count_ || first_id >= count_) {
		return std::nullopt;
	}
	std::uint32_t const pos = mode_ == BufferRingMode::recv_bundle && bundle_has_saved_pos_[first_id] != 0 ?
								  bundle_saved_pos_[first_id] :
								  (bundle ? id_pos_[first_id] : head_pos_);
	if (pos + cnt < pos || pos + cnt > tail_pos_) {
		return std::nullopt;
	}
	if (mode_ != BufferRingMode::recv_bundle && pos < recycle_head_pos_) {
		return std::nullopt;
	}
	if (mode_ == BufferRingMode::recv_bundle && !preserve_bundle_positions_until(pos + cnt)) [[unlikely]] {
		return std::nullopt;
	}
	if (ring_id_at(pos) != first_id) {
		return std::nullopt;
	}
	return pos;
}

bool BufferRing::reclaim_incremental_partial(
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

std::uint16_t BufferRing::ring_id_at(
	std::uint32_t pos) const noexcept {
	if (mode_ == BufferRingMode::recv_bundle) {
		auto const id = bundle_saved_find(pos);
		if (id) {
			return *id;
		}
	}
	return ring_order_[pos % count_];
}

RecvBuffer BufferRing::lease(
	std::uint16_t id,
	std::size_t len) noexcept {
	return RecvBuffer{this, id, len};
}

RecvBuffer::RecvBuffer(
	BufferRing *ring,
	std::uint16_t id,
	std::size_t len) noexcept
	: ring_{ring}
	, id_{id}
	, len_{len} {}

RecvBuffer::RecvBuffer(
	RecvBuffer &&o) noexcept
	: ring_{std::exchange(o.ring_, nullptr)}
	, id_{o.id_}
	, len_{o.len_}
	, armed_{std::exchange(o.armed_, false)} {}

RecvBuffer &RecvBuffer::operator =(
	RecvBuffer &&o) noexcept {
	if (this != &o) {
		if ((ring_ != nullptr) && armed_) {
			ring_->recycle(id_);
		}
		ring_ = std::exchange(o.ring_, nullptr);
		id_ = o.id_;
		len_ = o.len_;
		armed_ = std::exchange(o.armed_, false);
	}
	return *this;
}

RecvBuffer::~RecvBuffer() {
	if ((ring_ != nullptr) && armed_) {
		ring_->recycle(id_);
	}
}

std::span<std::byte const> RecvBuffer::view() const noexcept {
	return (ring_ != nullptr) ? ring_->buffer_view_checked(id_, len_) : std::span<std::byte const>{};
}

std::uint16_t RecvBuffer::id() const noexcept {
	return id_;
}

std::size_t RecvBuffer::size() const noexcept {
	return len_;
}

void RecvBuffer::release() noexcept {
	if ((ring_ != nullptr) && armed_) {
		ring_->recycle(id_);
		armed_ = false;
	}
}

void RecvBuffer::detach() noexcept {
	armed_ = false;
}

RecvSlices::RecvSlices(
	BufferRing *ring,
	std::uint32_t start,
	std::uint32_t cnt,
	std::size_t total) noexcept
	: ring_{ring}
	, start_pos_{start}
	, count_{cnt}
	, total_{total} {}

RecvSlices::RecvSlices(
	RecvSlices &&o) noexcept
	: ring_{std::exchange(o.ring_, nullptr)}
	, start_pos_{o.start_pos_}
	, count_{o.count_}
	, total_{o.total_}
	, detached_{o.detached_} {}

RecvSlices &RecvSlices::operator =(
	RecvSlices &&o) noexcept {
	if (this != &o) {
		ring_ = std::exchange(o.ring_, nullptr);
		start_pos_ = o.start_pos_;
		count_ = o.count_;
		total_ = o.total_;
		detached_ = o.detached_;
	}
	return *this;
}

bool RecvSlices::valid() const noexcept {
	return ring_ != nullptr && count_ > 0;
}

std::size_t RecvSlices::total_size() const noexcept {
	return total_;
}

std::uint32_t RecvSlices::count() const noexcept {
	return count_;
}

RecvSlice RecvSlices::iterator::operator *() const noexcept {
	std::uint16_t const id = slices_->ring_->ring_id_at(slices_->start_pos_ + idx_);
	std::size_t const off = static_cast<std::size_t>(idx_) * slices_->ring_->buf_size();
	std::size_t const len = (idx_ + 1 < slices_->count_) ? slices_->ring_->buf_size() : slices_->total_ - off;
	return {id, slices_->ring_->buffer_view_unchecked(id, len)};
}

RecvSlices::iterator &RecvSlices::iterator::operator ++() noexcept {
	++idx_;
	return *this;
}

bool RecvSlices::iterator::operator ==(
	iterator const &o) const noexcept {
	return idx_ == o.idx_;
}

bool RecvSlices::iterator::operator !=(
	iterator const &o) const noexcept {
	return idx_ != o.idx_;
}

RecvSlices::iterator RecvSlices::begin() const noexcept {
	return {this, 0};
}

RecvSlices::iterator RecvSlices::end() const noexcept {
	return {this, count_};
}

void RecvSlices::recycle_all() noexcept {
	if ((ring_ == nullptr) || detached_) {
		return;
	}
	ring_->recycle_range(start_pos_, count_);
	ring_ = nullptr;
}

void RecvSlices::detach() noexcept {
	detached_ = true;
}

IncrementalRecvSlice::IncrementalRecvSlice(
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

IncrementalRecvSlice::IncrementalRecvSlice(
	IncrementalRecvSlice &&o) noexcept
	: ring_{std::exchange(o.ring_, nullptr)}
	, id_{o.id_}
	, offset_{o.offset_}
	, len_{o.len_}
	, more_{o.more_}
	, detached_{o.detached_} {}

IncrementalRecvSlice::~IncrementalRecvSlice() noexcept {
	recycle_if_final();
}

IncrementalRecvSlice &IncrementalRecvSlice::operator =(
	IncrementalRecvSlice &&o) noexcept {
	if (this != &o) {
		recycle_if_final();
		ring_ = std::exchange(o.ring_, nullptr);
		id_ = o.id_;
		offset_ = o.offset_;
		len_ = o.len_;
		more_ = o.more_;
		detached_ = o.detached_;
	}
	return *this;
}

bool IncrementalRecvSlice::valid() const noexcept {
	return ring_ != nullptr && len_ > 0;
}

std::uint16_t IncrementalRecvSlice::id() const noexcept {
	return id_;
}

std::size_t IncrementalRecvSlice::offset() const noexcept {
	return offset_;
}

std::size_t IncrementalRecvSlice::size() const noexcept {
	return len_;
}

bool IncrementalRecvSlice::more() const noexcept {
	return more_;
}

std::span<std::byte const> IncrementalRecvSlice::bytes() const noexcept {
	if (ring_ == nullptr) {
		return {};
	}
	return ring_->buffer_view_at_offset(id_, offset_, len_);
}

void IncrementalRecvSlice::recycle_if_final() noexcept {
	if (ring_ == nullptr || detached_ || more_) {
		return;
	}
	ring_->recycle(id_);
	ring_ = nullptr;
}

void IncrementalRecvSlice::detach() noexcept {
	detached_ = true;
}

RecvPayload RecvPayload::from_slices(
	RecvPayloadDescriptor descriptor,
	RecvSlices slices) noexcept {
	RecvPayload payload;
	payload.variant_ = Variant::slices;
	payload.descriptor_ = descriptor;
	payload.slices_ = std::move(slices);
	return payload;
}

RecvPayload RecvPayload::from_incremental(
	RecvPayloadDescriptor descriptor,
	IncrementalRecvSlice slice) noexcept {
	RecvPayload payload;
	payload.variant_ = Variant::incremental;
	payload.descriptor_ = descriptor;
	payload.incremental_ = std::move(slice);
	return payload;
}

RecvPayload::RecvPayload(
	RecvPayload &&o) noexcept
	: variant_{std::exchange(o.variant_, Variant::none)}
	, descriptor_{o.descriptor_}
	, slices_{std::move(o.slices_)}
	, incremental_{std::move(o.incremental_)} {}

RecvPayload &RecvPayload::operator =(
	RecvPayload &&o) noexcept {
	if (this != &o) {
		recycle_all();
		variant_ = std::exchange(o.variant_, Variant::none);
		descriptor_ = o.descriptor_;
		slices_ = std::move(o.slices_);
		incremental_ = std::move(o.incremental_);
	}
	return *this;
}

RecvPayload::~RecvPayload() noexcept {
	recycle_all();
}

bool RecvPayload::valid() const noexcept {
	return variant_ != Variant::none;
}

RecvPayloadDescriptor RecvPayload::descriptor() const noexcept {
	return descriptor_;
}

RecvPayloadStorage RecvPayload::storage() const noexcept {
	return descriptor_.storage;
}

RecvPayloadPinning RecvPayload::pinning() const noexcept {
	return descriptor_.pinning;
}

bool RecvPayload::incremental() const noexcept {
	return descriptor_.incremental;
}

bool RecvPayload::multi_buffer() const noexcept {
	return descriptor_.multi_buffer;
}

std::size_t RecvPayload::total_size() const noexcept {
	switch (variant_) {
	case Variant::slices     : return slices_.total_size();
	case Variant::incremental: return incremental_.size();
	case Variant::none       : return 0;
	}
	return 0;
}

std::uint32_t RecvPayload::chunk_count() const noexcept {
	switch (variant_) {
	case Variant::slices     : return slices_.count();
	case Variant::incremental: return incremental_.valid() ? 1u : 0u;
	case Variant::none       : return 0u;
	}
	return 0u;
}

RecvPayloadChunk RecvPayload::iterator::operator *() const noexcept {
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

RecvPayload::iterator &RecvPayload::iterator::operator ++() noexcept {
	++idx;
	return *this;
}

bool RecvPayload::iterator::operator ==(
	iterator const &o) const noexcept {
	return payload == o.payload && idx == o.idx;
}

bool RecvPayload::iterator::operator !=(
	iterator const &o) const noexcept {
	return !(*this == o);
}

RecvPayload::iterator RecvPayload::begin() const noexcept {
	return {this, 0};
}

RecvPayload::iterator RecvPayload::end() const noexcept {
	return {this, chunk_count()};
}

void RecvPayload::recycle_all() noexcept {
	if (variant_ == Variant::slices) {
		slices_.recycle_all();
	}
	if (variant_ == Variant::incremental) {
		incremental_.recycle_if_final();
	}
	variant_ = Variant::none;
}

void RecvPayload::detach() noexcept {
	if (variant_ == Variant::slices) {
		slices_.detach();
	}
	if (variant_ == Variant::incremental) {
		incremental_.detach();
	}
}

IncrementalRecvSlice buffer_slice_from_incremental_cqe(
	BufferRing &ring,
	int res,
	conflux::uring::CqeFlags flags) noexcept {
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

std::expected<IncrementalRecvSlice, RecvDecodeError> try_buffer_slice_from_incremental_cqe(
	BufferRing &ring,
	int res,
	conflux::uring::CqeFlags flags) noexcept {
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

std::expected<RecvSlices, RecvDecodeError> try_buffer_slices_from_cqe(
	BufferRing &ring,
	int res,
	conflux::uring::CqeFlags flags,
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

RecvSlices buffer_slices_from_cqe(
	BufferRing &ring,
	int res,
	conflux::uring::CqeFlags flags,
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
	return std::move(*slices);
}

std::expected<RecvPayload, RecvDecodeError> try_recv_payload_from_cqe(
	BufferRing &ring,
	int res,
	conflux::uring::CqeFlags flags,
	bool bundle) noexcept {
	auto descriptor = recv_payload_descriptor(ring.mode(), bundle);
	if (ring.mode() == BufferRingMode::incremental) {
		auto slice = try_buffer_slice_from_incremental_cqe(ring, res, flags);
		if (!slice) [[unlikely]] {
			return std::unexpected(slice.error());
		}
		return RecvPayload::from_incremental(descriptor, std::move(*slice));
	}
	auto slices = try_buffer_slices_from_cqe(ring, res, flags, bundle);
	if (!slices) [[unlikely]] {
		return std::unexpected(slices.error());
	}
	return RecvPayload::from_slices(descriptor, std::move(*slices));
}

RecvPayload recv_payload_from_cqe(
	BufferRing &ring,
	int res,
	conflux::uring::CqeFlags flags,
	bool bundle) noexcept {
	auto payload = try_recv_payload_from_cqe(ring, res, flags, bundle);
	if (!payload) [[unlikely]] {
		assert(false && "CQE recv payload is not present in the userspace ownership window");
		return {};
	}
	return std::move(*payload);
}

} // namespace conflux::socket_io
