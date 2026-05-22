module;
#include <cassert>
#include <liburing.h>
#include <sys/resource.h>

export module conflux.uring;
import std;
import conflux.types;
export import conflux_uring_fd;
export import conflux_uring_sqe;
export namespace conflux::uring {

// ── RingSize / build cap ──────────────────────────────────────────────────────

struct RingSize {
	std::uint32_t sq_entries{};
	std::uint32_t cq_entries{};
};
inline constexpr bool build_has_io_uring_resize_rings =
#if defined(CONFLUX_HAVE_IO_URING_RESIZE_RINGS) && CONFLUX_HAVE_IO_URING_RESIZE_RINGS
	true;
#else
	false;
#endif
// ── RingRef ──────────────────────────────────────────────────────────────────

class RingRef {
	io_uring *ring_{};

public:
	explicit RingRef(
		io_uring &r) noexcept
		: ring_{&r} {}
	explicit RingRef(
		io_uring *r) noexcept
		: ring_{r} {}
	[[nodiscard]] bool valid() const noexcept { return ring_ != nullptr; }
	[[nodiscard]] io_uring *raw() const noexcept {
		assert(ring_ != nullptr);
		return ring_;
	}
	[[nodiscard]] Sqe try_get_sqe() const noexcept {
		assert(ring_ != nullptr);
		return Sqe{io_uring_get_sqe(ring_)};
	}
	[[nodiscard]] unsigned sq_space_left() const noexcept {
		assert(ring_ != nullptr);
		return io_uring_sq_space_left(ring_);
	}
	[[nodiscard]] int submit() const noexcept {
		assert(ring_ != nullptr);
		return io_uring_submit(ring_);
	}
	[[nodiscard]] int register_files_sparse(
		unsigned nr) const noexcept {
		assert(ring_ != nullptr);
		return io_uring_register_files_sparse(ring_, nr);
	}
	[[nodiscard]] int register_files_update(
		unsigned off,
		std::span<int const> fds) const noexcept {
		assert(ring_ != nullptr);
		return io_uring_register_files_update(ring_, off, fds.data(), static_cast<unsigned>(fds.size()));
	}
	[[nodiscard]] int unregister_files() const noexcept {
		assert(ring_ != nullptr);
		return io_uring_unregister_files(ring_);
	}
	[[nodiscard]] bool cq_has_overflow() const noexcept {
		assert(ring_ != nullptr);
		return static_cast<int>(io_uring_cq_has_overflow(ring_)) != 0;
	}
	[[nodiscard]] bool has_feature(
		std::uint32_t feature) const noexcept {
		assert(ring_ != nullptr);
		return (ring_->features & feature) != 0u;
	}
	[[nodiscard]] std::uint32_t cq_overflow_count() const noexcept {
		assert(ring_ != nullptr);
		auto *p = ring_->cq.koverflow;
		return p != nullptr ? *p : 0u;
	}
	[[nodiscard]] bool is_sqpoll() const noexcept {
		assert(ring_ != nullptr);
		return (ring_->flags & IORING_SETUP_SQPOLL) != 0u;
	}
	[[nodiscard]] std::uint32_t sq_entries() const noexcept {
		assert(ring_ != nullptr);
		return ring_->sq.ring_entries;
	}
	[[nodiscard]] std::uint32_t cq_entries() const noexcept {
		assert(ring_ != nullptr);
		return ring_->cq.ring_entries;
	}
	[[nodiscard]] std::expected<void, int> resize(
		RingSize sz) const noexcept {
		if (!valid()) {
			return std::unexpected{-EINVAL};
		}
		if (sz.sq_entries == 0 || sz.cq_entries == 0) {
			return std::unexpected{-EINVAL};
		}
		if (cq_has_overflow()) {
			return std::unexpected{-EBUSY};
		}
		if (io_uring_sq_ready(ring_) != 0) {
			return std::unexpected{-EBUSY};
		}
// no CQ-ready guard: kernel copies pending CQEs during resize; only CQ overflow is illegal
#if defined(CONFLUX_HAVE_IO_URING_RESIZE_RINGS) && CONFLUX_HAVE_IO_URING_RESIZE_RINGS
		if ((ring_->flags & IORING_SETUP_DEFER_TASKRUN) == 0) {
			return std::unexpected{-EINVAL};
		}
		if ((ring_->flags & IORING_SETUP_NO_MMAP) != 0) {
			return std::unexpected{-EOPNOTSUPP};
		}
		io_uring_params p{};
		p.flags = IORING_SETUP_CQSIZE; // required: without it kernel ignores cq_entries and derives from sq_entries
		p.sq_entries = sz.sq_entries;
		p.cq_entries = sz.cq_entries;
		if (int rc = io_uring_resize_rings(ring_, &p); rc < 0) {
			return std::unexpected{rc};
		}
		return {};
#else
		return std::unexpected{-ENOSYS};
#endif
	}
	[[nodiscard]] std::expected<void, int> grow_cq_to(
		std::uint32_t entries) const noexcept {
		if (!valid()) {
			return std::unexpected{-EINVAL};
		}
		std::uint32_t const cur_sq = ring_->sq.ring_entries;
		std::uint32_t const cur_cq = ring_->cq.ring_entries;
		if (entries <= cur_cq) {
			return {};
		}
		return resize({.sq_entries = cur_sq, .cq_entries = entries});
	}
};

// ── Ring ─────────────────────────────────────────────────────────────────────

class Linked;
class Ring {
	io_uring ring_{};
	bool valid_{false};

public:
	Ring() noexcept = default;
	Ring(Ring const &) = delete;
	Ring &operator =(Ring const &) = delete;
	Ring(
		Ring &&o) noexcept
		: ring_{o.ring_}
		, valid_{o.valid_} {
		o.valid_ = false;
	}
	Ring &operator =(
		Ring &&o) noexcept {
		if (this != &o) {
			if (valid_) {
				io_uring_queue_exit(&ring_);
			}
			ring_ = o.ring_;
			valid_ = o.valid_;
			o.valid_ = false;
		}
		return *this;
	}
	~Ring() {
		if (valid_) {
			io_uring_queue_exit(&ring_);
		}
	}
	[[nodiscard]] static std::expected<Ring, int> init(
		unsigned entries,
		SetupFlags flags) noexcept {
		Ring r;
		if (int const rc = io_uring_queue_init(entries, &r.ring_, flags.raw()); rc < 0) {
			return std::unexpected{rc};
		}
		r.valid_ = true;
		return r;
	}
	[[nodiscard]] static std::expected<Ring, int> init_params(
		unsigned entries,
		io_uring_params &p) noexcept {
		Ring r;
		if (int const rc = io_uring_queue_init_params(entries, &r.ring_, &p); rc < 0) {
			return std::unexpected{rc};
		}
		r.valid_ = true;
		return r;
	}
	[[nodiscard]] static std::expected<Ring, int> init_mem(
		unsigned entries,
		io_uring_params &p,
		void *buf,
		std::size_t buf_sz) noexcept {
		Ring r;
		if (int const rc = io_uring_queue_init_mem(entries, &r.ring_, &p, buf, buf_sz); rc < 0) {
			return std::unexpected{rc};
		}
		r.valid_ = true;
		return r;
	}
	[[nodiscard]] bool valid() const noexcept { return valid_; }
	[[nodiscard]] explicit operator bool() const noexcept { return valid_; }
	// ── SQE ────────────────────────────────────────────────────────────────

