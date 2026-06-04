module;
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

namespace conflux::socket_io {

// ─── handle types ────────────────────────────────────────────────────────────

using conflux::uring::CompletionTable;
using conflux::uring::DirectFd;
using conflux::uring::IoHandle;
using conflux::uring::OsFd;
using conflux::uring::RingFd;
using conflux::uring::UserDataFn;

export using OwnedSocketHandle = conflux::uring::IoHandle;
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
		void operator ()(std::byte *p) const noexcept;
	};
	conflux::uring::BufRing ring_{};
	conflux::uring::RingRef uring_{static_cast<io_uring *>(nullptr)};
	std::unique_ptr<std::byte[], SlabDeleter> slab_;
	std::size_t buf_size_{};
	std::uint32_t count_{};
	std::uint16_t group_id_{};
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
	IncrementalRecvSlice friend buffer_slice_from_incremental_cqe(BufferRing &, int, conflux::uring::CqeFlags) noexcept;
	std::expected<IncrementalRecvSlice, RecvDecodeError> friend try_buffer_slice_from_incremental_cqe(
		BufferRing &,
		int,
		conflux::uring::CqeFlags) noexcept;
	[[nodiscard]] std::size_t &incremental_offset_ref(std::uint16_t id) noexcept;
	[[nodiscard]] std::span<std::byte const>
	buffer_view_at_offset(std::uint16_t id, std::size_t offset, std::size_t len) const noexcept;

