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
namespace wroot = conflux::work::root;
using TP = std::chrono::steady_clock::time_point;

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
	TlsError const &e) {
	auto q = tls_error_string();
	return q.empty() || q == "TLS error" ? std::string{e.what()} : q;
}
#endif
struct PlainStreamRef {
	TcpStream &s;
	std::shared_ptr<ActiveTaskCancelRelay> cancel;
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
	TcpTlsStream &s;
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
	if (!client_wire::is_redirect_status(resp.head.status)) {
		return std::nullopt;
	}
	auto const location = resp.head.headers["location"];
	if (location.empty()) {
		return std::nullopt;
	}
	if (!req.follows_redirects()) {
		return std::nullopt;
	}
	if (req.max_redirects() <= 0) {
		return std::unexpected(HttpError{.kind = HttpErrorKind::redirect_limit, .message = "redirect limit exceeded"});
	}
	auto next_url = client_wire::resolve_redirect_target(req.url(), location);
	if (!next_url) {
		return std::nullopt;
	}
	bool const cross_origin = !client_wire::same_origin(req.url(), *next_url);
	HttpFields next_headers{req.headers().case_insensitive()};
	next_headers.clear();
	for (auto const &[k, v]: req.headers()) {
		if (ascii_iequals(k, "host")) {
			continue;
		}
		if (cross_origin
			&& (ascii_iequals(k, "authorization")
				|| ascii_iequals(k, "cookie")
				|| ascii_iequals(k, "proxy-authorization"))) {
			continue;
		}
		next_headers.set(k, v);
	}
	auto builder = ClientRequest::method(req.method(), next_url->str())
					   .headers(next_headers)
					   .timeouts(req.timeouts())
					   .verify_peer(req.verify_peer());
	if (!req.body().empty()) {
		builder.body(req.body());
	}
	if (!req.server_name().empty()) {
		builder.server_name(req.server_name());
	}
	builder.follow_redirects(req.max_redirects() - 1);
	return std::move(builder).build();
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
	std::size_t consumed = 0;
	std::array<std::uint8_t, 4096> tmp{};
	for (;;) {
		auto const st = client_wire::decode_chunked_prefix(encoded, decoded, consumed);
		if (st == client_wire::ChunkedDecodeStatus::complete) {
			co_return true;
		}
		if (st == client_wire::ChunkedDecodeStatus::invalid) {
			co_return false;
		}
		if (decoded.size() > cap || encoded.size() > buf_cap) {
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
struct HappyConnectState {
	std::atomic<bool> won{false};
	std::atomic<bool> fast_fail{false};
	std::atomic<bool> cancelled{false};
	std::atomic<int> pending{0};
	std::mutex m;
	std::vector<wroot::TaskControl> attempts;
	void register_attempt(
		wroot::TaskControl c) {
		std::optional<wroot::TaskControl> cancel_now;
		{
			std::lock_guard lk{m};
			if (cancelled.load(std::memory_order_acquire)) {
				cancel_now = c;
			} else {
				attempts.push_back(std::move(c));
			}
		}
		if (cancel_now) {
			auto _ = cancel_now->request_cancel();
		}
	}
	void cancel_all() noexcept {
		std::vector<wroot::TaskControl> copy;
		{
			std::lock_guard lk{m};
			cancelled.store(true, std::memory_order_release);
			copy = attempts;
		}
		for (auto &c: copy) {
			auto _ = c.request_cancel();
		}
	}
};
wroot::Task<void> happy_attempt(
	SocketTaskRing &ring,
	int fam,
	sockaddr_storage ss,
	socklen_t addr_len,
	ConnectOptions copts,
	std::shared_ptr<HappyConnectState> hs,
	std::shared_ptr<wroot::TaskSource<TcpStream>> winner_src) {
	try {
		auto connect_task = async_tcp_connect(ring, fam, ss, addr_len, copts);
		hs->register_attempt(connect_task.control());
		auto s = co_await std::move(connect_task);
		bool won = false;
		if (hs->won.compare_exchange_strong(won, true, std::memory_order_acq_rel)) {
			auto _ = winner_src->try_set_value(wroot::Success<TcpStream>{std::move(s)});
		}
	} catch (wroot::CancelledError const &) {
	} catch (...) { hs->fast_fail.store(true, std::memory_order_release); }
	int const left = hs->pending.fetch_sub(1, std::memory_order_acq_rel) - 1;
	if (left == 0 && !hs->won.load(std::memory_order_acquire) && !hs->cancelled.load(std::memory_order_acquire)) {
		auto _ = winner_src->try_set_exception(
			std::make_exception_ptr(IoError{ECONNREFUSED, "connect: all endpoints failed"}));
	}
}
wroot::Task<TcpStream> staggered_parallel_connect(
	SocketTaskRing &ring,
	std::vector<client_dns_bridge::Endpoint> const &endpoints,
	ConnectOptions copts) {
	constexpr std::chrono::milliseconds kStagger{250};
	auto hs = std::make_shared<HappyConnectState>();
	hs->pending.store(static_cast<int>(endpoints.size()), std::memory_order_relaxed);
	auto [task, raw_src] = wroot::make_task_source<TcpStream>(wroot::SubmitOptions{.enable_cancellation = true});
	auto winner_src = std::make_shared<wroot::TaskSource<TcpStream>>(std::move(raw_src));
	std::weak_ptr<wroot::TaskSource<TcpStream>> weak_src{winner_src};
	auto _ = winner_src->install_cancel_hook([hs, weak_src](wroot::CancelReason) noexcept {
		hs->cancel_all();
		if (auto src = weak_src.lock()) {
			auto _ = src->try_set_cancelled();
		}
	});
	for (std::size_t i = 0; i < endpoints.size(); ++i) {
		if (hs->cancelled.load(std::memory_order_acquire)) {
			break;
		}
		auto const &ep = endpoints[i];
		sockaddr_storage ss{};
		memcpy(&ss, ep.addr, ep.addr_len);
		int const fam = (ep.family == 6) ? AF_INET6 : AF_INET;
		happy_attempt(ring, fam, ss, static_cast<socklen_t>(ep.addr_len), copts, hs, winner_src).detach();
		if (i + 1 < endpoints.size()) {
			hs->fast_fail.store(false, std::memory_order_relaxed);
			auto const t_stagger = std::chrono::steady_clock::now() + kStagger;
			while (!hs->fast_fail.load(std::memory_order_acquire)
				   && !hs->won.load(std::memory_order_acquire)
				   && !hs->cancelled.load(std::memory_order_acquire)) {
				auto const now = std::chrono::steady_clock::now();
				if (now >= t_stagger) {
					break;
				}
				auto const rem = std::chrono::ceil<std::chrono::milliseconds>(t_stagger - now);
				co_await async_sleep_for(ring, std::min(std::chrono::milliseconds{10}, rem));
			}
			if (hs->won.load(std::memory_order_acquire) || hs->cancelled.load(std::memory_order_acquire)) {
				break;
			}
		}
	}
	co_return co_await std::move(task);
}
wroot::Task<ClientResult> do_async_request(
	SocketTaskRing &ring,
	ClientRequest const &req,
	HttpClientOptions const &opts,
	std::shared_ptr<ActiveTaskCancelRelay> cancel) {
	auto const &url = req.url();
	constexpr HttpTimeouts kDef{};
	auto const &rt = req.timeouts();
	auto const &cd = opts.default_timeouts;
	HttpTimeouts const timeouts{
		.resolve = rt.resolve != kDef.resolve ? rt.resolve : cd.resolve,
		.connect = rt.connect != kDef.connect ? rt.connect : cd.connect,
		.tls = rt.tls != kDef.tls ? rt.tls : cd.tls,
		.write = rt.write != kDef.write ? rt.write : cd.write,
		.first_byte = rt.first_byte != kDef.first_byte ? rt.first_byte : cd.first_byte,
		.between_bytes = rt.between_bytes != kDef.between_bytes ? rt.between_bytes : cd.between_bytes,
	};
	HttpTelemetry tel{};
	std::vector<client_dns_bridge::Endpoint> endpoints;
	auto const t0 = std::chrono::steady_clock::now();
	cancel->throw_if_cancelled();
	if (opts.resolver) {
		std::array<char, 256> errbuf{};
		auto *ctx = &endpoints;
		client_dns_bridge::resolve(
			opts.resolver,
			url.host.data(),
			url.host.size(),
			static_cast<std::uint16_t>(url.port),
			timeouts.resolve.count() > 0 ? timeouts.resolve.count() : 30000LL,
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
			for (auto const *ai = res_raw; ai; ai = ai->ai_next) {
				client_dns_bridge::Endpoint ep{};
				memcpy(ep.addr, ai->ai_addr, std::min(sizeof(ep.addr), static_cast<std::size_t>(ai->ai_addrlen)));
				ep.addr_len = static_cast<unsigned>(ai->ai_addrlen);
				ep.family = (ai->ai_family == AF_INET6) ? 6 : 4;
				endpoints.push_back(ep);
			}
			::freeaddrinfo(res_raw);
		}
	}
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
	cancel->throw_if_cancelled();
	tel.connect = std::chrono::steady_clock::now() - t1;
	bool const is_tls = (url.scheme == "https");
#if CONFLUX_HAS_TLS
	std::optional<TcpTlsStream> tls_stream;
	if (is_tls) {
		bool const verify = req.verify_peer() && opts.verify_peer;
		auto const sni_sv = req.server_name().empty() ? std::string_view{url.host} : req.server_name();
		TlsContext tls_ctx;
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
		} catch (wroot::CancelledError const &) { throw; } catch (TlsError const &e) {
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
			co_await tr.write(
				std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const *>(wire.data()), wire.size()});
			if (!req.body().empty()) {
				co_await tr.write(
					std::span<std::uint8_t const>{
						reinterpret_cast<std::uint8_t const *>(req.body().data()),
						req.body().size()});
			}
		} else
#endif
		{
			PlainStreamRef pr{stream, cancel, timeouts.between_bytes, timeouts.write};
			co_await pr.write(
				std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const *>(wire.data()), wire.size()});
			if (!req.body().empty()) {
				co_await pr.write(
					std::span<std::uint8_t const>{
						reinterpret_cast<std::uint8_t const *>(req.body().data()),
						req.body().size()});
			}
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
	ClientResponse response;
	auto const nl = headers_str.find("\r\n");
	auto const status_line = (nl != std::string_view::npos) ? headers_str.substr(0, nl) : headers_str;
	auto const sp1 = status_line.find(' ');
	if (sp1 == std::string_view::npos) {
		co_return std::unexpected(HttpError{.kind = HttpErrorKind::protocol, .message = "malformed status line"});
	}
	auto const rest = status_line.substr(sp1 + 1);
	auto const sp2 = rest.find(' ');
	auto const code_sv = (sp2 != std::string_view::npos) ? rest.substr(0, sp2) : rest;
	int status = 0;
	auto const [ptr, ec] = std::from_chars(code_sv.data(), code_sv.data() + code_sv.size(), status);
	if (ec != std::errc{} || status < 100 || status > 999) {
		co_return std::unexpected(
			HttpError{.kind = HttpErrorKind::protocol, .message = std::format("invalid status code '{}'", code_sv)});
	}
	response.head.status = status;
	if (sp2 != std::string_view::npos) {
		response.head.status_text = std::string{rest.substr(sp2 + 1)};
	}
	std::size_t content_length = 0;
	bool has_content_length = false;
	bool chunked = false;
	std::size_t pos = (nl != std::string_view::npos) ? nl + 2 : headers_str.size();
	while (pos < headers_str.size()) {
		auto const end = headers_str.find("\r\n", pos);
		auto const hdr = (end != std::string_view::npos) ? headers_str.substr(pos, end - pos) : headers_str.substr(pos);
		auto const colon = hdr.find(':');
		if (colon != std::string_view::npos) {
			auto k = hdr.substr(0, colon);
			auto v = hdr.substr(colon + 1);
			while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) {
				v.remove_prefix(1);
			}
			if (ascii_iequals(k, "content-length")) {
				std::from_chars(v.data(), v.data() + v.size(), content_length);
				has_content_length = true;
			} else if (ascii_iequals(k, "transfer-encoding") && header_token_contains(v, "chunked")) {
				chunked = true;
			} else if (ascii_iequals(k, "set-cookie")) {
				response.head.set_cookies.push_back(std::string{v});
			} else if (!conflux::http::is_hop_by_hop_header(k)) {
				response.head.headers.set(std::string{k}, std::string{v});
			}
		}
		pos = (end != std::string_view::npos) ? end + 2 : headers_str.size();
	}
	if (has_content_length && content_length > max_body_sz) {
		co_return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::body_too_large,
				.message = std::format("Content-Length {} exceeds limit {}", content_length, max_body_sz)});
	}
	std::size_t const body_offset = header_end + 4;
	std::size_t const initial_body_bytes = raw.size() - body_offset;
	raw.erase(0, body_offset);
	response.body = std::move(raw);
	auto do_body = [&]() -> wroot::Task<std::optional<HttpError>> {
#if CONFLUX_HAS_TLS
		if (tls_stream) {
			TlsStreamRef tr{*tls_stream, timeouts.between_bytes, timeouts.write};
			if (req.method() == "HEAD") {
				response.body.clear();
			} else if (chunked) {
				std::string decoded;
				bool too_large = false;
				try {
					if (!co_await async_recv_chunked(tr, response.body, decoded, max_body_sz, max_buf, too_large)) {
						if (too_large)
							co_return HttpError{
								.kind = HttpErrorKind::body_too_large,
								.message = std::format("chunked body exceeds limit {}", max_body_sz)};
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
			} else if (has_content_length && content_length > response.body.size()) {
				try {
					if (!co_await async_recv_exact(tr, response.body, content_length, max_body_sz)) {
						if (response.body.size() >= max_body_sz)
							co_return HttpError{
								.kind = HttpErrorKind::body_too_large,
								.message = std::format("body exceeds limit {}", max_body_sz)};
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
				tel.bytes_received += content_length - initial_body_bytes;
			} else if (!has_content_length && !chunked) {
				bool too_large = false;
				try {
					co_await async_recv_to_eof(tr, response.body, max_body_sz, too_large);
				} catch (IoError const &e) {
					co_return HttpError{
						.kind = HttpErrorKind::read,
						.phase = HttpPhase::between_bytes,
						.os_errno = e.code().value(),
						.message = "timed out receiving body"};
				}
				if (too_large)
					co_return HttpError{
						.kind = HttpErrorKind::body_too_large,
						.message = std::format("EOF-delimited body exceeds limit {}", max_body_sz)};
				tel.bytes_received += response.body.size();
			}
			co_return std::nullopt;
		}
#endif
		{
			PlainStreamRef pr{stream, cancel, timeouts.between_bytes, timeouts.write};
			if (req.method() == "HEAD") {
				response.body.clear();
			} else if (chunked) {
				std::string decoded;
				bool too_large = false;
				try {
					if (!co_await async_recv_chunked(pr, response.body, decoded, max_body_sz, max_buf, too_large)) {
						if (too_large)
							co_return HttpError{
								.kind = HttpErrorKind::body_too_large,
								.message = std::format("chunked body exceeds limit {}", max_body_sz)};
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
			} else if (has_content_length && content_length > response.body.size()) {
				try {
					if (!co_await async_recv_exact(pr, response.body, content_length, max_body_sz)) {
						if (response.body.size() >= max_body_sz)
							co_return HttpError{
								.kind = HttpErrorKind::body_too_large,
								.message = std::format("body exceeds limit {}", max_body_sz)};
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
				tel.bytes_received += content_length - initial_body_bytes;
			} else if (!has_content_length && !chunked) {
				bool too_large = false;
				try {
					co_await async_recv_to_eof(pr, response.body, max_body_sz, too_large);
				} catch (IoError const &e) {
					co_return HttpError{
						.kind = HttpErrorKind::read,
						.phase = HttpPhase::between_bytes,
						.os_errno = e.code().value(),
						.message = "timed out receiving body"};
				}
				if (too_large)
					co_return HttpError{
						.kind = HttpErrorKind::body_too_large,
						.message = std::format("EOF-delimited body exceeds limit {}", max_body_sz)};
				tel.bytes_received += response.body.size();
			}
			co_return std::nullopt;
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
	std::shared_ptr<ActiveTaskCancelRelay> cancel) {
	try {
		ClientRequest current = req;
		HttpTelemetry total_tel{};
		for (;;) {
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
	auto [out, raw_src] = wroot::make_task_source<ClientResult>(wroot::SubmitOptions{.enable_cancellation = true});
	auto src = std::make_shared<wroot::TaskSource<ClientResult>>(std::move(raw_src));
	auto cancel = std::make_shared<ActiveTaskCancelRelay>();
	std::weak_ptr<wroot::TaskSource<ClientResult>> weak_src{src};
	auto _ = src->install_cancel_hook([cancel, weak_src](wroot::CancelReason) noexcept {
		cancel->cancel();
		if (auto src = weak_src.lock()) {
			auto _ = src->try_set_cancelled();
		}
	});
	auto driver = async_detail::run_async_request_driver(ring, req, client.options(), src, cancel);
	std::move(driver).detach();
	return std::move(out);
}

} // namespace conflux::http