	[[nodiscard]] Sqe get_sqe() noexcept { return Sqe{io_uring_get_sqe(&ring_)}; }
	[[nodiscard]] RingRef ref() noexcept { return RingRef{ring_}; }
	[[nodiscard]] Linked linked() noexcept;
	[[nodiscard]] Sqe get_sqe_or_submit() noexcept {
		Sqe s{io_uring_get_sqe(&ring_)};
		if (s) {
			return s;
		}
		io_uring_submit(&ring_);
		return Sqe{io_uring_get_sqe(&ring_)};
	}
	[[nodiscard]] unsigned sq_space_left() const noexcept { return io_uring_sq_space_left(&ring_); }
	[[nodiscard]] bool has_feature(
		std::uint32_t feat) const noexcept {
		return (ring_.features & feat) != 0u;
	}
	[[nodiscard]] bool is_sqpoll() const noexcept { return (ring_.flags & IORING_SETUP_SQPOLL) != 0u; }
	[[nodiscard]] bool cq_has_overflow() const noexcept {
		return static_cast<int>(io_uring_cq_has_overflow(&ring_)) != 0;
	}
	[[nodiscard]] std::uint32_t cq_overflow_count() const noexcept {
		auto *p = ring_.cq.koverflow;
		return p != nullptr ? *p : 0u;
	}
	[[nodiscard]] std::expected<void, int> resize(
		RingSize sz) noexcept {
		return ref().resize(sz);
	}
	[[nodiscard]] std::expected<void, int> grow_cq_to(
		std::uint32_t entries) noexcept {
		return ref().grow_cq_to(entries);
	}
	// ── Submit ──────────────────────────────────────────────────────────────

