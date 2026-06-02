module;
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#if CONFLUX_HAS_TLS
	#include <openssl/err.h>
	#include <openssl/ssl.h>
	#include <openssl/x509.h>
#endif

module conflux.net.async_client;
import std;
import conflux.types;
import conflux.net.http.parse_helpers;
import conflux.net.http.types;
import conflux.net.http.request;
import conflux.net.client_wire;
import conflux.utils;
import conflux.work;
import conflux.uring.completion;
import conflux.socket_io;
import conflux.socket_io.coro;
import conflux.net.client;
import conflux.net.cancel;
import conflux.dns_bridge;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
namespace async_detail {

using namespace conflux::http;
using namespace conflux::socket_io;
using conflux::IoError;
namespace wroot = conflux::work::root;
using TP = std::chrono::steady_clock::time_point;

constexpr std::size_t kClientMaxChunkCount = 100000;

[[nodiscard]] std::span<std::uint8_t const> byte_view(
	std::string_view text) noexcept {
	return {reinterpret_cast<std::uint8_t const *>(text.data()), text.size()};
}

struct AddrInfoDeleter {
	void operator ()(
		addrinfo *p) const noexcept {
		::freeaddrinfo(p);
	}
};

#if CONFLUX_HAS_TLS
[[nodiscard]] std::string tls_error_string() {
	std::string out;
	unsigned long e;
	while ((e = ERR_get_error()) != 0) {
		std::array<char, 256> buf{};
		ERR_error_string_n(e, buf.data(), buf.size());
		if (!out.empty()) {
			out += "; ";
		}
		out += buf.data();
	}
	return out.empty() ? "TLS error" : out;
}
[[nodiscard]] std::string tls_error_string(
	conflux::net_tls::TlsError const &e) {
	auto q = tls_error_string();
	return q.empty() || q == "TLS error" ? std::string{e.what()} : q;
}
#endif
struct PlainStreamRef {
	TcpStream &s;
	std::shared_ptr<conflux::net::detail::ActiveTaskCancelRelay> cancel;
	std::chrono::milliseconds per_recv;
	std::chrono::milliseconds per_write;
	[[nodiscard]] wroot::Task<std::size_t> recv(
		std::span<std::uint8_t> buf) {
		auto child = per_recv.count() <= 0 ? s.async_recv_borrowed(buf) : s.async_recv_borrowed(buf, per_recv);
		return cancel->await_child(std::move(child));
	}
	[[nodiscard]] wroot::Task<std::size_t> recv(
		std::span<std::uint8_t> buf,
		std::chrono::milliseconds t) {
		auto child = t.count() <= 0 ? s.async_recv_borrowed(buf) : s.async_recv_borrowed(buf, t);
		return cancel->await_child(std::move(child));
	}
	[[nodiscard]] wroot::Task<void> write(
		std::span<std::uint8_t const> buf) {
		std::size_t sent = 0;
		while (sent < buf.size()) {
			cancel->throw_if_cancelled();
			auto child =
				s.async_write_borrowed(std::span<std::uint8_t const>{buf.data() + sent, buf.size() - sent}, per_write);
			std::size_t const n = co_await cancel->await_child(std::move(child));
			if (n == 0) {
				throw IoError{ECONNRESET, "tcp: connection closed"};
			}
			sent += n;
		}
	}
};
#if CONFLUX_HAS_TLS
struct TlsStreamRef {
	conflux::net_tls::TcpTlsStream &s;
	std::chrono::milliseconds per_recv;
	std::chrono::milliseconds per_write;
	[[nodiscard]] wroot::Task<std::size_t> recv(
		std::span<std::uint8_t> buf) {
		return s.read_some(buf, per_recv);
	}
	[[nodiscard]] wroot::Task<std::size_t> recv(
		std::span<std::uint8_t> buf,
		std::chrono::milliseconds t) {
		return s.read_some(buf, t);
	}
	[[nodiscard]] wroot::Task<void> write(
		std::span<std::uint8_t const> buf) {
		return s.write_all(buf, per_write);
	}
};
#endif
[[nodiscard]] std::expected<std::optional<ClientRequest>, HttpError> follow_redirect(
	ClientRequest const &req,
	ClientResponse const &resp) {
	return client_wire::follow_redirect_request(req, resp.head.status, resp.head.headers);
}

template<typename T>
wroot::Task<std::string> async_recv_until(
	T &t,
	std::string_view delim,
	std::size_t max_size,
	TP deadline) {
	std::string buf;
	buf.reserve(std::min<std::size_t>(4096, max_size));
	std::array<std::uint8_t, 4096> tmp{};
	while (buf.size() < max_size) {
		std::size_t n;
		try {
			if (deadline == TP::max()) {
				n = co_await t.recv(std::span<std::uint8_t>{tmp.data(), tmp.size()}, std::chrono::milliseconds{0});
			} else {
				auto const now = std::chrono::steady_clock::now();
				if (now >= deadline) {
					throw IoError{ETIMEDOUT, "tcp: recv timed out"};
				}
				n = co_await t.recv(
					std::span<std::uint8_t>{tmp.data(), tmp.size()},
					std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
			}
		} catch (IoError const &) { throw; } catch (...) {
			throw;
		}
		if (n == 0) {
			break;
		}
		buf.append(reinterpret_cast<char const *>(tmp.data()), n);
		if (buf.find(delim) != std::string::npos) {
			break;
		}
	}
	co_return buf;
}
template<typename T>
wroot::Task<bool> async_recv_exact(
	T &t,
	std::string &out,
	std::size_t target,
	std::size_t cap) {
	std::array<std::uint8_t, 4096> tmp{};
	while (out.size() < target) {
		if (out.size() >= cap) {
			co_return false;
		}
		auto const want = std::min(tmp.size(), target - out.size());
		std::size_t n;
		try {
			n = co_await t.recv(std::span<std::uint8_t>{tmp.data(), want});
		} catch (IoError const &) { throw; } catch (...) {
			throw;
		}
		if (n == 0) {
			co_return false;
		}
		out.append(reinterpret_cast<char const *>(tmp.data()), n);
	}
	co_return true;
}
template<typename T>
wroot::Task<void> async_recv_to_eof(
	T &t,
	std::string &out,
	std::size_t cap,
	bool &too_large) {
	too_large = false;
	std::array<std::uint8_t, 4096> tmp{};
	for (;;) {
		std::size_t n;
		try {
			n = co_await t.recv(std::span<std::uint8_t>{tmp.data(), tmp.size()});
		} catch (IoError const &) { throw; } catch (...) {
			throw;
		}
		if (n == 0) {
			break;
		}
		out.append(reinterpret_cast<char const *>(tmp.data()), n);
		if (out.size() > cap) {
			too_large = true;
			co_return;
		}
	}
}
template<typename T>
wroot::Task<bool> async_recv_chunked(
	T &t,
	std::string &encoded,
	std::string &decoded,
	std::size_t cap,
	std::size_t buf_cap,
	bool &too_large) {
	too_large = false;
	decoded.clear();
	decoded.reserve(std::min(encoded.size(), cap));
	conflux::http::ChunkedDecodeState chunked;
	std::array<std::uint8_t, 4096> tmp{};
	for (;;) {
		auto const rc = conflux::http::decode_chunked_incremental(encoded, 0, cap, kClientMaxChunkCount, chunked);
		if (rc > 0) {
			decoded = std::move(chunked.body);
			co_return true;
		}
		if (rc == -1) {
			co_return false;
		}
		if (rc == -2 || chunked.body.size() > cap || encoded.size() > buf_cap) {
			too_large = true;
			co_return false;
		}
		std::size_t n;
		try {
			n = co_await t.recv(std::span<std::uint8_t>{tmp.data(), tmp.size()});
		} catch (IoError const &) { throw; } catch (...) {
			throw;
		}
		if (n == 0) {
			co_return false;
		}
		encoded.append(reinterpret_cast<char const *>(tmp.data()), n);
	}
}
wroot::Task<TcpStream> staggered_parallel_connect(
	SocketTaskRing &ring,
	std::vector<client_dns_bridge::Endpoint> const &endpoints,
	ConnectOptions copts) {
	std::exception_ptr last_error;
	for (auto const &ep: endpoints) {
		sockaddr_storage ss{};
		memcpy(&ss, ep.addr, ep.addr_len);
		int const fam = (ep.family == 6) ? AF_INET6 : AF_INET;
		try {
			co_return co_await async_tcp_connect(ring, fam, ss, static_cast<socklen_t>(ep.addr_len), copts);
		} catch (wroot::CancelledError const &) { throw; } catch (...) {
			last_error = std::current_exception();
		}
	}
	if (last_error) {
		std::rethrow_exception(last_error);
	}
	throw IoError{ECONNREFUSED, "connect: all endpoints failed"};
}

template<typename T>
wroot::Task<void> write_http1_request(
	T &stream,
	std::string_view wire,
	std::string_view body) {
	co_await stream.write(byte_view(wire));
	if (!body.empty()) {
		co_await stream.write(byte_view(body));
	}
}

struct BodyReceiveContext {
	std::string_view method;
	bool chunked{};
	bool has_content_length{};
	std::size_t content_length{};
	std::size_t counted_initial_body_bytes{};
	std::size_t max_body_size{};
	std::size_t max_buffered_size{};
};

template<typename T>
wroot::Task<std::optional<HttpError>> receive_http1_body(
	T &stream,
	ClientResponse &response,
	HttpTelemetry &tel,
	BodyReceiveContext ctx) {
	if (ctx.method == "HEAD") {
		response.body.clear();
	} else if (ctx.chunked) {
		std::string decoded;
		bool too_large = false;
		try {
			if (!co_await async_recv_chunked(
					stream,
					response.body,
					decoded,
					ctx.max_body_size,
					ctx.max_buffered_size,
					too_large)) {
				if (too_large) {
					co_return HttpError{
						.kind = HttpErrorKind::body_too_large,
						.message = std::format("chunked body exceeds limit {}", ctx.max_body_size)};
				}
				co_return HttpError{
					.kind = HttpErrorKind::read,
					.phase = HttpPhase::between_bytes,
					.message = "failed to receive chunked body"};
			}
		} catch (IoError const &e) {
			co_return HttpError{
				.kind = HttpErrorKind::read,
				.phase = HttpPhase::between_bytes,
				.os_errno = e.code().value(),
				.message = "timed out receiving chunked body"};
		}
		tel.bytes_received += decoded.size();
		response.body = std::move(decoded);
	} else if (ctx.has_content_length && ctx.content_length > response.body.size()) {
		try {
			if (!co_await async_recv_exact(stream, response.body, ctx.content_length, ctx.max_body_size)) {
				if (response.body.size() >= ctx.max_body_size) {
					co_return HttpError{
						.kind = HttpErrorKind::body_too_large,
						.message = std::format("body exceeds limit {}", ctx.max_body_size)};
				}
				co_return HttpError{
					.kind = HttpErrorKind::read,
					.phase = HttpPhase::between_bytes,
					.message = "failed to receive body"};
			}
		} catch (IoError const &e) {
			co_return HttpError{
				.kind = HttpErrorKind::read,
				.phase = HttpPhase::between_bytes,
				.os_errno = e.code().value(),
				.message = "timed out receiving body"};
		}
		tel.bytes_received += ctx.content_length - ctx.counted_initial_body_bytes;
	} else if (!ctx.has_content_length && !ctx.chunked) {
		bool too_large = false;
		try {
			co_await async_recv_to_eof(stream, response.body, ctx.max_body_size, too_large);
		} catch (IoError const &e) {
			co_return HttpError{
				.kind = HttpErrorKind::read,
				.phase = HttpPhase::between_bytes,
				.os_errno = e.code().value(),
				.message = "timed out receiving body"};
		}
		if (too_large) {
			co_return HttpError{
				.kind = HttpErrorKind::body_too_large,
				.message = std::format("EOF-delimited body exceeds limit {}", ctx.max_body_size)};
		}
		tel.bytes_received += response.body.size();
	}
	co_return std::nullopt;
}

[[nodiscard]] std::vector<client_dns_bridge::Endpoint> resolve_client_endpoints(
	Url const &url,
	HttpClientOptions const &opts,
	std::chrono::milliseconds resolve_timeout) {
	std::vector<client_dns_bridge::Endpoint> endpoints;
	if (opts.resolver) {
		std::array<char, 256> errbuf{};
		auto *ctx = &endpoints;
		client_dns_bridge::resolve(
			opts.resolver,
			url.host.data(),
			url.host.size(),
			static_cast<std::uint16_t>(url.port),
			resolve_timeout.count() > 0 ? resolve_timeout.count() : 30000LL,
			ctx,
			[](void *c, client_dns_bridge::Endpoint const &ep) noexcept {
				static_cast<std::vector<client_dns_bridge::Endpoint> *>(c)->push_back(ep);
				return true;
			},
			errbuf.data(),
			errbuf.size());
	}
	if (endpoints.empty()) {
		std::string const host_str{url.host};
		std::string const port_str = std::to_string(url.port);
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags = AI_ADDRCONFIG;
		addrinfo *res_raw = nullptr;
		if (::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &res_raw) == 0) {
			std::unique_ptr<addrinfo, AddrInfoDeleter> const res{res_raw};
			for (auto const *ai = res.get(); ai; ai = ai->ai_next) {
				client_dns_bridge::Endpoint ep{};
				memcpy(ep.addr, ai->ai_addr, std::min(sizeof(ep.addr), static_cast<std::size_t>(ai->ai_addrlen)));
				ep.addr_len = static_cast<unsigned>(ai->ai_addrlen);
				ep.family = (ai->ai_family == AF_INET6) ? 6 : 4;
				endpoints.push_back(ep);
			}
		}
	}
	return endpoints;
}

wroot::Task<std::expected<TcpStream, HttpError>> connect_client_stream(
	SocketTaskRing &ring,
	std::vector<client_dns_bridge::Endpoint> const &endpoints,
	ConnectOptions copts,
	Url const &url,
	std::shared_ptr<conflux::net::detail::ActiveTaskCancelRelay> cancel) {
	TcpStream stream;
	try {
		auto connect_task = staggered_parallel_connect(ring, endpoints, copts);
		cancel->set_active(connect_task.control());
		stream = co_await std::move(connect_task);
		cancel->clear_active();
		cancel->throw_if_cancelled();
	} catch (wroot::CancelledError const &) {
		cancel->clear_active();
		throw;
	} catch (IoError const &e) {
		cancel->clear_active();
		cancel->throw_if_cancelled();
		co_return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::connect,
				.phase = HttpPhase::connect,
				.os_errno = e.code().value(),
				.message = std::format("connect to '{}:{}' failed: {}", url.host, url.port, e.what())});
	} catch (...) {
		cancel->clear_active();
		co_return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::connect,
				.phase = HttpPhase::connect,
				.message = std::format("connect to '{}:{}' failed", url.host, url.port)});
	}
	co_return stream;
}