public:
	BufferRing(io_uring *uring, BufferRingOptions opts, conflux::uring::IoUringCaps const &caps);
	BufferRing(conflux::uring::RingRef uring, BufferRingOptions opts, conflux::uring::IoUringCaps const &caps);
	~BufferRing() = default;
	BufferRing(BufferRing const &) = delete;
	BufferRing &operator =(BufferRing const &) = delete;
	BufferRing(BufferRing &&) = delete;
	BufferRing &operator =(BufferRing &&) = delete;
	[[nodiscard]] std::span<std::byte const> buffer_view_checked(std::uint16_t id, std::size_t len) const noexcept;
	[[nodiscard]] std::span<std::byte const> buffer_view_unchecked(std::uint16_t id, std::size_t len) const noexcept;
	[[nodiscard]] std::span<std::byte const> buffer_view(std::uint16_t id, std::size_t len) const noexcept;
	[[nodiscard]] std::span<std::byte> buffer_mut_checked(std::uint16_t id) noexcept;
	[[nodiscard]] std::span<std::byte> buffer_mut_unchecked(std::uint16_t id) noexcept;
	[[nodiscard]] std::span<std::byte> buffer_mut(std::uint16_t id) noexcept;
	void recycle(std::uint16_t id) noexcept;
	void recycle_batch(std::span<std::uint16_t const> ids) noexcept;
	void flush_recycle_ready() noexcept;
	[[nodiscard]] bool recycle_selected_buffer(std::uint16_t id) noexcept;
	void recycle_range(std::uint32_t start_pos, std::uint32_t cnt) noexcept;
	[[nodiscard]] bool preserve_bundle_positions_until(std::uint32_t end_pos) noexcept;
	[[nodiscard]] static std::uint32_t bundle_hash(std::uint32_t pos) noexcept;
	[[nodiscard]] std::optional<std::uint16_t> bundle_saved_find(std::uint32_t pos) const noexcept;
	[[nodiscard]] bool bundle_saved_insert_or_assign(std::uint32_t pos, std::uint16_t id) noexcept;
	void bundle_saved_erase(std::uint32_t pos) noexcept;
	void consume_at(std::uint32_t start_pos, std::uint32_t cnt) noexcept;
	std::uint32_t consume(std::uint32_t cnt) noexcept;
	[[nodiscard]] std::optional<std::uint32_t>
	find_start_pos(std::uint16_t first_id, std::uint32_t cnt, bool bundle) noexcept;
	bool reclaim_incremental_partial(std::uint16_t id) noexcept;
	[[nodiscard]] std::uint16_t ring_id_at(std::uint32_t pos) const noexcept;
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
	RecvBuffer(BufferRing *ring, std::uint16_t id, std::size_t len) noexcept;
	RecvBuffer(RecvBuffer const &) = delete;
	RecvBuffer &operator =(RecvBuffer const &) = delete;
	RecvBuffer(RecvBuffer &&o) noexcept;
	RecvBuffer &operator =(RecvBuffer &&o) noexcept;
	~RecvBuffer();
	[[nodiscard]] std::span<std::byte const> view() const noexcept;
	[[nodiscard]] std::uint16_t id() const noexcept;
	[[nodiscard]] std::size_t size() const noexcept;
	void release() noexcept;
	void detach() noexcept;
};

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
	RecvSlices(BufferRing *ring, std::uint32_t start, std::uint32_t cnt, std::size_t total) noexcept;
	RecvSlices(RecvSlices const &) = delete;
	RecvSlices &operator =(RecvSlices const &) = delete;
	RecvSlices(RecvSlices &&o) noexcept;
	RecvSlices &operator =(RecvSlices &&o) noexcept;
	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] std::size_t total_size() const noexcept;
	[[nodiscard]] std::uint32_t count() const noexcept;
	struct iterator {
		RecvSlices const *slices_;
		std::uint32_t idx_;
		[[nodiscard]] RecvSlice operator *() const noexcept;
		iterator &operator ++() noexcept;
		bool operator ==(iterator const &o) const noexcept;
		bool operator !=(iterator const &o) const noexcept;
	};
	[[nodiscard]] iterator begin() const noexcept;
	[[nodiscard]] iterator end() const noexcept;
	void recycle_all() noexcept;
	void detach() noexcept;
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
	IncrementalRecvSlice(BufferRing *ring, std::uint16_t id, std::size_t offset, std::size_t len, bool more) noexcept;
	IncrementalRecvSlice(IncrementalRecvSlice const &) = delete;
	IncrementalRecvSlice &operator =(IncrementalRecvSlice const &) = delete;
	IncrementalRecvSlice(IncrementalRecvSlice &&o) noexcept;
	~IncrementalRecvSlice() noexcept;
	IncrementalRecvSlice &operator =(IncrementalRecvSlice &&o) noexcept;
	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] std::uint16_t id() const noexcept;
	[[nodiscard]] std::size_t offset() const noexcept;
	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] bool more() const noexcept;
	[[nodiscard]] std::span<std::byte const> bytes() const noexcept;
	void recycle_if_final() noexcept;
	void detach() noexcept;
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
	static RecvPayload from_slices(RecvPayloadDescriptor descriptor, RecvSlices slices) noexcept;
	static RecvPayload from_incremental(RecvPayloadDescriptor descriptor, IncrementalRecvSlice slice) noexcept;
	RecvPayload(RecvPayload const &) = delete;
	RecvPayload &operator =(RecvPayload const &) = delete;
	RecvPayload(RecvPayload &&o) noexcept;
	RecvPayload &operator =(RecvPayload &&o) noexcept;
	~RecvPayload() noexcept;
	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] RecvPayloadDescriptor descriptor() const noexcept;
	[[nodiscard]] RecvPayloadStorage storage() const noexcept;
	[[nodiscard]] RecvPayloadPinning pinning() const noexcept;
	[[nodiscard]] bool incremental() const noexcept;
	[[nodiscard]] bool multi_buffer() const noexcept;
	[[nodiscard]] std::size_t total_size() const noexcept;
	[[nodiscard]] std::uint32_t chunk_count() const noexcept;
	struct iterator {
		RecvPayload const *payload{};
		std::uint32_t idx{};
		[[nodiscard]] RecvPayloadChunk operator *() const noexcept;
		iterator &operator ++() noexcept;
		bool operator ==(iterator const &o) const noexcept;
		bool operator !=(iterator const &o) const noexcept;
	};
	[[nodiscard]] iterator begin() const noexcept;
	[[nodiscard]] iterator end() const noexcept;
	void recycle_all() noexcept;
	void detach() noexcept;
};
// ─── CQE helpers ─────────────────────────────────────────────────────────────