	int submit() noexcept { return io_uring_submit(&ring_); }
	int submit_and_wait(
		unsigned wait_nr) noexcept {
		return io_uring_submit_and_wait(&ring_, wait_nr);
	}
	int submit_and_wait_timeout(
		io_uring_cqe **cqe_ptr,
		unsigned wait_nr,
		__kernel_timespec *ts,
		sigset_t *sigmask) noexcept {
		return io_uring_submit_and_wait_timeout(&ring_, cqe_ptr, wait_nr, ts, sigmask);
	}
	int get_events() noexcept { return io_uring_get_events(&ring_); }
	int submit_and_get_events() noexcept { return io_uring_submit_and_get_events(&ring_); }
	// ── CQE consumption ─────────────────────────────────────────────────────

	int peek_cqe(
		io_uring_cqe **cqe_ptr) noexcept {
		return io_uring_peek_cqe(&ring_, cqe_ptr);
	}
	unsigned peek_batch_cqe(
		io_uring_cqe **cqes,
		unsigned count) noexcept {
		return io_uring_peek_batch_cqe(&ring_, cqes, count);
	}
	int wait_cqe(
		io_uring_cqe **cqe_ptr) noexcept {
		return io_uring_wait_cqe(&ring_, cqe_ptr);
	}
	void cq_advance(
		unsigned nr) noexcept {
		io_uring_cq_advance(&ring_, nr);
	}
	void cqe_seen(
		io_uring_cqe *cqe) noexcept {
		io_uring_cqe_seen(&ring_, cqe);
	}
	// ── Registration: files ─────────────────────────────────────────────────

	int register_files(
		std::span<int const> fds) noexcept {
		return io_uring_register_files(&ring_, fds.data(), static_cast<unsigned>(fds.size()));
	}
	int register_files_sparse(
		unsigned nr) noexcept {
		return io_uring_register_files_sparse(&ring_, nr);
	}
	int register_files_update(
		unsigned off,
		std::span<int const> fds) noexcept {
		return io_uring_register_files_update(&ring_, off, fds.data(), static_cast<unsigned>(fds.size()));
	}
	int unregister_files() noexcept { return io_uring_unregister_files(&ring_); }
	// ── Registration: buffers ───────────────────────────────────────────────

	int register_buffers(
		std::span<iovec const> iovs) noexcept {
		return io_uring_register_buffers(&ring_, iovs.data(), static_cast<unsigned>(iovs.size()));
	}
	int register_buffers_sparse(
		unsigned nr) noexcept {
		return io_uring_register_buffers_sparse(&ring_, nr);
	}
	// tags: one __u64 tag per iovec slot; null = no tags
	int register_buffers_update_tag(
		unsigned off,
		std::span<iovec const> iovs,
		std::uint64_t const *tags) noexcept {
		return io_uring_register_buffers_update_tag(
			&ring_,
			off,
			iovs.data(),
			reinterpret_cast<__u64 const *>(tags),
			static_cast<unsigned>(iovs.size()));
	}
	int unregister_buffers() noexcept { return io_uring_unregister_buffers(&ring_); }
	// ── Registration: eventfd / ring fd ─────────────────────────────────────

	int register_eventfd(
		SqeFd fd) noexcept {
		return io_uring_register_eventfd(&ring_, fd.v);
	}
	int register_eventfd_async(
		SqeFd fd) noexcept {
		return io_uring_register_eventfd_async(&ring_, fd.v);
	}
	int unregister_eventfd() noexcept { return io_uring_unregister_eventfd(&ring_); }
	int register_ring_fd() noexcept { return io_uring_register_ring_fd(&ring_); }
	int unregister_ring_fd() noexcept { return io_uring_unregister_ring_fd(&ring_); }
	// ── Registration: misc ──────────────────────────────────────────────────

	int register_file_alloc_range(
		unsigned off,
		unsigned len) noexcept {
		return io_uring_register_file_alloc_range(&ring_, off, len);
	}
	int register_iowq_max_workers(
		unsigned *values) noexcept {
		return io_uring_register_iowq_max_workers(&ring_, values);
	}
	// ── Raw access — for gradual migration only ──────────────────────────────

