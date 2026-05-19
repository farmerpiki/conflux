module;
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstring>
#include <liburing.h>

export module conflux.uring.flow;
import std;
import conflux.types;
import conflux.uring;
// ── Exported types ────────────────────────────────────────────────────────────

export namespace conflux::uring::flow {

enum class FlowOpKind : std::uint8_t {
	open_direct,
	read,
	write,
	close_direct,
};
struct BorrowedPath {
	char const *ptr;
};
struct OwnedInlinePath {
	// hack: cap = NAME_MAX (255). Not PATH_MAX. from_sv() rejects longer inputs with -ENAMETOOLONG.
	static constexpr std::size_t cap = 255;
	std::array<char, cap + 1> buf{};
	std::size_t len{};
	[[nodiscard]] static std::expected<OwnedInlinePath, int> from_sv(
		std::string_view sv) noexcept {
		if (sv.size() > cap) {
			return std::unexpected{-ENAMETOOLONG};
		}
		if (sv.find('\0') != std::string_view::npos) {
			return std::unexpected{-EINVAL};
		}
		OwnedInlinePath p{};
		std::memcpy(p.buf.data(), sv.data(), sv.size());
		p.buf[sv.size()] = '\0';
		p.len = sv.size();
		return p;
	}
	[[nodiscard]] char const *c_str() const noexcept { return buf.data(); }
};
struct OpResult {
	std::int32_t res = 0;
	std::uint32_t requested = 0;
	FlowOpKind kind = FlowOpKind::open_direct;
	[[nodiscard]] bool ok() const noexcept { return res >= 0; }
	[[nodiscard]] bool is_io() const noexcept { return kind == FlowOpKind::read || kind == FlowOpKind::write; }
	[[nodiscard]] bool short_io() const noexcept { return is_io() && res >= 0 && std::uint32_t(res) < requested; }
	[[nodiscard]] bool full_io() const noexcept { return is_io() && res >= 0 && std::uint32_t(res) == requested; }
};
struct FlowRejection {
	std::uint32_t flow_local_index;
	int err;
};
struct FlowResult {
	std::span<OpResult const> ops;
	bool close_needed;
	bool close_in_chain;
	bool close_cqe_seen;
	std::int32_t close_raw_res;
	[[nodiscard]] bool open_ok() const noexcept { return !ops.empty() && ops[0].res >= 0; }
	[[nodiscard]] std::optional<std::int32_t> cleanup_result() const noexcept {
		if (!close_needed || !close_cqe_seen) {
			return std::nullopt;
		}
		return close_raw_res;
	}
	[[nodiscard]] std::optional<std::int32_t> raw_close_result() const noexcept {
		return close_cqe_seen ? std::optional<std::int32_t>{close_raw_res} : std::nullopt;
	}
};
// encode_tag / encode_tag_raw — exported so tests can craft CQEs without
// duplicating the bitfield layout.
[[nodiscard]] inline std::uint64_t encode_tag_raw(
	std::uint32_t idx,
	std::uint32_t gen,
	std::uint8_t op_idx,
	std::uint8_t raw_kind) noexcept {
	struct Tag {
		std::uint64_t flow_index :24;
		std::uint64_t generation :24;
		std::uint64_t op_index   :8;
		std::uint64_t op_kind    :8;
	};
	static_assert(sizeof(Tag) == 8);
	Tag d{};
	d.flow_index = idx & 0xFFFFFFu;
	d.generation = gen & 0xFFFFFFu;
	d.op_index = op_idx;
	d.op_kind = raw_kind;
	std::uint64_t v{};
	std::memcpy(&v, &d, 8);
	return v;
}
[[nodiscard]] inline std::uint64_t encode_tag(
	std::uint32_t idx,
	std::uint32_t gen,
	std::uint8_t op_idx,
	FlowOpKind kind) noexcept {
	return encode_tag_raw(idx, gen, op_idx, std::to_underlying(kind));
}

} // namespace conflux::uring::flow
// ── Internal constants and types ──────────────────────────────────────────────

