module;
#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#if CONFLUX_HAS_TLS
	#include <openssl/err.h>
	#include <openssl/ssl.h>
	#include <openssl/x509.h>
#endif

export module conflux.net.client;
import std;
import conflux.types;
import conflux.net.http.parse_helpers;
import conflux.net.http.types;
import conflux.net.http.request;
import conflux.net.client_wire;
import conflux.utils;
import conflux.work;
import conflux.dns_bridge;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
// ─── exported response types ─────────────────────────────────────────────────

export namespace conflux::http {

using conflux::utils::wait_fd;

struct ClientResponseHead {
	int status{502};
	std::string status_text{"Bad Gateway"};
	conflux::http::HttpFields headers = conflux::http::HttpFields(true);
	std::vector<std::string> set_cookies{};
};
struct ClientResponse {
	ClientResponseHead head{};
	std::string body{};
	HttpTelemetry telemetry{};

	// Phase 2: json() / json_borrowed() accessors.
};
using ClientResult = std::expected<ClientResponse, HttpError>;
using ClientBodyChunkSink = std::function<bool(std::string_view)>;
struct ClientStreamResponse {
	ClientResponseHead head{};
	HttpTelemetry telemetry{};
};
using ClientStreamResult = std::expected<ClientStreamResponse, HttpError>;
struct HttpClientOptions {
	HttpTimeouts default_timeouts{};
	bool verify_peer{true};
	std::string ca_bundle_path{}; // empty = system default
	std::size_t max_header_bytes{64 * 1024};
	std::size_t max_body_bytes{16 * 1024 * 1024};
	std::size_t max_buffered_bytes{4 * 1024 * 1024};
	conflux::http::HttpFields default_headers = conflux::http::HttpFields(true);
	void *resolver{nullptr}; // conflux::net::dns::Resolver*; void* avoids exporting dns types
};

} // namespace conflux::http
// ─── internal transport ───────────────────────────────────────────────────────

namespace client_detail {

using namespace conflux::http;

constexpr std::size_t kClientMaxChunkCount = 100000;
struct Connection {
	int fd{-1};
	bool use_tls{false};
#if CONFLUX_HAS_TLS
	std::optional<conflux::net_tls::TlsContext> tls_ctx;
	std::optional<conflux::net_tls::TlsStream> tls_stream;
#endif
};
// Convert std::chrono::milliseconds to seconds for wait_fd (ceiling, ≥1 if ms>0).
[[nodiscard]] int to_sec(
	std::chrono::milliseconds ms) noexcept {
	if (ms.count() <= 0) {
		return -1; // indefinite
	}
	long long const s = (ms.count() + 999) / 1000;
	return static_cast<int>(std::min<long long>(s, INT_MAX));
}
void close_conn(
	Connection &conn) noexcept {
#if CONFLUX_HAS_TLS
	if (conn.tls_stream) {
		conn.tls_stream->shutdown_safe();
		conn.tls_stream.reset();
	}
	conn.tls_ctx.reset();
#endif
	if (conn.fd >= 0) {
		::close(conn.fd);
		conn.fd = -1;
	}
}
bool connect_with_timeout(
	int fd,
	sockaddr const *addr,
	socklen_t addrlen,
	int timeout_sec) {
	if (timeout_sec <= 0) {
		return ::connect(fd, addr, addrlen) == 0;
	}
	int const flags = ::fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		return false;
	}
	if (::fcntl(fd, F_SETFL, static_cast<int>(static_cast<unsigned>(flags) | static_cast<unsigned>(O_NONBLOCK))) < 0) {
		return false;
	}
	int const rc = ::connect(fd, addr, addrlen);
	if (rc == 0) {
		return ::fcntl(fd, F_SETFL, flags) == 0;
	}
	if (errno != EINPROGRESS) {
		auto const restore_rc = ::fcntl(fd, F_SETFL, flags);
		if (restore_rc < 0) {
			return false;
		}
		return false;
	}
	bool const ready = wait_fd(fd, POLLOUT, timeout_sec);
	if (::fcntl(fd, F_SETFL, flags) < 0) {
		return false;
	}
	if (!ready) {
		return false;
	}
	int so_error = 0;
	socklen_t len = sizeof(so_error);
	return ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0;
}

