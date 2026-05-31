// HTTP/2 live-server benchmark.
//
// This is intentionally narrower than http_server_bench: it starts one real
// TLS+ALPN H2 server and drives it with a synchronous nghttp2/OpenSSL client.
// The goal is to expose H2-specific costs that the generic HTTP/1 live and
// user-space app-path benches cannot isolate: ALPN/preface setup, HPACK/header
// handling, DATA provider chunking, flow-control WINDOW_UPDATE, trailers, SSE,
// and multiplexed streams on one connection.
#include <arpa/inet.h>
#include <cstddef> // must precede openssl/nghttp2 on some libstdc++ builds.
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import conflux.work;
import bench_common;

using namespace std::literals;
using conflux::http::Config;
using HttpRequest = conflux::http::Request;
using conflux::http::Response;
using Router = ::Router;
using conflux::http::SseChannel;

namespace {

// ── TLS/nghttp2 client helpers ─────────────────────────────────────────────

struct SslCtxDeleter {
	void operator ()(
		SSL_CTX *p) const noexcept {
		SSL_CTX_free(p);
	}
};
struct SslDeleter {
	void operator ()(
		SSL *p) const noexcept {
		SSL_free(p);
	}
};
using UniqueSslCtx = std::unique_ptr<SSL_CTX, SslCtxDeleter>;
using UniqueSsl = std::unique_ptr<SSL, SslDeleter>;

struct H2Response {
	int status = 0;
	std::string body;
	bool closed = false;
	std::uint32_t error_code = 0;
	std::vector<std::pair<std::string, std::string>> trailers;
};

struct H2Client {
	struct ReqBody {
		std::string data;
		std::size_t off{0};
	};

	H2Client(H2Client const &) = delete;
	H2Client &operator =(H2Client const &) = delete;

	explicit H2Client(
		std::uint16_t port)
		: ctx_(SSL_CTX_new(TLS_client_method()))
		, fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
		if (!ctx_) {
			throw std::runtime_error{"H2Client: SSL_CTX_new failed"};
		}
		if (fd_ < 0) {
			throw std::runtime_error{"H2Client: socket failed"};
		}

		SSL_CTX_set_verify(ctx_.get(), SSL_VERIFY_NONE, nullptr);
		static constexpr unsigned char kAlpn[] = "\x02h2";
		SSL_CTX_set_alpn_protos(ctx_.get(), kAlpn, sizeof(kAlpn) - 1);

		static constexpr int one = 1;
		::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		::setsockopt(fd_, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof one);
		timeval tv{.tv_sec = 5, .tv_usec = 0};
		::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
			throw std::runtime_error{"H2Client: connect failed"};
		}

		ssl_.reset(SSL_new(ctx_.get()));
		if (!ssl_) {
			throw std::runtime_error{"H2Client: SSL_new failed"};
		}
		SSL_set_fd(ssl_.get(), fd_);
		SSL_ctrl(
			ssl_.get(),
			SSL_CTRL_SET_TLSEXT_HOSTNAME,
			TLSEXT_NAMETYPE_host_name,
			const_cast<void *>(static_cast<void const *>("localhost")));
		if (SSL_connect(ssl_.get()) != 1) {
			throw std::runtime_error{"H2Client: TLS handshake failed"};
		}

		unsigned char const *proto = nullptr;
		unsigned int proto_len = 0;
		SSL_get0_alpn_selected(ssl_.get(), &proto, &proto_len);
		if (proto_len != 2 || proto[0] != 'h' || proto[1] != '2') {
			throw std::runtime_error{"H2Client: server did not negotiate h2"};
		}