namespace conflux::uring::flow {

inline constexpr std::uint8_t max_initial_ops = 8;
inline constexpr std::uint8_t max_chain_cqes = max_initial_ops + 1;
inline constexpr std::uint32_t kMaxFlows = 4096;
inline constexpr std::uint32_t kMaxBatch = 64;

enum class LinkVariant : std::uint8_t {
	then_,
	hard_,
};
[[nodiscard]] constexpr bool is_valid_flow_op_kind(
	std::uint8_t v) noexcept {
	switch (static_cast<FlowOpKind>(v)) {
	case FlowOpKind::open_direct:
	case FlowOpKind::read:
	case FlowOpKind::write:
	case FlowOpKind::close_direct: return true;
	}
	return false;
}
[[nodiscard]] constexpr bool direct_open_succeeded(
	std::int32_t res) noexcept {
	return res >= 0;
}
struct PendingRead {
	void *buf;
	std::uint32_t len;
	std::uint64_t offset;
};
struct PendingWrite {
	void const *buf;
	std::uint32_t len;
	std::uint64_t offset;
};
struct PendingOpenDirect {
	DirectSlot slot;
	int dfd;
	char const *path;
	int open_flags;
	mode_t mode;
};
struct PendingOp {
	FlowOpKind kind;
	LinkVariant variant = LinkVariant::then_;
	union {
		PendingOpenDirect open;
		PendingRead read;
		PendingWrite write;
	};
};
struct FlowUserData {
	std::uint64_t flow_index :24;
	std::uint64_t generation :24;
	std::uint64_t op_index   :8;
	std::uint64_t op_kind    :8;
};
static_assert(sizeof(FlowUserData) == 8);
[[nodiscard]] inline FlowUserData decode_tag(
	std::uint64_t v) noexcept {
	FlowUserData d{};
	std::memcpy(&d, &v, 8);
	return d;
}
struct DirectFileFlowState {
	std::uint32_t flow_index;
	std::uint32_t generation;
	DirectSlot slot;
	std::uint8_t initial_op_count = 0;
	std::uint8_t expected_cqes = 0;
	std::uint8_t seen_cqes = 0;
	bool open_seen = false;
	bool open_ok = false;
	std::int32_t open_res = 0;
	bool close_requested = false;
	bool close_in_chain = false;
	bool close_submitted = false;
	bool close_pending = false;
	bool close_seen = false;
	std::int32_t close_res = 0;
	std::array<OpResult, max_initial_ops> results{};
};
[[nodiscard]] inline bool close_needed_pred(
	DirectFileFlowState const &st) noexcept {
	return st.close_requested && st.open_ok;
}
struct DirectFileBuilder {
	DirectSlot slot;
	std::array<PendingOp, max_initial_ops> ops{};
	std::uint8_t op_count = 0;
	bool close_requested = false;
	bool owns_path = false;
	OwnedInlinePath owned_path_buf{};
	int err = 0;
};
// ── Slab with O(1) freelist ───────────────────────────────────────────────────
// generation==0 is the never-allocated sentinel; release() leaves it intact.

class FlowSlab {
	std::array<DirectFileFlowState, kMaxFlows> cells_{};
	std::array<std::uint32_t, kMaxFlows> free_{};
	std::uint32_t free_top_ = 0; // index into free_ (stack grows up)
public:
	FlowSlab() noexcept {
		for (std::uint32_t i = 0; i < kMaxFlows; ++i) {
			cells_[i].flow_index = i;
			cells_[i].generation = 0;
			free_[i] = kMaxFlows - 1 - i; // reverse so first pop gives index 0
		}
		free_top_ = kMaxFlows;
	}
	[[nodiscard]] DirectFileFlowState *try_allocate() noexcept {
		if (free_top_ == 0) {
			return nullptr;
		}
		std::uint32_t const i = free_[--free_top_];
		auto &cell = cells_[i];
		std::uint32_t g = (cell.generation + 1) & 0xFFFFFFu;
		if (g == 0) {
			g = 1;
		}
		cell.generation = g;
		return &cell;
	}
	void release(
		DirectFileFlowState &st) noexcept {
		// generation left intact; next try_allocate will bump it.
		// Safety: finish_flow is called only after all std::expected CQEs are observed,
		// so no live CQE for the released owner can arrive after this point.
		free_[free_top_++] = st.flow_index;
	}
	[[nodiscard]] DirectFileFlowState *try_get(
		std::uint32_t flow_idx,
		std::uint32_t gen) noexcept {
		if (flow_idx >= kMaxFlows) {
			return nullptr;
		}
		auto &st = cells_[flow_idx];
		if (st.generation == 0) {
			return nullptr; // never-allocated cell
		}
		if (st.generation != gen) {
			return nullptr; // stale or reallocated
		}
		return &st;
	}
// hack: test-only slab manipulation
#ifdef CONFLUX_TESTING
	void test_hack_generation(
		std::uint32_t idx,
		std::uint32_t gen) noexcept {
		if (idx < kMaxFlows) {
			cells_[idx].generation = gen;
		}
	}
	void test_hack_drain_freelist() noexcept { free_top_ = 0; }
#endif
};
// ── SBO callback wrapper (internal, never heap-allocates) ─────────────────────
// Capacity: 64 bytes of inline storage. Triggers static_assert at construction
// if the callable is larger.

class FlowCb {
	static constexpr std::size_t kBufSize = 64;
	alignas(std::max_align_t) char buf_[kBufSize]{};
	void (*call_)(char *, FlowResult) noexcept = nullptr;
	void (*dtor_)(char *) noexcept = nullptr;

public:
	FlowCb() noexcept = default;
	FlowCb(FlowCb const &) = delete;
	FlowCb &operator =(FlowCb const &) = delete;
	FlowCb(FlowCb &&) = delete;
	FlowCb &operator =(FlowCb &&) = delete;
	~FlowCb() noexcept {
		if (dtor_ != nullptr) {
			dtor_(buf_);
		}
	}
	template<class F>
	void set(
		F &&f) noexcept {
		using T = std::decay_t<F>;
		static_assert(sizeof(T) <= kBufSize, "FlowCb: callable too large for SBO buffer");
		static_assert(noexcept(std::declval<T &>()(std::declval<FlowResult>())), "FlowCb: callable must be noexcept");
		new (buf_) T(std::forward<F>(f));
		call_ = [](char *p, FlowResult r) noexcept { (*reinterpret_cast<T *>(p))(r); };
		dtor_ = [](char *p) noexcept { reinterpret_cast<T *>(p)->~T(); };
	}
	void operator ()(
		FlowResult r) noexcept {
		if (call_ != nullptr) {
			call_(buf_, r);
		}
	}
	explicit operator bool() const noexcept { return call_ != nullptr; }
};
// ── Link flag helpers ─────────────────────────────────────────────────────────

[[nodiscard]] inline bool uses_fixed_file_fd(
	FlowOpKind k) noexcept {
	return k == FlowOpKind::read || k == FlowOpKind::write;
}
constexpr unsigned link_mask = IOSQE_IO_LINK | IOSQE_IO_HARDLINK;
[[nodiscard]] unsigned link_flag_for_boundary(
	std::uint8_t /*from*/,
	std::uint8_t to,
	DirectFileBuilder const &b,
	bool mode_b) noexcept {
	if (mode_b && to == b.op_count) {
		return (b.op_count == 1) ? IOSQE_IO_LINK : IOSQE_IO_HARDLINK;
	}
	return (b.ops[to].variant == LinkVariant::hard_) ? IOSQE_IO_HARDLINK : IOSQE_IO_LINK;
}
// ── mode_b_eligible ───────────────────────────────────────────────────────────

[[nodiscard]] bool mode_b_eligible(
	DirectFileBuilder const &b) noexcept {
	if (b.op_count < 1) {
		return false;
	}
	if (b.op_count == 1) {
		return true;
	}
	if (b.ops[1].variant != LinkVariant::then_) {
		return false;
	}
	for (std::uint8_t i = 2; i < b.op_count; ++i) {
		if (b.ops[i].variant != LinkVariant::hard_) {
			return false;
		}
	}
	return true;
}
// ── prep helpers ──────────────────────────────────────────────────────────────

void prep_op(
	io_uring_sqe *sqe,
	std::uint8_t i,
	DirectFileBuilder const &b,
	bool is_close,
	char const *open_path_override = nullptr) noexcept {
	if (is_close) {
		io_uring_prep_close_direct(sqe, b.slot.value);
		return;
	}
	auto const &op = b.ops[i];
	switch (op.kind) {
	case FlowOpKind::open_direct:
		io_uring_prep_openat_direct(
			sqe,
			op.open.dfd,
			open_path_override != nullptr ? open_path_override : op.open.path,
			op.open.open_flags,
			op.open.mode,
			op.open.slot.value);
		break;
	case FlowOpKind::read:
		io_uring_prep_read(sqe, static_cast<int>(b.slot.value), op.read.buf, op.read.len, op.read.offset);
		break;
	case FlowOpKind::write:
		io_uring_prep_write(sqe, static_cast<int>(b.slot.value), op.write.buf, op.write.len, op.write.offset);
		break;
	case FlowOpKind::close_direct: break;
	}
}
[[nodiscard]] inline std::uint32_t byte_count_of(
	PendingOp const &op) noexcept {
	switch (op.kind) {
	case FlowOpKind::read : return op.read.len;
	case FlowOpKind::write: return op.write.len;
	default               : return 0;
	}
}

} // namespace conflux::uring::flow
// ── Exported: DirectFileFlow (builder handle) ─────────────────────────────────

