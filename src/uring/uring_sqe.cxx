module;
#include <liburing.h>
#include <linux/futex.h>
#include <linux/openat2.h>
#include <sys/epoll.h>

export module conflux.uring.sqe;
import std;
import conflux.types;
import conflux.uring.fd;
export namespace conflux::uring {

// ── Flags<Tag> ────────────────────────────────────────────────────────────────
// All flag types use unsigned. Cast to/from signed only at liburing call sites.

template<typename Tag, typename U = unsigned>
struct Flags {
	U bits{};
	constexpr Flags() noexcept = default;
	constexpr explicit Flags(
		U v) noexcept
		: bits{v} {}
	[[nodiscard]] constexpr Flags friend operator |(
		Flags a,
		Flags b) noexcept {
		return Flags{static_cast<U>(a.bits | b.bits)};
	}
	[[nodiscard]] constexpr Flags friend operator &(
		Flags a,
		Flags b) noexcept {
		return Flags{static_cast<U>(a.bits & b.bits)};
	}
	[[nodiscard]] constexpr Flags friend operator ^(
		Flags a,
		Flags b) noexcept {
		return Flags{static_cast<U>(a.bits ^ b.bits)};
	}
	[[nodiscard]] constexpr Flags friend operator ~(
		Flags a) noexcept {
		return Flags{static_cast<U>(~a.bits)};
	}
	constexpr Flags &operator |=(
		Flags o) noexcept {
		bits = static_cast<U>(bits | o.bits);
		return *this;
	}
	constexpr Flags &operator &=(
		Flags o) noexcept {
		bits = static_cast<U>(bits & o.bits);
		return *this;
	}
	[[nodiscard]] constexpr bool operator ==(Flags const &) const noexcept = default;
	[[nodiscard]] constexpr explicit operator bool() const noexcept { return bits != 0; }
	[[nodiscard]] constexpr bool any(
		Flags mask) const noexcept {
		return (bits & mask.bits) != 0;
	}
	[[nodiscard]] constexpr bool all(
		Flags mask) const noexcept {
		return (bits & mask.bits) == mask.bits;
	}
	[[nodiscard]] constexpr U raw() const noexcept { return bits; }
};
// ── Flag type tags + aliases ──────────────────────────────────────────────────

struct SqeFlagsTag {};
struct SetupFlagsTag {};
struct CqeFlagsTag {};
struct IoPrioFlagsTag {};
struct CancelFlagsTag {};
struct TimeoutFlagsTag {};
struct FsyncFlagsTag {};
struct MsgFlagsTag {};
struct UringCmdFlagsTag {};
struct PollFlagsTag {};
struct PollAddFlagsTag {};
struct SpliceFlagsTag {};
struct MsgRingFlagsTag {};
struct InstallFdFlagsTag {};
struct NopFlagsTag {};
using SqeFlags = Flags<SqeFlagsTag, std::uint8_t>;
using SetupFlags = Flags<SetupFlagsTag>;
using CqeFlags = Flags<CqeFlagsTag>;
using IoPrioFlags = Flags<IoPrioFlagsTag, std::uint16_t>;
using CancelFlags = Flags<CancelFlagsTag>;
using TimeoutFlags = Flags<TimeoutFlagsTag>;
using FsyncFlags = Flags<FsyncFlagsTag>;
using MsgFlags = Flags<MsgFlagsTag>;
using UringCmdFlags = Flags<UringCmdFlagsTag>;
using PollFlags = Flags<PollFlagsTag>;
using PollAddFlags = Flags<PollAddFlagsTag>;
using SpliceFlags = Flags<SpliceFlagsTag>;
using MsgRingFlags = Flags<MsgRingFlagsTag>;
using InstallFdFlags = Flags<InstallFdFlagsTag>;
using NopFlags = Flags<NopFlagsTag>;
struct SqeFd {
	int v{-1};
};
// ── SQE value types ─────────────────────────────────────────────────────────

struct BufGroupId {
	std::uint16_t v{};
};
struct BufId {
	std::uint16_t v{};
};
struct UserData {
	std::uint64_t v{};
};
struct FixedBufIdx {
	std::int32_t v{-1};
};
// ── SQE flag constants ────────────────────────────────────────────────────────

namespace sqe_flags {

inline constexpr SqeFlags fixed_file{static_cast<std::uint8_t>(IOSQE_FIXED_FILE)};
inline constexpr SqeFlags io_drain{static_cast<std::uint8_t>(IOSQE_IO_DRAIN)};
inline constexpr SqeFlags io_link{static_cast<std::uint8_t>(IOSQE_IO_LINK)};
inline constexpr SqeFlags io_hardlink{static_cast<std::uint8_t>(IOSQE_IO_HARDLINK)};
inline constexpr SqeFlags async_{static_cast<std::uint8_t>(IOSQE_ASYNC)};
inline constexpr SqeFlags buffer_select{static_cast<std::uint8_t>(IOSQE_BUFFER_SELECT)};
inline constexpr SqeFlags cqe_skip_success{static_cast<std::uint8_t>(IOSQE_CQE_SKIP_SUCCESS)};

} // namespace sqe_flags
// ── Setup flag constants ──────────────────────────────────────────────────────

namespace setup_flags {

inline constexpr SetupFlags iopoll{IORING_SETUP_IOPOLL};
inline constexpr SetupFlags sqpoll{IORING_SETUP_SQPOLL};
inline constexpr SetupFlags sq_aff{IORING_SETUP_SQ_AFF};
inline constexpr SetupFlags cqsize{IORING_SETUP_CQSIZE};
inline constexpr SetupFlags clamp{IORING_SETUP_CLAMP};
inline constexpr SetupFlags attach_wq{IORING_SETUP_ATTACH_WQ};
inline constexpr SetupFlags r_disabled{IORING_SETUP_R_DISABLED};
inline constexpr SetupFlags submit_all{IORING_SETUP_SUBMIT_ALL};
inline constexpr SetupFlags coop_taskrun{IORING_SETUP_COOP_TASKRUN};
inline constexpr SetupFlags taskrun_flag{IORING_SETUP_TASKRUN_FLAG};
inline constexpr SetupFlags sqe128{IORING_SETUP_SQE128};
inline constexpr SetupFlags cqe32{IORING_SETUP_CQE32};
inline constexpr SetupFlags single_issuer{IORING_SETUP_SINGLE_ISSUER};
inline constexpr SetupFlags defer_taskrun{IORING_SETUP_DEFER_TASKRUN};
inline constexpr SetupFlags no_mmap{IORING_SETUP_NO_MMAP};
inline constexpr SetupFlags registered_fd_only{IORING_SETUP_REGISTERED_FD_ONLY};
inline constexpr SetupFlags no_sqarray{IORING_SETUP_NO_SQARRAY};
inline constexpr SetupFlags hybrid_iopoll{IORING_SETUP_HYBRID_IOPOLL};
inline constexpr SetupFlags cqe_mixed{IORING_SETUP_CQE_MIXED};
inline constexpr SetupFlags sqe_mixed{IORING_SETUP_SQE_MIXED};
inline constexpr SetupFlags sq_rewind{IORING_SETUP_SQ_REWIND};

} // namespace setup_flags

namespace setup_flag_fallback {

inline constexpr SetupFlags strip_order[] = {
	setup_flags::cqe_mixed,
	setup_flags::no_sqarray,
	setup_flags::submit_all,
	setup_flags::taskrun_flag,
	setup_flags::defer_taskrun,
	setup_flags::single_issuer,
};

} // namespace setup_flag_fallback

[[nodiscard]] std::optional<SetupFlags> next_setup_flag_to_strip(
	SetupFlags flags) {
	for (SetupFlags const f: setup_flag_fallback::strip_order) {
		if (flags.any(f)) {
			return f;
		}
	}
	return std::nullopt;
}

namespace setup_flag_detail {

template<typename Fn>
std::string setup_flag_list(
	Fn &&fn) {
	std::string s;
	auto app = [&](char const *name) {
		if (!s.empty()) {
			s += ',';
		}
		s += name;
	};
	fn(app);
	return s.empty() ? "none" : s;
}

} // namespace setup_flag_detail

[[nodiscard]] std::string setup_flags_str(
	SetupFlags flags) {
	return setup_flag_detail::setup_flag_list([&](auto app) {
		if (flags.any(setup_flags::single_issuer)) {
			app("SINGLE_ISSUER");
		}
		if (flags.any(setup_flags::defer_taskrun)) {
			app("DEFER_TASKRUN");
		}
		if (flags.any(setup_flags::sqpoll)) {
			app("SQPOLL");
		}
		if (flags.any(setup_flags::sq_aff)) {
			app("SQ_AFF");
		}
		if (flags.any(setup_flags::iopoll)) {
			app("IOPOLL");
		}
		if (flags.any(setup_flags::cqsize)) {
			app("CQSIZE");
		}
		if (flags.any(setup_flags::clamp)) {
			app("CLAMP");
		}
		if (flags.any(setup_flags::coop_taskrun)) {
			app("COOP_TASKRUN");
		}
		if (flags.any(setup_flags::taskrun_flag)) {
			app("TASKRUN_FLAG");
		}
		if (flags.any(setup_flags::submit_all)) {
			app("SUBMIT_ALL");
		}
		if (flags.any(setup_flags::attach_wq)) {
			app("ATTACH_WQ");
		}
		if (flags.any(setup_flags::r_disabled)) {
			app("R_DISABLED");
		}
		if (flags.any(setup_flags::sqe128)) {
			app("SQE128");
		}
		if (flags.any(setup_flags::cqe32)) {
			app("CQE32");
		}
		if (flags.any(setup_flags::no_mmap)) {
			app("NO_MMAP");
		}
		if (flags.any(setup_flags::registered_fd_only)) {
			app("REGISTERED_FD_ONLY");
		}
		if (flags.any(setup_flags::no_sqarray)) {
			app("NO_SQARRAY");
		}
		if (flags.any(setup_flags::hybrid_iopoll)) {
			app("HYBRID_IOPOLL");
		}
		if (flags.any(setup_flags::cqe_mixed)) {
			app("CQE_MIXED");
		}
		if (flags.any(setup_flags::sqe_mixed)) {
			app("SQE_MIXED");
		}
		if (flags.any(setup_flags::sq_rewind)) {
			app("SQ_REWIND");
		}
	});
}
// ── CQE flag constants ────────────────────────────────────────────────────────

namespace cqe_flags {

inline constexpr CqeFlags buffer{IORING_CQE_F_BUFFER};
inline constexpr CqeFlags more{IORING_CQE_F_MORE};
inline constexpr CqeFlags sock_nonempty{IORING_CQE_F_SOCK_NONEMPTY};
inline constexpr CqeFlags notif{IORING_CQE_F_NOTIF};
inline constexpr CqeFlags buf_more{IORING_CQE_F_BUF_MORE};
inline constexpr CqeFlags skip{IORING_CQE_F_SKIP};
inline constexpr CqeFlags f32{IORING_CQE_F_32};
[[nodiscard]] constexpr BufId buf_id(
	CqeFlags f) noexcept {
	return BufId{static_cast<std::uint16_t>(f.raw() >> IORING_CQE_BUFFER_SHIFT)};
}
[[nodiscard]] constexpr CqeFlags selected_buffer(
	BufId id,
	bool has_more_buffer = false) noexcept {
	auto flags = CqeFlags{IORING_CQE_F_BUFFER | (static_cast<std::uint32_t>(id.v) << IORING_CQE_BUFFER_SHIFT)};
	if (has_more_buffer) {
		flags |= buf_more;
	}
	return flags;
}

} // namespace cqe_flags
// ── IoPrio flag constants ─────────────────────────────────────────────────────
// ioprio field is shared by recv/send and accept ops; values don't overlap
// when used in their respective preps.

namespace ioprio_flags {

inline constexpr IoPrioFlags recvsend_poll_first{static_cast<std::uint16_t>(IORING_RECVSEND_POLL_FIRST)};
inline constexpr IoPrioFlags recv_multishot{static_cast<std::uint16_t>(IORING_RECV_MULTISHOT)};
inline constexpr IoPrioFlags recvsend_fixed_buf{static_cast<std::uint16_t>(IORING_RECVSEND_FIXED_BUF)};
inline constexpr IoPrioFlags send_zc_report_usage{static_cast<std::uint16_t>(IORING_SEND_ZC_REPORT_USAGE)};
inline constexpr IoPrioFlags recvsend_bundle{static_cast<std::uint16_t>(IORING_RECVSEND_BUNDLE)};
inline constexpr IoPrioFlags send_vectorized{static_cast<std::uint16_t>(IORING_SEND_VECTORIZED)};
inline constexpr IoPrioFlags accept_multishot{static_cast<std::uint16_t>(IORING_ACCEPT_MULTISHOT)};
inline constexpr IoPrioFlags accept_dontwait{static_cast<std::uint16_t>(IORING_ACCEPT_DONTWAIT)};
inline constexpr IoPrioFlags accept_poll_first{static_cast<std::uint16_t>(IORING_ACCEPT_POLL_FIRST)};

} // namespace ioprio_flags
// ── Cancel flag constants ─────────────────────────────────────────────────────

namespace cancel_flags {

inline constexpr CancelFlags all{IORING_ASYNC_CANCEL_ALL};
inline constexpr CancelFlags fd{IORING_ASYNC_CANCEL_FD};
inline constexpr CancelFlags any{IORING_ASYNC_CANCEL_ANY};
inline constexpr CancelFlags fd_fixed{IORING_ASYNC_CANCEL_FD_FIXED};
inline constexpr CancelFlags userdata{IORING_ASYNC_CANCEL_USERDATA};
inline constexpr CancelFlags op{IORING_ASYNC_CANCEL_OP};

} // namespace cancel_flags
[[nodiscard]] constexpr CancelFlags cancel_fd_flags(
	OsFd) noexcept {
	return CancelFlags{};
}
[[nodiscard]] constexpr CancelFlags cancel_fd_flags(
	DirectFd fd) noexcept {
	return fd.valid() ? cancel_flags::fd_fixed : CancelFlags{};
}
// ── Timeout flag constants ────────────────────────────────────────────────────

namespace timeout_flags {

inline constexpr TimeoutFlags abs{IORING_TIMEOUT_ABS};
inline constexpr TimeoutFlags update{IORING_TIMEOUT_UPDATE};
inline constexpr TimeoutFlags boottime{IORING_TIMEOUT_BOOTTIME};
inline constexpr TimeoutFlags realtime{IORING_TIMEOUT_REALTIME};
inline constexpr TimeoutFlags link_update{IORING_LINK_TIMEOUT_UPDATE};
inline constexpr TimeoutFlags etime_success{IORING_TIMEOUT_ETIME_SUCCESS};
inline constexpr TimeoutFlags multishot{IORING_TIMEOUT_MULTISHOT};

} // namespace timeout_flags
// ── Fsync flag constants ──────────────────────────────────────────────────────

namespace fsync_flags {

inline constexpr FsyncFlags datasync{IORING_FSYNC_DATASYNC};

} // namespace fsync_flags
// ── Poll flag constants ───────────────────────────────────────────────────────

namespace poll_add_flags {

inline constexpr PollAddFlags add_multi{IORING_POLL_ADD_MULTI};
inline constexpr PollAddFlags update_events{IORING_POLL_UPDATE_EVENTS};
inline constexpr PollAddFlags update_user_data{IORING_POLL_UPDATE_USER_DATA};
inline constexpr PollAddFlags add_level{IORING_POLL_ADD_LEVEL};

} // namespace poll_add_flags
// ── Splice flag constants ─────────────────────────────────────────────────────

namespace splice_flags {

inline constexpr SpliceFlags fd_in_fixed{SPLICE_F_FD_IN_FIXED};

} // namespace splice_flags
// ── Msg ring flag constants ───────────────────────────────────────────────────

namespace msg_ring_flags {

inline constexpr MsgRingFlags cqe_skip{IORING_MSG_RING_CQE_SKIP};
inline constexpr MsgRingFlags flags_pass{IORING_MSG_RING_FLAGS_PASS};

} // namespace msg_ring_flags
// ── Install-fd flag constants ─────────────────────────────────────────────────

namespace install_fd_flags {

inline constexpr InstallFdFlags no_cloexec{IORING_FIXED_FD_NO_CLOEXEC};

} // namespace install_fd_flags
// ── Nop flag constants ────────────────────────────────────────────────────────

namespace nop_flags {

inline constexpr NopFlags inject_result{IORING_NOP_INJECT_RESULT};

} // namespace nop_flags
// ── Uring cmd flag constants ──────────────────────────────────────────────────

namespace uring_cmd_flags {

inline constexpr UringCmdFlags fixed{IORING_URING_CMD_FIXED};

} // namespace uring_cmd_flags
namespace uring_cmd_op {

inline constexpr int setsockopt{SOCKET_URING_OP_SETSOCKOPT};

} // namespace uring_cmd_op
// ── Cqe ──────────────────────────────────────────────────────────────────────

struct Cqe {
	std::int32_t res{};
	CqeFlags flags;
	UserData user_data;
	[[nodiscard]] bool has_more() const noexcept { return flags.any(cqe_flags::more); }
	[[nodiscard]] bool has_buffer() const noexcept { return flags.any(cqe_flags::buffer); }
	[[nodiscard]] bool sock_nonempty() const noexcept { return flags.any(cqe_flags::sock_nonempty); }
	[[nodiscard]] BufId buffer_id() const noexcept { return cqe_flags::buf_id(flags); }
};
[[nodiscard]] inline Cqe to_cqe(
	io_uring_cqe const &c) noexcept {
	return {.res = c.res, .flags = CqeFlags{c.flags}, .user_data = UserData{c.user_data}};
}
// ── Sqe ──────────────────────────────────────────────────────────────────────
// Non-owning wrapper. Preps mutually exclusive — call exactly one per SQE.
// Setters chain. Check valid() before use; null sqe → UB in preps.

class Sqe {
	io_uring_sqe *p_;

public:
	Sqe() noexcept = delete;
	explicit Sqe(
		io_uring_sqe *p) noexcept
		: p_{p} {}
	[[nodiscard]] bool valid() const noexcept { return p_ != nullptr; }
	[[nodiscard]] explicit operator bool() const noexcept { return p_ != nullptr; }
	[[nodiscard]] io_uring_sqe *raw() const noexcept { return p_; }
	// ── SQE metadata setters ────────────────────────────────────────────────