wroot::Task<ClientResult> do_async_request(
	SocketTaskRing &ring,
	ClientRequest const &req,
	HttpClientOptions const &opts,
	std::shared_ptr<conflux::net::detail::ActiveTaskCancelRelay> cancel) {
	auto const &url = req.url();
	auto const timeouts = detail::effective_http_timeouts(req.timeouts(), opts.default_timeouts);
	HttpTelemetry tel{};
	auto const t0 = std::chrono::steady_clock::now();
	cancel->throw_if_cancelled();
	auto endpoints = resolve_client_endpoints(url, opts, timeouts.resolve);
	tel.dns = std::chrono::steady_clock::now() - t0;
	cancel->throw_if_cancelled();
	if (endpoints.empty()) {
		co_return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::dns,
				.phase = HttpPhase::resolve,
				.message = std::format("failed to resolve '{}'", url.host)});
	}
	ConnectOptions copts{};
	copts.timeout = timeouts.connect;
	cancel->throw_if_cancelled();
	auto const t1 = std::chrono::steady_clock::now();
	auto connected = co_await connect_client_stream(ring, endpoints, copts, url, cancel);
	if (!connected) {
		co_return std::unexpected(std::move(connected).error());
	}
	auto stream = std::move(*connected);
	cancel->throw_if_cancelled();
	tel.connect = std::chrono::steady_clock::now() - t1;
	bool const is_tls = (url.scheme == "https");