export namespace conflux::uring::flow {

class DirectFileFlow {
	DirectFileBuilder *b_;

public:
	explicit DirectFileFlow(
		DirectFileBuilder *b) noexcept
		: b_{b} {}
	DirectFileFlow &then_read(
		void *buf,
		std::size_t len,
		std::uint64_t offset) noexcept {
		return append_read(LinkVariant::then_, buf, len, offset);
	}
	DirectFileFlow &hard_read(
		void *buf,
		std::size_t len,
		std::uint64_t offset) noexcept {
		return append_read(LinkVariant::hard_, buf, len, offset);
	}
	DirectFileFlow &then_write(
		void const *buf,
		std::size_t len,
		std::uint64_t offset) noexcept {
		return append_write(LinkVariant::then_, buf, len, offset);
	}
	DirectFileFlow &hard_write(
		void const *buf,
		std::size_t len,
		std::uint64_t offset) noexcept {
		return append_write(LinkVariant::hard_, buf, len, offset);
	}
	void close_if_opened() noexcept {
		if (b_ != nullptr && b_->err == 0) {
			b_->close_requested = true;
		}
	}
	[[nodiscard]] bool valid() const noexcept { return b_ != nullptr && b_->err == 0; }
	[[nodiscard]] int last_error() const noexcept { return b_ != nullptr ? b_->err : -EINVAL; }
	[[nodiscard]] explicit operator bool() const noexcept { return valid(); }

private:
	[[nodiscard]] bool check_append(
		LinkVariant var,
		std::size_t len) noexcept {
		if (b_ == nullptr || b_->err != 0) {
			return false;
		}
		if (b_->op_count == 1 && var == LinkVariant::hard_) {
			b_->err = -EINVAL;
			return false;
		}
		if (b_->op_count >= max_initial_ops) {
			b_->err = -ENOBUFS;
			return false;
		}
		if (len > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
			b_->err = -EOVERFLOW;
			return false;
		}
		return true;
	}
	DirectFileFlow &append_read(
		LinkVariant var,
		void *buf,
		std::size_t len,
		std::uint64_t off) noexcept {
		if (!check_append(var, len)) {
			return *this;
		}
		auto &op = b_->ops[b_->op_count++];
		op.kind = FlowOpKind::read;
		op.variant = var;
		op.read = PendingRead{buf, static_cast<std::uint32_t>(len), off};
		return *this;
	}
	DirectFileFlow &append_write(
		LinkVariant var,
		void const *buf,
		std::size_t len,
		std::uint64_t off) noexcept {
		if (!check_append(var, len)) {
			return *this;
		}
		auto &op = b_->ops[b_->op_count++];
		op.kind = FlowOpKind::write;
		op.variant = var;
		op.write = PendingWrite{buf, static_cast<std::uint32_t>(len), off};
		return *this;
	}
};

} // namespace conflux::uring::flow
// ── FlowRuntime forward declaration ──────────────────────────────────────────