export [[nodiscard]] inline std::uint16_t cqe_buffer_id(
	conflux::uring::CqeFlags cqe_flags) noexcept {
	return conflux::uring::cqe_flags::buf_id(cqe_flags).v;
}
export [[nodiscard]] inline bool cqe_has_more(
	conflux::uring::CqeFlags cqe_flags) noexcept {
	return cqe_flags.any(conflux::uring::cqe_flags::more);
}
export [[nodiscard]] inline bool cqe_has_buffer(
	conflux::uring::CqeFlags cqe_flags) noexcept {
	return cqe_flags.any(conflux::uring::cqe_flags::buffer);
}
export [[nodiscard]] inline bool cqe_has_buf_more(
	conflux::uring::CqeFlags cqe_flags) noexcept {
	return cqe_flags.any(conflux::uring::cqe_flags::buf_more);
}
export [[nodiscard]] inline conflux::uring::CqeFlags cqe_buffer_flags(
	conflux::uring::BufId id,
	bool more = false) noexcept {
	return conflux::uring::cqe_flags::selected_buffer(id, more);
}
export [[nodiscard]] IncrementalRecvSlice
buffer_slice_from_incremental_cqe(BufferRing &ring, int res, conflux::uring::CqeFlags flags) noexcept;
export [[nodiscard]] std::expected<IncrementalRecvSlice, RecvDecodeError>
try_buffer_slice_from_incremental_cqe(BufferRing &ring, int res, conflux::uring::CqeFlags flags) noexcept;
export [[nodiscard]] std::expected<RecvSlices, RecvDecodeError>
try_buffer_slices_from_cqe(BufferRing &ring, int res, conflux::uring::CqeFlags flags, bool bundle) noexcept;
export [[nodiscard]] RecvSlices
buffer_slices_from_cqe(BufferRing &ring, int res, conflux::uring::CqeFlags flags, bool bundle) noexcept;
export [[nodiscard]] std::expected<RecvPayload, RecvDecodeError>
try_recv_payload_from_cqe(BufferRing &ring, int res, conflux::uring::CqeFlags flags, bool bundle) noexcept;
export [[nodiscard]] RecvPayload
recv_payload_from_cqe(BufferRing &ring, int res, conflux::uring::CqeFlags flags, bool bundle) noexcept;
// ─── DirectFdTable ───────────────────────────────────────────────────────────
// Registers a sparse fixed-file table with io_uring.
// accept_direct auto-allocates slots; close_direct frees them.