		nghttp2_session_callbacks *cbs = nullptr;
		if (nghttp2_session_callbacks_new(&cbs) != 0) {
			throw std::runtime_error{"H2Client: callbacks_new failed"};
		}
		nghttp2_session_callbacks_set_send_callback(cbs, send_cb);
		nghttp2_session_callbacks_set_on_header_callback(cbs, on_header_cb);
		nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_cb);
		nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close_cb);
		nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv_cb);

		nghttp2_option *opts = nullptr;
		nghttp2_option_new(&opts);
		nghttp2_option_set_no_http_messaging(opts, 1);
		if (nghttp2_session_client_new2(&session_, cbs, this, opts) != 0) {
			nghttp2_option_del(opts);
			nghttp2_session_callbacks_del(cbs);
			throw std::runtime_error{"H2Client: session_client_new failed"};
		}
		nghttp2_option_del(opts);
		nghttp2_session_callbacks_del(cbs);

		nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, nullptr, 0);
		if (nghttp2_session_send(session_) != 0) {
			throw std::runtime_error{"H2Client: preface send failed"};
		}
	}

	~H2Client() {
		if (session_ != nullptr) {
			nghttp2_session_del(session_);
		}
		if (ssl_) {
			SSL_shutdown(ssl_.get());
			ssl_.reset();
		}
		ctx_.reset();
		if (fd_ >= 0) {
			::close(fd_);
		}
	}

	[[nodiscard]] H2Response get(
		std::string_view path) {
		std::int32_t const sid = submit_request("GET", path, nullptr);
		pump_until_closed(sid);
		return take_response(sid);
	}

	[[nodiscard]] H2Response post(
		std::string_view path,
		std::string_view body_data) {
		auto rb = std::make_unique<ReqBody>(ReqBody{.data = std::string{body_data}, .off = 0});
		ReqBody *rb_ptr = rb.get();
		nghttp2_data_provider prd{};
		prd.read_callback = read_cb;
		prd.source.ptr = rb_ptr;

		std::int32_t const sid = submit_request("POST", path, &prd);
		req_bodies_.emplace(sid, std::move(rb));
		pump_until_closed(sid);
		return take_response(sid);
	}

	[[nodiscard]] std::int32_t submit_get(
		std::string_view path) {
		return submit_request("GET", path, nullptr);
	}

	void pump_all(
		std::span<std::int32_t const> sids) {
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
		auto all_done = [&] {
			return std::ranges::all_of(sids, [&](std::int32_t sid) {
				auto const it = responses_.find(sid);
				return it != responses_.end() && it->second.closed;
			});
		};
		while (!all_done() && std::chrono::steady_clock::now() < deadline) {
			pump_once();
		}
		if (!all_done()) {
			throw std::runtime_error{"H2Client: timed out waiting for multiplexed streams"};
		}
	}

	[[nodiscard]] std::size_t checked_body_bytes_and_clear(
		std::span<std::int32_t const> sids) {
		std::size_t bytes = 0;
		for (auto const sid: sids) {
			auto resp = take_response(sid);
			if (resp.status != 200 || resp.error_code != 0) {
				throw std::runtime_error{std::format(
					"H2Client: stream {} expected status 200, got {} err {}",
					sid,
					resp.status,
					resp.error_code)};
			}
			bytes += resp.body.size();
		}
		return bytes;
	}