export namespace conflux::uring::flow {

class FlowRuntime;

} // namespace conflux::uring::flow
// ── Exported: FlowBuilder ───────────────────────────────────────────────────────

export namespace conflux::uring::flow {

class FlowBuilder {
	Ring &ring_;
	FlowRuntime &rt_;

public:
	FlowBuilder(
		Ring &ring,
		FlowRuntime &rt) noexcept
		: ring_{ring}
		, rt_{rt} {}
	[[nodiscard]] DirectFileFlow
	open_direct(DirectSlot slot, int dfd, BorrowedPath path, int open_flags, mode_t mode = 0) noexcept;
	[[nodiscard]] DirectFileFlow
	open_direct_owned(DirectSlot slot, int dfd, OwnedInlinePath path, int open_flags, mode_t mode = 0) noexcept;
	[[nodiscard]] DirectFileFlow
	open_direct_owned(DirectSlot slot, int dfd, std::string_view path, int open_flags, mode_t mode = 0) noexcept;
	template<class Fn>
	void with_direct_file(
		DirectSlot slot,
		int dfd,
		BorrowedPath path,
		int open_flags,
		mode_t mode,
		Fn &&build_ops) noexcept {
		auto f = open_direct(slot, dfd, path, open_flags, mode);
		std::forward<Fn>(build_ops)(f);
		f.close_if_opened();
	}
	template<class Fn>
	void with_direct_file_owned(
		DirectSlot slot,
		int dfd,
		OwnedInlinePath path,
		int open_flags,
		mode_t mode,
		Fn &&build_ops) noexcept {
		auto f = open_direct_owned(slot, dfd, path, open_flags, mode);
		std::forward<Fn>(build_ops)(f);
		f.close_if_opened();
	}
	[[nodiscard]] std::uint32_t submit() noexcept;