export class DirectFdTable {
	conflux::uring::RingRef ring_;
	std::uint32_t capacity_{};
	int err_{0};
	bool registered_{false};

public:
	DirectFdTable(io_uring *ring, std::uint32_t max_slots);
	DirectFdTable(conflux::uring::RingRef ring, std::uint32_t max_slots);
	~DirectFdTable();
	DirectFdTable(DirectFdTable const &) = delete;
	DirectFdTable &operator =(DirectFdTable const &) = delete;
	DirectFdTable(DirectFdTable &&) = delete;
	DirectFdTable &operator =(DirectFdTable &&) = delete;
	[[nodiscard]] bool install(std::uint32_t slot, int fd);
	[[nodiscard]] bool registered() const noexcept { return registered_; }
	[[nodiscard]] int error() const noexcept { return err_; }
	[[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
};
// ─── raw submission: accept ──────────────────────────────────────────────────
// All borrowed data (buffers, iovecs) must remain valid until CQE completion.

export bool submit_accept_multishot_borrowed(
	SocketRawRing &ring,
	RingFd auto listen,
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
		sqe.prep_multishot_accept_direct(listen, addr, addrlen, accept_flags);
	} else {
		sqe.prep_multishot_accept(listen, addr, addrlen, accept_flags);
	}
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_accept_multishot_borrowed(
	SocketRawRing &ring,
	RingFd auto listen,
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
	RingFd auto listen,
	sockaddr *addr,
	socklen_t *addrlen,
	std::uint64_t user_data,
	bool direct = true) noexcept {
	return submit_accept_multishot_borrowed(ring, listen, addr, addrlen, user_data, 0, direct);
}
export bool submit_accept_multishot_borrowed(
	SocketRawRing &ring,
	RingFd auto listen,
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
	conflux::uring::CqeFlags last_flags) noexcept {
	if (!auto_enabled || !recv_poll_first || !have_last_flags) {
		return RecvArmPolicy::default_;
	}
	bool const nonempty = last_flags.any(conflux::uring::cqe_flags::sock_nonempty);
	return nonempty ? RecvArmPolicy::default_ : RecvArmPolicy::poll_first;
}
export bool submit_recv_multishot(
	SocketRawRing &ring,
	RingFd auto handle,
	BufferRing &bufs,
	std::uint64_t user_data,
	bool bundle = false,
	RecvArmPolicy arm = RecvArmPolicy::default_) {
	assert(!(bundle && bufs.mode() == BufferRingMode::incremental));
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_recv_multishot(handle, nullptr, 0, conflux::uring::MsgFlags{});
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

	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: send ────────────────────────────────────────────────────

export bool submit_send_borrowed(
	SocketRawRing &ring,
	RingFd auto handle,
	void const *data,
	std::size_t len,
	std::uint64_t user_data,
	int msg_flags = MSG_NOSIGNAL) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_send(handle, data, len, conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)});

	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_send_fixed_borrowed(
	SocketRawRing &ring,
	RingFd auto handle,
	std::uint32_t buf_idx,
	void const *data,
	std::size_t len,
	std::uint64_t user_data,
	int msg_flags = MSG_NOSIGNAL) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_send(handle, data, len, conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)});

	sqe.ioprio(conflux::uring::ioprio_flags::recvsend_fixed_buf);
	sqe.buf_index(conflux::uring::FixedBufIdx{static_cast<std::int32_t>(buf_idx)});
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_writev_borrowed(
	SocketRawRing &ring,
	RingFd auto handle,
	iovec const *iov,
	unsigned nr_vecs,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_writev(handle, iov, nr_vecs, 0);

	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_send_zc_borrowed(
	SocketRawRing &ring,
	RingFd auto handle,
	void const *data,
	std::size_t len,
	std::uint64_t user_data,
	bool report_usage = true,
	int msg_flags = MSG_NOSIGNAL) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_send_zc(handle, data, len, conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)}, 0);
	if (report_usage) {
		sqe.ioprio(conflux::uring::ioprio_flags::send_zc_report_usage);
	}
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: shutdown + close ────────────────────────────────────────
// Linked HARDLINK: shutdown(WR) then close. Requires 2 SQE slots.
// Returns false if SQ has fewer than 2 slots (caller should submit and retry).

export [[nodiscard]] bool submit_shutdown_close(
	SocketRawRing &ring,
	RingFd auto handle,
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
	shutdown_sqe.prep_shutdown(handle, SHUT_WR);
	shutdown_sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);

	shutdown_sqe.user_data(conflux::uring::UserData{shutdown_ud});
	close_sqe.prep_close(handle);
	close_sqe.user_data(conflux::uring::UserData{close_ud});
	return true;
}
// ─── raw submission: close ───────────────────────────────────────────────────