enum class ConnectFailure {
	dns,
	connect,
};
[[nodiscard]] int try_connect_endpoints(
	std::span<client_dns_bridge::Endpoint const> endpoints,
	std::uint16_t port,
	int timeout_sec,
	HttpTelemetry &tel) {
	constexpr auto kConnectAttemptDelay = std::chrono::milliseconds{250};
	auto const t1 = std::chrono::steady_clock::now();
	int fd = -1;
	int prev_family = -1;
	for (auto const &ep: endpoints) {
		if (fd != -1) {
			::close(fd);
			fd = -1;
		}
		int const fam = (ep.family == 4) ? AF_INET : AF_INET6;
		if (prev_family != -1 && fam != prev_family) {
			std::this_thread::sleep_for(kConnectAttemptDelay);
		}
		prev_family = fam;
		fd = ::socket(fam, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
		if (fd < 0) {
			continue;
		}
		sockaddr_storage addr{};
		std::memcpy(&addr, ep.addr, std::min(sizeof(addr), sizeof(ep.addr)));
		if (connect_with_timeout(
				fd,
				reinterpret_cast<sockaddr const *>(&addr),
				static_cast<socklen_t>(ep.addr_len),
				timeout_sec)) {
			char buf[INET6_ADDRSTRLEN]{};
			if (fam == AF_INET) {
				auto const *sa4 = reinterpret_cast<sockaddr_in const *>(&addr);
				inet_ntop(AF_INET, &sa4->sin_addr, buf, sizeof(buf));
				tel.peer_addr = std::format("{}:{}", buf, port);
			} else {
				auto const *sa6 = reinterpret_cast<sockaddr_in6 const *>(&addr);
				inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf));
				tel.peer_addr = std::format("[{}]:{}", buf, port);
			}
			break;
		}
		::close(fd);
		fd = -1;
	}
	tel.connect = std::chrono::steady_clock::now() - t1;
	return fd;
}
struct EndpointCollector {
	std::vector<client_dns_bridge::Endpoint> endpoints;
};
bool collect_endpoint(
	void *ctx,
	client_dns_bridge::Endpoint const &endpoint) noexcept {
	try {
		static_cast<EndpointCollector *>(ctx)->endpoints.push_back(endpoint);
		return true;
	} catch (...) { return false; }
}
[[nodiscard]] int resolve_and_connect(
	std::string_view host,
	std::uint16_t port,
	int timeout_sec,
	std::chrono::milliseconds resolve_timeout,
	HttpTelemetry &tel,
	ConnectFailure &failure,
	std::string &failure_message,
	int &failure_errno,
	void *resolver_ptr = nullptr) {
	if (resolver_ptr) {
		EndpointCollector collector;
		std::array<char, 256> error_buf{};
		auto const t0 = std::chrono::steady_clock::now();
		bool const resolved = client_dns_bridge::resolve(
			resolver_ptr,
			host.data(),
			host.size(),
			port,
			resolve_timeout.count(),
			&collector,
			collect_endpoint,
			error_buf.data(),
			error_buf.size());
		tel.dns = std::chrono::steady_clock::now() - t0;
		if (!resolved) {
			failure = ConnectFailure::dns;
			failure_errno = 0;
			failure_message = std::format("DNS resolution failed for '{}': {}", host, error_buf.data());
			return -1;
		}
		int const fd = try_connect_endpoints(collector.endpoints, port, timeout_sec, tel);
		if (fd < 0) {
			failure = ConnectFailure::connect;
			failure_errno = errno;
			failure_message = std::format("failed to connect to '{}:{}'", host, port);
		}
		return fd;
	}
	std::string const host_str{host};
	std::string const port_str = std::to_string(port);
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_ADDRCONFIG;
	auto const t0 = std::chrono::steady_clock::now();
	addrinfo *res_raw = nullptr;
	int const gai_err = ::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &res_raw);
	tel.dns = std::chrono::steady_clock::now() - t0;
	if (gai_err != 0) {
		failure = ConnectFailure::dns;
		failure_errno = 0;
		failure_message = std::format("DNS resolution failed for '{}': {}", host, ::gai_strerror(gai_err));
		return -1;
	}
	struct AddrInfoDeleter {
		void operator ()(
			addrinfo *p) const noexcept {
			::freeaddrinfo(p);
		}
	};
	std::unique_ptr<addrinfo, AddrInfoDeleter> const res{res_raw};
	auto const t1 = std::chrono::steady_clock::now();
	int fd = -1;
	int prev_family = -1;
	for (addrinfo const *ai = res.get(); ai != nullptr; ai = ai->ai_next) {
		if (fd != -1) {
			::close(fd);
			fd = -1;
		}
		if (prev_family != -1 && ai->ai_family != prev_family) {
			std::this_thread::sleep_for(std::chrono::milliseconds{250});
		}
		prev_family = ai->ai_family;
		fd = ::socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
		if (fd < 0) {
			continue;
		}
		if (connect_with_timeout(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen), timeout_sec)) {
			char buf[INET6_ADDRSTRLEN]{};
			if (ai->ai_family == AF_INET) {
				auto const *sa4 = reinterpret_cast<sockaddr_in const *>(ai->ai_addr);
				inet_ntop(AF_INET, &sa4->sin_addr, buf, sizeof(buf));
				tel.peer_addr = std::format("{}:{}", buf, port);
			} else {
				auto const *sa6 = reinterpret_cast<sockaddr_in6 const *>(ai->ai_addr);
				inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf));
				tel.peer_addr = std::format("[{}]:{}", buf, port);
			}
			break;
		}
		::close(fd);
		fd = -1;
	}
	tel.connect = std::chrono::steady_clock::now() - t1;
	if (fd < 0) {
		failure = ConnectFailure::connect;
		failure_errno = errno;
		failure_message = std::format("failed to connect to '{}:{}'", host, port);
	}
	return fd;
}
bool send_all(
	Connection &conn,
	std::string_view data,
	int timeout_sec) {
#if CONFLUX_HAS_TLS
	if (conn.use_tls) {
		return conn.tls_stream->write_all(data, timeout_sec);
	}
#endif
	std::size_t sent = 0;
	while (sent < data.size()) {
		if (!wait_fd(conn.fd, POLLOUT, timeout_sec)) {
			return false;
		}
		auto const n = ::send(conn.fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
		if (n <= 0) {
			return false;
		}
		sent += static_cast<std::size_t>(n);
	}
	return true;
}

bool send_all_plain_iov(
	int fd,
	std::string_view first,
	std::string_view second,
	int timeout_sec) {
	std::array<iovec, 2> iov{
		iovec{.iov_base = const_cast<char *>(first.data()), .iov_len = first.size()},
		iovec{.iov_base = const_cast<char *>(second.data()), .iov_len = second.size()}};
	int iovcnt = second.empty() ? 1 : 2;
	while (iovcnt > 0) {
		if (!wait_fd(fd, POLLOUT, timeout_sec)) {
			return false;
		}
		msghdr msg{};
		msg.msg_iov = iov.data();
		msg.msg_iovlen = static_cast<std::size_t>(iovcnt);
		auto n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
		if (n <= 0) {
			return false;
		}
		while (n > 0 && iovcnt > 0) {
			if (static_cast<std::size_t>(n) >= iov[0].iov_len) {
				n -= static_cast<ssize_t>(iov[0].iov_len);
				iov[0] = iov[1];
				--iovcnt;
				continue;
			}
			iov[0].iov_base = static_cast<char *>(iov[0].iov_base) + n;
			iov[0].iov_len -= static_cast<std::size_t>(n);
			n = 0;
		}
	}
	return true;
}

bool send_blocking_http1_request_bytes(
	Connection &conn,
	std::string_view wire,
	std::string_view body,
	int write_timeout_sec) {
#if CONFLUX_HAS_TLS
	if (conn.use_tls) {
		if (!send_all(conn, wire, write_timeout_sec)) {
			return false;
		}
		return body.empty() || send_all(conn, body, write_timeout_sec);
	}
#endif
	return send_all_plain_iov(conn.fd, wire, body, write_timeout_sec);
}
bool recv_some(
	Connection &conn,
	std::string &out,
	int timeout_sec) {
#if CONFLUX_HAS_TLS
	if (conn.use_tls) {
		return conn.tls_stream->read_some(out, timeout_sec);
	}
#endif
	std::array<char, 4096> tmp{};
	if (!wait_fd(conn.fd, POLLIN, timeout_sec)) {
		return false;
	}
	auto const n = ::recv(conn.fd, tmp.data(), tmp.size(), 0);
	if (n <= 0) {
		return false;
	}
	out.append(tmp.data(), static_cast<std::size_t>(n));
	return true;
}
enum class RecvSomeStatus : std::uint8_t {
	data,
	timeout,
	closed_or_error,
};
RecvSomeStatus recv_some_status(
	Connection &conn,
	std::string &out,
	int timeout_sec) {
#if CONFLUX_HAS_TLS
	if (conn.use_tls) {
		auto const before = out.size();
		if (conn.tls_stream->read_some(out, timeout_sec)) {
			return RecvSomeStatus::data;
		}
		return out.size() != before ? RecvSomeStatus::data : RecvSomeStatus::closed_or_error;
	}
#endif
	std::array<char, 4096> tmp{};
	if (!wait_fd(conn.fd, POLLIN, timeout_sec)) {
		return RecvSomeStatus::timeout;
	}
	auto const n = ::recv(conn.fd, tmp.data(), tmp.size(), 0);
	if (n <= 0) {
		return RecvSomeStatus::closed_or_error;
	}
	out.append(tmp.data(), static_cast<std::size_t>(n));
	return RecvSomeStatus::data;
}
struct RecvUntilResult {
	std::string bytes{};
	bool timed_out{false};
};
// Receive until delimiter or std::max bytes. Returns accumulated bytes (may contain
// data past the delimiter if overread from the socket).
RecvUntilResult recv_until(
	Connection &conn,
	std::string_view delim,
	int timeout_sec,
	std::size_t max_size) {
	RecvUntilResult result;
	result.bytes.reserve(std::min<std::size_t>(4096, max_size));
	std::size_t search_from = 0;
	while (result.bytes.size() < max_size) {
		bool closed = false;
		auto const previous_size = result.bytes.size();
		switch (recv_some_status(conn, result.bytes, timeout_sec)) {
		case RecvSomeStatus::data           : break;
		case RecvSomeStatus::timeout        : result.timed_out = true; return result;
		case RecvSomeStatus::closed_or_error: closed = true; break;
		}
		search_from = previous_size > delim.size() ? previous_size - delim.size() + 1 : 0;
		if (result.bytes.find(delim, search_from) != std::string::npos) {
			break;
		}
		if (closed) {
			break;
		}
	}
	return result;
}
bool recv_exact(
	Connection &conn,
	std::string &out,
	int timeout_sec,
	std::size_t target,
	std::size_t cap) {
	while (out.size() < target) {
		if (out.size() >= cap) {
			return false; // body_too_large
		}
		if (!recv_some(conn, out, timeout_sec)) {
			return false;
		}
	}
	return true;
}
void recv_to_eof(
	Connection &conn,
	std::string &out,
	int timeout_sec,
	std::size_t cap,
	bool &too_large) {
	too_large = false;
	while (recv_some(conn, out, timeout_sec)) {
		if (out.size() > cap) {
			too_large = true;
			return;
		}
	}
}

bool recv_chunked(
	Connection &conn,
	std::string &encoded,
	std::string &decoded,
	int timeout_sec,
	std::size_t cap,
	std::size_t buf_cap,
	bool &too_large) {
	too_large = false;
	decoded.clear();
	decoded.reserve(std::min(encoded.size(), cap));
	conflux::http::ChunkedDecodeState chunked;
	for (;;) {
		auto const rc = conflux::http::decode_chunked_incremental(encoded, 0, cap, kClientMaxChunkCount, chunked);
		if (rc > 0) {
			decoded = std::move(chunked.body);
			return true;
		}
		if (rc == -1) {
			return false;
		}
		if (rc == -2 || chunked.body.size() > cap || encoded.size() > buf_cap) {
			too_large = true;
			return false;
		}
		if (!recv_some(conn, encoded, timeout_sec)) {
			return false;
		}
	}
}
[[nodiscard]] std::expected<void, HttpError> emit_body_chunk(
	ClientBodyChunkSink const &sink,
	std::string_view chunk) {
	if (chunk.empty()) {
		return {};
	}
	if (!sink) {
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::read,
				.phase = HttpPhase::between_bytes,
				.message = "response body sink is empty"});
	}
	if (!sink(chunk)) {
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::read,
				.phase = HttpPhase::between_bytes,
				.message = "response body sink rejected chunk"});
	}
	return {};
}
[[nodiscard]] std::expected<void, HttpError> stream_chunked_body(
	Connection &conn,
	std::string &encoded,
	ClientBodyChunkSink const &sink,
	int timeout_sec,
	std::size_t cap,
	std::size_t buf_cap,
	std::uint64_t &bytes_received) {
	conflux::http::ChunkedDecodeState chunked;
	for (;;) {
		auto const rc = conflux::http::decode_chunked_incremental(encoded, 0, cap, kClientMaxChunkCount, chunked);
		if (!chunked.body.empty()) {
			if (auto emitted = emit_body_chunk(sink, chunked.body); !emitted) {
				return emitted;
			}
			chunked.body.clear();
		}
		if (rc > 0) {
			return {};
		}
		if (rc == -1) {
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::read,
					.phase = HttpPhase::between_bytes,
					.message = "failed to receive chunked body"});
		}
		if (rc == -2 || chunked.body.size() > cap || encoded.size() > buf_cap) {
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::body_too_large,
					.message = std::format("chunked body exceeds limit {}", cap)});
		}
		if (chunked.pos > 0) {
			encoded.erase(0, chunked.pos);
			chunked.body_start = 0;
			chunked.pos = 0;
		}
		auto const before = encoded.size();
		if (!recv_some(conn, encoded, timeout_sec)) {
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::read,
					.phase = HttpPhase::between_bytes,
					.os_errno = errno,
					.message = "failed to receive chunked body"});
		}
		bytes_received += encoded.size() - before;
	}
}
[[nodiscard]] std::expected<void, HttpError> stream_content_length_body(
	Connection &conn,
	std::string_view initial_body,
	ClientBodyChunkSink const &sink,
	int timeout_sec,
	std::size_t content_length,
	std::size_t max_body,
	std::uint64_t &bytes_received) {
	std::size_t delivered = 0;
	if (content_length > max_body) {
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::body_too_large,
				.message = std::format("body exceeds limit {}", max_body)});
	}
	if (!initial_body.empty()) {
		if (auto emitted = emit_body_chunk(sink, initial_body); !emitted) {
			return emitted;
		}
		delivered = initial_body.size();
	}
	std::string buffer;
	while (delivered < content_length) {
		buffer.clear();
		if (!recv_some(conn, buffer, timeout_sec)) {
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::read,
					.phase = HttpPhase::between_bytes,
					.os_errno = errno,
					.message = "failed to receive body"});
		}
		auto const need = content_length - delivered;
		auto const take = std::min(buffer.size(), need);
		if (delivered + take > max_body) {
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::body_too_large,
					.message = std::format("body exceeds limit {}", max_body)});
		}
		if (auto emitted = emit_body_chunk(sink, std::string_view{buffer}.substr(0, take)); !emitted) {
			return emitted;
		}
		delivered += take;
		bytes_received += take;
	}
	return {};
}
[[nodiscard]] std::expected<void, HttpError> stream_eof_body(
	Connection &conn,
	std::string_view initial_body,
	ClientBodyChunkSink const &sink,
	int timeout_sec,
	std::size_t max_body,
	std::uint64_t &bytes_received) {
	if (!initial_body.empty()) {
		if (initial_body.size() > max_body) {
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::body_too_large,
					.message = std::format("EOF-delimited body exceeds limit {}", max_body)});
		}
		if (auto emitted = emit_body_chunk(sink, initial_body); !emitted) {
			return emitted;
		}
	}
	std::string buffer;
	std::size_t delivered = initial_body.size();
	while (recv_some(conn, buffer, timeout_sec)) {
		auto const chunk = std::string_view{buffer};
		delivered += chunk.size();
		if (delivered > max_body) {
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::body_too_large,
					.message = std::format("EOF-delimited body exceeds limit {}", max_body)});
		}
		if (auto emitted = emit_body_chunk(sink, chunk); !emitted) {
			return emitted;
		}
		bytes_received += chunk.size();
		buffer.clear();
	}
	return {};
}
[[nodiscard]] std::expected<std::optional<ClientRequest>, HttpError> follow_redirect(
	ClientRequest const &req,
	ClientResponse const &resp) {
	return client_wire::follow_redirect_request(req, resp.head.status, resp.head.headers);
}