	[[nodiscard]] io_uring *raw() noexcept { return &ring_; }
	[[nodiscard]] io_uring const *raw() const noexcept { return &ring_; }
};
// ── BufRing ───────────────────────────────────────────────────────────────────

class BufRing {
	io_uring_buf_ring *p_{nullptr};
	io_uring *ring_{nullptr};
	unsigned count_{};
	BufGroupId group_{};
	std::uint32_t mask_{};

public:
	BufRing() noexcept = default;
	BufRing(BufRing const &) = delete;
	BufRing &operator =(BufRing const &) = delete;
	BufRing(
		BufRing &&o) noexcept
		: p_{o.p_}
		, ring_{o.ring_}
		, count_{o.count_}
		, group_{o.group_}
		, mask_{o.mask_} {
		o.p_ = nullptr;
	}
	BufRing &operator =(
		BufRing &&o) noexcept {
		if (this != &o) {
			if (p_ != nullptr) {
				io_uring_free_buf_ring(ring_, p_, count_, group_.v);
			}
			p_ = o.p_;
			ring_ = o.ring_;
			count_ = o.count_;
			group_ = o.group_;
			mask_ = o.mask_;
			o.p_ = nullptr;
		}
		return *this;
	}
	~BufRing() {
		if (p_ != nullptr) {
			io_uring_free_buf_ring(ring_, p_, count_, group_.v);
		}
	}
	[[nodiscard]] static std::expected<BufRing, int> setup(
		Ring &ring,
		unsigned count,
		BufGroupId group,
		unsigned flags = 0u) noexcept {
		int err{};
		auto *p = io_uring_setup_buf_ring(ring.raw(), count, group.v, flags, &err);
		if (p == nullptr) {
			return std::unexpected{err};
		}
		BufRing br;
		br.p_ = p;
		br.ring_ = ring.raw();
		br.count_ = count;
		br.group_ = group;
		br.mask_ = static_cast<std::uint32_t>(io_uring_buf_ring_mask(count));
		return br;
	}
	[[nodiscard]] static std::expected<BufRing, int> setup(
		RingRef ring,
		unsigned count,
		BufGroupId group,
		unsigned flags = 0u) noexcept {
		int err{};
		auto *p = io_uring_setup_buf_ring(ring.raw(), count, group.v, flags, &err);
		if (p == nullptr) {
			return std::unexpected{err};
		}
		BufRing br;
		br.p_ = p;
		br.ring_ = ring.raw();
		br.count_ = count;
		br.group_ = group;
		br.mask_ = static_cast<std::uint32_t>(io_uring_buf_ring_mask(count));
		return br;
	}
	[[nodiscard]] bool valid() const noexcept { return p_ != nullptr; }
	[[nodiscard]] BufGroupId group() const noexcept { return group_; }
	[[nodiscard]] std::uint32_t mask() const noexcept { return mask_; }
	[[nodiscard]] io_uring_buf_ring *raw() noexcept { return p_; }
	void add(
		void *addr,
		std::uint32_t len,
		BufId bid,
		int offset) noexcept {
		io_uring_buf_ring_add(p_, addr, static_cast<unsigned short>(len), bid.v, static_cast<int>(mask_), offset);
	}
	void advance(
		int count) noexcept {
		io_uring_buf_ring_advance(p_, count);
	}
};
// ── Linked ────────────────────────────────────────────────────────────────────
// Lazily sets IOSQE_IO_LINK on each SQE when the next one is requested,
// so the last SQE in the chain never gets the link flag.

class Linked {
	Ring &ring_;
	io_uring_sqe *pending_{};

public:
	explicit Linked(
		Ring &r) noexcept
		: ring_{r} {}
	Linked(Linked const &) = delete;
	Linked &operator =(Linked const &) = delete;
	Sqe then() noexcept {
		if (pending_ != nullptr) {
			pending_->flags = static_cast<decltype(pending_->flags)>(pending_->flags | sqe_flags::io_link.raw());
		}
		auto s = ring_.get_sqe();
		pending_ = s.raw();
		return s;
	}
	// hard() protects only against failure of the immediately preceding request.
	// It does not rescue cleanup if an earlier soft link cancelled the tail.
	// This is not a true finally.
	Sqe hard() noexcept {
		if (pending_ != nullptr) {
			pending_->flags = static_cast<decltype(pending_->flags)>(pending_->flags | sqe_flags::io_hardlink.raw());
		}
		auto s = ring_.get_sqe();
		pending_ = s.raw();
		return s;
	}
};
inline Linked Ring::linked() noexcept {
	return Linked{*this};
}
// ── mlock_size helper ─────────────────────────────────────────────────────────

[[nodiscard]] inline ssize_t mlock_size(
	unsigned entries,
	SetupFlags flags) noexcept {
	return io_uring_mlock_size(entries, flags.raw());
}
// ── buf_ring_flags / feat_bits ────────────────────────────────────────────────

namespace buf_ring_flags {

inline constexpr bool has_inc{true};
inline constexpr unsigned inc{static_cast<unsigned>(IOU_PBUF_RING_INC)};

} // namespace buf_ring_flags
// ── IoUringCaps ───────────────────────────────────────────────────────────────

struct IoUringCaps {
	// raw kernel feature bits
	bool feat_nodrop{};
	bool feat_submit_stable{};
	bool feat_recvsend_bundle{};
	bool feat_pbuf_ring_inc{};
	bool feat_resize_rings{};
	bool feat_reg_buf_clone{};
	// semantic caps (derived)
	bool path_lifetime_stable{}; // feat_submit_stable && !sqpoll
	bool recvsend_bundle{}; // alias for feat_recvsend_bundle
	// probe-based (IORING_REGISTER_PROBE)
	bool op_socket{};
	bool socket_direct_alloc{}; // op_socket && file table registered — set by caller after init
	bool accept_direct_supported{}; // derived proxy: op_socket-era kernel, no independent probe
	bool op_uring_cmd{};
	bool cmd_sock_setsockopt{}; // op_uring_cmd
	bool send_zc{};
	bool recv_zc{};
	// proposal-name aliases
	bool resize_rings{};
	bool registered_buffer_clone{};
	bool zc_rx{};
	bool recv_poll_first{}; // IORING_RECV/SEND ioprio poll_first, available since 5.19
};
[[nodiscard]] IoUringCaps detect_caps(
	RingRef ring) noexcept {
	IoUringCaps c;
	c.feat_nodrop = ring.has_feature(IORING_FEAT_NODROP);
	c.feat_submit_stable = ring.has_feature(IORING_FEAT_SUBMIT_STABLE);
	c.feat_recvsend_bundle = ring.has_feature(IORING_FEAT_RECVSEND_BUNDLE);
	c.feat_pbuf_ring_inc = CONFLUX_RUNTIME_HAS_IOU_PBUF_RING_INC != 0;
	c.feat_resize_rings = CONFLUX_RUNTIME_HAS_IO_URING_RESIZE_RINGS != 0;
	c.feat_reg_buf_clone = CONFLUX_RUNTIME_HAS_IO_URING_CLONE_BUFFERS != 0;
	c.path_lifetime_stable = c.feat_submit_stable && !ring.is_sqpoll();
	c.recvsend_bundle = c.feat_recvsend_bundle;
	c.resize_rings = c.feat_resize_rings;
	c.registered_buffer_clone = c.feat_reg_buf_clone;
	io_uring_probe *probe = io_uring_get_probe_ring(ring.raw());
	if (probe != nullptr) {
		c.op_socket = io_uring_opcode_supported(probe, IORING_OP_SOCKET) != 0;
		c.accept_direct_supported = c.op_socket;
		c.op_uring_cmd = io_uring_opcode_supported(probe, IORING_OP_URING_CMD) != 0;
		c.cmd_sock_setsockopt = c.op_uring_cmd;
		c.send_zc = io_uring_opcode_supported(probe, IORING_OP_SEND_ZC) != 0;
		c.recv_zc = io_uring_opcode_supported(probe, IORING_OP_RECV_ZC) != 0;
		io_uring_free_probe(probe);
	}
	c.zc_rx = c.recv_zc;
	c.recv_poll_first = true; // present since 5.19, below our kernel floor
	return c;
}
[[nodiscard]] IoUringCaps detect_caps(
	Ring &ring) noexcept {
	return detect_caps(ring.ref());
}
[[nodiscard]] std::string caps_to_log_string(
	IoUringCaps const &c) {
	std::string s;
	auto app = [&](char const *name, bool v) {
		if (!v) {
			return;
		}
		if (!s.empty()) {
			s += ',';
		}
		s += name;
	};
	app("feat_nodrop", c.feat_nodrop);
	app("feat_submit_stable", c.feat_submit_stable);
	app("feat_recvsend_bundle", c.feat_recvsend_bundle);
	app("feat_pbuf_ring_inc", c.feat_pbuf_ring_inc);
	app("feat_resize_rings", c.feat_resize_rings);
	app("feat_reg_buf_clone", c.feat_reg_buf_clone);
	app("path_lifetime_stable", c.path_lifetime_stable);
	app("recvsend_bundle", c.recvsend_bundle);
	app("op_socket", c.op_socket);
	app("socket_direct_alloc", c.socket_direct_alloc);
	app("accept_direct_supported", c.accept_direct_supported);
	app("op_uring_cmd", c.op_uring_cmd);
	app("cmd_sock_setsockopt", c.cmd_sock_setsockopt);
	app("send_zc", c.send_zc);
	app("recv_zc", c.recv_zc);
	app("recv_poll_first", c.recv_poll_first);
	return s;
}

} // namespace conflux::uring

