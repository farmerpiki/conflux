module;
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

module conflux.socket_io;
import std;

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

TcpListener::TcpListener(
	TcpListenerOptions opts) {
	bool const is_v6 =
		(opts.bind == TcpBindAddress::loopback_v6
		 || opts.bind == TcpBindAddress::any_v6_dual
		 || opts.bind == TcpBindAddress::any_v6_only);
	int const domain = is_v6 ? AF_INET6 : AF_INET;
	int const raw = ::socket(domain, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
	if (raw < 0) {
		throw std::system_error(errno, std::system_category(), "socket");
	}
	FdGuard guard{raw};
	int const on = 1;
	if (opts.reuse_addr) {
		if (::setsockopt(raw, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
			throw std::system_error(errno, std::system_category(), "SO_REUSEADDR");
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
		addr.sin_addr.s_addr = (opts.bind == TcpBindAddress::loopback_v4) ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
		if (::bind(raw, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
			throw std::system_error(errno, std::system_category(), "bind");
		}
	}
	if (::listen(raw, opts.backlog) < 0) {
		throw std::system_error(errno, std::system_category(), "listen");
	}
	sockaddr_storage ss{};
	socklen_t sslen = sizeof(ss);
	if (::getsockname(raw, reinterpret_cast<sockaddr *>(&ss), &sslen) < 0) {
		throw std::system_error(errno, std::system_category(), "getsockname");
	}
	port_ = (ss.ss_family == AF_INET6) ? ntohs(reinterpret_cast<sockaddr_in6 const *>(&ss)->sin6_port) :
										 ntohs(reinterpret_cast<sockaddr_in const *>(&ss)->sin_port);
	accept_flags_ = opts.accept_flags;
	fd_ = std::exchange(guard.fd, -1);
}

TcpListener::~TcpListener() noexcept {
	if (fd_ >= 0) {
		::close(fd_);
	}
}

TcpListener::TcpListener(
	TcpListener &&o) noexcept
	: fd_{std::exchange(o.fd_, -1)}
	, port_{std::exchange(o.port_, std::uint16_t{})}
	, accept_flags_{o.accept_flags_} {}

TcpListener &TcpListener::operator =(
	TcpListener &&o) noexcept {
	if (this != &o) {
		if (fd_ >= 0) {
			::close(fd_);
		}
		fd_ = std::exchange(o.fd_, -1);
		port_ = std::exchange(o.port_, std::uint16_t{});
		accept_flags_ = o.accept_flags_;
	}
	return *this;
}