[[nodiscard]] std::optional<HttpError> send_blocking_http1_request(
	Connection &conn,
	ClientRequest const &req,
	HttpClientOptions const &opts,
	int write_timeout_sec,
	HttpTelemetry &tel) {
	auto wire_result = conflux::http::client_wire::build_http1_request_wire(req, opts.default_headers);
	if (!wire_result) {
		return std::move(wire_result).error();
	}
	std::string const wire = std::move(*wire_result);
	auto const body = std::string_view{req.body()};
	if (!send_blocking_http1_request_bytes(conn, wire, body, write_timeout_sec)) {
		return HttpError{
			.kind = HttpErrorKind::write,
			.phase = HttpPhase::write,
			.os_errno = errno,
			.message = body.empty() ? "failed to send request headers" : "failed to send request"};
	}
	tel.bytes_sent += wire.size() + body.size();
	return std::nullopt;
}

[[nodiscard]] HttpError make_blocking_connect_error(
	std::string_view host,
	std::uint16_t port,
	ConnectFailure failure,
	std::string const &failure_message,
	int failure_errno) {
	bool const is_dns = failure == ConnectFailure::dns;
	return HttpError{
		.kind = is_dns ? HttpErrorKind::dns : HttpErrorKind::connect,
		.phase = is_dns ? HttpPhase::resolve : HttpPhase::connect,
		.os_errno = failure_errno,
		.message = failure_message.empty() ?
					   std::format("failed to {} '{}:{}'", is_dns ? "resolve" : "connect", host, port) :
					   failure_message,
	};
}