export namespace conflux::runtime {

[[nodiscard]] std::expected<RuntimeCapabilities, CapabilityIssue> detect_capabilities() {
	RuntimeCapabilities caps{};
#if CONFLUX_USE_MOCK_LIBURING
	caps.mock_backend = true;
	caps.issues.push_back(
		CapabilityIssue{
			.code = CapabilityIssueCode::mock_backend,
			.feature = "io_uring",
			.message = "mock liburing backend is active",
			.hint = "configure without CONFLUX_USE_MOCK_LIBURING to probe the running kernel"});
	return caps;
#else
	auto ring = conflux::uring::Ring::init(32, {});
	if (!ring) {
		CapabilityIssue issue{
			.code = CapabilityIssueCode::unavailable,
			.feature = "io_uring",
			.message = std::format("io_uring init failed: {}", ring.error()),
			.hint = "check kernel io_uring support, seccomp policy, and process limits"};
		return std::unexpected{std::move(issue)};
	}
	caps.io_uring = true;
	auto const uring_caps = conflux::uring::detect_caps(*ring);
	caps.sqpoll = ring->is_sqpoll();
	caps.single_issuer = true;
	caps.defer_taskrun = true;
	caps.coop_taskrun = true;
	caps.taskrun_flag = true;
	caps.multishot_accept = uring_caps.accept_direct_supported;
	caps.multishot_recv = uring_caps.recv_poll_first;
	caps.provided_buffers = true;
	caps.incremental_buffers = uring_caps.feat_pbuf_ring_inc;
	caps.registered_files = true;
	caps.fixed_buffers = true;
	caps.send_zc = uring_caps.send_zc;
	caps.recv_zc = uring_caps.recv_zc;
	caps.openat2 = true;
	struct rlimit limit{};
	if (::getrlimit(RLIMIT_MEMLOCK, &limit) == 0) {
		caps.memlock_soft = limit.rlim_cur == RLIM_INFINITY ? UINT64_MAX : static_cast<std::uint64_t>(limit.rlim_cur);
		caps.memlock_hard = limit.rlim_max == RLIM_INFINITY ? UINT64_MAX : static_cast<std::uint64_t>(limit.rlim_max);
	}
	return caps;
#endif
}

[[nodiscard]] std::string capability_report(
	RuntimeCapabilities const &caps) {
	auto yes_no = [](bool value) { return value ? "yes" : "no"; };
	std::string out;
	out += std::format("io_uring: {}\n", caps.mock_backend ? "mock" : yes_no(caps.io_uring));
	out += std::format("provided_buffers: {}\n", yes_no(caps.provided_buffers));
	out += std::format("incremental_buffers: {}\n", yes_no(caps.incremental_buffers));
	out += std::format("send_zc: {}\n", yes_no(caps.send_zc));
	out += std::format("recv_zc: {}\n", yes_no(caps.recv_zc));
	out += std::format("memlock_soft: {}\n", caps.memlock_soft);
	for (auto const &issue: caps.issues) {
		out += std::format("issue {} {}: {}", capability_issue_code_string(issue.code), issue.feature, issue.message);
		if (!issue.hint.empty()) {
			out += std::format(" ({})", issue.hint);
		}
		out += '\n';
	}
	return out;
}

} // namespace conflux::runtime
