module;
#include <cassert>
#include <liburing.h>
#include <linux/futex.h>
#include <linux/openat2.h>
#include <sys/epoll.h>
#include <sys/resource.h>

export module conflux.uring;
import std;
import conflux.types;
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
// ── Handle types ──────────────────────────────────────────────────────────────

struct Fd {
	int v{-1};
};
struct DirectSlot {
	std::uint32_t value{};
};
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
		Fd fd,
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
		io_uring_sqe_set_flags(p_, sqe_flags::fixed_file.raw());
		return *this;
	}
	inline Sqe &prep_read_multishot(
		Fd fd,
		std::uint32_t len,
		std::uint64_t off,
		BufGroupId grp) noexcept {
		io_uring_prep_read_multishot(p_, fd.v, len, static_cast<__u64>(off), grp.v);
		return *this;
	}
	inline Sqe &prep_readv(
		Fd fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off) noexcept {
		io_uring_prep_readv(p_, fd.v, iov, nr, static_cast<__u64>(off));
		return *this;
	}
	inline Sqe &prep_readv2(
		Fd fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off,
		int flags) noexcept {
		io_uring_prep_readv2(p_, fd.v, iov, nr, static_cast<__u64>(off), flags);
		return *this;
	}
	inline Sqe &prep_read_fixed(
		Fd fd,
		void *buf,
		std::uint32_t len,
		std::uint64_t off,
		FixedBufIdx idx) noexcept {
		io_uring_prep_read_fixed(p_, fd.v, buf, len, static_cast<__u64>(off), idx.v);
		return *this;
	}
	inline Sqe &prep_readv_fixed(
		Fd fd,
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
		Fd fd,
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
		io_uring_sqe_set_flags(p_, sqe_flags::fixed_file.raw());
		return *this;
	}
	inline Sqe &prep_writev(
		Fd fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off) noexcept {
		io_uring_prep_writev(p_, fd.v, iov, nr, static_cast<__u64>(off));
		return *this;
	}
	inline Sqe &prep_writev2(
		Fd fd,
		iovec const *iov,
		unsigned nr,
		std::uint64_t off,
		int flags) noexcept {
		io_uring_prep_writev2(p_, fd.v, iov, nr, static_cast<__u64>(off), flags);
		return *this;
	}
	inline Sqe &prep_write_fixed(
		Fd fd,
		void const *buf,
		std::uint32_t len,
		std::uint64_t off,
		FixedBufIdx idx) noexcept {
		io_uring_prep_write_fixed(p_, fd.v, buf, len, static_cast<__u64>(off), idx.v);
		return *this;
	}
	inline Sqe &prep_writev_fixed(
		Fd fd,
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
		Fd fd,
		void const *buf,
		std::size_t len,
		MsgFlags flags) noexcept {
		io_uring_prep_send(p_, fd.v, buf, len, static_cast<int>(flags.raw()));
		return *this;
	}
	inline Sqe &prep_send_bundle(
		Fd fd,
		std::size_t len,
		MsgFlags flags) noexcept {
		io_uring_prep_send_bundle(p_, fd.v, len, static_cast<int>(flags.raw()));
		return *this;
	}
	inline Sqe &prep_sendto(
		Fd fd,
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
		Fd fd,
		msghdr const *msg,
		MsgFlags flags) noexcept {
		io_uring_prep_sendmsg(p_, fd.v, msg, flags.raw());
		return *this;
	}
	inline Sqe &prep_send_zc(
		Fd fd,
		void const *buf,
		std::size_t len,
		MsgFlags flags,
		unsigned zc_flags) noexcept {
		io_uring_prep_send_zc(p_, fd.v, buf, len, static_cast<int>(flags.raw()), zc_flags);
		return *this;
	}
	inline Sqe &prep_send_zc_fixed(
		Fd fd,
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
		Fd fd,
		msghdr const *msg,
		MsgFlags flags) noexcept {
		io_uring_prep_sendmsg_zc(p_, fd.v, msg, flags.raw());
		return *this;
	}
	inline Sqe &prep_sendmsg_zc_fixed(
		Fd fd,
		msghdr const *msg,
		MsgFlags flags,
		FixedBufIdx idx) noexcept {
		io_uring_prep_sendmsg_zc_fixed(p_, fd.v, msg, flags.raw(), static_cast<unsigned>(idx.v));
		return *this;
	}
	// ── Recv ────────────────────────────────────────────────────────────────

	inline Sqe &prep_recv(
		Fd fd,
		void *buf,
		std::size_t len,
		MsgFlags flags) noexcept {
		io_uring_prep_recv(p_, fd.v, buf, len, static_cast<int>(flags.raw()));
		return *this;
	}
	inline Sqe &prep_recv_multishot(
		Fd fd,
		void *buf,
		std::size_t len,
		MsgFlags flags) noexcept {
		io_uring_prep_recv_multishot(p_, fd.v, buf, len, static_cast<int>(flags.raw()));
		return *this;
	}
	inline Sqe &prep_recvmsg(
		Fd fd,
		msghdr *msg,
		MsgFlags flags) noexcept {
		io_uring_prep_recvmsg(p_, fd.v, msg, flags.raw());
		return *this;
	}
	inline Sqe &prep_recvmsg_multishot(
		Fd fd,
		msghdr *msg,
		MsgFlags flags) noexcept {
		io_uring_prep_recvmsg_multishot(p_, fd.v, msg, flags.raw());
		return *this;
	}
	// ── Accept / connect ────────────────────────────────────────────────────

	inline Sqe &prep_accept(
		Fd fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags) noexcept {
		io_uring_prep_accept(p_, fd.v, addr, addrlen, flags);
		return *this;
	}
	inline Sqe &prep_accept_direct(
		Fd fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags,
		DirectSlot slot) noexcept {
		io_uring_prep_accept_direct(p_, fd.v, addr, addrlen, flags, slot.value);
		return *this;
	}
	inline Sqe &prep_multishot_accept(
		Fd fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags) noexcept {
		io_uring_prep_multishot_accept(p_, fd.v, addr, addrlen, flags);
		return *this;
	}
	inline Sqe &prep_multishot_accept_direct(
		Fd fd,
		sockaddr *addr,
		socklen_t *addrlen,
		int flags) noexcept {
		io_uring_prep_multishot_accept_direct(p_, fd.v, addr, addrlen, flags);
		return *this;
	}
	inline Sqe &prep_connect(
		Fd fd,
		sockaddr const *addr,
		socklen_t addrlen) noexcept {
		io_uring_prep_connect(p_, fd.v, addr, addrlen);
		return *this;
	}
	inline Sqe &prep_bind(
		Fd fd,
		sockaddr *addr,
		socklen_t addrlen) noexcept {
		io_uring_prep_bind(p_, fd.v, addr, addrlen);
		return *this;
	}
	inline Sqe &prep_listen(
		Fd fd,
		int backlog) noexcept {
		io_uring_prep_listen(p_, fd.v, backlog);
		return *this;
	}
	inline Sqe &prep_shutdown(
		Fd fd,
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
		Fd dfd,
		char const *path,
		int flags,
		mode_t mode) noexcept {
		io_uring_prep_openat(p_, dfd.v, path, flags, mode);
		return *this;
	}
	inline Sqe &prep_openat_direct(
		Fd dfd,
		char const *path,
		int flags,
		mode_t mode,
		DirectSlot slot) noexcept {
		io_uring_prep_openat_direct(p_, dfd.v, path, flags, mode, slot.value);
		return *this;
	}
	inline Sqe &open_direct(
		Fd dfd,
		char const *path,
		int flags,
		mode_t mode,
		DirectSlot slot) noexcept {
		io_uring_prep_openat_direct(p_, dfd.v, path, flags, mode, slot.value);
		return *this;
	}
	inline Sqe &prep_openat2(
		Fd dfd,
		char const *path,
		open_how *how) noexcept {
		io_uring_prep_openat2(p_, dfd.v, path, how);
		return *this;
	}
	inline Sqe &prep_openat2_direct(
		Fd dfd,
		char const *path,
		open_how *how,
		DirectSlot slot) noexcept {
		io_uring_prep_openat2_direct(p_, dfd.v, path, how, slot.value);
		return *this;
	}
	inline Sqe &prep_close(
		Fd fd) noexcept {
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
		Fd dfd,
		char const *path,
		int flags,
		unsigned mask,
		struct statx *stx) noexcept {
		io_uring_prep_statx(p_, dfd.v, path, flags, mask, stx);
		return *this;
	}
	inline Sqe &prep_fsync(
		Fd fd,
		FsyncFlags flags) noexcept {
		io_uring_prep_fsync(p_, fd.v, flags.raw());
		return *this;
	}
	inline Sqe &prep_fallocate(
		Fd fd,
		std::uint32_t mode,
		std::uint64_t offset,
		std::uint64_t len) noexcept {
		io_uring_prep_fallocate(p_, fd.v, static_cast<int>(mode), static_cast<__u64>(offset), static_cast<__u64>(len));
		return *this;
	}
	inline Sqe &prep_fadvise(
		Fd fd,
		std::uint64_t offset,
		std::uint32_t len,
		std::uint32_t advice) noexcept {
		io_uring_prep_fadvise(p_, fd.v, static_cast<__u64>(offset), len, static_cast<int>(advice));
		return *this;
	}
	inline Sqe &prep_fadvise64(
		Fd fd,
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
		Fd fd,
		std::uint32_t len,
		std::uint64_t offset,
		int flags) noexcept {
		io_uring_prep_sync_file_range(p_, fd.v, len, static_cast<__u64>(offset), flags);
		return *this;
	}
	inline Sqe &prep_ftruncate(
		Fd fd,
		std::int64_t len) noexcept {
		io_uring_prep_ftruncate(p_, fd.v, len);
		return *this;
	}
	inline Sqe &prep_renameat(
		Fd old_dfd,
		char const *old_path,
		Fd new_dfd,
		char const *new_path,
		unsigned flags) noexcept {
		io_uring_prep_renameat(p_, old_dfd.v, old_path, new_dfd.v, new_path, flags);
		return *this;
	}
	inline Sqe &prep_unlinkat(
		Fd dfd,
		char const *path,
		int flags) noexcept {
		io_uring_prep_unlinkat(p_, dfd.v, path, flags);
		return *this;
	}
	inline Sqe &prep_mkdirat(
		Fd dfd,
		char const *path,
		mode_t mode) noexcept {
		io_uring_prep_mkdirat(p_, dfd.v, path, mode);
		return *this;
	}
	inline Sqe &prep_symlinkat(
		char const *target,
		Fd new_dfd,
		char const *link_path) noexcept {
		io_uring_prep_symlinkat(p_, target, new_dfd.v, link_path);
		return *this;
	}
	inline Sqe &prep_linkat(
		Fd old_dfd,
		char const *old_path,
		Fd new_dfd,
		char const *new_path,
		int flags) noexcept {
		io_uring_prep_linkat(p_, old_dfd.v, old_path, new_dfd.v, new_path, flags);
		return *this;
	}
	inline Sqe &prep_fsetxattr(
		Fd fd,
		char const *name,
		char const *val,
		int flags,
		std::uint32_t len) noexcept {
		io_uring_prep_fsetxattr(p_, fd.v, name, val, flags, len);
		return *this;
	}
	inline Sqe &prep_setxattr(
		char const *path,
		char const *name,
		char const *val,
		int flags,
		std::uint32_t len) noexcept {
		io_uring_prep_setxattr(p_, path, name, val, flags, len);
		return *this;
	}
	inline Sqe &prep_fgetxattr(
		Fd fd,
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
		Fd fd,
		CancelFlags flags) noexcept {
		io_uring_prep_cancel_fd(p_, fd.v, flags.raw());
		return *this;
	}
	// ── Poll / epoll ─────────────────────────────────────────────────────────

	inline Sqe &prep_poll_add(
		Fd fd,
		PollFlags events) noexcept {
		io_uring_prep_poll_add(p_, fd.v, events.raw());
		return *this;
	}
	inline Sqe &prep_poll_multishot(
		Fd fd,
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
		Fd epfd,
		int op,
		Fd fd,
		epoll_event *ev) noexcept {
		io_uring_prep_epoll_ctl(p_, epfd.v, op, fd.v, ev);
		return *this;
	}
	inline Sqe &prep_epoll_wait(
		Fd epfd,
		epoll_event *events,
		std::uint32_t maxevents,
		std::uint32_t flags) noexcept {
		io_uring_prep_epoll_wait(p_, epfd.v, events, static_cast<int>(maxevents), flags);
		return *this;
	}
	// ── Splice / tee / pipe ──────────────────────────────────────────────────

	inline Sqe &prep_splice(
		Fd fd_in,
		std::int64_t off_in,
		Fd fd_out,
		std::int64_t off_out,
		std::uint32_t len,
		SpliceFlags flags) noexcept {
		io_uring_prep_splice(p_, fd_in.v, off_in, fd_out.v, off_out, len, flags.raw());
		return *this;
	}
	inline Sqe &prep_tee(
		Fd fd_in,
		Fd fd_out,
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
		Fd ring_fd,
		std::uint32_t len,
		UserData data,
		MsgRingFlags flags) noexcept {
		io_uring_prep_msg_ring(p_, ring_fd.v, len, data.v, flags.raw());
		return *this;
	}
	inline Sqe &prep_msg_ring_cqe_flags(
		Fd ring_fd,
		std::uint32_t len,
		UserData data,
		MsgRingFlags flags,
		std::uint32_t cqe_flags) noexcept {
		io_uring_prep_msg_ring_cqe_flags(p_, ring_fd.v, len, data.v, flags.raw(), cqe_flags);
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
		Fd fd) noexcept {
		io_uring_prep_uring_cmd(p_, cmd_op, fd.v);
		return *this;
	}
	inline Sqe &prep_cmd_sock(
		int cmd_op,
		Fd fd,
		int level,
		int optname,
		void *optval,
		int optlen) noexcept {
		io_uring_prep_cmd_sock(p_, cmd_op, fd.v, level, optname, optval, optlen);
		return *this;
	}
};
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
		Fd fd) noexcept {
		return io_uring_register_eventfd(&ring_, fd.v);
	}
	int register_eventfd_async(
		Fd fd) noexcept {
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