#if CONFLUX_HAS_TLS
struct BlockingTlsSetup {
	std::optional<conflux::net_tls::TlsContext> ctx;
	std::optional<conflux::net_tls::TlsStream> stream;
};

[[nodiscard]] std::expected<void, HttpError> configure_blocking_tls_verification(
	conflux::net_tls::TlsContext &tls_ctx,
	bool verify,
	HttpClientOptions const &opts) {
	tls_ctx.set_verify_peer(verify);
	if (!verify) {
		return {};
	}
	if (!opts.ca_bundle_path.empty()) {
		if (SSL_CTX_load_verify_locations(tls_ctx.native_handle(), opts.ca_bundle_path.c_str(), nullptr) != 1) {
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::tls,
					.phase = HttpPhase::tls,
					.message = "TLS CA bundle load failed",
				});
		}
		return {};
	}
	if (!tls_ctx.set_default_verify_paths()) {
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::tls,
				.phase = HttpPhase::tls,
				.message = "TLS default verify paths load failed",
			});
	}
	return {};
}

[[nodiscard]] std::expected<void, HttpError> configure_blocking_tls_stream(
	conflux::net_tls::TlsStream &tls_stream,
	std::string_view server_name,
	bool verify) {
	if (!tls_stream.set_server_name(server_name)) {
		tls_stream.shutdown_safe();
		return std::unexpected(
			HttpError{.kind = HttpErrorKind::tls, .phase = HttpPhase::tls, .message = "SNI setup failed"});
	}
	if (verify && !tls_stream.set_verify_hostname(server_name)) {
		tls_stream.shutdown_safe();
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::tls,
				.phase = HttpPhase::tls,
				.message = "hostname verification setup failed"});
	}
	return {};
}

