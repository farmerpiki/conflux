module;

#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

export module conflux.net.udp;

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;

using std::make_exception_ptr;
using std::make_shared;
using std::shared_ptr;
using std::span;
using std::string;
using std::system_error;

namespace conflux::net::udp {

// ─── error type ─────────────────────────────────────────────────────────────

export struct UdpError final : system_error {
	UdpError(
		int err,
		string const &what)
		: system_error{err, std::generic_category(), what} {}
};

// ─── UdpSocket: RAII fd owner for SOCK_DGRAM ────────────────────────────────

export class UdpSocket {
	FileHandle fh_{};
	int family_{AF_INET};

public:
	UdpSocket() noexcept = default;

	UdpSocket(UdpSocket const &) = delete;
	UdpSocket &operator =(UdpSocket const &) = delete;
	UdpSocket(UdpSocket &&) noexcept = default;
	UdpSocket &operator =(UdpSocket &&) noexcept = default;

	[[nodiscard]] FileHandle const &handle() const noexcept { return fh_; }
	[[nodiscard]] int raw_fd() const noexcept { return fh_.raw_fd(); }
	[[nodiscard]] int family() const noexcept { return family_; }
	[[nodiscard]] bool is_open() const noexcept { return fh_.valid(); }

	// Open a SOCK_DGRAM socket for `family` (AF_INET or AF_INET6) bound to
	// port 0 with the wildcard address. The kernel assigns an ephemeral
	// source port — used by DNS to mitigate RFC 5452 spoofing.
	static UdpSocket open_ephemeral(
		int family) {
		int const fd = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
		if (fd < 0) {
			throw UdpError{errno, "udp: socket(SOCK_DGRAM)"};
		}
		if (family == AF_INET) {
			::sockaddr_in sa{};
			sa.sin_family = AF_INET;
			sa.sin_port = 0;
			sa.sin_addr.s_addr = htonl(INADDR_ANY);
			if (::bind(fd, reinterpret_cast<::sockaddr *>(&sa), sizeof(sa)) < 0) {
				int const e = errno;
				::close(fd);
				throw UdpError{e, "udp: bind(AF_INET, port 0)"};
			}
		} else if (family == AF_INET6) {
			::sockaddr_in6 sa{};
			sa.sin6_family = AF_INET6;
			sa.sin6_port = 0;
			sa.sin6_addr = in6addr_any;
			if (::bind(fd, reinterpret_cast<::sockaddr *>(&sa), sizeof(sa)) < 0) {
				int const e = errno;
				::close(fd);
				throw UdpError{e, "udp: bind(AF_INET6, port 0)"};
			}
		} else {
			::close(fd);
			throw UdpError{EAFNOSUPPORT, "udp: unsupported family"};
		}
		UdpSocket out;
		out.fh_ = FileHandle::from_fd(fd);
		out.family_ = family;
		return out;
	}

	// Wrap an existing fd. Takes ownership; closes on destruction.
	static UdpSocket from_fd(
		int fd,
		int family) noexcept {
		UdpSocket out;
		out.fh_ = FileHandle::from_fd(fd);
		out.family_ = family;
		return out;
	}