	inline Sqe &set_flags(
		SqeFlags f) noexcept {
		io_uring_sqe_set_flags(p_, f.raw());
		return *this;
	}
	inline Sqe &add_flags(
		SqeFlags f) noexcept {
		p_->flags = static_cast<decltype(p_->flags)>(p_->flags | f.raw());
		return *this;
	}
	inline Sqe &clear_flags(
		SqeFlags f) noexcept {
		p_->flags = static_cast<decltype(p_->flags)>(p_->flags & ~f.raw());
		return *this;
	}
	inline Sqe &fixed_file(
		bool enabled) noexcept {
		return enabled ? add_flags(sqe_flags::fixed_file) : clear_flags(sqe_flags::fixed_file);
	}
	[[nodiscard]] inline SqeFd ring_fd(
		auto const &fd) noexcept {
		fixed_file(fd.is_direct() && fd.valid());
		return SqeFd{static_cast<int>(fd.fd())};
	}
	[[nodiscard]] inline DirectSlot direct_slot(
		auto const &fd) noexcept {
		fixed_file(fd.is_direct() && fd.valid());
		return DirectSlot{fd.fd()};
	}
	inline Sqe &prep_close(
		RingFd auto const &fd) noexcept {
		if constexpr (DirectFdLike<decltype(fd)>) {
			prep_close_direct(direct_slot(fd));
		} else {
			prep_close(ring_fd(fd));
		}
		return fixed_file(false);
	}
	inline Sqe &prep_cancel_fd(
		RingFd auto const &fd) noexcept {
		prep_cancel_fd(ring_fd(fd), cancel_fd_flags(fd));
		return fixed_file(false);
	}
	inline Sqe &prep_read(
		RingFd auto const &fd,
		void *buf,
		std::size_t len,
		std::uint64_t off) noexcept {
		return prep_read(ring_fd(fd), buf, static_cast<std::uint32_t>(len), off)
			.fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_read_multishot(
		RingFd auto const &fd,
		std::uint32_t len,
		std::uint64_t off,
		BufGroupId grp) noexcept {
		return prep_read_multishot(ring_fd(fd), len, off, grp).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_readv(
		RingFd auto const &fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off) noexcept {
		return prep_readv(ring_fd(fd), iov, nr, off).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_readv2(
		RingFd auto const &fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off,
		int flags) noexcept {
		return prep_readv2(ring_fd(fd), iov, nr, off, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_read_fixed(
		RingFd auto const &fd,
		void *buf,
		std::size_t len,
		std::uint64_t off,
		FixedBufIdx idx) noexcept {
		return prep_read_fixed(ring_fd(fd), buf, static_cast<std::uint32_t>(len), off, idx)
			.fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_readv_fixed(
		RingFd auto const &fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off,
		int rw_flags,
		FixedBufIdx idx) noexcept {
		return prep_readv_fixed(ring_fd(fd), iov, nr, off, rw_flags, idx).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_write(
		RingFd auto const &fd,
		void const *buf,
		std::size_t len,
		std::uint64_t off) noexcept {
		return prep_write(ring_fd(fd), buf, static_cast<std::uint32_t>(len), off)
			.fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_writev(
		RingFd auto const &fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off) noexcept {
		return prep_writev(ring_fd(fd), iov, nr, off).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_writev2(
		RingFd auto const &fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off,
		int flags) noexcept {
		return prep_writev2(ring_fd(fd), iov, nr, off, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_write_fixed(
		RingFd auto const &fd,
		void const *buf,
		std::size_t len,
		std::uint64_t off,
		FixedBufIdx idx) noexcept {
		return prep_write_fixed(ring_fd(fd), buf, static_cast<std::uint32_t>(len), off, idx)
			.fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_writev_fixed(
		RingFd auto const &fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off,
		int rw_flags,
		FixedBufIdx idx) noexcept {
		return prep_writev_fixed(ring_fd(fd), iov, nr, off, rw_flags, idx).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_send(
		RingFd auto const &fd,
		void const *buf,
		std::size_t len,
		MsgFlags flags) noexcept {
		return prep_send(ring_fd(fd), buf, len, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_send_bundle(
		RingFd auto const &fd,
		std::size_t len,
		MsgFlags flags) noexcept {
		return prep_send_bundle(ring_fd(fd), len, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_sendto(
		RingFd auto const &fd,
		void const *buf,
		std::size_t len,
		MsgFlags flags,
		sockaddr const *addr,
		socklen_t addrlen) noexcept {
		return prep_sendto(ring_fd(fd), buf, len, flags, addr, addrlen).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_sendmsg(
		RingFd auto const &fd,
		msghdr const *msg,
		MsgFlags flags) noexcept {
		return prep_sendmsg(ring_fd(fd), msg, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_send_zc(
		RingFd auto const &fd,
		void const *buf,
		std::size_t len,
		MsgFlags flags,
		unsigned zc_flags) noexcept {
		return prep_send_zc(ring_fd(fd), buf, len, flags, zc_flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_send_zc_fixed(
		RingFd auto const &fd,
		void const *buf,
		std::size_t len,
		MsgFlags flags,
		unsigned zc_flags,
		FixedBufIdx idx) noexcept {
		return prep_send_zc_fixed(ring_fd(fd), buf, len, flags, zc_flags, idx).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_sendmsg_zc(
		RingFd auto const &fd,
		msghdr const *msg,
		MsgFlags flags) noexcept {
		return prep_sendmsg_zc(ring_fd(fd), msg, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_sendmsg_zc_fixed(
		RingFd auto const &fd,
		msghdr const *msg,
		MsgFlags flags,
		FixedBufIdx idx) noexcept {
		return prep_sendmsg_zc_fixed(ring_fd(fd), msg, flags, idx).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_recv(
		RingFd auto const &fd,
		void *buf,
		std::size_t len,
		MsgFlags flags) noexcept {
		return prep_recv(ring_fd(fd), buf, len, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_recv_multishot(
		RingFd auto const &fd,
		void *buf,
		std::size_t len,
		MsgFlags flags) noexcept {
		return prep_recv_multishot(ring_fd(fd), buf, len, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_recvmsg(
		RingFd auto const &fd,
		msghdr *msg,
		MsgFlags flags) noexcept {
		return prep_recvmsg(ring_fd(fd), msg, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_recvmsg_multishot(
		RingFd auto const &fd,
		msghdr *msg,
		MsgFlags flags) noexcept {
		return prep_recvmsg_multishot(ring_fd(fd), msg, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_accept(
		RingFd auto const &fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags) noexcept {
		return prep_accept(ring_fd(fd), addr, addrlen, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_accept_direct(
		RingFd auto const &fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags,
		DirectSlot slot) noexcept {
		return prep_accept_direct(ring_fd(fd), addr, addrlen, flags, slot).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_multishot_accept(
		RingFd auto const &fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags) noexcept {
		return prep_multishot_accept(ring_fd(fd), addr, addrlen, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_multishot_accept_direct(
		RingFd auto const &fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags) noexcept {
		return prep_multishot_accept_direct(ring_fd(fd), addr, addrlen, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_connect(
		RingFd auto const &fd,
		sockaddr const *addr,
		socklen_t addrlen) noexcept {
		return prep_connect(ring_fd(fd), addr, addrlen).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_bind(
		RingFd auto const &fd,
		sockaddr *addr,
		socklen_t addrlen) noexcept {
		return prep_bind(ring_fd(fd), addr, addrlen).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_listen(
		RingFd auto const &fd,
		int backlog) noexcept {
		return prep_listen(ring_fd(fd), backlog).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_shutdown(
		RingFd auto const &fd,
		int how) noexcept {
		return prep_shutdown(ring_fd(fd), how).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_statx(
		RingFd auto const &fd,
		char const *path,
		int flags,
		unsigned mask,
		struct statx *stx) noexcept {
		return prep_statx(ring_fd(fd), path, flags, mask, stx).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_fsync(
		RingFd auto const &fd,
		FsyncFlags flags) noexcept {
		return prep_fsync(ring_fd(fd), flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_fallocate(
		RingFd auto const &fd,
		std::uint32_t mode,
		std::uint64_t offset,
		std::uint64_t len) noexcept {
		return prep_fallocate(ring_fd(fd), mode, offset, len).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_fadvise(
		RingFd auto const &fd,
		std::uint64_t offset,
		std::uint32_t len,
		std::uint32_t advice) noexcept {
		return prep_fadvise(ring_fd(fd), offset, len, advice).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_fadvise64(
		RingFd auto const &fd,
		std::uint64_t offset,
		std::uint64_t len,
		std::uint32_t advice) noexcept {
		return prep_fadvise64(ring_fd(fd), offset, len, advice).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_sync_file_range(
		RingFd auto const &fd,
		std::uint32_t len,
		std::uint64_t offset,
		int flags) noexcept {
		return prep_sync_file_range(ring_fd(fd), len, offset, flags).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_ftruncate(
		RingFd auto const &fd,
		std::int64_t len) noexcept {
		return prep_ftruncate(ring_fd(fd), len).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_fsetxattr(
		RingFd auto const &fd,
		char const *name,
		char const *val,
		int flags,
		std::uint32_t len) noexcept {
		return prep_fsetxattr(ring_fd(fd), name, val, flags, len).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_fgetxattr(
		RingFd auto const &fd,
		char const *name,
		char *val,
		std::uint32_t len) noexcept {
		return prep_fgetxattr(ring_fd(fd), name, val, len).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_poll_add(
		RingFd auto const &fd,
		PollFlags events) noexcept {
		return prep_poll_add(ring_fd(fd), events).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_poll_multishot(
		RingFd auto const &fd,
		PollFlags events) noexcept {
		return prep_poll_multishot(ring_fd(fd), events).fixed_file(fd.is_direct() && fd.valid());
	}
	inline Sqe &prep_fixed_fd_install(
		DirectFd fd,
		InstallFdFlags flags) noexcept {
		return prep_fixed_fd_install(direct_slot(fd), flags);
	}
	inline Sqe &prep_cmd_sock(
		int cmd_op,
		DirectFd fd,
		int level,
		int optname,
		void *optval,
		int optlen) noexcept {
		return prep_cmd_sock(cmd_op, ring_fd(fd), level, optname, optval, optlen);
	}
	inline Sqe &prep_cmd_sock_setsockopt(
		DirectFd fd,
		int level,
		int optname,
		void const *optval,
		int optlen) noexcept {
		return prep_cmd_sock(uring_cmd_op::setsockopt, fd, level, optname, const_cast<void *>(optval), optlen);
	}
	inline Sqe &sqe_flags(
		SqeFlags f) noexcept {
		return set_flags(f);
	}
	inline Sqe &user_data(
		UserData ud) noexcept {
		io_uring_sqe_set_data64(p_, ud.v);
		return *this;
	}
	inline Sqe &buf_group(
		BufGroupId g) noexcept {
		p_->buf_group = g.v;
		return *this;
	}
	inline Sqe &buf_index(
		FixedBufIdx idx) noexcept {
		p_->buf_index = static_cast<__u16>(idx.v);
		return *this;
	}
	inline Sqe &ioprio(
		IoPrioFlags f) noexcept {
		p_->ioprio = static_cast<__u16>(p_->ioprio | f.raw());
		return *this;
	}
	inline Sqe &personality(
		std::uint16_t p) noexcept {
		p_->personality = p;
		return *this;
	}
	// Set uring_cmd_flags for prep_uring_cmd (IORING_URING_CMD_FIXED etc.)
	inline Sqe &cmd_flags(
		UringCmdFlags f) noexcept {
		p_->uring_cmd_flags = f.raw();
		return *this;
	}
	// ── Read ────────────────────────────────────────────────────────────────
	// Offsets are std::uint64_t matching io_uring's __u64; use UINT64_MAX for "current pos"

	inline Sqe &prep_read(
		SqeFd fd,
		void *buf,
		std::uint32_t len,
		std::uint64_t off) noexcept {
		io_uring_prep_read(p_, fd.v, buf, len, static_cast<__u64>(off));
		return *this;
	}
	inline Sqe &read(
		DirectSlot slot,
		void *buf,
		std::uint32_t len,
		std::uint64_t off) noexcept {
		io_uring_prep_read(p_, static_cast<int>(slot.value), buf, len, static_cast<__u64>(off));
		return fixed_file(true);
	}
	inline Sqe &prep_read_multishot(
		SqeFd fd,
		std::uint32_t len,
		std::uint64_t off,
		BufGroupId grp) noexcept {
		io_uring_prep_read_multishot(p_, fd.v, len, static_cast<__u64>(off), grp.v);
		return *this;
	}
	inline Sqe &prep_readv(
		SqeFd fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off) noexcept {
		io_uring_prep_readv(p_, fd.v, iov, nr, static_cast<__u64>(off));
		return *this;
	}
	inline Sqe &prep_readv2(
		SqeFd fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off,
		int flags) noexcept {
		io_uring_prep_readv2(p_, fd.v, iov, nr, static_cast<__u64>(off), flags);
		return *this;
	}
	inline Sqe &prep_read_fixed(
		SqeFd fd,
		void *buf,
		std::uint32_t len,
		std::uint64_t off,
		FixedBufIdx idx) noexcept {
		io_uring_prep_read_fixed(p_, fd.v, buf, len, static_cast<__u64>(off), idx.v);
		return *this;
	}
	inline Sqe &prep_readv_fixed(
		SqeFd fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off,
		int rw_flags,
		FixedBufIdx idx) noexcept {
		io_uring_prep_readv_fixed(p_, fd.v, iov, nr, static_cast<__u64>(off), rw_flags, idx.v);
		return *this;
	}
	// ── Write ───────────────────────────────────────────────────────────────

	inline Sqe &prep_write(
		SqeFd fd,
		void const *buf,
		std::uint32_t len,
		std::uint64_t off) noexcept {
		io_uring_prep_write(p_, fd.v, buf, len, static_cast<__u64>(off));
		return *this;
	}
	inline Sqe &write(
		DirectSlot slot,
		void const *buf,
		std::uint32_t len,
		std::uint64_t off) noexcept {
		io_uring_prep_write(p_, static_cast<int>(slot.value), buf, len, static_cast<__u64>(off));
		return fixed_file(true);
	}
	inline Sqe &prep_writev(
		SqeFd fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off) noexcept {
		io_uring_prep_writev(p_, fd.v, iov, nr, static_cast<__u64>(off));
		return *this;
	}
	inline Sqe &prep_writev2(
		SqeFd fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off,
		int flags) noexcept {
		io_uring_prep_writev2(p_, fd.v, iov, nr, static_cast<__u64>(off), flags);
		return *this;
	}
	inline Sqe &prep_write_fixed(
		SqeFd fd,
		void const *buf,
		std::uint32_t len,
		std::uint64_t off,
		FixedBufIdx idx) noexcept {
		io_uring_prep_write_fixed(p_, fd.v, buf, len, static_cast<__u64>(off), idx.v);
		return *this;
	}
	inline Sqe &prep_writev_fixed(
		SqeFd fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off,
		int rw_flags,
		FixedBufIdx idx) noexcept {
		io_uring_prep_writev_fixed(p_, fd.v, iov, nr, static_cast<__u64>(off), rw_flags, idx.v);
		return *this;
	}
	// ── Send ────────────────────────────────────────────────────────────────
	// msg_flags from POSIX (MSG_*) stored unsigned; cast to int at call site

	inline Sqe &prep_send(
		SqeFd fd,
		void const *buf,
		std::size_t len,
		MsgFlags flags) noexcept {
		io_uring_prep_send(p_, fd.v, buf, len, static_cast<int>(flags.raw()));
		return *this;
	}
	inline Sqe &prep_send_bundle(
		SqeFd fd,
		std::size_t len,
		MsgFlags flags) noexcept {
		io_uring_prep_send_bundle(p_, fd.v, len, static_cast<int>(flags.raw()));
		return *this;
	}
	inline Sqe &prep_sendto(
		SqeFd fd,
		void const *buf,
		std::size_t len,
		MsgFlags flags,
		sockaddr const *addr,
		socklen_t addrlen) noexcept {
		io_uring_prep_sendto(p_, fd.v, buf, len, static_cast<int>(flags.raw()), addr, addrlen);
		return *this;
	}
	inline Sqe &set_send_addr(
		sockaddr const *addr,
		std::uint16_t addrlen) noexcept {
		io_uring_prep_send_set_addr(p_, addr, addrlen);
		return *this;
	}
	inline Sqe &prep_sendmsg(
		SqeFd fd,
		msghdr const *msg,
		MsgFlags flags) noexcept {
		io_uring_prep_sendmsg(p_, fd.v, msg, flags.raw());
		return *this;
	}
	inline Sqe &prep_send_zc(
		SqeFd fd,
		void const *buf,
		std::size_t len,
		MsgFlags flags,
		unsigned zc_flags) noexcept {
		io_uring_prep_send_zc(p_, fd.v, buf, len, static_cast<int>(flags.raw()), zc_flags);
		return *this;
	}
	inline Sqe &prep_send_zc_fixed(
		SqeFd fd,
		void const *buf,
		std::size_t len,
		MsgFlags flags,
		unsigned zc_flags,
		FixedBufIdx idx) noexcept {
		io_uring_prep_send_zc_fixed(
			p_,
			fd.v,
			buf,
			len,
			static_cast<int>(flags.raw()),
			zc_flags,
			static_cast<std::uint16_t>(idx.v));
		return *this;
	}
	inline Sqe &prep_sendmsg_zc(
		SqeFd fd,
		msghdr const *msg,
		MsgFlags flags) noexcept {
		io_uring_prep_sendmsg_zc(p_, fd.v, msg, flags.raw());
		return *this;
	}
	inline Sqe &prep_sendmsg_zc_fixed(
		SqeFd fd,
		msghdr const *msg,
		MsgFlags flags,
		FixedBufIdx idx) noexcept {
		io_uring_prep_sendmsg_zc_fixed(p_, fd.v, msg, flags.raw(), static_cast<unsigned>(idx.v));
		return *this;
	}
	// ── Recv ────────────────────────────────────────────────────────────────

	inline Sqe &prep_recv(
		SqeFd fd,
		void *buf,
		std::size_t len,
		MsgFlags flags) noexcept {
		io_uring_prep_recv(p_, fd.v, buf, len, static_cast<int>(flags.raw()));
		return *this;
	}
	inline Sqe &prep_recv_multishot(
		SqeFd fd,
		void *buf,
		std::size_t len,
		MsgFlags flags) noexcept {
		io_uring_prep_recv_multishot(p_, fd.v, buf, len, static_cast<int>(flags.raw()));
		return *this;
	}
	inline Sqe &prep_recvmsg(
		SqeFd fd,
		msghdr *msg,
		MsgFlags flags) noexcept {
		io_uring_prep_recvmsg(p_, fd.v, msg, flags.raw());
		return *this;
	}
	inline Sqe &prep_recvmsg_multishot(
		SqeFd fd,
		msghdr *msg,
		MsgFlags flags) noexcept {
		io_uring_prep_recvmsg_multishot(p_, fd.v, msg, flags.raw());
		return *this;
	}
	// ── Accept / connect ────────────────────────────────────────────────────

	inline Sqe &prep_accept(
		SqeFd fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags) noexcept {
		io_uring_prep_accept(p_, fd.v, addr, addrlen, flags);
		return *this;
	}
	inline Sqe &prep_accept_direct(
		SqeFd fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags,
		DirectSlot slot) noexcept {
		io_uring_prep_accept_direct(p_, fd.v, addr, addrlen, flags, slot.value);
		return *this;
	}
	inline Sqe &prep_multishot_accept(
		SqeFd fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags) noexcept {
		io_uring_prep_multishot_accept(p_, fd.v, addr, addrlen, flags);
		return *this;
	}
	inline Sqe &prep_multishot_accept_direct(
		SqeFd fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags) noexcept {
		io_uring_prep_multishot_accept_direct(p_, fd.v, addr, addrlen, flags);
		return *this;
	}
	inline Sqe &prep_connect(
		SqeFd fd,
		sockaddr const *addr,
		socklen_t addrlen) noexcept {
		io_uring_prep_connect(p_, fd.v, addr, addrlen);
		return *this;
	}
	inline Sqe &prep_bind(
		SqeFd fd,
		sockaddr *addr,
		socklen_t addrlen) noexcept {
		io_uring_prep_bind(p_, fd.v, addr, addrlen);
		return *this;
	}
	inline Sqe &prep_listen(
		SqeFd fd,
		int backlog) noexcept {
		io_uring_prep_listen(p_, fd.v, backlog);
		return *this;
	}
	inline Sqe &prep_shutdown(
		SqeFd fd,
		int how) noexcept {
		io_uring_prep_shutdown(p_, fd.v, how);
		return *this;
	}
	// ── Socket creation ─────────────────────────────────────────────────────

	inline Sqe &prep_socket(
		int domain,
		int type,
		int protocol,
		unsigned flags) noexcept {
		io_uring_prep_socket(p_, domain, type, protocol, flags);
		return *this;
	}
	inline Sqe &prep_socket_direct(
		int domain,
		int type,
		int protocol,
		DirectSlot slot,
		unsigned flags) noexcept {
		io_uring_prep_socket_direct(p_, domain, type, protocol, slot.value, flags);
		return *this;
	}
	inline Sqe &prep_socket_direct_alloc(
		int domain,
		int type,
		int protocol,
		unsigned flags) noexcept {
		io_uring_prep_socket_direct_alloc(p_, domain, type, protocol, flags);
		return *this;
	}
	// ── Open / close ────────────────────────────────────────────────────────

	inline Sqe &prep_openat(
		SqeFd dfd,
		char const *path,
		int flags,
		mode_t mode) noexcept {
		io_uring_prep_openat(p_, dfd.v, path, flags, mode);
		return *this;
	}
	inline Sqe &prep_openat_direct(
		SqeFd dfd,
		char const *path,
		int flags,
		mode_t mode,
		DirectSlot slot) noexcept {
		io_uring_prep_openat_direct(p_, dfd.v, path, flags, mode, slot.value);
		return *this;
	}
	inline Sqe &open_direct(
		SqeFd dfd,
		char const *path,
		int flags,
		mode_t mode,
		DirectSlot slot) noexcept {
		io_uring_prep_openat_direct(p_, dfd.v, path, flags, mode, slot.value);
		return *this;
	}
	inline Sqe &prep_openat2(
		SqeFd dfd,
		char const *path,
		open_how *how) noexcept {
		io_uring_prep_openat2(p_, dfd.v, path, how);
		return *this;
	}
	inline Sqe &prep_openat2_direct(
		SqeFd dfd,
		char const *path,
		open_how *how,
		DirectSlot slot) noexcept {
		io_uring_prep_openat2_direct(p_, dfd.v, path, how, slot.value);
		return *this;
	}
	inline Sqe &prep_close(
		SqeFd fd) noexcept {
		io_uring_prep_close(p_, fd.v);
		return *this;
	}
	inline Sqe &prep_close_direct(
		DirectSlot slot) noexcept {
		io_uring_prep_close_direct(p_, slot.value);
		return *this;
	}
	inline Sqe &close_direct(
		DirectSlot slot) noexcept {
		io_uring_prep_close_direct(p_, slot.value);
		return *this;
	}
	inline Sqe &prep_fixed_fd_install(
		DirectSlot slot,
		InstallFdFlags flags) noexcept {
		io_uring_prep_fixed_fd_install(p_, static_cast<int>(slot.value), flags.raw());
		return *this;
	}
	// ── File ops ────────────────────────────────────────────────────────────

	inline Sqe &prep_statx(
		SqeFd dfd,
		char const *path,
		int flags,
		unsigned mask,
		struct statx *stx) noexcept {
		io_uring_prep_statx(p_, dfd.v, path, flags, mask, stx);
		return *this;
	}
	inline Sqe &prep_fsync(
		SqeFd fd,
		FsyncFlags flags) noexcept {
		io_uring_prep_fsync(p_, fd.v, flags.raw());
		return *this;
	}
	inline Sqe &prep_fallocate(
		SqeFd fd,
		std::uint32_t mode,
		std::uint64_t offset,
		std::uint64_t len) noexcept {
		io_uring_prep_fallocate(p_, fd.v, static_cast<int>(mode), static_cast<__u64>(offset), static_cast<__u64>(len));
		return *this;
	}
	inline Sqe &prep_fadvise(
		SqeFd fd,
		std::uint64_t offset,
		std::uint32_t len,
		std::uint32_t advice) noexcept {
		io_uring_prep_fadvise(p_, fd.v, static_cast<__u64>(offset), len, static_cast<int>(advice));
		return *this;
	}
	inline Sqe &prep_fadvise64(
		SqeFd fd,
		std::uint64_t offset,
		std::uint64_t len,
		std::uint32_t advice) noexcept {
		io_uring_prep_fadvise64(
			p_,
			fd.v,
			static_cast<__u64>(offset),
			static_cast<off_t>(len),
			static_cast<int>(advice));
		return *this;
	}
	inline Sqe &prep_madvise(
		void *addr,
		std::uint32_t len,
		std::uint32_t advice) noexcept {
		io_uring_prep_madvise(p_, addr, len, static_cast<int>(advice));
		return *this;
	}
	inline Sqe &prep_madvise64(
		void *addr,
		std::uint64_t len,
		std::uint32_t advice) noexcept {
		io_uring_prep_madvise64(p_, addr, static_cast<off_t>(len), static_cast<int>(advice));
		return *this;
	}
	inline Sqe &prep_sync_file_range(
		SqeFd fd,
		std::uint32_t len,
		std::uint64_t offset,
		int flags) noexcept {
		io_uring_prep_sync_file_range(p_, fd.v, len, static_cast<__u64>(offset), flags);
		return *this;
	}
	inline Sqe &prep_ftruncate(
		SqeFd fd,
		std::int64_t len) noexcept {
		io_uring_prep_ftruncate(p_, fd.v, len);
		return *this;
	}
	inline Sqe &prep_renameat(
		SqeFd old_dfd,
		char const *old_path,
		SqeFd new_dfd,
		char const *new_path,
		unsigned flags) noexcept {
		io_uring_prep_renameat(p_, old_dfd.v, old_path, new_dfd.v, new_path, flags);
		return *this;
	}
	inline Sqe &prep_unlinkat(
		SqeFd dfd,
		char const *path,
		int flags) noexcept {
		io_uring_prep_unlinkat(p_, dfd.v, path, flags);
		return *this;
	}
	inline Sqe &prep_mkdirat(
		SqeFd dfd,
		char const *path,
		mode_t mode) noexcept {
		io_uring_prep_mkdirat(p_, dfd.v, path, mode);
		return *this;
	}
	inline Sqe &prep_mkdir(
		char const *path,
		mode_t mode) noexcept {
		io_uring_prep_mkdir(p_, path, mode);
		return *this;
	}
	inline Sqe &prep_symlinkat(
		char const *target,
		SqeFd new_dfd,
		char const *link_path) noexcept {
		io_uring_prep_symlinkat(p_, target, new_dfd.v, link_path);
		return *this;
	}
	inline Sqe &prep_linkat(
		SqeFd old_dfd,
		char const *old_path,
		SqeFd new_dfd,
		char const *new_path,
		int flags) noexcept {
		io_uring_prep_linkat(p_, old_dfd.v, old_path, new_dfd.v, new_path, flags);
		return *this;
	}
	inline Sqe &prep_fsetxattr(
		SqeFd fd,
		char const *name,
		char const *val,
		int flags,
		std::uint32_t len) noexcept {
		io_uring_prep_fsetxattr(p_, fd.v, name, val, flags, len);
		return *this;
	}
	inline Sqe &prep_setxattr(
		char const *name,
		char const *val,
		char const *path,
		int flags,
		std::uint32_t len) noexcept {
		io_uring_prep_setxattr(p_, name, val, path, flags, len);
		return *this;
	}
	inline Sqe &prep_fgetxattr(
		SqeFd fd,
		char const *name,
		char *val,
		std::uint32_t len) noexcept {
		io_uring_prep_fgetxattr(p_, fd.v, name, val, len);
		return *this;
	}
	// val is output buffer (char*, not const)
	inline Sqe &prep_getxattr(
		char const *name,
		char *val,
		char const *path,
		std::uint32_t len) noexcept {
		io_uring_prep_getxattr(p_, name, val, path, len);
		return *this;
	}
	// ── Control / timers / cancel ────────────────────────────────────────────

	inline Sqe &prep_nop() noexcept {
		io_uring_prep_nop(p_);
		return *this;
	}
	inline Sqe &prep_nop128() noexcept {
		io_uring_prep_nop128(p_);
		return *this;
	}
	inline Sqe &prep_timeout(
		__kernel_timespec *ts,
		std::uint32_t count,
		TimeoutFlags flags) noexcept {
		io_uring_prep_timeout(p_, ts, count, flags.raw());
		return *this;
	}
	inline Sqe &prep_timeout_remove(
		UserData ud,
		TimeoutFlags flags) noexcept {
		io_uring_prep_timeout_remove(p_, ud.v, flags.raw());
		return *this;
	}
	inline Sqe &prep_timeout_update(
		__kernel_timespec *ts,
		UserData ud,
		TimeoutFlags flags) noexcept {
		io_uring_prep_timeout_update(p_, ts, ud.v, flags.raw());
		return *this;
	}
	inline Sqe &prep_link_timeout(
		__kernel_timespec *ts,
		TimeoutFlags flags) noexcept {
		io_uring_prep_link_timeout(p_, ts, flags.raw());
		return *this;
	}
	// cancel_flags is unsigned; prep_cancel64 takes int — explicit cast
	inline Sqe &prep_cancel64(
		UserData ud,
		CancelFlags flags) noexcept {
		io_uring_prep_cancel64(p_, ud.v, static_cast<int>(flags.raw()));
		return *this;
	}
	inline Sqe &prep_cancel_fd(
		SqeFd fd,
		CancelFlags flags) noexcept {
		io_uring_prep_cancel_fd(p_, fd.v, flags.raw());
		return *this;
	}
	// ── Poll / epoll ─────────────────────────────────────────────────────────

	inline Sqe &prep_poll_add(
		SqeFd fd,
		PollFlags events) noexcept {
		io_uring_prep_poll_add(p_, fd.v, events.raw());
		return *this;
	}
	inline Sqe &prep_poll_multishot(
		SqeFd fd,
		PollFlags events) noexcept {
		io_uring_prep_poll_multishot(p_, fd.v, events.raw());
		return *this;
	}
	inline Sqe &prep_poll_remove(
		UserData ud) noexcept {
		io_uring_prep_poll_remove(p_, ud.v);
		return *this;
	}
	inline Sqe &prep_poll_update(
		UserData old_ud,
		UserData new_ud,
		PollFlags events,
		PollAddFlags flags) noexcept {
		io_uring_prep_poll_update(p_, old_ud.v, new_ud.v, events.raw(), flags.raw());
		return *this;
	}
	inline Sqe &prep_epoll_ctl(
		SqeFd epfd,
		SqeFd fd,
		int op,
		epoll_event const *ev) noexcept {
		io_uring_prep_epoll_ctl(p_, epfd.v, fd.v, op, ev);
		return *this;
	}
	inline Sqe &prep_epoll_wait(
		SqeFd epfd,
		epoll_event *events,
		std::uint32_t maxevents,
		std::uint32_t flags) noexcept {
		io_uring_prep_epoll_wait(p_, epfd.v, events, static_cast<int>(maxevents), flags);
		return *this;
	}
	// ── Splice / tee / pipe ──────────────────────────────────────────────────

	inline Sqe &prep_splice(
		SqeFd fd_in,
		std::int64_t off_in,
		SqeFd fd_out,
		std::int64_t off_out,
		std::uint32_t len,
		SpliceFlags flags) noexcept {
		io_uring_prep_splice(p_, fd_in.v, off_in, fd_out.v, off_out, len, flags.raw());
		return *this;
	}
	inline Sqe &prep_tee(
		SqeFd fd_in,
		SqeFd fd_out,
		std::uint32_t len,
		SpliceFlags flags) noexcept {
		io_uring_prep_tee(p_, fd_in.v, fd_out.v, len, flags.raw());
		return *this;
	}
	inline Sqe &prep_pipe(
		int *fds,
		int pipe_flags) noexcept {
		io_uring_prep_pipe(p_, fds, pipe_flags);
		return *this;
	}
	// slot = direct fd index for the write end; use IORING_FILE_INDEX_ALLOC for auto
	inline Sqe &prep_pipe_direct(
		int *fds,
		int pipe_flags,
		DirectSlot slot) noexcept {
		io_uring_prep_pipe_direct(p_, fds, pipe_flags, slot.value);
		return *this;
	}
	// ── Msg ring ─────────────────────────────────────────────────────────────

	inline Sqe &prep_msg_ring(
		SqeFd ring_fd,
		std::uint32_t len,
		UserData data,
		MsgRingFlags flags) noexcept {
		io_uring_prep_msg_ring(p_, ring_fd.v, len, data.v, flags.raw());
		return *this;
	}
	inline Sqe &prep_msg_ring_cqe_flags(
		SqeFd ring_fd,
		std::uint32_t len,
		UserData data,
		MsgRingFlags flags,
		std::uint32_t cqe_flags) noexcept {
		io_uring_prep_msg_ring_cqe_flags(p_, ring_fd.v, len, data.v, flags.raw(), cqe_flags);
		return *this;
	}
	inline Sqe &prep_msg_ring_fd(
		SqeFd ring_fd,
		SqeFd source_fd,
		SqeFd target_fd,
		UserData data,
		MsgRingFlags flags) noexcept {
		io_uring_prep_msg_ring_fd(p_, ring_fd.v, source_fd.v, target_fd.v, data.v, flags.raw());
		return *this;
	}
	inline Sqe &prep_msg_ring_fd_alloc(
		SqeFd ring_fd,
		SqeFd source_fd,
		UserData data,
		MsgRingFlags flags) noexcept {
		io_uring_prep_msg_ring_fd_alloc(p_, ring_fd.v, source_fd.v, data.v, flags.raw());
		return *this;
	}
	// ── Files update ─────────────────────────────────────────────────────────

	inline Sqe &prep_files_update(
		int *fds,
		std::uint32_t nr,
		std::int32_t offset) noexcept {
		io_uring_prep_files_update(p_, fds, nr, offset);
		return *this;
	}
	// ── Provided buffers (legacy non-ring API) ────────────────────────────────

	inline Sqe &prep_provide_buffers(
		void *addr,
		std::int32_t len,
		std::int32_t nr,
		BufGroupId bgid,
		std::int32_t bid) noexcept {
		io_uring_prep_provide_buffers(p_, addr, len, nr, bgid.v, bid);
		return *this;
	}
	inline Sqe &prep_remove_buffers(
		std::int32_t nr,
		BufGroupId bgid) noexcept {
		io_uring_prep_remove_buffers(p_, nr, bgid.v);
		return *this;
	}
	// ── Futex / waitid ───────────────────────────────────────────────────────

	inline Sqe &prep_waitid(
		idtype_t idtype,
		id_t id,
		siginfo_t *infop,
		int options,
		unsigned flags) noexcept {
		io_uring_prep_waitid(p_, idtype, id, infop, options, flags);
		return *this;
	}
	inline Sqe &prep_futex_wait(
		std::uint32_t *futex,
		std::uint64_t val,
		std::uint64_t mask,
		std::uint32_t futex_flags,
		unsigned flags) noexcept {
		io_uring_prep_futex_wait(p_, futex, val, mask, futex_flags, flags);
		return *this;
	}
	inline Sqe &prep_futex_wake(
		std::uint32_t *futex,
		std::uint64_t val,
		std::uint64_t mask,
		std::uint32_t futex_flags,
		unsigned flags) noexcept {
		io_uring_prep_futex_wake(p_, futex, val, mask, futex_flags, flags);
		return *this;
	}
	inline Sqe &prep_futex_waitv(
		futex_waitv const *waiters,
		std::uint32_t nr,
		unsigned flags) noexcept {
		io_uring_prep_futex_waitv(p_, waiters, nr, flags);
		return *this;
	}
	// ── Uring cmd / socket cmd ───────────────────────────────────────────────
	// Use cmd_flags() setter to apply UringCmdFlags after this prep

	inline Sqe &prep_uring_cmd(
		int cmd_op,
		SqeFd fd) noexcept {
		io_uring_prep_uring_cmd(p_, cmd_op, fd.v);
		return *this;
	}
	inline Sqe &prep_cmd_sock(
		int cmd_op,
		SqeFd fd,
		int level,
		int optname,
		void *optval,
		int optlen) noexcept {
		io_uring_prep_cmd_sock(p_, cmd_op, fd.v, level, optname, optval, optlen);
		return *this;
	}
};

} // namespace conflux::uring