	[[nodiscard]] std::span<FlowRejection const> rejected_flows() const noexcept;
};

} // namespace conflux::uring::flow
// ── Exported: FlowRuntime ─────────────────────────────────────────────────────

export namespace conflux::uring::flow {

class FlowRuntime {
	friend class FlowBuilder;

	Ring &ring_;
	bool path_lifetime_stable_;
	FlowSlab slab_;
	FlowCb cb_;

	std::array<DirectFileBuilder, kMaxBatch> builders_{};
	std::array<FlowRejection, kMaxBatch> rejections_{};
	std::array<OwnedInlinePath, kMaxFlows> owned_paths_{};
	std::uint32_t builder_count_ = 0;
	std::uint32_t rejection_count_ = 0;
	std::uint32_t invalid_cqe_count_ = 0;
	struct DeferredClose {
		std::uint32_t flow_index;
		std::uint32_t generation;
	};
	std::array<DeferredClose, kMaxFlows> deferred_{};
	std::uint32_t deferred_count_ = 0;

public:
	template<class Cb>
	FlowRuntime(
		Ring &ring,
		conflux::uring::IoUringCaps const &caps,
		Cb &&cb) noexcept
		: ring_{ring}
		, path_lifetime_stable_{caps.path_lifetime_stable} {
		cb_.set(std::forward<Cb>(cb));
	}
	~FlowRuntime() noexcept {
		assert(
			deferred_count_ == 0
			&& "FlowRuntime destroyed with pending deferred closes; call abandon_deferred_closes() first");
	}
	[[nodiscard]] std::uint32_t invalid_cqe_count() const noexcept { return invalid_cqe_count_; }
	void on_cqe(
		io_uring_cqe *cqe) noexcept {
		// hack: user_data=0 sentinel for NOPs from the unreachable single-issuer fallback path
		if (cqe->user_data == 0) {
			return;
		}
		auto tag = decode_tag(cqe->user_data);
		auto *st =
			slab_.try_get(static_cast<std::uint32_t>(tag.flow_index), static_cast<std::uint32_t>(tag.generation));
		if (st == nullptr) {
			handle_invalid(cqe);
			return;
		}

		if (!is_valid_flow_op_kind(static_cast<std::uint8_t>(tag.op_kind))) {
			handle_invalid(cqe);
			return;
		}
		auto kind = static_cast<FlowOpKind>(tag.op_kind);

		if (kind == FlowOpKind::close_direct) {
			bool const close_expected = st->close_in_chain || st->close_submitted;
			if (!close_expected) {
				handle_invalid(cqe);
				return;
			}
			if (static_cast<std::uint8_t>(tag.op_index) != st->initial_op_count) {
				handle_invalid(cqe);
				return;
			}
			if (st->close_seen) {
				handle_invalid(cqe);
				return;
			}
			st->close_seen = true;
			st->close_res = cqe->res;
			if (st->close_in_chain) {
				st->seen_cqes++;
			} else {
				finish_flow(*st);
				return;
			}
		} else {
			if (st->seen_cqes >= st->expected_cqes) {
				handle_invalid(cqe);
				return;
			}
			if (static_cast<std::uint8_t>(tag.op_index) >= st->initial_op_count) {
				handle_invalid(cqe);
				return;
			}
			auto &r = st->results[tag.op_index];
			if (r.kind != kind) {
				handle_invalid(cqe);
				return;
			}
			st->seen_cqes++;
			r.res = cqe->res;
			if (kind == FlowOpKind::open_direct) {
				st->open_seen = true;
				st->open_res = cqe->res;
				st->open_ok = direct_open_succeeded(cqe->res);
			}
		}

		if (st->seen_cqes == st->expected_cqes) {
			on_chain_complete(*st);
		}
	}
	// resume_deferred_close — spec's framework hook; idempotent if close already submitted.
	void resume_deferred_close(
		std::uint32_t flow_idx,
		std::uint32_t gen) noexcept {
		auto *st = slab_.try_get(flow_idx, gen);
		if (st == nullptr || !st->close_pending) {
			return;
		}
		auto sqe = ring_.get_sqe();
		if (!sqe) {
			return;
		}
		submit_close_sqe(sqe.raw(), *st);
		for (std::uint32_t i = 0; i < deferred_count_; ++i) {
			if (deferred_[i].flow_index == flow_idx && deferred_[i].generation == gen) {
				deferred_[i] = deferred_[--deferred_count_];
				break;
			}
		}
	}
	// drain_deferred_closes — retries every pending deferred close; iterate
	// end-to-front so resume_deferred_close's swap-with-last removal never shifts
	// an unvisited entry past the current cursor.
	void drain_deferred_closes() noexcept {
		std::uint32_t r = deferred_count_;
		while (r-- > 0) {
			resume_deferred_close(deferred_[r].flow_index, deferred_[r].generation);
		}
	}
	[[nodiscard]] std::uint32_t deferred_close_count() const noexcept { return deferred_count_; }
	[[nodiscard]] bool has_deferred_closes() const noexcept { return deferred_count_ != 0; }
	struct AbandonedDeferredClose {
		DirectSlot slot;
		std::uint32_t flow_index;
		std::uint32_t generation;
	};
	// Abandons all pending deferred closes without submitting SQEs and without
	// invoking the normal FlowResult callback. Only for shutdown/fatal-exit cleanup.
	// Caller owns DirectSlotPool and must poison returned slots via on_abandon.
	template<class Fn>
	std::uint32_t abandon_deferred_closes(
		Fn &&on_abandon) noexcept {
		static_assert(
			std::is_nothrow_invocable_v<Fn &, AbandonedDeferredClose>,
			"FlowRuntime::abandon_deferred_closes callback must be noexcept");
		std::uint32_t n = 0;
		for (std::uint32_t i = 0; i < deferred_count_; ++i) {
			auto const e = deferred_[i];
			auto *st = slab_.try_get(e.flow_index, e.generation);
			if (st == nullptr || !st->close_pending) {
				continue;
			}
			on_abandon(
				AbandonedDeferredClose{
					.slot = st->slot,
					.flow_index = e.flow_index,
					.generation = e.generation,
				});
			st->close_pending = false;
			++n;
			slab_.release(*st);
		}
		deferred_count_ = 0;
		return n;
	}
	[[nodiscard]] FlowBuilder flow() noexcept {
		assert(builder_count_ == 0);
		builder_count_ = 0;
		rejection_count_ = 0;
		return FlowBuilder{ring_, *this};
	}
// hack: test-only slab manipulation forwarded from FlowSlab
#ifdef CONFLUX_TESTING
	void test_hack_slab_generation(
		std::uint32_t idx,
		std::uint32_t gen) noexcept {
		slab_.test_hack_generation(idx, gen);
	}
	void test_hack_drain_slab_freelist() noexcept { slab_.test_hack_drain_freelist(); }
	[[nodiscard]] char const *test_owned_path_ptr(
		std::uint32_t flow_index) const noexcept {
		if (flow_index >= kMaxFlows) {
			return nullptr;
		}
		return owned_paths_[flow_index].c_str();
	}
#endif
private:
	void handle_invalid(
		io_uring_cqe *) noexcept {
		++invalid_cqe_count_;
	}
	void finish_flow(
		DirectFileFlowState &st) noexcept {
		FlowResult const r{
			.ops = std::span<OpResult const>{st.results.data(), st.initial_op_count},
			.close_needed = close_needed_pred(st),
			.close_in_chain = st.close_in_chain,
			.close_cqe_seen = st.close_seen,
			.close_raw_res = st.close_res,
		};
		if (cb_) {
			cb_(r);
		}
		slab_.release(st);
	}
	void on_chain_complete(
		DirectFileFlowState &st) noexcept {
		if (st.close_in_chain) {
			finish_flow(st);
			return;
		}
		if (!close_needed_pred(st)) {
			finish_flow(st);
			return;
		}
		if (st.close_submitted) {
			return;
		}

		auto sqe = ring_.get_sqe();
		if (!sqe) {
			st.close_pending = true;
			// invariant: deferred_count_ < kMaxFlows because deferred entries are 1:1 with slab cells
			assert(deferred_count_ < kMaxFlows);
			deferred_[deferred_count_++] = {st.flow_index, st.generation};
			return;
		}
		submit_close_sqe(sqe.raw(), st);
	}
	void submit_close_sqe(
		io_uring_sqe *sqe,
		DirectFileFlowState &st) noexcept {
		io_uring_prep_close_direct(sqe, st.slot.value);
		// io_uring_prep_close_direct calls io_uring_initialize_sqe which zeroes flags;
		// explicitly clear link bits anyway — defensive against future prep changes.
		sqe->flags &= static_cast<std::uint8_t>(~link_mask);
		sqe->user_data = encode_tag(st.flow_index, st.generation, st.initial_op_count, FlowOpKind::close_direct);
		st.close_submitted = true;
		st.close_pending = false;
	}
};

} // namespace conflux::uring::flow
// ── FlowBuilder method bodies ───────────────────────────────────────────────────