	// Resolve the kernel-assigned local port via getsockname(2).
	[[nodiscard]] u16 local_port() const {
		::sockaddr_storage stor{};
		::socklen_t len = sizeof(stor);
		if (::getsockname(fh_.raw_fd(), reinterpret_cast<::sockaddr *>(&stor), &len) < 0) {
			throw UdpError{errno, "udp: getsockname"};
		}
		if (stor.ss_family == AF_INET) {
			return ntohs(reinterpret_cast<::sockaddr_in const *>(&stor)->sin_port);
		}
		if (stor.ss_family == AF_INET6) {
			return ntohs(reinterpret_cast<::sockaddr_in6 const *>(&stor)->sin6_port);
		}
		throw UdpError{EAFNOSUPPORT, "udp: getsockname returned unsupported family"};
	}
};

// ─── UdpRecvResult — bytes + source address ────────────────────────────────

export struct UdpRecvResult {
	size_t bytes{0};
	::sockaddr_storage from{};
	::socklen_t from_len{0};
};

// ─── recv state holder ──────────────────────────────────────────────────────
//
// A msghdr/iovec/sockaddr_storage triple kept alive across the io_uring
// completion. The kernel may write into msg_namelen, so the holder MUST
// outlive the SQE; we shared_ptr it into the completion callback.

namespace detail {

struct RecvHolder {
	::msghdr msg{};
	::iovec iov{};
	::sockaddr_storage from{};
};

inline shared_ptr<RecvHolder> make_recv_holder(
	span<u8> buf) {
	auto h = make_shared<RecvHolder>();
	h->iov.iov_base = buf.data();
	h->iov.iov_len = buf.size();
	h->msg.msg_name = &h->from;
	h->msg.msg_namelen = sizeof(h->from);
	h->msg.msg_iov = &h->iov;
	h->msg.msg_iovlen = 1;
	return h;
}

inline UdpRecvResult holder_to_result(
	shared_ptr<RecvHolder> const &h,
	size_t bytes) {
	UdpRecvResult r;
	r.bytes = bytes;
	r.from = h->from;
	r.from_len = h->msg.msg_namelen;
	return r;
}

} // namespace detail

// ─── sendto: thin wrapper around FileReader::sendto_async ───────────────────

export [[nodiscard]] Flow<size_t> sendto(
	FileReader &reader,
	UdpSocket const &sock,
	span<u8 const> bytes,
	::sockaddr const *dest,
	::socklen_t dest_len,
	int flags = 0) {
	if (dest == nullptr || dest_len == 0 || dest_len > static_cast<::socklen_t>(sizeof(::sockaddr_storage))) {
		FlowSource<size_t> const src;
		auto flow = src.flow();
		src.reject(make_exception_ptr(UdpError{EINVAL, "udp: invalid destination"}));
		return flow;
	}
	::sockaddr_storage stor{};
	std::memcpy(&stor, dest, static_cast<size_t>(dest_len));
	return reader.sendto_async(sock.handle(), bytes.data(), bytes.size(), flags, stor, dest_len);
}

// ─── recvfrom: wraps recvmsg_async with caller-managed msghdr ──────────────

export [[nodiscard]] Flow<UdpRecvResult> recvfrom(
	FileReader &reader,
	UdpSocket const &sock,
	span<u8> buf,
	int flags = 0) {
	auto h = detail::make_recv_holder(buf);
	return reader.recvmsg_async(sock.handle(), &h->msg, static_cast<unsigned>(flags))
		 | then([h](size_t n) { return detail::holder_to_result(h, n); });
}

// ─── recvfrom_with_timeout: linked recvmsg + link_timeout SQE pair ──────────
//
// On timeout, recvmsg's CQE arrives with res = -ECANCELED; we surface that
// as UdpError{ETIMEDOUT}. On send-side error, res is the negated errno.
// The link_timeout's CQE is consumed by a no-op slot whose only job is to
// keep the timespec alive until the kernel is done with it.

export [[nodiscard]] Flow<UdpRecvResult> recvfrom_with_timeout(
	FileReader &reader,
	UdpSocket const &sock,
	span<u8> buf,
	std::chrono::milliseconds timeout,
	int flags = 0) {
	FlowSource<UdpRecvResult> const src;
	auto flow = src.flow();

	auto *ring = reader.ring();
	auto *completions = reader.completions();

	// Two SQEs are required: the linked recvmsg and the LINK_TIMEOUT.
	// io_uring rejects a partial submission if either get_sqe fails, so
	// we acquire both up front before any state is mutated.
	auto *sqe_recv = ::io_uring_get_sqe(ring);
	auto *sqe_to = sqe_recv != nullptr ? ::io_uring_get_sqe(ring) : nullptr;
	if (sqe_recv == nullptr || sqe_to == nullptr) {
		src.reject(make_exception_ptr(UdpError{ENOSPC, "udp: SQ full"}));
		return flow;
	}

	auto h = detail::make_recv_holder(buf);

	auto ts = make_shared<__kernel_timespec>();
	{
		auto const sec = std::chrono::duration_cast<std::chrono::seconds>(timeout);
		ts->tv_sec = sec.count();
		ts->tv_nsec = (timeout - sec).count() * 1'000'000LL;
	}

	int const fd = sock.raw_fd();

	::io_uring_prep_recvmsg(sqe_recv, fd, &h->msg, static_cast<unsigned>(flags));
	sqe_recv->flags |= IOSQE_IO_LINK;

	auto [slot_recv, gen_recv] = completions->reserve([src, h](IoResult r) mutable {
		try {
			if (r.res == -ECANCELED) {
				src.reject(make_exception_ptr(UdpError{ETIMEDOUT, "udp: recvfrom timed out"}));
				return;
			}
			if (r.res < 0) {
				src.reject(make_exception_ptr(UdpError{-r.res, "udp: recvfrom failed"}));
				return;
			}
			src.resolve(detail::holder_to_result(h, static_cast<size_t>(r.res)));
		} catch (...) { src.reject(std::current_exception()); }
	});
	::io_uring_sqe_set_data64(sqe_recv, reader.encode_ud(slot_recv, gen_recv));

	::io_uring_prep_link_timeout(sqe_to, ts.get(), 0);
	auto [slot_to, gen_to] = completions->reserve([ts](IoResult) mutable { (void)ts; });
	::io_uring_sqe_set_data64(sqe_to, reader.encode_ud(slot_to, gen_to));

	return flow;
}

} // namespace conflux::net::udp