#if CONFLUX_HAS_TLS
	std::optional<conflux::net_tls::TcpTlsStream> tls_stream;
	if (is_tls) {
		bool const verify = req.verify_peer() && opts.verify_peer;
		auto const sni_sv = req.server_name().empty() ? std::string_view{url.host} : req.server_name();
		conflux::net_tls::TlsContext tls_ctx;
		tls_ctx.set_verify_peer(verify);
		if (verify) {
			if (!opts.ca_bundle_path.empty()) {
				if (!SSL_CTX_load_verify_locations(tls_ctx.native_handle(), opts.ca_bundle_path.c_str(), nullptr)) {
					co_return std::unexpected(
						HttpError{.kind = HttpErrorKind::tls, .phase = HttpPhase::tls, .message = tls_error_string()});
				}
			} else {
				tls_ctx.set_default_verify_paths();
			}
		}
		cancel->throw_if_cancelled();
		tls_stream.emplace(tls_ctx, std::move(stream), cancel);
		if (!tls_stream->set_server_name(sni_sv) || (verify && !tls_stream->set_verify_hostname(sni_sv))) {
			co_return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::tls,
					.phase = HttpPhase::tls,
					.message = "TLS SNI/hostname setup failed"});
		}
		auto const t_tls = std::chrono::steady_clock::now();
		TP const tls_dl = timeouts.tls.count() > 0 ? t_tls + timeouts.tls : TP::max();
		try {
			co_await tls_stream->handshake_connect(tls_dl);
		} catch (wroot::CancelledError const &) { throw; } catch (conflux::net_tls::TlsError const &e) {
			co_return std::unexpected(
				HttpError{.kind = HttpErrorKind::tls, .phase = HttpPhase::tls, .message = tls_error_string(e)});
		} catch (IoError const &e) {
			co_return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::tls,
					.phase = HttpPhase::tls,
					.os_errno = e.code().value(),
					.message = std::format("TLS handshake failed: {}", e.what())});
		}
		if (verify) {
			long const vr = SSL_get_verify_result(tls_stream->native_handle());
			if (vr != X509_V_OK) {
				co_return std::unexpected(
					HttpError{
						.kind = HttpErrorKind::tls,
						.phase = HttpPhase::tls,
						.message = X509_verify_cert_error_string(vr)});
			}
		}
		tel.tls = std::chrono::steady_clock::now() - t_tls;
		tel.tls_verified = verify;
		tel.negotiated_protocol = "https/1.1";
		if (auto const *cipher = SSL_get_current_cipher(tls_stream->native_handle())) {
			tel.tls_cipher = SSL_CIPHER_get_name(cipher);
			tel.tls_version = SSL_CIPHER_get_version(cipher);
		}
	}
