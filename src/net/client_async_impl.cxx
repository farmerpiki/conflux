module;
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <memory>
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
using TP = chrono::steady_clock::time_point;

#if CONFLUX_HAS_TLS
[[nodiscard]] S tls_error_string() {
	S out;
	unsigned long e;
	while ((e = ERR_get_error()) != 0) {
		A<char, 256> buf{};
		ERR_error_string_n(e, buf.data(), buf.size());
		if (!out.empty()) {
			out += "; ";
		}
		out += buf.data();
	}
	return out.empty() ? "TLS error" : out;
}
[[nodiscard]] S tls_error_string(
	TlsError const &e) {
	auto q = tls_error_string();
	return q.empty() || q == "TLS error" ? S{e.what()} : q;
}
#endif
struct PlainStreamRef {
	TcpStream &s;
	SP<ActiveTaskCancelRelay> cancel;
	chrono::milliseconds per_recv;
	chrono::milliseconds per_write;
	[[nodiscard]] wroot::Task<SZ> recv(
		span<u8> buf) {
		auto child = per_recv.count() <= 0 ? s.recv_borrowed(buf) : s.recv_borrowed(buf, per_recv);
		return cancel->await_child(move(child));
	}
	[[nodiscard]] wroot::Task<SZ> recv(
		span<u8> buf,
		chrono::milliseconds t) {
		auto child = t.count() <= 0 ? s.recv_borrowed(buf) : s.recv_borrowed(buf, t);
		return cancel->await_child(move(child));
	}
	[[nodiscard]] wroot::Task<void> write(
		span<u8 const> buf) {
		SZ sent = 0;
		while (sent < buf.size()) {
			cancel->throw_if_cancelled();
			auto child = s.write_borrowed(span<u8 const>{buf.data() + sent, buf.size() - sent}, per_write);
			SZ const n = co_await cancel->await_child(move(child));
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
	chrono::milliseconds per_recv;
	chrono::milliseconds per_write;
	[[nodiscard]] wroot::Task<SZ> recv(
		span<u8> buf) {
		return s.read_some(buf, per_recv);
	}
	[[nodiscard]] wroot::Task<SZ> recv(
		span<u8> buf,
		chrono::milliseconds t) {
		return s.read_some(buf, t);
	}
	[[nodiscard]] wroot::Task<void> write(
		span<u8 const> buf) {
		return s.write_all(buf, per_write);
	}
};
#endif
[[nodiscard]] S build_host_header(
	Url const &url) {
	bool const default_port = (url.scheme == "http" && url.port == 80) || (url.scheme == "https" && url.port == 443);
	return default_port ? url.host : format("{}:{}", url.host, url.port);
}
[[nodiscard]] bool is_redirect_status(
	int status) noexcept {
	return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
[[nodiscard]] bool same_origin(
	Url const &a,
	Url const &b) noexcept {
	return a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}
[[nodiscard]] Opt<Url> resolve_redirect_target(
	Url const &base,
	SV location) {
	if (location.empty() || location.find_first_of("\r\n") != SV::npos) {
		return nullopt;
	}
	S loc{location};
	auto const frag = loc.find('#');
	if (frag != S::npos) {
		loc.erase(frag);
	}
	if (loc.empty()) {
		return nullopt;
	}
	if (loc.starts_with("//")) {
		auto abs = Url::parse(format("{}:{}", base.scheme, loc));
		return abs ? Opt<Url>{move(*abs)} : nullopt;
	}
	if (auto abs = Url::parse(loc); abs) {
		return move(*abs);
	}
	Url next = base;
	auto const q = loc.find('?');
	if (q != S::npos) {
		next.query = loc.substr(q + 1);
		loc.erase(q);
		if (loc.empty()) {
			return next;
		}
	} else {
		next.query.clear();
	}
	if (loc.starts_with('/')) {
		next.path = move(loc);
		return next;
	}
	S base_path = next.path.empty() ? S{"/"} : next.path;
	auto const slash = base_path.rfind('/');
	if (slash == S::npos) {
		next.path = S{"/"} + loc;
	} else {
		next.path = S{base_path.substr(0, slash + 1)} + loc;
	}
	return next;
}
[[nodiscard]] expected<Opt<HttpRequest>, HttpError> follow_redirect(
	HttpRequest const &req,
	HttpResponse const &resp) {
	if (!is_redirect_status(resp.head.status)) {
		return nullopt;
	}
	auto const location = resp.head.headers["location"];
	if (location.empty()) {
		return nullopt;
	}
	if (req.max_redirects() <= 0) {
		return unexpected(
			HttpError{
				.kind = HttpErrorKind::redirect_limit,
				.message = "redirect limit exceeded"});
	}
	auto next_url = resolve_redirect_target(req.url(), location);
	if (!next_url) {
		return nullopt;
	}
	bool const cross_origin = !same_origin(req.url(), *next_url);
	HttpFields next_headers{req.headers().case_insensitive()};
	next_headers.clear();
	for (auto const &[k, v]: req.headers()) {
		auto const lower = ascii_lower(k);
		if (lower == "host") {
			continue;
		}
		if (cross_origin && (lower == "authorization" || lower == "cookie" || lower == "proxy-authorization")) {
			continue;
		}
		next_headers.set(k, v);
	}
	auto builder = HttpRequest::method(req.method(), next_url->str())
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
	return move(builder).build();
}
void accumulate_telemetry(
	HttpTelemetry &total,
	HttpTelemetry const &hop) {
	total.dns += hop.dns;
	total.connect += hop.connect;
	total.tls += hop.tls;
	total.ttfb += hop.ttfb;
	total.body += hop.body;
	if (hop.pool_wait) {
		total.pool_wait = total.pool_wait ? *total.pool_wait + *hop.pool_wait : hop.pool_wait;
	}
	total.bytes_sent += hop.bytes_sent;
	total.bytes_received += hop.bytes_received;
	total.reused_connection = total.reused_connection || hop.reused_connection;
	if (!hop.negotiated_protocol.empty()) {
		total.negotiated_protocol = hop.negotiated_protocol;
	}
	if (!hop.tls_cipher.empty()) {
		total.tls_cipher = hop.tls_cipher;
	}
	if (!hop.tls_version.empty()) {
		total.tls_version = hop.tls_version;
	}
	total.tls_verified = total.tls_verified || hop.tls_verified;
	if (!hop.peer_addr.empty()) {
		total.peer_addr = hop.peer_addr;
	}
	if (hop.decoded_encoding) {
		total.decoded_encoding = hop.decoded_encoding;
	}
}

enum class ChunkedDecodeStatus : u8 {
	complete,
	incomplete,
	invalid,
};
ChunkedDecodeStatus decode_chunked_prefix(
	SV encoded,
	S &decoded,
	SZ &consumed) {
	for (;;) {
		auto const line_end = encoded.find("\r\n", consumed);
		if (line_end == SV::npos) {
			return ChunkedDecodeStatus::incomplete;
		}
		auto size_str = trim(encoded.substr(consumed, line_end - consumed));
		if (auto const semi = size_str.find(';'); semi != SV::npos) {
			size_str = trim(size_str.substr(0, semi));
		}
		if (size_str.empty()) {
			return ChunkedDecodeStatus::invalid;
		}
		SZ chunk_size = 0;
		auto const parsed = from_chars(size_str.data(), size_str.data() + size_str.size(), chunk_size, 16);
		if (parsed.ec != errc{} || parsed.ptr != size_str.data() + size_str.size()) {
			return ChunkedDecodeStatus::invalid;
		}
		consumed = line_end + 2;
		if (chunk_size == 0) {
			for (;;) {
				auto const eol = encoded.find("\r\n", consumed);
				if (eol == SV::npos) {
					return ChunkedDecodeStatus::incomplete;
				}
				bool const empty = (eol == consumed);
				consumed = eol + 2;
				if (empty) {
					return ChunkedDecodeStatus::complete;
				}
			}
		}
		if (encoded.size() < consumed + chunk_size + 2) {
			return ChunkedDecodeStatus::incomplete;
		}
		decoded.append(encoded.substr(consumed, chunk_size));
		consumed += chunk_size;
		if (encoded.substr(consumed, 2) != "\r\n") {
			return ChunkedDecodeStatus::invalid;
		}
		consumed += 2;
	}
}
template<typename T>
wroot::Task<S> async_recv_until(
	T &t,
	SV delim,
	SZ max,
	TP deadline) {
	S buf;
	buf.reserve(min<SZ>(4096, max));
	A<u8, 4096> tmp{};
	while (buf.size() < max) {
		SZ n;
		try {
			if (deadline == TP::max()) {
				n = co_await t.recv(span<u8>{tmp.data(), tmp.size()}, chrono::milliseconds{0});
			} else {
				auto const now = chrono::steady_clock::now();
				if (now >= deadline) {
					throw IoError{ETIMEDOUT, "tcp: recv timed out"};
				}
				n = co_await t.recv(
					span<u8>{tmp.data(), tmp.size()},
					chrono::ceil<chrono::milliseconds>(deadline - now));
			}
		} catch (IoError const &) { throw; } catch (...) {
			throw;
		}
		if (n == 0) {
			break;
		}
		buf.append(reinterpret_cast<char const *>(tmp.data()), n);
		if (buf.find(delim) != S::npos) {
			break;
		}
	}
	co_return buf;
}
template<typename T>
wroot::Task<bool> async_recv_exact(
	T &t,
	S &out,
	SZ target,
	SZ cap) {
	A<u8, 4096> tmp{};
	while (out.size() < target) {
		if (out.size() >= cap) {
			co_return false;
		}
		auto const want = min(tmp.size(), target - out.size());
		SZ n;
		try {
			n = co_await t.recv(span<u8>{tmp.data(), want});
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
	S &out,
	SZ cap,
	bool &too_large) {
	too_large = false;
	A<u8, 4096> tmp{};
	for (;;) {
		SZ n;
		try {
			n = co_await t.recv(span<u8>{tmp.data(), tmp.size()});
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
	S &encoded,
	S &decoded,
	SZ cap,
	SZ buf_cap,
	bool &too_large) {
	too_large = false;
	decoded.clear();
	SZ consumed = 0;
	A<u8, 4096> tmp{};
	for (;;) {
		auto const st = decode_chunked_prefix(encoded, decoded, consumed);
		if (st == ChunkedDecodeStatus::complete) {
			co_return true;
		}
		if (st == ChunkedDecodeStatus::invalid) {
			co_return false;
		}
		if (decoded.size() > cap || encoded.size() > buf_cap) {
			too_large = true;
			co_return false;
		}
		SZ n;
		try {
			n = co_await t.recv(span<u8>{tmp.data(), tmp.size()});
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
	Atom<bool> won{false};
	Atom<bool> fast_fail{false};
	Atom<bool> cancelled{false};
	Atom<int> pending{0};
	mutex m;
	V<wroot::TaskControl> attempts;
	void register_attempt(
		wroot::TaskControl c) {
		Opt<wroot::TaskControl> cancel_now;
		{
			lock_guard lk{m};
			if (cancelled.load(memory_order_acquire)) {
				cancel_now = c;
			} else {
				attempts.push_back(move(c));
			}
		}
		if (cancel_now) {
			auto _ = cancel_now->request_cancel();
		}
	}
	void cancel_all() noexcept {
		V<wroot::TaskControl> copy;
		{
			lock_guard lk{m};
			cancelled.store(true, memory_order_release);
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
	SP<HappyConnectState> hs,
	SP<wroot::TaskSource<TcpStream>> winner_src) {
	try {
		auto connect_task = tcp_connect(ring, fam, ss, addr_len, copts);
		hs->register_attempt(connect_task.control());
		auto s = co_await move(connect_task);
		bool expected = false;
		if (hs->won.compare_exchange_strong(expected, true, memory_order_acq_rel)) {
			auto _ = winner_src->try_set_value(wroot::Success<TcpStream>{move(s)});
		}
	} catch (wroot::CancelledError const &) {
	} catch (...) { hs->fast_fail.store(true, memory_order_release); }
	int const left = hs->pending.fetch_sub(1, memory_order_acq_rel) - 1;
	if (left == 0 && !hs->won.load(memory_order_acquire) && !hs->cancelled.load(memory_order_acquire)) {
		auto _ =
			winner_src->try_set_exception(make_exception_ptr(IoError{ECONNREFUSED, "connect: all endpoints failed"}));
	}
}
wroot::Task<TcpStream> staggered_parallel_connect(
	SocketTaskRing &ring,
	V<client_dns_bridge::Endpoint> const &endpoints,
	ConnectOptions copts) {
	constexpr chrono::milliseconds kStagger{250};
	auto hs = make_shared<HappyConnectState>();
	hs->pending.store(static_cast<int>(endpoints.size()), memory_order_relaxed);
	auto [task, raw_src] = wroot::make_task_source<TcpStream>(wroot::SubmitOptions{.enable_cancellation = true});
	auto winner_src = make_shared<wroot::TaskSource<TcpStream>>(move(raw_src));
	weak_ptr<wroot::TaskSource<TcpStream>> weak_src{winner_src};
	auto _ = winner_src->install_cancel_hook([hs, weak_src](wroot::CancelReason) noexcept {
		hs->cancel_all();
		if (auto src = weak_src.lock()) {
			auto _ = src->try_set_cancelled();
		}
	});
	for (SZ i = 0; i < endpoints.size(); ++i) {
		if (hs->cancelled.load(memory_order_acquire)) {
			break;
		}
		auto const &ep = endpoints[i];
		sockaddr_storage ss{};
		memcpy(&ss, ep.addr, ep.addr_len);
		int const fam = (ep.family == 6) ? AF_INET6 : AF_INET;
		happy_attempt(ring, fam, ss, static_cast<socklen_t>(ep.addr_len), copts, hs, winner_src).detach();
		if (i + 1 < endpoints.size()) {
			hs->fast_fail.store(false, memory_order_relaxed);
			auto const t_stagger = chrono::steady_clock::now() + kStagger;
			while (!hs->fast_fail.load(memory_order_acquire)
				   && !hs->won.load(memory_order_acquire)
				   && !hs->cancelled.load(memory_order_acquire)) {
				auto const now = chrono::steady_clock::now();
				if (now >= t_stagger) {
					break;
				}
				auto const rem = chrono::ceil<chrono::milliseconds>(t_stagger - now);
				co_await sleep_for(ring, min(chrono::milliseconds{10}, rem));
			}
			if (hs->won.load(memory_order_acquire) || hs->cancelled.load(memory_order_acquire)) {
				break;
			}
		}
	}
	co_return co_await move(task);
}
wroot::Task<HttpResult> do_async_request(
	SocketTaskRing &ring,
	HttpRequest const &req,
	HttpClientOptions const &opts,
	SP<ActiveTaskCancelRelay> cancel) {
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
	V<client_dns_bridge::Endpoint> endpoints;
	auto const t0 = chrono::steady_clock::now();
	cancel->throw_if_cancelled();
	if (opts.resolver) {
		A<char, 256> errbuf{};
		auto *ctx = &endpoints;
		client_dns_bridge::resolve(
			opts.resolver,
			url.host.data(),
			url.host.size(),
			static_cast<u16>(url.port),
			timeouts.resolve.count() > 0 ? timeouts.resolve.count() : 30000LL,
			ctx,
			[](void *c, client_dns_bridge::Endpoint const &ep) noexcept {
				static_cast<V<client_dns_bridge::Endpoint> *>(c)->push_back(ep);
				return true;
			},
			errbuf.data(),
			errbuf.size());
	}
	if (endpoints.empty()) {
		S const host_str{url.host};
		S const port_str = to_string(url.port);
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags = AI_ADDRCONFIG;
		addrinfo *res_raw = nullptr;
		if (::getaddrinfo(host_str.c_str(), port_str.c_str(), &hints, &res_raw) == 0) {
			for (auto const *ai = res_raw; ai; ai = ai->ai_next) {
				client_dns_bridge::Endpoint ep{};
				memcpy(ep.addr, ai->ai_addr, min(sizeof(ep.addr), static_cast<SZ>(ai->ai_addrlen)));
				ep.addr_len = static_cast<unsigned>(ai->ai_addrlen);
				ep.family = (ai->ai_family == AF_INET6) ? 6 : 4;
				endpoints.push_back(ep);
			}
			::freeaddrinfo(res_raw);
		}
	}
	tel.dns = chrono::steady_clock::now() - t0;
	cancel->throw_if_cancelled();
	if (endpoints.empty()) {
		co_return unexpected(
			HttpError{
				.kind = HttpErrorKind::dns,
				.phase = HttpPhase::resolve,
				.message = format("failed to resolve '{}'", url.host)});
	}
	ConnectOptions copts{};
	copts.timeout = timeouts.connect;
	cancel->throw_if_cancelled();
	auto const t1 = chrono::steady_clock::now();
	TcpStream stream;
	try {
		auto connect_task = staggered_parallel_connect(ring, endpoints, copts);
		cancel->set_active(connect_task.control());
		stream = co_await move(connect_task);
		cancel->clear_active();
		cancel->throw_if_cancelled();
	} catch (wroot::CancelledError const &) {
		cancel->clear_active();
		throw;
	} catch (IoError const &e) {
		cancel->clear_active();
		co_return unexpected(
			HttpError{
				.kind = HttpErrorKind::connect,
				.phase = HttpPhase::connect,
				.os_errno = e.code().value(),
				.message = format("connect to '{}:{}' failed: {}", url.host, url.port, e.what())});
	} catch (...) {
		cancel->clear_active();
		co_return unexpected(
			HttpError{
				.kind = HttpErrorKind::connect,
				.phase = HttpPhase::connect,
				.message = format("connect to '{}:{}' failed", url.host, url.port)});
	}
	cancel->throw_if_cancelled();
	tel.connect = chrono::steady_clock::now() - t1;
	bool const is_tls = (url.scheme == "https");
#if CONFLUX_HAS_TLS
	Opt<TcpTlsStream> tls_stream;
	if (is_tls) {
		bool const verify = req.verify_peer() && opts.verify_peer;
		auto const sni_sv = req.server_name().empty() ? SV{url.host} : req.server_name();
		TlsContext tls_ctx;
		tls_ctx.set_verify_peer(verify);
		if (verify) {
			if (!opts.ca_bundle_path.empty()) {
				if (!SSL_CTX_load_verify_locations(tls_ctx.native_handle(), opts.ca_bundle_path.c_str(), nullptr)) {
					co_return unexpected(
						HttpError{.kind = HttpErrorKind::tls, .phase = HttpPhase::tls, .message = tls_error_string()});
				}
			} else {
				tls_ctx.set_default_verify_paths();
			}
		}
		cancel->throw_if_cancelled();
		tls_stream.emplace(tls_ctx, move(stream), cancel);
		if (!tls_stream->set_server_name(sni_sv) || (verify && !tls_stream->set_verify_hostname(sni_sv))) {
			co_return unexpected(
				HttpError{
					.kind = HttpErrorKind::tls,
					.phase = HttpPhase::tls,
					.message = "TLS SNI/hostname setup failed"});
		}
		auto const t_tls = chrono::steady_clock::now();
		TP const tls_dl = timeouts.tls.count() > 0 ? t_tls + timeouts.tls : TP::max();
		try {
			co_await tls_stream->handshake_connect(tls_dl);
		} catch (wroot::CancelledError const &) { throw; } catch (TlsError const &e) {
			co_return unexpected(
				HttpError{.kind = HttpErrorKind::tls, .phase = HttpPhase::tls, .message = tls_error_string(e)});
		} catch (IoError const &e) {
			co_return unexpected(
				HttpError{
					.kind = HttpErrorKind::tls,
					.phase = HttpPhase::tls,
					.os_errno = e.code().value(),
					.message = format("TLS handshake failed: {}", e.what())});
		}
		if (verify) {
			long const vr = SSL_get_verify_result(tls_stream->native_handle());
			if (vr != X509_V_OK) {
				co_return unexpected(
					HttpError{
						.kind = HttpErrorKind::tls,
						.phase = HttpPhase::tls,
						.message = X509_verify_cert_error_string(vr)});
			}
		}
		tel.tls = chrono::steady_clock::now() - t_tls;
		tel.tls_verified = verify;
		tel.negotiated_protocol = "https/1.1";
		if (auto const *cipher = SSL_get_current_cipher(tls_stream->native_handle())) {
			tel.tls_cipher = SSL_CIPHER_get_name(cipher);
			tel.tls_version = SSL_CIPHER_get_version(cipher);
		}
	}
#else
	if (is_tls) {
		co_return unexpected(
			HttpError{.kind = HttpErrorKind::tls, .phase = HttpPhase::tls, .message = "TLS not compiled in"});
	}
#endif
	S path = url.path.empty() ? S{"/"} : S{url.path};
	if (!url.query.empty()) {
		path += '?';
		path += url.query;
	}
	S wire;
	wire.reserve(256);
	auto const caller_host = req.headers()["host"];
	S const host_hdr = caller_host.empty() ? build_host_header(url) : S{caller_host};
	wire += format("{} {} HTTP/1.1\r\nHost: {}\r\n", req.method(), path, host_hdr);
	HttpFields merged = opts.default_headers;
	for (auto const &[k, v]: req.headers()) {
		auto const lower = ascii_lower(k);
		if (lower == "host" || conflux::http::is_hop_by_hop_header(lower)) {
			continue;
		}
		merged.set(k, v);
	}
	for (auto const &[k, v]: merged) {
		auto const lower = ascii_lower(k);
		if (lower == "host" || conflux::http::is_hop_by_hop_header(lower)) {
			continue;
		}
		wire += format("{}: {}\r\n", k, v);
	}
	wire += "Connection: close\r\n";
	if (!req.body().empty()) {
		wire += format("Content-Length: {}\r\n", req.body().size());
	}
	wire += "\r\n";
	cancel->throw_if_cancelled();
	try {
#if CONFLUX_HAS_TLS
		if (tls_stream) {
			TlsStreamRef tr{*tls_stream, timeouts.between_bytes, timeouts.write};
			co_await tr.write(span<u8 const>{reinterpret_cast<u8 const *>(wire.data()), wire.size()});
			if (!req.body().empty()) {
				co_await tr.write(span<u8 const>{reinterpret_cast<u8 const *>(req.body().data()), req.body().size()});
			}
		} else
#endif
		{
			PlainStreamRef pr{stream, cancel, timeouts.between_bytes, timeouts.write};
			co_await pr.write(span<u8 const>{reinterpret_cast<u8 const *>(wire.data()), wire.size()});
			if (!req.body().empty()) {
				co_await pr.write(span<u8 const>{reinterpret_cast<u8 const *>(req.body().data()), req.body().size()});
			}
		}
	} catch (wroot::CancelledError const &) { throw; } catch (IoError const &e) {
		co_return unexpected(
			HttpError{
				.kind = HttpErrorKind::write,
				.phase = HttpPhase::write,
				.os_errno = e.code().value(),
				.message = "failed to send request"});
	}
	tel.bytes_sent += wire.size() + req.body().size();
	SZ const max_hdr = opts.max_header_bytes;
	SZ const max_body_sz = opts.max_body_bytes;
	SZ const max_buf = opts.max_buffered_bytes;
	auto const t2 = chrono::steady_clock::now();
	TP const first_byte_dl = timeouts.first_byte.count() > 0 ? t2 + timeouts.first_byte : TP::max();
	cancel->throw_if_cancelled();
	S raw;
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
		co_return unexpected(
			HttpError{
				.kind = HttpErrorKind::read,
				.phase = HttpPhase::first_byte,
				.os_errno = e.code().value(),
				.message = "timed out waiting for response headers"});
	}
	auto const header_end = raw.find("\r\n\r\n");
	if (header_end == S::npos) {
		if (raw.size() >= max_hdr) {
			co_return unexpected(
				HttpError{
					.kind = HttpErrorKind::header_too_large,
					.message = format("response headers exceed {} bytes", max_hdr)});
		}
		co_return unexpected(
			HttpError{.kind = HttpErrorKind::protocol, .message = "response headers missing CRLFCRLF"});
	}
	if (header_end > max_hdr) {
		co_return unexpected(
			HttpError{
				.kind = HttpErrorKind::header_too_large,
				.message = format("response headers exceed {} bytes", max_hdr)});
	}
	tel.bytes_received += raw.size();
	auto const headers_str = SV{raw}.substr(0, header_end);
	HttpResponse response;
	auto const nl = headers_str.find("\r\n");
	auto const status_line = (nl != SV::npos) ? headers_str.substr(0, nl) : headers_str;
	auto const sp1 = status_line.find(' ');
	if (sp1 == SV::npos) {
		co_return unexpected(HttpError{.kind = HttpErrorKind::protocol, .message = "malformed status line"});
	}
	auto const rest = status_line.substr(sp1 + 1);
	auto const sp2 = rest.find(' ');
	auto const code_sv = (sp2 != SV::npos) ? rest.substr(0, sp2) : rest;
	int status = 0;
	auto const [ptr, ec] = from_chars(code_sv.data(), code_sv.data() + code_sv.size(), status);
	if (ec != errc{} || status < 100 || status > 999) {
		co_return unexpected(
			HttpError{.kind = HttpErrorKind::protocol, .message = format("invalid status code '{}'", code_sv)});
	}
	response.head.status = status;
	if (sp2 != SV::npos) {
		response.head.status_text = S{rest.substr(sp2 + 1)};
	}
	SZ content_length = 0;
	bool has_content_length = false;
	bool chunked = false;
	SZ pos = (nl != SV::npos) ? nl + 2 : headers_str.size();
	while (pos < headers_str.size()) {
		auto const end = headers_str.find("\r\n", pos);
		auto const hdr = (end != SV::npos) ? headers_str.substr(pos, end - pos) : headers_str.substr(pos);
		auto const colon = hdr.find(':');
		if (colon != SV::npos) {
			auto k = hdr.substr(0, colon);
			auto v = hdr.substr(colon + 1);
			while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) {
				v.remove_prefix(1);
			}
			auto const kl = ascii_lower(k);
			auto const vl = ascii_lower(v);
			if (kl == "content-length") {
				from_chars(v.data(), v.data() + v.size(), content_length);
				has_content_length = true;
			} else if (kl == "transfer-encoding" && vl.find("chunked") != S::npos) {
				chunked = true;
			} else if (kl == "set-cookie") {
				response.head.set_cookies.push_back(S{v});
			} else if (!conflux::http::is_hop_by_hop_header(kl)) {
				response.head.headers.set(S{k}, S{v});
			}
		}
		pos = (end != SV::npos) ? end + 2 : headers_str.size();
	}
	if (has_content_length && content_length > max_body_sz) {
		co_return unexpected(
			HttpError{
				.kind = HttpErrorKind::body_too_large,
				.message = format("Content-Length {} exceeds limit {}", content_length, max_body_sz)});
	}
	response.body = raw.substr(header_end + 4);
	auto do_body = [&]() -> wroot::Task<Opt<HttpError>> {
#if CONFLUX_HAS_TLS
		if (tls_stream) {
			TlsStreamRef tr{*tls_stream, timeouts.between_bytes, timeouts.write};
			if (req.method() == "HEAD") {
				response.body.clear();
			} else if (chunked) {
				S decoded;
				bool too_large = false;
				try {
					if (!co_await async_recv_chunked(tr, response.body, decoded, max_body_sz, max_buf, too_large)) {
						if (too_large)
							co_return HttpError{
								.kind = HttpErrorKind::body_too_large,
								.message = format("chunked body exceeds limit {}", max_body_sz)};
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
				response.body = move(decoded);
			} else if (has_content_length && content_length > response.body.size()) {
				try {
					if (!co_await async_recv_exact(tr, response.body, content_length, max_body_sz)) {
						if (response.body.size() >= max_body_sz)
							co_return HttpError{
								.kind = HttpErrorKind::body_too_large,
								.message = format("body exceeds limit {}", max_body_sz)};
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
				tel.bytes_received += content_length - (raw.size() - (header_end + 4));
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
						.message = format("EOF-delimited body exceeds limit {}", max_body_sz)};
				tel.bytes_received += response.body.size();
			}
			co_return nullopt;
		}
#endif
		{
			PlainStreamRef pr{stream, cancel, timeouts.between_bytes, timeouts.write};
			if (req.method() == "HEAD") {
				response.body.clear();
			} else if (chunked) {
				S decoded;
				bool too_large = false;
				try {
					if (!co_await async_recv_chunked(pr, response.body, decoded, max_body_sz, max_buf, too_large)) {
						if (too_large)
							co_return HttpError{
								.kind = HttpErrorKind::body_too_large,
								.message = format("chunked body exceeds limit {}", max_body_sz)};
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
				response.body = move(decoded);
			} else if (has_content_length && content_length > response.body.size()) {
				try {
					if (!co_await async_recv_exact(pr, response.body, content_length, max_body_sz)) {
						if (response.body.size() >= max_body_sz)
							co_return HttpError{
								.kind = HttpErrorKind::body_too_large,
								.message = format("body exceeds limit {}", max_body_sz)};
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
				tel.bytes_received += content_length - (raw.size() - (header_end + 4));
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
						.message = format("EOF-delimited body exceeds limit {}", max_body_sz)};
				tel.bytes_received += response.body.size();
			}
			co_return nullopt;
		}
	};
	if (auto berr = co_await do_body(); berr) {
		co_return unexpected(move(*berr));
	}
#if CONFLUX_HAS_TLS
	if (tls_stream) {
		try {
			co_await tls_stream->close(timeouts.write);
		} catch (...) {}
	} else
#endif
		co_await stream.close();
	response.telemetry = tel;
	co_return response;
}
wroot::Task<void> run_async_request_driver(
	SocketTaskRing &ring,
	HttpRequest const &req,
	HttpClientOptions const &opts,
	SP<wroot::TaskSource<HttpResult>> src,
	SP<ActiveTaskCancelRelay> cancel) {
	try {
		HttpRequest current = req;
		HttpTelemetry total_tel{};
		for (;;) {
			auto result = co_await do_async_request(ring, current, opts, cancel);
			if (!result) {
				auto _ = src->try_set_value(wroot::Success<HttpResult>{move(result)});
				break;
			}
			accumulate_telemetry(total_tel, result->telemetry);
			auto next = follow_redirect(current, *result);
			if (!next) {
				auto _ = src->try_set_value(wroot::Success<HttpResult>{unexpected(next.error())});
				break;
			}
			if (!next->has_value()) {
				result->telemetry = move(total_tel);
				auto _ = src->try_set_value(wroot::Success<HttpResult>{move(result)});
				break;
			}
			current = move(**next);
		}
	} catch (wroot::CancelledError const &) { auto _ = src->try_set_cancelled(); } catch (...) {
		auto _ = src->try_set_exception(current_exception());
	}
}

} // namespace async_detail
namespace conflux::http {

[[nodiscard]] conflux::work::root::Task<HttpResult> async_send(
	HttpClient const &client,
	SocketTaskRing &ring,
	HttpRequest const &req) {
	namespace wroot = conflux::work::root;
	auto [out, raw_src] = wroot::make_task_source<HttpResult>(wroot::SubmitOptions{.enable_cancellation = true});
	auto src = make_shared<wroot::TaskSource<HttpResult>>(move(raw_src));
	auto cancel = make_shared<ActiveTaskCancelRelay>();
	auto _ = src->install_cancel_hook([cancel](wroot::CancelReason) noexcept { cancel->cancel(); });
	auto driver = async_detail::run_async_request_driver(ring, req, client.options(), src, cancel);
	move(driver).detach();
	return move(out);
}

[[nodiscard]] conflux::work::root::Task<HttpResult> send_async(
	HttpClient const &client,
	SocketTaskRing &ring,
	HttpRequest const &req) {
	return async_send(client, ring, req);
}

} // namespace conflux::http