[[nodiscard]] std::expected<void, HttpError> perform_blocking_tls_handshake(
	conflux::net_tls::TlsStream &tls_stream,
	bool verify,
	int tls_timeout_sec,
	HttpTelemetry &tel) {
	auto t_tls = std::chrono::steady_clock::now();
	if (tls_stream.handshake_connect(tls_timeout_sec)) {
		tel.tls = std::chrono::steady_clock::now() - t_tls;
		tel.tls_verified = verify;
		tel.negotiated_protocol = "https/1.1";
		if (auto const *ssl = tls_stream.native_handle(); ssl != nullptr) {
			if (auto const *cipher = SSL_get_current_cipher(ssl)) {
				tel.tls_cipher = SSL_CIPHER_get_name(cipher);
				tel.tls_version = SSL_CIPHER_get_version(cipher);
			}
		}
		return {};
	}
	long const vr = SSL_get_verify_result(tls_stream.native_handle());
	int const alert = ERR_GET_REASON(ERR_get_error());
	std::string verify_reason;
	if (verify && vr != X509_V_OK) {
		if (auto const *s = X509_verify_cert_error_string(vr); s != nullptr) {
			verify_reason = s;
		}
	}
	tls_stream.shutdown_safe();
	return std::unexpected(
		HttpError{
			.kind = HttpErrorKind::tls,
			.phase = HttpPhase::tls,
			.tls_alert = alert,
			.verify_reason = std::move(verify_reason),
			.message = "TLS handshake failed",
		});
}