#else
	if (is_tls) {
		co_return std::unexpected(
			HttpError{.kind = HttpErrorKind::tls, .phase = HttpPhase::tls, .message = "TLS not compiled in"});
	}
#endif
	std::string const wire = conflux::http::client_wire::build_http1_request_wire(req, opts.default_headers);
	cancel->throw_if_cancelled();
	try {
#if CONFLUX_HAS_TLS
		if (tls_stream) {
			TlsStreamRef tr{*tls_stream, timeouts.between_bytes, timeouts.write};
			co_await write_http1_request(tr, wire, req.body());
		} else
#endif
		{
			PlainStreamRef pr{stream, cancel, timeouts.between_bytes, timeouts.write};
			co_await write_http1_request(pr, wire, req.body());
		}
	} catch (wroot::CancelledError const &) { throw; } catch (IoError const &e) {
		co_return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::write,
				.phase = HttpPhase::write,
				.os_errno = e.code().value(),
				.message = "failed to send request"});
	}
	tel.bytes_sent += wire.size() + req.body().size();
	std::size_t const max_hdr = opts.max_header_bytes;
	std::size_t const max_body_sz = opts.max_body_bytes;
	std::size_t const max_buf = opts.max_buffered_bytes;
	auto const t2 = std::chrono::steady_clock::now();
	TP const first_byte_dl = timeouts.first_byte.count() > 0 ? t2 + timeouts.first_byte : TP::max();
	cancel->throw_if_cancelled();
	std::string raw;
	try {
#if CONFLUX_HAS_TLS
		if (tls_stream) {
			TlsStreamRef tr{*tls_stream, timeouts.between_bytes, timeouts.write};
			raw = co_await async_recv_until(tr, "\r\n\r\n", max_hdr + 4096, first_byte_dl);
		} else
#endif
		{
			PlainStreamRef pr{stream, cancel, timeouts.between_bytes, timeouts.write};
			raw = co_await async_recv_until(pr, "\r\n\r\n", max_hdr + 4096, first_byte_dl);
		}
	} catch (wroot::CancelledError const &) { throw; } catch (IoError const &e) {
		co_return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::read,
				.phase = HttpPhase::first_byte,
				.os_errno = e.code().value(),
				.message = "timed out waiting for response headers"});
	}
	auto const header_end = raw.find("\r\n\r\n");
	if (header_end == std::string::npos) {
		if (raw.size() >= max_hdr) {
			co_return std::unexpected(
				HttpError{
					.kind = HttpErrorKind::header_too_large,
					.message = std::format("response headers exceed {} bytes", max_hdr)});
		}
		co_return std::unexpected(
			HttpError{.kind = HttpErrorKind::protocol, .message = "response headers missing CRLFCRLF"});
	}
	if (header_end > max_hdr) {
		co_return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::header_too_large,
				.message = std::format("response headers exceed {} bytes", max_hdr)});
	}
	tel.bytes_received += raw.size();
	auto const headers_str = std::string_view{raw}.substr(0, header_end);
	auto parsed_head = client_wire::parse_http1_response_head(headers_str, max_body_sz);
	if (!parsed_head) {
		co_return std::unexpected(std::move(parsed_head).error());
	}
	auto const content_length = parsed_head->content_length;
	auto const has_content_length = parsed_head->has_content_length;
	auto const chunked = parsed_head->chunked;
	ClientResponse response;
	response.head.status = parsed_head->status;
	response.head.status_text = std::move(parsed_head->status_text);
	response.head.headers = std::move(parsed_head->headers);
	response.head.set_cookies = std::move(parsed_head->set_cookies);
	std::size_t const body_offset = header_end + 4;
	std::size_t const initial_body_bytes = raw.size() - body_offset;
	raw.erase(0, body_offset);
	response.body = std::move(raw);
	if (has_content_length && response.body.size() > content_length) {
		response.body.resize(content_length);
	}
	std::size_t const counted_initial_body_bytes =
		has_content_length ? std::min(initial_body_bytes, content_length) : initial_body_bytes;
	BodyReceiveContext const body_ctx{
		.method = req.method(),
		.chunked = chunked,
		.has_content_length = has_content_length,
		.content_length = content_length,
		.counted_initial_body_bytes = counted_initial_body_bytes,
		.max_body_size = max_body_sz,
		.max_buffered_size = max_buf,
	};
	auto do_body = [&]() -> wroot::Task<std::optional<HttpError>> {
#if CONFLUX_HAS_TLS
		if (tls_stream) {
			TlsStreamRef tr{*tls_stream, timeouts.between_bytes, timeouts.write};
			co_return co_await receive_http1_body(tr, response, tel, body_ctx);
		}
#endif
		{
			PlainStreamRef pr{stream, cancel, timeouts.between_bytes, timeouts.write};
			co_return co_await receive_http1_body(pr, response, tel, body_ctx);
		}
	};
	if (auto berr = co_await do_body(); berr) {
		co_return std::unexpected(std::move(*berr));
	}