namespace conflux::uring::flow {

DirectFileFlow FlowBuilder::open_direct(
	DirectSlot slot,
	int dfd,
	BorrowedPath path,
	int open_flags,
	mode_t mode) noexcept {
	if (rt_.builder_count_ >= kMaxBatch) {
		if (rt_.rejection_count_ < kMaxBatch) {
			rt_.rejections_[rt_.rejection_count_++] = {rt_.builder_count_, -ENOBUFS};
		}
		return DirectFileFlow{nullptr};
	}
	auto &b = rt_.builders_[rt_.builder_count_++];
	b = {};
	b.slot = slot;
	auto &op = b.ops[b.op_count++];
	op.kind = FlowOpKind::open_direct;
	op.open = {slot, dfd, path.ptr, open_flags, mode};
	return DirectFileFlow{&b};
}
DirectFileFlow FlowBuilder::open_direct_owned(
	DirectSlot slot,
	int dfd,
	OwnedInlinePath path,
	int open_flags,
	mode_t mode) noexcept {
	if (rt_.builder_count_ >= kMaxBatch) {
		if (rt_.rejection_count_ < kMaxBatch) {
			rt_.rejections_[rt_.rejection_count_++] = {rt_.builder_count_, -ENOBUFS};
		}
		return DirectFileFlow{nullptr};
	}
	auto &b = rt_.builders_[rt_.builder_count_++];
	b = {};
	b.slot = slot;
	b.owns_path = true;
	b.owned_path_buf = path;
	auto &op = b.ops[b.op_count++];
	op.kind = FlowOpKind::open_direct;
	op.open = {slot, dfd, b.owned_path_buf.c_str(), open_flags, mode};
	return DirectFileFlow{&b};
}
DirectFileFlow FlowBuilder::open_direct_owned(
	DirectSlot slot,
	int dfd,
	std::string_view path,
	int open_flags,
	mode_t mode) noexcept {
	if (rt_.builder_count_ >= kMaxBatch) {
		if (rt_.rejection_count_ < kMaxBatch) {
			rt_.rejections_[rt_.rejection_count_++] = {rt_.builder_count_, -ENOBUFS};
		}
		return DirectFileFlow{nullptr};
	}
	auto p = OwnedInlinePath::from_sv(path);
	if (!p) {
		auto &b = rt_.builders_[rt_.builder_count_++];
		b = {};
		b.err = p.error();
		return DirectFileFlow{&b};
	}
	return open_direct_owned(slot, dfd, *p, open_flags, mode);
}
std::span<FlowRejection const> FlowBuilder::rejected_flows() const noexcept {
	return {rt_.rejections_.data(), rt_.rejection_count_};
}
std::uint32_t FlowBuilder::submit() noexcept {
	std::uint32_t accepted = 0;

	for (std::uint32_t local_idx = 0; local_idx < rt_.builder_count_; ++local_idx) {
		auto &b = rt_.builders_[local_idx];
		// Preserve earlier builder errors such as -EINVAL, -ENOBUFS,
		// -EOVERFLOW, -ENAMETOOLONG. Only apply path-lifetime rejection
		// to otherwise-valid borrowed-path builders.
		if (b.err == 0 && !rt_.path_lifetime_stable_ && !b.owns_path) {
			b.err = -EOPNOTSUPP;
		}
	}

	for (std::uint32_t local_idx = 0; local_idx < rt_.builder_count_; ++local_idx) {
		auto &b = rt_.builders_[local_idx];

		auto reject = [&](int err) {
			if (rt_.rejection_count_ < kMaxBatch) {
				rt_.rejections_[rt_.rejection_count_++] = {local_idx, err};
			}
		};

		if (b.err != 0) {
			reject(b.err);
			continue;
		}

		bool const mode_b = b.close_requested && mode_b_eligible(b);
		std::uint8_t const emitted = static_cast<std::uint8_t>(b.op_count + (mode_b ? 1u : 0u));

		auto *state_ptr = rt_.slab_.try_allocate();
		if (state_ptr == nullptr) {
			reject(-ENOSPC);
			continue;
		}

		if (ring_.sq_space_left() < emitted) {
			rt_.slab_.release(*state_ptr);
			reject(-EAGAIN);
			continue;
		}

		// Acquire all SQEs upfront so state is only initialized after full reservation.
		// Under single-issuer serialization the sq_space_left check above guarantees
		// these succeed.
		std::array<io_uring_sqe *, max_chain_cqes> sqes{};
		{
			bool sq_ok = true;
			for (std::uint8_t n = 0; n < emitted; ++n) {
				auto s = ring_.get_sqe();
				if (!s) {
					// hack: emit NOPs for already-acquired slots; cannot un-get SQEs.
					// This path is unreachable under correct single-issuer usage.
					assert(false);
					for (std::uint8_t j = 0; j < n; ++j) {
						io_uring_prep_nop(sqes[j]);
						sqes[j]->user_data = 0; // io_uring_prep_nop does not zero user_data; must be explicit
					}
					rt_.slab_.release(*state_ptr);
					reject(-EAGAIN);
					sq_ok = false;
					break;
				}
				sqes[n] = s.raw();
			}
			if (!sq_ok) {
				continue;
			}
		}

		auto &state = *state_ptr;
		state.initial_op_count = b.op_count;
		state.expected_cqes = emitted;
		state.seen_cqes = 0;
		state.slot = b.slot;
		state.open_seen = false;
		state.open_ok = false;
		state.open_res = 0;
		state.close_requested = b.close_requested;
		state.close_in_chain = mode_b;
		state.close_submitted = false;
		state.close_pending = false;
		state.close_seen = false;
		state.close_res = 0;

		char const *open_path_override = nullptr;
		if (b.owns_path) {
			rt_.owned_paths_[state.flow_index] = b.owned_path_buf;
			open_path_override = rt_.owned_paths_[state.flow_index].c_str();
		}

		for (std::uint8_t i = 0; i < emitted; ++i) {
			bool const is_close = mode_b && (i == b.op_count);
			FlowOpKind const kind = is_close ? FlowOpKind::close_direct : b.ops[i].kind;
			io_uring_sqe *sqe = sqes[i];

			prep_op(sqe, i, b, is_close, i == 0 ? open_path_override : nullptr);
			// prep_op zeroes sqe->flags; layer our flags on top

			if (uses_fixed_file_fd(kind)) {
				sqe->flags |= IOSQE_FIXED_FILE;
			}

			sqe->flags &= static_cast<std::uint8_t>(~link_mask);
			if (i + 1 < emitted) {
				sqe->flags |= static_cast<std::uint8_t>(link_flag_for_boundary(i, std::uint8_t(i + 1), b, mode_b));
			}

			sqe->user_data = encode_tag(state.flow_index, state.generation, i, kind);

			if (!is_close) {
				state.results[i] = OpResult{
					.res = 0,
					.requested = byte_count_of(b.ops[i]),
					.kind = kind,
				};
			}
		}

		++accepted;
	}

	rt_.builder_count_ = 0;
	return accepted;
}

} // namespace conflux::uring::flow