[[nodiscard]] std::expected<BlockingTlsSetup, HttpError> setup_blocking_tls(
	int fd,
	ClientRequest const &req,
	HttpClientOptions const &opts,
	HttpTimeouts const &timeouts,
	std::string_view url_host,
	HttpTelemetry &tel) {
	bool const verify = req.verify_peer() && opts.verify_peer;
	auto const server_name = req.server_name().empty() ? url_host : req.server_name();

	BlockingTlsSetup setup;
	try {
		setup.ctx.emplace();
	} catch (conflux::net_tls::TlsError const &e) {
		::close(fd);
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::tls,
				.phase = HttpPhase::tls,
				.message = e.what(),
			});
	}
	if (auto verified = configure_blocking_tls_verification(*setup.ctx, verify, opts); !verified) {
		::close(fd);
		return std::unexpected(std::move(verified).error());
	}

	try {
		setup.stream.emplace(*setup.ctx, fd);
	} catch (conflux::net_tls::TlsError const &e) {
		::close(fd);
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::tls,
				.phase = HttpPhase::tls,
				.message = e.what(),
			});
	}
	if (auto configured = configure_blocking_tls_stream(*setup.stream, server_name, verify); !configured) {
		::close(fd);
		return std::unexpected(std::move(configured).error());
	}
	if (auto handshake = perform_blocking_tls_handshake(*setup.stream, verify, to_sec(timeouts.tls), tel); !handshake) {
		::close(fd);
		return std::unexpected(std::move(handshake).error());
	}
	return setup;
}
#endif

[[nodiscard]] std::expected<Connection, HttpError> open_blocking_connection(
	ClientRequest const &req,
	HttpClientOptions const &opts,
	HttpTimeouts const &timeouts,
	HttpTelemetry &tel) {
	auto const &url = req.url();
	bool const use_tls = (url.scheme == "https");
	int const connect_sec = to_sec(timeouts.connect);
	ConnectFailure conn_fail{};
	std::string conn_fail_message{};
	int conn_fail_errno{0};
	int const fd = resolve_and_connect(
		url.host,
		url.port,
		connect_sec,
		timeouts.resolve,
		tel,
		conn_fail,
		conn_fail_message,
		conn_fail_errno,
		opts.resolver);
	if (fd < 0) {
		return std::unexpected(
			make_blocking_connect_error(url.host, url.port, conn_fail, conn_fail_message, conn_fail_errno));
	}

#if CONFLUX_HAS_TLS
	std::optional<conflux::net_tls::TlsContext> tls_ctx;
	std::optional<conflux::net_tls::TlsStream> tls_stream;
#endif

	if (use_tls) {
#if CONFLUX_HAS_TLS
		auto tls_setup = setup_blocking_tls(fd, req, opts, timeouts, url.host, tel);
		if (!tls_setup) {
			return std::unexpected(std::move(tls_setup).error());
		}
		tls_ctx = std::move(tls_setup->ctx);
		tls_stream = std::move(tls_setup->stream);
#else
		::close(fd);
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::tls,
				.phase = HttpPhase::tls,
				.message = "TLS not available (built without TLS)"});
#endif
	}

	Connection conn;
	conn.fd = fd;
#if CONFLUX_HAS_TLS
	conn.use_tls = use_tls;
	conn.tls_ctx = std::move(tls_ctx);
	conn.tls_stream = std::move(tls_stream);
#endif
	return conn;
}

struct BlockingResponseHeadRead {
	std::string raw;
	std::size_t header_end{};
	client_wire::ParsedResponseHead parsed;
};