#if CONFLUX_HAS_TLS
	if (tls_stream) {
		try {
			co_await tls_stream->close(timeouts.write);
		} catch (...) {} // NOLINT(bugprone-empty-catch): best-effort TLS close during error/cleanup path.
	} else
#endif
		co_await stream.async_close();
	response.telemetry = tel;
	co_return response;
}
wroot::Task<void> run_async_request_driver(
	SocketTaskRing &ring,
	ClientRequest const &req,
	HttpClientOptions const &opts,
	std::shared_ptr<wroot::TaskSource<ClientResult>> src,
	std::shared_ptr<conflux::net::detail::ActiveTaskCancelRelay> cancel) {
	try {
		ClientRequest current = req;
		HttpTelemetry total_tel{};
		for (;;) {
			if (cancel->is_cancelled()) {
				auto _ = src->try_set_cancelled();
				break;
			}
			auto result = co_await do_async_request(ring, current, opts, cancel);
			if (!result) {
				auto _ = src->try_set_value(wroot::Success<ClientResult>{std::move(result)});
				break;
			}
			client_wire::accumulate_telemetry(total_tel, result->telemetry);
			auto next = follow_redirect(current, *result);
			if (!next) {
				auto _ = src->try_set_value(wroot::Success<ClientResult>{std::unexpected(next.error())});
				break;
			}
			if (!next->has_value()) {
				result->telemetry = std::move(total_tel);
				auto _ = src->try_set_value(wroot::Success<ClientResult>{std::move(result)});
				break;
			}
			current = std::move(**next);
		}
	} catch (wroot::CancelledError const &) { auto _ = src->try_set_cancelled(); } catch (...) {
		auto _ = src->try_set_exception(std::current_exception());
	}
}

} // namespace async_detail
namespace conflux::http {

[[nodiscard]] conflux::work::root::Task<ClientResult> async_send(
	HttpClient const &client,
	SocketTaskRing &ring,
	ClientRequest const &req) {
	namespace wroot = conflux::work::root;
	auto [out, src] = wroot::make_shared_task_source<ClientResult>(wroot::SubmitOptions{.enable_cancellation = true});
	auto cancel = std::make_shared<conflux::net::detail::ActiveTaskCancelRelay>();
	auto _ = src->install_cancel_hook([cancel](wroot::CancelReason) noexcept {
		cancel->cancel();
	});
	auto driver = async_detail::run_async_request_driver(ring, req, client.options(), src, cancel);
	std::move(driver).detach();
	return std::move(out);
}

} // namespace conflux::http