private:
	UniqueSslCtx ctx_;
	UniqueSsl ssl_;
	int fd_ = -1;
	nghttp2_session *session_ = nullptr;
	std::map<std::int32_t, H2Response> responses_;
	std::map<std::int32_t, std::unique_ptr<ReqBody>> req_bodies_;
	bool goaway_received_ = false;
	std::uint32_t goaway_error_code_ = 0;

	[[nodiscard]] H2Response take_response(
		std::int32_t sid) {
		auto it = responses_.find(sid);
		if (it == responses_.end()) {
			throw std::runtime_error{"H2Client: missing response"};
		}
		H2Response out = std::move(it->second);
		responses_.erase(it);
		req_bodies_.erase(sid);
		return out;
	}

	std::int32_t submit_request(
		std::string_view method,
		std::string_view path,
		nghttp2_data_provider const *prd) {
		std::string ms{method};
		std::string ps{path};
		std::vector<std::pair<std::string, std::string>> nv_store;
		nv_store.reserve(prd == nullptr ? 4 : 5);
		nv_store.emplace_back(":method", ms);
		nv_store.emplace_back(":path", ps);
		nv_store.emplace_back(":scheme", "https");
		nv_store.emplace_back(":authority", "localhost");
		if (prd != nullptr) {
			nv_store.emplace_back("content-type", "text/plain");
		}

		std::vector<nghttp2_nv> nva;
		nva.reserve(nv_store.size());
		for (auto &[n, v]: nv_store) {
			nva.push_back(
				{reinterpret_cast<std::uint8_t *>(n.data()),
				 reinterpret_cast<std::uint8_t *>(v.data()),
				 n.size(),
				 v.size(),
				 NGHTTP2_NV_FLAG_NONE});
		}

		std::int32_t const sid = nghttp2_submit_request(session_, nullptr, nva.data(), nva.size(), prd, nullptr);
		if (sid < 0) {
			throw std::runtime_error{"nghttp2_submit_request failed"};
		}
		return sid;
	}

	void pump_once() {
		if (nghttp2_session_send(session_) != 0) {
			throw std::runtime_error{"nghttp2_session_send failed"};
		}

		std::array<char, 16384> buf{};
		int const n = SSL_read(ssl_.get(), buf.data(), static_cast<int>(buf.size()));
		if (n > 0) {
			ssize_t const rc = nghttp2_session_mem_recv(
				session_,
				reinterpret_cast<std::uint8_t const *>(buf.data()),
				static_cast<std::size_t>(n));
			if (rc < 0) {
				throw std::runtime_error{"nghttp2_session_mem_recv failed"};
			}
			return;
		}
		auto const err = SSL_get_error(ssl_.get(), n);
		if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
			// Keep the caller's deadline as the authoritative failure signal.
			return;
		}
	}

	void pump_until_closed(
		std::int32_t sid) {
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
		while (!responses_[sid].closed && !goaway_received_ && std::chrono::steady_clock::now() < deadline) {
			pump_once();
		}
		if (goaway_received_ && !responses_[sid].closed) {
			responses_[sid].closed = true;
			responses_[sid].error_code = goaway_error_code_;
		}
		if (!responses_[sid].closed) {
			throw std::runtime_error{"H2Client: timed out waiting for stream close"};
		}
	}

	static ssize_t send_cb(
		nghttp2_session * /*unused*/,
		std::uint8_t const *data,
		std::size_t length,
		int /*unused*/,
		void *ud) {
		auto *c = static_cast<H2Client *>(ud);
		int const n = SSL_write(c->ssl_.get(), data, static_cast<int>(length));
		return n > 0 ? static_cast<ssize_t>(n) : static_cast<ssize_t>(NGHTTP2_ERR_CALLBACK_FAILURE);
	}

	static int on_header_cb(
		nghttp2_session * /*unused*/,
		nghttp2_frame const *frame,
		std::uint8_t const *name,
		std::size_t namelen,
		std::uint8_t const *value,
		std::size_t valuelen,
		std::uint8_t /*unused*/,
		void *ud) {
		auto *c = static_cast<H2Client *>(ud);
		std::string_view const n{reinterpret_cast<char const *>(name), namelen};
		std::string_view const v{reinterpret_cast<char const *>(value), valuelen};
		if (frame->headers.cat == NGHTTP2_HCAT_HEADERS) {
			c->responses_[frame->hd.stream_id].trailers.emplace_back(std::string{n}, std::string{v});
		} else if (n == ":status") {
			int st = 0;
			std::from_chars(v.data(), v.data() + v.size(), st);
			c->responses_[frame->hd.stream_id].status = st;
		}
		return 0;
	}

	static int on_data_chunk_cb(
		nghttp2_session * /*unused*/,
		std::uint8_t /*unused*/,
		std::int32_t stream_id,
		std::uint8_t const *data,
		std::size_t len,
		void *ud) {
		auto *c = static_cast<H2Client *>(ud);
		c->responses_[stream_id].body.append(reinterpret_cast<char const *>(data), len);
		return 0;
	}

	static int on_stream_close_cb(
		nghttp2_session * /*unused*/,
		std::int32_t stream_id,
		std::uint32_t error_code,
		void *ud) {
		auto *c = static_cast<H2Client *>(ud);
		c->responses_[stream_id].closed = true;
		c->responses_[stream_id].error_code = error_code;
		return 0;
	}

	static int on_frame_recv_cb(
		nghttp2_session * /*unused*/,
		nghttp2_frame const *frame,
		void *ud) {
		auto *c = static_cast<H2Client *>(ud);
		if (frame->hd.type == NGHTTP2_GOAWAY) {
			c->goaway_received_ = true;
			c->goaway_error_code_ = frame->goaway.error_code;
		}
		return 0;
	}

	static ssize_t read_cb(
		nghttp2_session * /*unused*/,
		std::int32_t /*unused*/,
		std::uint8_t *buf,
		std::size_t length,
		std::uint32_t *data_flags,
		nghttp2_data_source *source,
		void * /*unused*/) {
		auto &rb = *static_cast<ReqBody *>(source->ptr);
		auto const remaining = rb.data.size() - rb.off;
		auto const to_copy = std::min(remaining, length);
		std::memcpy(
			buf,
			rb.data.data() + rb.off, // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
			to_copy);
		rb.off += to_copy;
		if (rb.off >= rb.data.size()) {
			*data_flags |= NGHTTP2_DATA_FLAG_EOF;
		}
		return static_cast<ssize_t>(to_copy);
	}
};