[[nodiscard]] std::expected<BlockingResponseHeadRead, HttpError> receive_blocking_http1_response_head(
	Connection &conn,
	int first_byte_timeout_sec,
	std::size_t max_header_bytes,
	std::size_t max_body_bytes,
	HttpTelemetry &tel) {
	auto t_ttfb = std::chrono::steady_clock::now();
	auto received_head = recv_until(conn, "\r\n\r\n", first_byte_timeout_sec, max_header_bytes + 4096);
	auto raw = std::move(received_head.bytes);
	auto const header_end = raw.find("\r\n\r\n");
	if (header_end == std::string::npos) {
		if (received_head.timed_out) {
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::timeout,
					.phase = HttpPhase::first_byte,
					.message = "timed out waiting for response headers"});
		}
		if (raw.size() >= max_header_bytes) {
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::header_too_large,
					.message = std::format("response headers exceed {} bytes", max_header_bytes)});
		}
		return std::unexpected(
			HttpError{.kind = HttpErrorKind::protocol, .message = "response headers missing CRLFCRLF"});
	}
	if (header_end > max_header_bytes) {
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::header_too_large,
				.message = std::format("response headers exceed {} bytes", max_header_bytes)});
	}
	tel.ttfb = std::chrono::steady_clock::now() - t_ttfb;
	auto const headers_str = std::string_view{raw}.substr(0, header_end);
	auto parsed_head = client_wire::parse_http1_response_head(headers_str, max_body_bytes);
	if (!parsed_head) {
		return std::unexpected(std::move(parsed_head).error());
	}
	return BlockingResponseHeadRead{
		.raw = std::move(raw),
		.header_end = header_end,
		.parsed = std::move(*parsed_head),
	};
}

// Core blocking transport — returns ClientResult.
ClientResult do_blocking_request(
	conflux::http::ClientRequest const &req,
	HttpClientOptions const &opts) {
	auto const timeouts = detail::effective_http_timeouts(req.timeouts(), opts.default_timeouts);
	HttpTelemetry tel{};

	auto opened_conn = open_blocking_connection(req, opts, timeouts, tel);
	if (!opened_conn) {
		return std::unexpected(std::move(opened_conn).error());
	}
	auto conn = std::move(*opened_conn);

	int const write_sec = to_sec(timeouts.write);
	if (auto write_error = send_blocking_http1_request(conn, req, opts, write_sec, tel)) {
		close_conn(conn);
		return std::unexpected(std::move(*write_error));
	}

	// Receive response headers.
	int const first_byte_sec = to_sec(timeouts.first_byte);
	int const between_sec = to_sec(timeouts.between_bytes);
	std::size_t const max_hdr = opts.max_header_bytes;
	std::size_t const max_body = opts.max_body_bytes;
	std::size_t const max_buf = opts.max_buffered_bytes;

	auto received_head = receive_blocking_http1_response_head(conn, first_byte_sec, max_hdr, max_body, tel);
	if (!received_head) {
		close_conn(conn);
		return std::unexpected(std::move(received_head).error());
	}
	auto raw = std::move(received_head->raw);
	auto const header_end = received_head->header_end;
	auto &parsed_head = received_head->parsed;
	auto const content_length = parsed_head.content_length;
	auto const has_content_length = parsed_head.has_content_length;
	auto const chunked = parsed_head.chunked;
	ClientResponse response;
	response.head.status = parsed_head.status;
	response.head.status_text = std::move(parsed_head.status_text);
	response.head.headers = std::move(parsed_head.headers);
	response.head.set_cookies = std::move(parsed_head.set_cookies);

	// Receive body.
	std::size_t const body_offset = header_end + 4;
	std::size_t const initial_body_bytes = raw.size() - body_offset;
	tel.bytes_received += raw.size();
	raw.erase(0, body_offset);
	response.body = std::move(raw);
	if (has_content_length && response.body.size() > content_length) {
		response.body.resize(content_length);
	}
	std::size_t const counted_initial_body_bytes =
		has_content_length ? std::min(initial_body_bytes, content_length) : initial_body_bytes;

	auto t_body = std::chrono::steady_clock::now();
	if (req.method() == "HEAD") {
		response.body.clear();
	} else if (chunked) {
		std::string decoded;
		bool too_large = false;
		if (!recv_chunked(conn, response.body, decoded, between_sec, max_body, max_buf, too_large)) {
			close_conn(conn);
			if (too_large) {
				return std::unexpected(
					HttpError{
						.kind = HttpErrorKind::body_too_large,
						.message = std::format("chunked body exceeds limit {}", max_body)});
			}
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::read,
					.phase = HttpPhase::between_bytes,
					.os_errno = errno,
					.message = "failed to receive chunked body"});
		}
		tel.bytes_received += decoded.size();
		response.body = std::move(decoded);
	} else if (has_content_length && content_length > response.body.size()) {
		if (!recv_exact(conn, response.body, between_sec, content_length, max_body)) {
			close_conn(conn);
			if (response.body.size() >= max_body) {
				return std::unexpected(
					HttpError{
						.kind = HttpErrorKind::body_too_large,
						.message = std::format("body exceeds limit {}", max_body)});
			}
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::read,
					.phase = HttpPhase::between_bytes,
					.os_errno = errno,
					.message = "failed to receive body"});
		}
		tel.bytes_received += content_length - counted_initial_body_bytes;
	} else if (!has_content_length && !chunked) {
		bool too_large = false;
		recv_to_eof(conn, response.body, between_sec, max_body, too_large);
		if (too_large) {
			close_conn(conn);
			return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::body_too_large,
					.message = std::format("EOF-delimited body exceeds limit {}", max_body)});
		}
		tel.bytes_received += response.body.size();
	}
	tel.body = std::chrono::steady_clock::now() - t_body;

	close_conn(conn);
	response.telemetry = tel;
	return response;
}
ClientStreamResult do_blocking_request_streaming(
	conflux::http::ClientRequest const &req,
	HttpClientOptions const &opts,
	ClientBodyChunkSink const &sink) {
	auto const timeouts = detail::effective_http_timeouts(req.timeouts(), opts.default_timeouts);
	HttpTelemetry tel{};

	auto opened_conn = open_blocking_connection(req, opts, timeouts, tel);
	if (!opened_conn) {
		return std::unexpected(std::move(opened_conn).error());
	}
	auto conn = std::move(*opened_conn);

	int const write_sec = to_sec(timeouts.write);
	if (auto write_error = send_blocking_http1_request(conn, req, opts, write_sec, tel)) {
		close_conn(conn);
		return std::unexpected(std::move(*write_error));
	}

	int const first_byte_sec = to_sec(timeouts.first_byte);
	int const between_sec = to_sec(timeouts.between_bytes);
	std::size_t const max_hdr = opts.max_header_bytes;
	std::size_t const max_body = opts.max_body_bytes;
	std::size_t const max_buf = opts.max_buffered_bytes;

	auto received_head = receive_blocking_http1_response_head(conn, first_byte_sec, max_hdr, max_body, tel);
	if (!received_head) {
		close_conn(conn);
		return std::unexpected(std::move(received_head).error());
	}
	auto raw = std::move(received_head->raw);
	auto const header_end = received_head->header_end;
	auto &parsed_head = received_head->parsed;
	ClientStreamResponse response;
	response.head.status = parsed_head.status;
	response.head.status_text = std::move(parsed_head.status_text);
	response.head.headers = std::move(parsed_head.headers);
	response.head.set_cookies = std::move(parsed_head.set_cookies);

	auto const content_length = parsed_head.content_length;
	auto const has_content_length = parsed_head.has_content_length;
	auto const chunked = parsed_head.chunked;
	std::size_t const body_offset = header_end + 4;
	tel.bytes_received += raw.size();
	raw.erase(0, body_offset);
	if (has_content_length && raw.size() > content_length) {
		raw.resize(content_length);
	}

	auto t_body = std::chrono::steady_clock::now();
	if (req.method() != "HEAD") {
		if (chunked) {
			if (auto streamed =
					stream_chunked_body(conn, raw, sink, between_sec, max_body, max_buf, tel.bytes_received);
				!streamed) {
				close_conn(conn);
				return std::unexpected(std::move(streamed).error());
			}
		} else if (has_content_length) {
			if (auto streamed = stream_content_length_body(
					conn,
					raw,
					sink,
					between_sec,
					content_length,
					max_body,
					tel.bytes_received);
				!streamed) {
				close_conn(conn);
				return std::unexpected(std::move(streamed).error());
			}
		} else {
			if (auto streamed = stream_eof_body(conn, raw, sink, between_sec, max_body, tel.bytes_received);
				!streamed) {
				close_conn(conn);
				return std::unexpected(std::move(streamed).error());
			}
		}
	}
	tel.body = std::chrono::steady_clock::now() - t_body;
	close_conn(conn);
	response.telemetry = tel;
	return response;
}

} // namespace client_detail
// ─── HttpClient ───────────────────────────────────────────────────────────────