export bool submit_close(
	SocketRawRing &ring,
	RingFd auto handle,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_close(handle);
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
	RingFd auto handle,
	std::uint64_t shutdown_ud,
	std::uint64_t close_ud,
	SocketCloseOptions opts) noexcept {
	bool const needs_shutdown = handle.is_direct() ? opts.shutdown_write : opts.allow_async_shutdown_for_os_fd;
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
		shut_sqe.prep_shutdown(handle, SHUT_WR);
		shut_sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);

		if (opts.skip_shutdown_success_cqe) {
			shut_sqe.add_flags(conflux::uring::sqe_flags::cqe_skip_success);
		}
		shut_sqe.user_data(conflux::uring::UserData{shutdown_ud});
		close_sqe.prep_close(handle);
		close_sqe.user_data(conflux::uring::UserData{close_ud});
	} else {
		auto sqe = ring.try_get_sqe();
		if (!sqe) {
			return false;
		}
		sqe.prep_close(handle);
		sqe.user_data(conflux::uring::UserData{close_ud});
	}
	return true;
}
// ─── raw submission: setsockopt ──────────────────────────────────────────────
// Async socket option via io_uring cmd_sock. Only works with fixed fds.

export bool submit_setsockopt_borrowed(
	SocketRawRing &ring,
	DirectFd handle,
	int level,
	int optname,
	void const *optval,
	socklen_t optlen,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_cmd_sock_setsockopt(handle, level, optname, optval, static_cast<int>(optlen));
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
		 + ((opts.busy_poll_us_optval != nullptr) && *opts.busy_poll_us_optval > 0 ? 1U : 0U);
}
void prepare_direct_accept_sockopt_sqe(
	conflux::uring::Sqe sqe,
	DirectFd direct_socket,
	int level,
	int optname,
	void const *optval,
	int optlen,
	std::uint64_t user_data,
	bool skip_success_cqe) noexcept {
	sqe.prep_cmd_sock_setsockopt(direct_socket, level, optname, optval, optlen);
	sqe.add_flags(conflux::uring::sqe_flags::io_hardlink);
	if (skip_success_cqe) {
		sqe.add_flags(conflux::uring::sqe_flags::cqe_skip_success);
	}
	sqe.user_data(conflux::uring::UserData{user_data});
}

} // namespace

export [[nodiscard]] bool submit_direct_tcp_accept_setup_recv_to_group(
	SocketRawRing &ring,
	DirectFd direct_socket,
	DirectTcpAcceptRecvTarget target,
	std::uint64_t sockopt_ud,
	std::uint64_t recv_ud,
	DirectTcpAcceptSetup opts) noexcept {
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
	if ((opts.busy_poll_us_optval != nullptr) && *opts.busy_poll_us_optval > 0) {
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
	recv_sqe.prep_recv_multishot(direct_socket, nullptr, 0, conflux::uring::MsgFlags{});
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
	recv_sqe.user_data(conflux::uring::UserData{recv_ud});
	return true;
}
export [[nodiscard]] bool submit_direct_tcp_accept_setup_recv(
	SocketRawRing &ring,
	DirectFd direct_socket,
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
	RingFd auto listen,
	sockaddr *addr,
	socklen_t *addrlen,
	std::uint64_t user_data,
	int accept_flags) noexcept {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_accept(listen, addr, addrlen, accept_flags);

	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}

export bool submit_accept_direct_borrowed(
	SocketRawRing &ring,
	RingFd auto listen,
	sockaddr *addr,
	socklen_t *addrlen,
	std::uint64_t user_data,
	int accept_flags,
	std::uint32_t file_index) noexcept {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_accept_direct(listen, addr, addrlen, accept_flags, conflux::uring::DirectSlot{file_index});

	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: cancel ──────────────────────────────────────────────────

export bool submit_cancel_fd(
	SocketRawRing &ring,
	RingFd auto handle,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_cancel_fd(handle);
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
	RingFd auto handle,
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
	RingFd auto handle,
	sockaddr const *addr,
	socklen_t addrlen,
	std::uint64_t user_data,
	bool link_next = false) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_connect(handle, addr, addrlen);

	if (link_next) {
		sqe.add_flags(conflux::uring::sqe_flags::io_link);
	}
	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: recv (single-shot, into caller buffer) ──────────────────

export bool submit_async_recv_borrowed(
	SocketRawRing &ring,
	RingFd auto handle,
	void *buf,
	std::size_t len,
	std::uint64_t user_data,
	int msg_flags = 0) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_recv(handle, buf, len, conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)});

	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: sendmsg / recvmsg (UDP) ─────────────────────────────────