// ── Server helpers ─────────────────────────────────────────────────────────

struct TempCertFiles {
	std::string cert;
	std::string key;
	TempCertFiles() = default;
	~TempCertFiles() {
		if (!cert.empty()) {
			::unlink(cert.c_str());
		}
		if (!key.empty()) {
			::unlink(key.c_str());
		}
	}
	TempCertFiles(TempCertFiles const &) = delete;
	TempCertFiles &operator =(TempCertFiles const &) = delete;
	TempCertFiles(
		TempCertFiles &&o) noexcept
		: cert{std::move(o.cert)}
		, key{std::move(o.key)} {
		o.cert.clear();
		o.key.clear();
	}
	TempCertFiles &operator =(TempCertFiles &&o) noexcept = delete;
};

[[nodiscard]] TempCertFiles make_self_signed_cert_files() {
	auto make_path = [](std::string pattern) {
		std::vector<char> buf(pattern.begin(), pattern.end());
		buf.push_back('\0');
		int const fd = ::mkstemps(buf.data(), 4);
		if (fd < 0) {
			throw std::runtime_error{"mkstemps failed"};
		}
		::close(fd);
		return std::string{buf.data()};
	};
	TempCertFiles files;
	files.cert = make_path("/tmp/conflux_h2_bench_cert_XXXXXX.pem");
	files.key = make_path("/tmp/conflux_h2_bench_key_XXXXXX.pem");
	std::string const cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		files.key,
		files.cert);
	if (::system(cmd.c_str()) != 0) {
		throw std::runtime_error{"openssl req failed"};
	}
	return files;
}