export namespace conflux::http {

class HttpClient {
	HttpClientOptions opts_;

public:
	explicit HttpClient(
		HttpClientOptions opts = {})
		: opts_{std::move(opts)} {}
	[[nodiscard]] HttpClientOptions const &options() const noexcept { return opts_; }
	[[nodiscard]] ClientResult blocking_send(
		ClientRequest const &req) const {
		auto effective_opts = opts_;
		ClientRequest current = req;
		HttpTelemetry total_tel{};
		for (;;) {
			auto result = client_detail::do_blocking_request(current, effective_opts);
			if (!result) {
				return result;
			}
			client_wire::accumulate_telemetry(total_tel, result->telemetry);
			auto next = client_detail::follow_redirect(current, *result);
			if (!next) {
				return std::unexpected(next.error());
			}
			if (!next->has_value()) {
				result->telemetry = std::move(total_tel);
				return result;
			}
			auto redirected = std::move(**next);
			if (!client_wire::same_origin(current.url(), redirected.url())) {
				client_wire::strip_redirect_sensitive_request_headers(effective_opts.default_headers);
			}
			current = std::move(redirected);
		}
	}
	[[nodiscard]] ClientStreamResult blocking_send_streaming(
		ClientRequest const &req,
		ClientBodyChunkSink sink) const {
		return client_detail::do_blocking_request_streaming(req, opts_, sink);
	}
};

} // namespace conflux::http