export bool submit_sendmsg_borrowed(
	SocketRawRing &ring,
	RingFd auto handle,
	msghdr const *msg,
	std::uint64_t user_data,
	unsigned flags = 0) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_sendmsg(handle, msg, conflux::uring::MsgFlags{flags});

	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
export bool submit_recvmsg_borrowed(
	SocketRawRing &ring,
	RingFd auto handle,
	msghdr *msg,
	std::uint64_t user_data,
	unsigned flags = 0) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_recvmsg(handle, msg, conflux::uring::MsgFlags{flags});

	sqe.user_data(conflux::uring::UserData{user_data});
	return true;
}
// ─── raw submission: linked recvmsg + timeout (UDP pattern) ──────────────────
// Requires 2 SQE slots. recvmsg is IO_LINK'd to a link_timeout.
// On timeout, recvmsg CQE arrives with res=-ECANCELED.
// Returns false if SQ has fewer than 2 slots.

export [[nodiscard]] bool submit_recvmsg_timeout_borrowed(
	SocketRawRing &ring,
	RingFd auto handle,
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
	recv_sqe.prep_recvmsg(handle, msg, conflux::uring::MsgFlags{recv_flags});
	recv_sqe.add_flags(conflux::uring::sqe_flags::io_link);

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
	RingFd auto handle,
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
	recv_sqe.prep_recv(handle, buf, len, conflux::uring::MsgFlags{});
	recv_sqe.add_flags(conflux::uring::sqe_flags::io_link);

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
	RingFd auto handle,
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
	send_sqe.prep_send(handle, data, len, conflux::uring::MsgFlags{static_cast<unsigned>(msg_flags)});
	send_sqe.add_flags(conflux::uring::sqe_flags::io_link);

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
	sqe.prep_fixed_fd_install(DirectFd::from_direct(direct_slot), conflux::uring::InstallFdFlags{});
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
export class TcpListener {
	OwnedSocketHandle fd_{};
	std::uint16_t port_{};
	int accept_flags_{SOCK_CLOEXEC | SOCK_NONBLOCK};

public:
	explicit TcpListener(TcpListenerOptions opts = {});
	~TcpListener() noexcept = default;
	TcpListener(TcpListener const &) = delete;
	TcpListener &operator =(TcpListener const &) = delete;
	TcpListener(TcpListener &&o) noexcept;
	TcpListener &operator =(TcpListener &&o) noexcept;
	[[nodiscard]] std::uint16_t port() const noexcept { return port_; }
	[[nodiscard]] int raw_fd() const noexcept { return fd_.raw_fd(); }
	[[nodiscard]] int accept_flags() const noexcept { return accept_flags_; }
	[[nodiscard]] RingFd auto handle() const noexcept { return fd_.get(); }
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
	RingFd auto handle,
	int how,
	std::uint64_t user_data) {
	auto sqe = ring.try_get_sqe();
	if (!sqe) {
		return false;
	}
	sqe.prep_shutdown(handle, how);
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
	// Must enqueue fn on ring-owner std::thread and return true, or return false.
	// Must NOT std::invoke fn inline from an arbitrary cancelling std::thread.
	// If null: ring is treated as single-threaded; submit_on_owner asserts caller==owner
	// and calls fn inline. Cross-std::thread cancel callers MUST provide submit_on_ring_owner.
	std::function<bool(RingOpFn)> submit_on_ring_owner{};
};
export class SocketTaskRing {
	SocketRawRing raw_;
	CompletionTable *completions_{};
	UserDataFn encode_ud_{};
	SocketTaskRingOptions opts_{};
	std::thread::id owner_thread_{std::this_thread::get_id()};

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
			return opts_.submit_on_ring_owner(std::move(fn));
		}
		assert(std::this_thread::get_id() == owner_thread_);
		fn(*this);
		return true;
	}
};

} // namespace conflux::socket_io