void wait_for_tcp_port(
	std::uint16_t port) {
	for (int i = 0; i < 200; ++i) {
		int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		bool const up = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
		::close(fd);
		if (up) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	throw std::runtime_error{"server did not start in time"};
}

Config h2_bench_config(
	unsigned rings = 1,
	unsigned ring_entries = 256) {
	Config cfg = Config::benchmark();
	cfg.port = 0;
	cfg.rings = rings;
	cfg.ring_entries = ring_entries;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	cfg.max_body_size = std::size_t{2} * 1024 * 1024;
	return cfg;
}

struct ServerHandle {
	TempCertFiles certs;
	std::shared_ptr<HttpServer> server;
	std::thread thr;
	std::uint16_t port{};
};

ServerHandle start_h2_server(
	Router router) {
	(void)::signal(SIGPIPE, SIG_IGN);
	auto certs = make_self_signed_cert_files();
	auto cfg = h2_bench_config();
	cfg.cert_file = certs.cert;
	cfg.key_file = certs.key;

	auto srv = std::make_shared<HttpServer>(cfg, std::move(router));
	std::thread t{[srv] {
		try {
			auto _ = srv->run();
		} catch (std::exception const &e) { std::println(std::cerr, "h2 bench server: {}", e.what()); }
	}};
	auto p = srv->port();
	wait_for_tcp_port(p);

	// TLS/ALPN readiness probe, outside measured rows.
	for (int i = 0; i < 50; ++i) {
		try {
			H2Client probe{p};
			auto resp = probe.get("/api/ping");
			if (resp.status == 200) {
				break;
			}
		} catch (...) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			if (i == 49) {
				throw;
			}
		}
	}
	return {.certs = std::move(certs), .server = srv, .thr = std::move(t), .port = p};
}

Router make_h2_router(
	std::string const &body_64k,
	std::string const &body_128k) {
	Router r;
	r.get("/api/ping", [](HttpRequest const &) { return conflux::http::Response::json(R"({"status":"ok"})"); });
	r.get("/hello/{name}", [](HttpRequest const &req) {
		return conflux::http::Response::text(std::format("hello {}", req.params["name"]));
	});
	r.post("/api/echo-body", [](HttpRequest const &req) {
		return conflux::http::Response::text(std::string{req.body});
	});
	r.get("/body/64k", [&body_64k](HttpRequest const &) { return conflux::http::Response::text(body_64k); });
	r.get("/body/128k", [&body_128k](HttpRequest const &) { return conflux::http::Response::text(body_128k); });
	r.get("/with-trailers", [](HttpRequest const &) {
		conflux::http::Response resp;
		resp.status = 200;
		resp.status_text = "OK";
		resp.content_type = "text/plain; charset=utf-8";
		resp.set_text_body("hello trailers");
		resp.trailers = {
			{"x-checksum", "crc32:deadbeef"},
			{  "x-server",        "conflux"}
        };
		return resp;
	});
	r.sse("/events", [](HttpRequest const &, std::shared_ptr<SseChannel> const &ch) {
		(void)ch->send("data: alpha\n\n");
		(void)ch->send("data: beta\n\n");
		(void)ch->send("data: gamma\n\n");
		ch->close();
	});
	return r;
}

// ── Benchmark variants ─────────────────────────────────────────────────────

struct Variant {
	std::string_view name;
	std::string_view bottleneck;
	std::function<std::size_t()> run;
	std::size_t streams_per_iter = 1;
	std::size_t iters_override = 0;
};

struct H2Stats {
	std::string_view config;
	std::string_view variant;
	std::string_view bottleneck;
	std::size_t iterations{};
	std::uint64_t total_ns{};
	double ns_per_iter{};
	std::size_t streams_per_iter{};
	std::size_t body_bytes_per_iter{};
};

[[nodiscard]] H2Stats run_variant(
	Variant const &v,
	std::size_t iterations,
	std::size_t warmup,
	std::string_view config_name) {
	if (v.iters_override > 0) {
		iterations = std::min(iterations, v.iters_override);
		warmup = std::min(warmup, std::max(std::size_t{1}, v.iters_override / 5));
	}

	for (std::size_t i = 0; i < warmup; ++i) {
		(void)v.run();
	}

	std::size_t measured_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iterations; ++i) {
		measured_bytes += v.run();
	}
	auto const t1 = bench_now_ns();

	auto const total = t1 - t0;
	return H2Stats{
		.config = config_name,
		.variant = v.name,
		.bottleneck = v.bottleneck,
		.iterations = iterations,
		.total_ns = total,
		.ns_per_iter = static_cast<double>(total) / static_cast<double>(iterations),
		.streams_per_iter = v.streams_per_iter,
		.body_bytes_per_iter = measured_bytes / iterations};
}

void print_h2_stats(
	H2Stats const &s,
	bool json_out) {
	if (json_out) {
		auto const total_streams = s.iterations * s.streams_per_iter;
		auto const ns_per_stream = s.ns_per_iter / static_cast<double>(s.streams_per_iter);
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"protocol\":\"h2\",\"label\":\"live-kernel-sanity\",\"bottleneck\":\"{}\","
			"\"streams_per_iter\":{},\"total_streams\":{},\"ns_per_stream\":{:.2f},\"body_bytes_per_iter\":{}}}",
			s.config,
			s.variant,
			s.iterations,
			s.total_ns,
			s.ns_per_iter,
			s.bottleneck,
			s.streams_per_iter,
			total_streams,
			ns_per_stream,
			s.body_bytes_per_iter);
		return;
	}
	std::println(
		"{:<28} {:>8} iters  {:>10.2f} ns/iter  {:>10.2f} ns/stream  {:>8} B/iter  {}",
		s.variant,
		s.iterations,
		s.ns_per_iter,
		s.ns_per_iter / static_cast<double>(s.streams_per_iter),
		s.body_bytes_per_iter,
		s.bottleneck);
}

[[nodiscard]] std::size_t require_ok_body_bytes(
	H2Response const &resp,
	std::string_view variant_name) {
	if (resp.status != 200 || resp.error_code != 0) {
		throw std::runtime_error{
			std::format("{} expected status 200, got {} err {}", variant_name, resp.status, resp.error_code)};
	}
	return resp.body.size();
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"http2_server","parser":"standard","configs":[{"name":"smoke","extra":{"protocol":"h2","label":"live-kernel-sanity","case":"all H2 rows, short smoke"},"target_ms":1000,"max_iterations":30,"calibration_iterations":1,"args":["--iterations","0","--warmup","0"],"reps":1},{"name":"serial_steady","extra":{"protocol":"h2","label":"end-to-end-proof-candidate","case":"serial H2 request/response rows"},"target_ms":750,"max_iterations":2000,"calibration_iterations":2,"args":["--filter","h2_seq_","--config-name","serial_steady","--iterations","0","--warmup","0"],"reps":2},{"name":"mux_steady","extra":{"protocol":"h2","label":"end-to-end-proof-candidate","case":"multiplexed H2 stream rows"},"target_ms":750,"max_iterations":1000,"calibration_iterations":2,"args":["--filter","h2_mux_","--config-name","mux_steady","--iterations","0","--warmup","0"],"reps":2},{"name":"handshake_smoke","extra":{"protocol":"h2","label":"live-kernel-sanity","case":"TLS+ALPN+H2 preface plus first request"},"target_ms":750,"max_iterations":30,"calibration_iterations":1,"args":["--filter","h2_handshake","--config-name","handshake_smoke","--iterations","0","--warmup","0"],"reps":1}]})");

	auto const argv_span = std::span{argv, static_cast<std::size_t>(argc)};
	auto const args = bench_parse_args(argv_span);
	auto const iters = args.iterations;
	auto const warmup = args.warmup;
	auto const json = args.json_out;
	auto const config_name = args.config_name.empty() ? "default"sv : std::string_view{args.config_name};

	std::string const body_4k(4 * 1024, 'P');
	std::string const body_64k(64 * 1024, 'B');
	std::string const body_128k(128 * 1024, 'F');

	auto server = start_h2_server(make_h2_router(body_64k, body_128k));
	auto client = std::make_unique<H2Client>(server.port);

	std::vector<Variant> variants;
	variants.push_back(
		{.name = "h2_handshake_first_get"sv,
		 .bottleneck = "tls_alpn_preface_first_stream"sv,
		 .run =
			 [&] {
				 H2Client c{server.port};
				 auto resp = c.get("/api/ping");
				 return require_ok_body_bytes(resp, "h2_handshake_first_get");
			 },
		 .iters_override = 30});
	variants.push_back({.name = "h2_seq_ping"sv, .bottleneck = "headers_route_small_response"sv, .run = [&] {
							return require_ok_body_bytes(client->get("/api/ping"), "h2_seq_ping");
						}});
	variants.push_back({.name = "h2_seq_route_param"sv, .bottleneck = "headers_route_param_response"sv, .run = [&] {
							return require_ok_body_bytes(client->get("/hello/conflux"), "h2_seq_route_param");
						}});
	variants.push_back({.name = "h2_seq_post_4k"sv, .bottleneck = "request_data_provider_body_echo"sv, .run = [&] {
							return require_ok_body_bytes(client->post("/api/echo-body", body_4k), "h2_seq_post_4k");
						}});
	variants.push_back({.name = "h2_seq_post_64k"sv, .bottleneck = "request_data_flow_control_body_echo"sv, .run = [&] {
							return require_ok_body_bytes(client->post("/api/echo-body", body_64k), "h2_seq_post_64k");
						}});
	variants.push_back({.name = "h2_seq_body_64k"sv, .bottleneck = "response_data_provider_chunking"sv, .run = [&] {
							return require_ok_body_bytes(client->get("/body/64k"), "h2_seq_body_64k");
						}});
	variants.push_back(
		{.name = "h2_seq_body_128k"sv, .bottleneck = "response_flow_control_window_update"sv, .run = [&] {
			 return require_ok_body_bytes(client->get("/body/128k"), "h2_seq_body_128k");
		 }});
	variants.push_back({.name = "h2_seq_sse_3_events"sv, .bottleneck = "sse_deferred_data_resume"sv, .run = [&] {
							return require_ok_body_bytes(client->get("/events"), "h2_seq_sse_3_events");
						}});
	variants.push_back({.name = "h2_seq_trailers"sv, .bottleneck = "response_trailing_headers"sv, .run = [&] {
							auto resp = client->get("/with-trailers");
							if (resp.trailers.size() < 2) {
								throw std::runtime_error{"h2_seq_trailers expected trailers"};
							}
							return require_ok_body_bytes(resp, "h2_seq_trailers");
						}});
	variants.push_back(
		{.name = "h2_mux_16_ping"sv,
		 .bottleneck = "multiplexed_small_streams"sv,
		 .run =
			 [&] {
				 std::array<std::int32_t, 16> ids{};
				 for (auto &id: ids) {
					 id = client->submit_get("/api/ping");
				 }
				 client->pump_all(ids);
				 return client->checked_body_bytes_and_clear(ids);
			 },
		 .streams_per_iter = 16});
	variants.push_back(
		{.name = "h2_mux_32_mixed"sv,
		 .bottleneck = "multiplexed_mixed_routes_and_bodies"sv,
		 .run =
			 [&] {
				 std::array<std::int32_t, 32> ids{};
				 for (std::size_t i = 0; i < ids.size(); ++i) {
					 if (i % 4 == 0) {
						 ids[i] = client->submit_get("/body/64k");
					 } else if (i % 4 == 1) {
						 ids[i] = client->submit_get("/hello/conflux");
					 } else {
						 ids[i] = client->submit_get("/api/ping");
					 }
				 }
				 client->pump_all(ids);
				 return client->checked_body_bytes_and_clear(ids);
			 },
		 .streams_per_iter = 32});

	if (!json) {
		std::println("http2_server_bench: {} iterations, {} warmup\n", iters, warmup);
	}

	for (auto const &v: variants) {
		if (!bench_matches_filter(args, v.name)) {
			continue;
		}
		try {
			auto const stats = run_variant(v, iters, warmup, config_name);
			print_h2_stats(stats, json);
		} catch (std::exception const &e) {
			if (json) {
				std::println(std::cerr, "error in {}: {}", v.name, e.what());
			} else {
				std::println("  {:<28} ERROR: {}", v.name, e.what());
			}
		}
	}

	client.reset();
	server.server->shutdown();
	server.thr.join();
}
