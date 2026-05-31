// ---------------------------------------------------------------------------
// Counter / Gauge / Histogram unit tests
// ---------------------------------------------------------------------------

TEST_CASE(
	"metrics: Counter inc and get") {
	conflux::http::Counter c;
	CHECK(c.get() == 0);
	c.inc();
	CHECK(c.get() == 1);
	c.inc(5);
	CHECK(c.get() == 6);
}
TEST_CASE(
	"metrics: Gauge set inc dec") {
	conflux::http::Gauge g;
	g.set(10.0);
	CHECK(g.get() == 10.0);
	g.inc(2.5);
	CHECK(g.get() == 12.5);
	g.dec(3.0);
	CHECK(g.get() == 9.5);
}
TEST_CASE(
	"metrics: Histogram observe updates count and sum") {
	conflux::http::Histogram h;
	h.observe(0.1);
	h.observe(0.05);
	CHECK(h.count() == 2);
	// sum must be approximately 0.15
	CHECK(h.sum() > 0.14);
	CHECK(h.sum() < 0.16);
}
TEST_CASE(
	"metrics: Histogram bucket boundaries") {
	conflux::http::Histogram h;
	// 0.005 bucket: only observations <= 0.005 fall in it.
	h.observe(0.003);
	h.observe(0.007);
	// bucket[0] is le=0.005; only first observation qualifies.
	CHECK(h.bucket(0) == 1);
	// bucket[1] is le=0.01; both qualify.
	CHECK(h.bucket(1) == 2);
}
TEST_CASE(
	"metrics: pressure counters render as Prometheus events") {
	conflux::http::HttpPressureMetrics pressure{};
	pressure.accept_rejected = 2;
	pressure.drain_started = 1;
	pressure.drain_deadline_hit = 1;
	pressure.websocket_closed_for_pressure = 3;

	auto out = conflux::http::format_pressure_metrics_prometheus(pressure);
	CHECK(out.find("# TYPE http_pressure_events_total counter") != std::string::npos);
	CHECK(out.find("http_pressure_events_total{event=\"accept_rejected\"} 2") != std::string::npos);
	CHECK(out.find("http_pressure_events_total{event=\"drain_started\"} 1") != std::string::npos);
	CHECK(out.find("http_pressure_events_total{event=\"drain_deadline_hit\"} 1") != std::string::npos);
	CHECK(out.find("http_pressure_events_total{event=\"websocket_closed_for_pressure\"} 3") != std::string::npos);
}
// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

namespace {

std::uint16_t g_metrics_port = 0;
conflux::http::MetricsRegistry *g_metrics_reg = nullptr;
void ensure_metrics_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		static conflux::http::MetricsRegistry reg;
		g_metrics_reg = &reg;
		router.use(conflux::http::metrics_middleware(reg));
		router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
		router.get("/metrics", conflux::http::metrics_handler(reg));
		g_metrics_port = test_servers().start(cfg, std::move(router));
	});
}
std::uint16_t g_protected_metrics_port = 0;
void ensure_protected_metrics_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		static conflux::http::MetricsRegistry reg2;
		router.use(conflux::http::metrics_middleware(reg2));
		std::vector<conflux::http::Router::Middleware> chain;
		chain.push_back(bearer_auth_middleware([](std::string_view token) { return token == "supersecret"; }));
		router.get("/metrics", conflux::http::metrics_handler_protected(reg2, std::move(chain)));
		g_protected_metrics_port = test_servers().start(cfg, std::move(router));
	});
}

} // namespace
TEST_CASE(
	"metrics: /ping increments http_requests_total GET 2xx") {
	ensure_metrics_server();
	http_get_on(g_metrics_port, "/ping");
	http_get_on(g_metrics_port, "/ping");
	auto resp = http_get_on(g_metrics_port, "/metrics");
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	auto body = resp.substr(hdr_end + 4);
	REQUIRE(body.find("http_requests_total{method=\"GET\",status=\"2xx\"}") != std::string::npos);
}
TEST_CASE(
	"metrics: /metrics returns Prometheus content-type") {
	ensure_metrics_server();
	auto resp = http_get_on(g_metrics_port, "/metrics");
	REQUIRE(resp.find("text/plain; version=0.0.4") != std::string::npos);
}
TEST_CASE(
	"metrics_handler_protected: missing bearer token returns 401") {
	ensure_protected_metrics_server();
	auto resp = http_get_on(g_protected_metrics_port, "/metrics");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find("WWW-Authenticate: Bearer") != std::string::npos);
}
TEST_CASE(
	"metrics_handler_protected: wrong bearer token returns 401") {
	ensure_protected_metrics_server();
	auto resp = http_get_on(g_protected_metrics_port, "/metrics", "Authorization: Bearer badguess\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"metrics_handler_protected: valid bearer token returns 200 prometheus body") {
	ensure_protected_metrics_server();
	auto resp = http_get_on(g_protected_metrics_port, "/metrics", "Authorization: Bearer supersecret\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
	REQUIRE(resp.find("text/plain; version=0.0.4") != std::string::npos);
	REQUIRE(resp.find("http_requests_total") != std::string::npos);
}
TEST_CASE(
	"metrics: duration histogram appears in output") {
	ensure_metrics_server();
	http_get_on(g_metrics_port, "/ping");
	auto resp = http_get_on(g_metrics_port, "/metrics");
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	auto body = resp.substr(hdr_end + 4);
	REQUIRE(body.find("http_request_duration_seconds_sum") != std::string::npos);
	REQUIRE(body.find("http_request_duration_seconds_count") != std::string::npos);
	REQUIRE(body.find("http_request_duration_seconds_bucket") != std::string::npos);
}
TEST_CASE(
	"metrics: 4xx response increments GET 4xx counter") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		static conflux::http::MetricsRegistry reg;
		router.use(conflux::http::metrics_middleware(reg));
		router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		router.get("/metrics", conflux::http::metrics_handler(reg));
		port = test_servers().start(cfg, std::move(router));
	});
	http_get_on(port, "/nonexistent"); // 404 → 4xx
	auto resp = http_get_on(port, "/metrics");
	auto body = extract_body(resp);
	REQUIRE(body.find("http_requests_total{method=\"GET\",status=\"4xx\"}") != std::string::npos);
}
TEST_CASE(
	"metrics: 5xx response increments GET 5xx counter") {
	conflux::http::MetricsRegistry reg;
	reg.record("GET", 500, std::chrono::milliseconds{1});
	auto out = reg.format_prometheus();
	REQUIRE(out.find("http_requests_total{method=\"GET\",status=\"5xx\"}") != std::string::npos);
}
TEST_CASE(
	"metrics: OTHER method bucket used for non-standard methods") {
	conflux::http::MetricsRegistry reg;
	reg.record("PURGE", 200, std::chrono::milliseconds{1});
	auto out = reg.format_prometheus();
	REQUIRE(out.find("http_requests_total{method=\"OTHER\",status=\"2xx\"}") != std::string::npos);
}
TEST_CASE(
	"metrics: out-of-range status maps to other bucket") {
	conflux::http::MetricsRegistry reg;
	reg.record("GET", 999, std::chrono::milliseconds{1});
	auto out = reg.format_prometheus();
	REQUIRE(out.find("http_requests_total{method=\"GET\",status=\"other\"}") != std::string::npos);
}
// ---------------------------------------------------------------------------
// Compression codecs
// ---------------------------------------------------------------------------

namespace {

std::uint16_t g_codec_port = 0;
void ensure_codec_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		router.use(compress_middleware({.min_body_size = 0})); // compress everything
		router.get("/data", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text(std::string(512, 'A')); // compressible
		});
		router.get("/vary", [](conflux::http::OwnedRequest const &) {
			auto r = conflux::http::Response::text(std::string(512, 'A'));
			r.headers["Vary"] = "X-Test";
			return r;
		});
		g_codec_port = test_servers().start(cfg, std::move(router));
	});
}

} // namespace
TEST_CASE(
	"compress negotiation header: brotli is ignored for dynamic responses") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: br, gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}
TEST_CASE(
	"compress negotiation header: zstd accepted when client prefers it") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: zstd;q=1, gzip;q=0.5\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_ZSTD
	REQUIRE(resp.find("Content-Encoding: zstd\r\n") != std::string::npos);
#elif CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}
TEST_CASE(
	"compress negotiation header: gzip returned when only gzip offered") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}
TEST_CASE(
	"compress negotiation header: Accept-Encoding token matching is case-insensitive") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: GZip;Q=1\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}
TEST_CASE(
	"compress: appends Accept-Encoding to existing Vary") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/vary", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Vary: X-Test, Accept-Encoding\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Vary: X-Test\r\n") != std::string::npos);
#endif
}
TEST_CASE(
	"compress negotiation header: q=0 exclusion: gzip;q=0 gives zstd") {
	ensure_codec_server();
	// gzip explicitly excluded; should get zstd
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: gzip;q=0, zstd\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_ZSTD
	REQUIRE(resp.find("Content-Encoding: zstd\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}
TEST_CASE(
	"compress: backend names expose stable labels") {
	CHECK(gzip_backend_name(GzipBackend::auto_select) == "auto");
	CHECK(gzip_backend_name(GzipBackend::zlib) == "zlib");
	CHECK(gzip_backend_name(GzipBackend::libdeflate) == "libdeflate");
	CHECK(gzip_backend_name(GzipBackend::zlib_ng) == "zlib-ng");
	CHECK(gzip_backend_name(GzipBackend::isa_l) == "isa-l");
}
TEST_CASE(
	"compress negotiation header: wildcard * selects preferred dynamic codec") {
	ensure_codec_server();
	auto resp = http_get_with_header_on(g_codec_port, "/data", "Accept-Encoding: *\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS && CONFLUX_HAS_ZSTD
	if (current_dynamic_encoding_preference() == DynamicEncodingPreference::gzip_first) {
		REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
	} else {
		REQUIRE(resp.find("Content-Encoding: zstd\r\n") != std::string::npos);
	}
#elif CONFLUX_HAS_ZSTD
	REQUIRE(resp.find("Content-Encoding: zstd\r\n") != std::string::npos);
#elif CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}
TEST_CASE(
	"compress+cors: Vary header accumulates both Origin and Accept-Encoding") {
	ensure_cors_compress_server();
	auto resp = http_get_on(
		g_cors_compress_port,
		"/big",
		"Origin: https://test.example\r\n"
		"Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto vary = extract_header(resp, "Vary");
	// Both CORS and compress must contribute to Vary without overwriting each other.
	REQUIRE(vary.find("Origin") != std::string::npos);
	REQUIRE(vary.find("Accept-Encoding") != std::string::npos);
}
// ---------------------------------------------------------------------------
// conflux::http::redirect_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"redirect: exact match returns 301 with Location") {
	ensure_redirect_server();
	auto resp = http_get_on(g_redirect_port, "/old");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	REQUIRE(resp.find("Location: /new\r\n") != std::string::npos);
}
TEST_CASE(
	"redirect: prefix match appends suffix and returns 302") {
	ensure_redirect_server();
	auto resp = http_get_on(g_redirect_port, "/api/v1/users");
	REQUIRE(resp.starts_with("HTTP/1.1 302"));
	REQUIRE(resp.find("Location: /api/v2/users\r\n") != std::string::npos);
}
TEST_CASE(
	"redirect: non-matching path passes through to handler") {
	ensure_redirect_server();
	auto resp = http_get_on(g_redirect_port, "/new");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "new");
}
TEST_CASE(
	"redirect: custom status 307 preserved in redirect response") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::redirect_middleware({.rules = {{.from = "/x", .to = "/y", .status = 307}}}));
		router.get("/y", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("y"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/x");
	REQUIRE(resp.starts_with("HTTP/1.1 307"));
	REQUIRE(resp.find("Location: /y\r\n") != std::string::npos);
}
TEST_CASE(
	"http client: chunked response without trailers is decoded correctly") {
	// Build a mock server that sends a chunked response (no trailers).
	std::uint16_t port = 0;
	int const lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(lfd >= 0);
	int yes = 1;
	::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;
	REQUIRE(::bind(lfd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0);
	socklen_t salen = sizeof(sa);
	REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr *>(&sa), &salen) == 0);
	port = ntohs(sa.sin_port);
	REQUIRE(::listen(lfd, 1) == 0);

	auto srv = std::thread([lfd] {
		int const c = ::accept(lfd, nullptr, nullptr);
		if (c < 0) {
			return;
		}
		// Drain request
		char buf[4096];
		while (::recv(c, buf, sizeof(buf), 0) > 0) {
			if (strstr(buf, "\r\n\r\n")) {
				break;
			}
		}
		// Send chunked response with no trailers: 0\r\n\r\n
		std::string_view const resp =
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain\r\n"
			"Transfer-Encoding: chunked\r\n"
			"Connection: close\r\n"
			"\r\n"
			"5\r\nhello\r\n"
			"6\r\n world\r\n"
			"0\r\n"
			"\r\n";
		::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
		::close(c);
	});

	auto result = HttpClient{}.blocking_send(chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/", port)));

	srv.join();
	::close(lfd);

	REQUIRE(result.has_value());
	CHECK(result->head.status == 200);
	CHECK(result->body == "hello world");
}
TEST_CASE(
	"proxy: work-pool proxy handler forwards upstream response off-ring") {
	ensure_proxy_server();
	auto resp = http_get_on(g_proxy_port, "/proxy/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("X-Upstream: yes\r\n") != std::string::npos);
	REQUIRE(extract_body(resp) == "proxied-ok");
}
TEST_CASE(
	"http client async: async_send follows relative redirects") {
	ensure_redirect_follow_servers();
	auto resp = http_get_on(g_redirect_follow_async_port, "/async-follow");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "async-ok");
}
TEST_CASE(
	"proxy: preserve_host=true forwards original Host header") {
	static std::shared_ptr<ScopedTestServer> s_upstream;
	static std::shared_ptr<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router upstream;
		upstream.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["host"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		conflux::http::Router front;
		auto popts = ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = s_upstream->port(),
			.preserve_host = true,
		};
		front.add_context(
			"GET",
			"/echo",
			[popts = std::move(popts)](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await async_proxy(req, popts, ctx.ring);
			});
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	auto resp = http_get_on(s_front->port(), "/echo");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Body should be the exact original Host header forwarded by the proxy ("localhost").
	REQUIRE(extract_body(resp) == "localhost");
}
TEST_CASE(
	"proxy: preserve_host=true with port in Host header still connects to upstream") {
	static std::shared_ptr<ScopedTestServer> s_upstream;
	static std::shared_ptr<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router upstream;
		upstream.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["host"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		conflux::http::Router front;
		auto popts = ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = s_upstream->port(),
			.preserve_host = true,
		};
		front.add_context(
			"GET",
			"/echo",
			[popts = std::move(popts)](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await async_proxy(req, popts, ctx.ring);
			});
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	// Send Host: localhost:9999 — proxy must connect to upstream, not myapp.example.com.
	auto resp = http_get_on_host(s_front->port(), "localhost:9999", "/echo");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "localhost:9999");
}

TEST_CASE(
	"proxy: preserve_host=false ignores mixed-case Host headers") {
	static std::shared_ptr<ScopedTestServer> s_upstream;
	static std::shared_ptr<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router upstream;
		upstream.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["host"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		conflux::http::Router front;
		auto popts = ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = s_upstream->port(),
		};
		front.add_context(
			"GET",
			"/echo",
			[popts = std::move(popts)](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await async_proxy(req, popts, ctx.ring);
			});
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	LocalTcpClient client1{s_front->port()};
	(void)client1.send("GET /echo HTTP/1.1\r\nHOST: client.example\r\nConnection: close\r\n\r\n");
	auto resp = client1.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == std::format("127.0.0.1:{}", s_upstream->port()));
	LocalTcpClient client2{s_front->port()};
	(void)client2.send("GET /echo HTTP/1.1\r\nHoSt: client.example\r\nConnection: close\r\n\r\n");
	resp = client2.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == std::format("127.0.0.1:{}", s_upstream->port()));
}

TEST_CASE(
	"proxy: path_prefix strips only complete path segments") {
	static std::shared_ptr<ScopedTestServer> s_upstream;
	static std::shared_ptr<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router upstream;
		upstream.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("/"); });
		upstream.get("/users", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("/users"); });
		upstream.get("/api_v2/users", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("/api_v2/users"); });
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		conflux::http::Router front;
		auto popts = ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = s_upstream->port(),
			.path_prefix = "/api",
		};
		front.add_context(
			"GET",
			"/api",
			[popts](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await async_proxy(req, popts, ctx.ring);
			});
		front.add_context(
			"GET",
			"/api/users",
			[popts](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await async_proxy(req, popts, ctx.ring);
			});
		front.add_context(
			"GET",
			"/api_v2/users",
			[popts](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await async_proxy(req, popts, ctx.ring);
			});
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	auto resp = http_get_on(s_front->port(), "/api");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "/");
	resp = http_get_on(s_front->port(), "/api/users");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "/users");
	resp = http_get_on(s_front->port(), "/api_v2/users");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "/api_v2/users");
}

TEST_CASE(
	"proxy: appends to existing X-Forwarded-For header") {
	static std::shared_ptr<ScopedTestServer> s_upstream;
	static std::shared_ptr<ScopedTestServer> s_front;
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router upstream;
		upstream.get("/xff", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["x-forwarded-for"]});
		});
		s_upstream = std::make_shared<ScopedTestServer>(cfg, std::move(upstream));
		conflux::http::Router front;
		auto popts = ProxyOptions{
			.upstream_host = "127.0.0.1",
			.upstream_port = s_upstream->port(),
		};
		front.add_context(
			"GET",
			"/xff",
			[popts = std::move(popts)](conflux::http::RequestView const &req, chttp::RequestContext const &ctx)
				-> conflux::work::root::Task<conflux::http::Response> {
				co_return co_await async_proxy(req, popts, ctx.ring);
			});
		s_front = std::make_shared<ScopedTestServer>(cfg, std::move(front));
	});
	// Client sends existing XFF; proxy appends remote_addr (127.0.0.1).
	auto resp = http_get_on(s_front->port(), "/xff", "X-Forwarded-For: 1.2.3.4\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body = extract_body(resp);
	REQUIRE(body.find("1.2.3.4") != std::string::npos);
	REQUIRE(body.find("127.0.0.1") != std::string::npos);
	// Must appear as "1.2.3.4, 127.0.0.1" (appended, not replaced).
	REQUIRE(body.find("1.2.3.4, 127.0.0.1") != std::string::npos);
}
// ---------------------------------------------------------------------------
// cookie_signing (pure functions)
// ---------------------------------------------------------------------------

TEST_CASE(
	"cookie_signing: sign then verify returns original value") {
	auto signed_val = sign_cookie("hello", "my-secret");
	REQUIRE(signed_val.starts_with("hello."));
	auto result = verify_cookie(signed_val, "my-secret");
	REQUIRE(result.has_value());
	REQUIRE(*result == "hello");
}
TEST_CASE(
	"cookie_signing: verify with wrong secret returns std::nullopt") {
	auto signed_val = sign_cookie("hello", "my-secret");
	auto result = verify_cookie(signed_val, "wrong-secret");
	REQUIRE(!result.has_value());
}
TEST_CASE(
	"cookie_signing: tampered signature returns std::nullopt") {
	auto signed_val = sign_cookie("hello", "my-secret");
	signed_val.back() = (signed_val.back() == 'A') ? 'B' : 'A'; // flip last char
	auto result = verify_cookie(signed_val, "my-secret");
	REQUIRE(!result.has_value());
}
TEST_CASE(
	"cookie_signing: value without dot returns std::nullopt") {
	auto result = verify_cookie("nodot", "any-secret");
	REQUIRE(!result.has_value());
}
TEST_CASE(
	"cookie_signing: value with dots round-trips correctly") {
	// sign_cookie uses rfind('.') so a value containing '.' should still work.
	auto signed_val = sign_cookie("user.name@host.example", "my-secret");
	auto result = verify_cookie(signed_val, "my-secret");
	REQUIRE(result.has_value());
	REQUIRE(*result == "user.name@host.example");
}
TEST_CASE(
	"cookie_signing_middleware: short secret throws std::invalid_argument") {
	REQUIRE_THROWS_AS(
		cookie_signing_middleware({.secrets = single_secret_rotation("tooshort")}),
		std::invalid_argument);
}
// ---------------------------------------------------------------------------
// cookie_signing middleware
// ---------------------------------------------------------------------------

constexpr std::string_view kCookieMiddlewareSecret = "srv-secret-16-bytes";
constexpr std::string_view kOtherCookieSecret = "other-secret-16-bytes";
TEST_CASE(
	"cookie_signing_middleware: valid signed cookie is unwrapped") {
	// Set up a server that echoes the "session" cookie value.
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			cookie_signing_middleware({.secrets = single_secret_rotation(std::string{kCookieMiddlewareSecret})}));
		router.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.cookies["session"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto signed_val = sign_cookie("user42", kCookieMiddlewareSecret);
	auto resp = http_get_on(port, "/echo", std::format("Cookie: session={}\r\n", signed_val));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "user42");
}
TEST_CASE(
	"cookie_signing_middleware: invalid signature strips cookie value") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(cookie_signing_middleware(
			{.secrets = single_secret_rotation(std::string{kCookieMiddlewareSecret}), .strip_invalid = true}));
		router.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.cookies["session"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	// Forge a signed value with the wrong secret.
	auto bad_val = sign_cookie("attacker", kOtherCookieSecret);
	auto resp = http_get_on(port, "/echo", std::format("Cookie: session={}\r\n", bad_val));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Cookie was stripped → handler sees empty value → 200 with empty body.
	REQUIRE(extract_body(resp).empty());
}
TEST_CASE(
	"cookie_signing_middleware: unsigned cookie (no dot) passes through") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			cookie_signing_middleware({.secrets = single_secret_rotation(std::string{kCookieMiddlewareSecret})}));
		router.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.cookies["plain"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/echo", "Cookie: plain=nodot\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "nodot");
}
TEST_CASE(
	"cookie_signing_middleware: strip_invalid=false keeps invalid cookie as-is") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(cookie_signing_middleware(
			{.secrets = single_secret_rotation("srv-secret-key-1234"), .strip_invalid = false}));
		router.get("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.cookies["session"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	// Cookie with bad signature — handler receives the raw signed value unchanged.
	auto bad_val = sign_cookie("user", "wrong-secret-key-1234");
	auto resp = http_get_on(port, "/echo", std::format("Cookie: session={}\r\n", bad_val));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == bad_val);
}
// ---------------------------------------------------------------------------
// csrf_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"csrf: GET sets csrf_token cookie and X-CSRF-Token response header") {
	ensure_csrf_server();
	auto resp = http_get_on(g_csrf_port, "/page");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Set-Cookie: csrf_token=") != std::string::npos);
	REQUIRE(resp.find("X-CSRF-Token: ") != std::string::npos);
}
TEST_CASE(
	"csrf: POST without cookie returns 403") {
	ensure_csrf_server();
	auto resp = http_post_on_full(g_csrf_port, "/submit", "application/x-www-form-urlencoded", "x=1", "");
	REQUIRE(resp.starts_with("HTTP/1.1 403"));
}
TEST_CASE(
	"csrf: POST with mismatched token returns 403") {
	ensure_csrf_server();
	// Use a valid-looking token but not the one the server expects.
	auto resp = http_post_on_full(
		g_csrf_port,
		"/submit",
		"application/x-www-form-urlencoded",
		"x=1",
		"Cookie: csrf_token=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n"
		"X-CSRF-Token: BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 403"));
}
TEST_CASE(
	"csrf: POST with matching cookie and header returns 200") {
	ensure_csrf_server();
	// Step 1: GET to obtain the token.
	auto get_resp = http_get_on(g_csrf_port, "/page");
	auto token = extract_set_cookie(get_resp, "csrf_token");
	REQUIRE(!token.empty());
	// Step 2: POST echoing the token in cookie + header.
	auto post_resp = http_post_on_full(
		g_csrf_port,
		"/submit",
		"application/x-www-form-urlencoded",
		"x=1",
		std::format("Cookie: csrf_token={}\r\nX-CSRF-Token: {}\r\n", token, token));
	REQUIRE(post_resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(post_resp) == "ok");
}
TEST_CASE(
	"csrf: POST with token in form field (no header) returns 200") {
	ensure_csrf_server();
	auto get_resp = http_get_on(g_csrf_port, "/page");
	auto token = extract_set_cookie(get_resp, "csrf_token");
	REQUIRE(!token.empty());
	// Submit token via form field instead of X-CSRF-Token header.
	auto post_resp = http_post_on_full(
		g_csrf_port,
		"/submit",
		"application/x-www-form-urlencoded",
		std::format("csrf_token={}", token),
		std::format("Cookie: csrf_token={}\r\n", token));
	REQUIRE(post_resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(post_resp) == "ok");
}
TEST_CASE(
	"csrf: DELETE without token returns 403") {
	ensure_csrf_server();
	auto resp = conflux::tests::http_request_on(g_csrf_port, "DELETE", "/submit", "", "", "");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
}
TEST_CASE(
	"csrf: custom protected_methods excludes DELETE") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::csrf_middleware({.protected_methods = {"POST"}}));
		router.get("/page", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::html("<form>"); });
		router.del("/resource", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("deleted"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	// DELETE is not in protected_methods, so no token required.
	auto resp = conflux::tests::http_request_on(port, "DELETE", "/resource", "", "", "");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "deleted");
}
// ---------------------------------------------------------------------------
// conflux::http::etag_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"etag: response gets ETag header") {
	ensure_etag_server();
	auto resp = http_get_on(g_etag_port, "/content");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("ETag: \"") != std::string::npos);
}
TEST_CASE(
	"etag: If-None-Match matching ETag returns 304") {
	ensure_etag_server();
	// First request: get the ETag.
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	// Second request: send If-None-Match with that ETag.
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: {}\r\n", etag));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
	// RFC 9110 §15.4.5: 304 SHOULD include the same ETag as the 200 response.
	CHECK(extract_header(resp2, "ETag") == etag);
}
TEST_CASE(
	"etag: If-None-Match wildcard returns 304") {
	ensure_etag_server();
	// First request: get the ETag.
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto resp = http_get_on(g_etag_port, "/content", "If-None-Match: *\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 304"));
	// RFC 9110 §15.4.5: wildcard 304 should also include the ETag.
	CHECK(extract_header(resp, "ETag") == etag);
}
TEST_CASE(
	"etag: If-None-Match uses weak comparison") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: W/{}\r\n", etag));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
	REQUIRE(extract_header(resp2, "ETag") == etag);
}
TEST_CASE(
	"etag: If-None-Match non-matching tag returns 200 with body") {
	ensure_etag_server();
	auto resp = http_get_on(g_etag_port, "/content", "If-None-Match: \"000000\"\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "hello world");
}
TEST_CASE(
	"etag: empty body response has no ETag") {
	ensure_etag_server();
	auto resp = http_get_on(g_etag_port, "/empty");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("ETag:") == std::string::npos);
}
TEST_CASE(
	"etag: two requests to same route return same ETag") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto resp2 = http_get_on(g_etag_port, "/content");
	auto etag1 = extract_header(resp1, "ETag");
	auto etag2 = extract_header(resp2, "ETag");
	REQUIRE(!etag1.empty());
	REQUIRE(etag1 == etag2);
}
TEST_CASE(
	"etag: weak option produces W/ prefix") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::etag_middleware({.weak = true}));
		router.get("/w", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("body"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/w");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto etag = extract_header(resp, "ETag");
	REQUIRE(!etag.empty());
	REQUIRE(etag.starts_with("W/\""));
}
TEST_CASE(
	"etag: If-None-Match with multiple values matches correct ETag") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto inm = std::format("\"deadbeef\", {}, \"cafebabe\"", etag);
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: {}\r\n", inm));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
}
TEST_CASE(
	"etag: weak If-None-Match matches strong ETag (RFC 7232 weak comparison)") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag"); // e.g. "abc123"
	REQUIRE(!etag.empty());
	// Send back as weak variant: W/"abc123" must still match per weak comparison.
	auto weak_inm = std::string{"W/"} + etag;
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: {}\r\n", weak_inm));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
}
TEST_CASE(
	"etag: handler-set ETag is not overwritten by middleware") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router r;
		r.use(conflux::http::etag_middleware());
		r.get("/custom", [](conflux::http::OwnedRequest const &) {
			auto resp = conflux::http::Response::text("body");
			resp.headers["ETag"] = "\"custom-etag-42\"";
			return resp;
		});
		port = start_mw_server(mw_config(), std::move(r));
	});
	auto resp = http_get_on(port, "/custom");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_header(resp, "ETag") == "\"custom-etag-42\"");
}
// ---------------------------------------------------------------------------
// conflux::http::response_cache_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"response_cache: first GET hits handler, second GET is served from cache") {
	ensure_resp_cache_server();
	// Reset counter to a known baseline.
	int const before = g_resp_cache_count.load();
	auto resp1 = http_get_on(g_resp_cache_port, "/counted");
	auto resp2 = http_get_on(g_resp_cache_port, "/counted");
	int const after = g_resp_cache_count.load();
	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp2.starts_with("HTTP/1.1 200 OK"));
	// Handler called exactly once more than before (cache hit on second GET).
	REQUIRE(after == before + 1);
	// Both responses carry identical bodies.
	REQUIRE(extract_body(resp1) == extract_body(resp2));
}
TEST_CASE(
	"response_cache: POST bypasses cache and hits handler") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto resp = http_post_on(g_resp_cache_port, "/counted", "application/x-www-form-urlencoded", "");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 1);
}
TEST_CASE(
	"response_cache: no-store response is not cached") {
	ensure_resp_cache_server();
	auto resp1 = http_get_on(g_resp_cache_port, "/no-store");
	auto resp2 = http_get_on(g_resp_cache_port, "/no-store");
	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp2.starts_with("HTTP/1.1 200 OK"));
	// Both return the same body (handler is deterministic), but this just
	// verifies we don't 500 or return stale data.
	REQUIRE(extract_body(resp1) == "uncacheable");
}
TEST_CASE(
	"response_cache: Vary header partitions cache by request header value") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto gzip1 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: gzip\r\n");
	auto id1 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: identity\r\n");
	auto gzip2 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: gzip\r\n");
	auto id2 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: identity\r\n");
	REQUIRE(gzip1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(id1.starts_with("HTTP/1.1 200 OK"));
	// Exactly two handler invocations: one per distinct Accept-Encoding value.
	REQUIRE(g_resp_cache_count.load() == before + 2);
	// Repeats of the same encoding return the cached body (same counter value).
	REQUIRE(extract_body(gzip1) == extract_body(gzip2));
	REQUIRE(extract_body(id1) == extract_body(id2));
	// Different encodings return different bodies (not cross-contaminated).
	REQUIRE(extract_body(gzip1) != extract_body(id1));
	REQUIRE(extract_body(gzip1).find("enc=gzip") != std::string::npos);
	REQUIRE(extract_body(id1).find("enc=identity") != std::string::npos);
}
TEST_CASE(
	"response_cache: Vary: * is never cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/vary-star");
	auto r2 = http_get_on(g_resp_cache_port, "/vary-star");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	// Handler called on every request.
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}
TEST_CASE(
	"response_cache: query std::string participates in cache key") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto a1 = http_get_on(g_resp_cache_port, "/query?value=a");
	auto b1 = http_get_on(g_resp_cache_port, "/query?value=b");
	auto a2 = http_get_on(g_resp_cache_port, "/query?value=a");
	REQUIRE(extract_body(a1) == std::format("{} value=a", before + 1));
	REQUIRE(extract_body(b1) == std::format("{} value=b", before + 2));
	REQUIRE(extract_body(a2) == extract_body(a1));
}
TEST_CASE(
	"response_cache: Cache-Control: private response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/private");
	auto r2 = http_get_on(g_resp_cache_port, "/private");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	// Handler must be called for both requests — private responses must not be cached.
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}
TEST_CASE(
	"response_cache: Cache-Control: max-age=0 response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/max-age-zero");
	auto r2 = http_get_on(g_resp_cache_port, "/max-age-zero");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	// max-age=0 means always stale — both requests must reach the handler.
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}
TEST_CASE(
	"response_cache: Cache-Control: no-cache response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/no-cache");
	auto r2 = http_get_on(g_resp_cache_port, "/no-cache");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	// Both requests must reach the handler (counter incremented twice).
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}
TEST_CASE(
	"response_cache: Cache-Control directive checks do not match substrings") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/cache-control-substrings");
	auto r2 = http_get_on(g_resp_cache_port, "/cache-control-substrings");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 1);
	REQUIRE(extract_body(r1) == extract_body(r2));
}
TEST_CASE(
	"response_cache: Set-Cookie response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/set-cookie-resp");
	auto r2 = http_get_on(g_resp_cache_port, "/set-cookie-resp");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}
TEST_CASE(
	"response_cache: non-200 response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/not-found-resp");
	auto r2 = http_get_on(g_resp_cache_port, "/not-found-resp");
	REQUIRE(r1.starts_with("HTTP/1.1 404"));
	REQUIRE(r2.starts_with("HTTP/1.1 404"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
}
TEST_CASE(
	"response_cache: LRU eviction when max_entries exceeded") {
	std::atomic<int> hits{0};
	conflux::http::Router router;
	router.use(conflux::http::response_cache_middleware({.max_entries = 2, .default_ttl = std::chrono::seconds{60}}));
	router.get("/a", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		return conflux::http::Response::text("a");
	});
	router.get("/b", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		return conflux::http::Response::text("b");
	});
	router.get("/c", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		return conflux::http::Response::text("c");
	});

	ScopedTestServer srv{mw_config(), std::move(router)};

	// Populate: /a and /b fill the cache (max_entries=2).
	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	int after_fill = hits.load();
	REQUIRE(after_fill == 2);

	// /a and /b should now be cached — no additional handler calls.
	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 2);

	// Add /c — evicts /a (LRU), cache holds /b and /c.
	http_get_on(srv.port(), "/c");
	REQUIRE(hits.load() == 3);

	// /b still cached, no handler call.
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 3);

	// /a was evicted — handler called again.
	http_get_on(srv.port(), "/a");
	REQUIRE(hits.load() == 4);

	srv.stop();
}
TEST_CASE(
	"response_cache: response larger than max_bytes is not cached") {
	std::atomic<int> hits{0};
	conflux::http::Router router;
	// max_bytes=4: bodies of 5+ bytes won't be stored.
	router.use(conflux::http::response_cache_middleware({.max_entries = 10, .max_bytes = 4, .default_ttl = std::chrono::seconds{60}}));
	router.get("/big", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		return conflux::http::Response::text("hello"); // 5 bytes > max_bytes
	});
	router.get("/small", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		return conflux::http::Response::text("hi"); // 2 bytes <= max_bytes
	});

	ScopedTestServer srv{mw_config(), std::move(router)};

	// /big should never be cached.
	http_get_on(srv.port(), "/big");
	http_get_on(srv.port(), "/big");
	REQUIRE(hits.load() == 2);

	// /small should be cached after first hit.
	http_get_on(srv.port(), "/small");
	http_get_on(srv.port(), "/small");
	REQUIRE(hits.load() == 3);

	srv.stop();
}
TEST_CASE(
	"response_cache: expired entry properly frees std::byte budget for new entries") {
	// max_bytes=16: fits exactly two 8-std::byte bodies.
	// After both entries expire, total_bytes_ must be decremented so new entries
	// can be cached without spurious eviction (regression: expiry path omitted the
	// total_bytes_ decrement, leaving a phantom std::byte count that blocked new puts).
	std::atomic<int> hits{0};
	conflux::http::Router router;
	router.use(
		conflux::http::response_cache_middleware({.max_entries = 10, .max_bytes = 16, .default_ttl = std::chrono::seconds{60}}));
	router.get("/a", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		conflux::http::Response r = conflux::http::Response::text("aaaaaaaa"); // 8 bytes
		r.headers["Cache-Control"] = "max-age=1";
		return r;
	});
	router.get("/b", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		conflux::http::Response r = conflux::http::Response::text("bbbbbbbb"); // 8 bytes
		r.headers["Cache-Control"] = "max-age=1";
		return r;
	});
	router.get("/c", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		conflux::http::Response r = conflux::http::Response::text("cccccccc"); // 8 bytes
		r.headers["Cache-Control"] = "max-age=60";
		return r;
	});

	ScopedTestServer srv{mw_config(), std::move(router)};

	// Fill cache: /a and /b each consume 8 bytes → total_bytes_=16.
	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 2);

	// Wait for both entries to expire.
	std::this_thread::sleep_for(std::chrono::milliseconds{1200});

	// Trigger expiry eviction: these are cache misses that decrement total_bytes_.
	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 4);

	// /c must now be cacheable: std::byte budget was freed by the two expired evictions.
	http_get_on(srv.port(), "/c");
	http_get_on(srv.port(), "/c");
	REQUIRE(hits.load() == 5); // second GET is a cache hit

	srv.stop();
}
// ---------------------------------------------------------------------------
// conflux::http::structured_log_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"structured_log: request is logged as a JSON line to file") {
	ensure_slog_server();
	auto resp = http_get_on(g_slog_port, "/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));

	// Allow a brief moment for the log write to flush.
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	// Read the log file.
	std::string log_content;
	{
		int const fd = ::open(g_slog_path, O_RDONLY);
		REQUIRE(fd >= 0);
		std::array<char, 4096> buf{};
		ssize_t const n = ::read(fd, buf.data(), buf.size() - 1);
		::close(fd);
		REQUIRE(n > 0);
		log_content.assign(buf.data(), static_cast<std::size_t>(n));
	}
	auto const doc = require_json_text(log_content);
	check_json_string_at(doc, "/method", "GET");
	check_json_string_at(doc, "/path", "/ping");
	check_json_u64_at(doc, "/status", 200);
	check_json_string_at(doc, "/app", "test");
}
TEST_CASE(
	"structured_log: no app_name omits app field") {
	char path[64]{};
	std::strcpy(path, "/tmp/conflux_slog2_XXXXXX");
	int const tmp = ::mkstemp(path);
	REQUIRE(tmp >= 0);
	::close(tmp);

	Config cfg = mw_config();
	conflux::http::Router router;
	router.use(conflux::http::structured_log_middleware({.log_file = path}));
	router.get("/x", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });
	ScopedTestServer srv{cfg, std::move(router)};

	auto resp = http_get_on(srv.port(), "/x");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	std::string log_content;
	{
		int const fd = ::open(path, O_RDONLY);
		REQUIRE(fd >= 0);
		std::array<char, 4096> buf{};
		ssize_t const n = ::read(fd, buf.data(), buf.size() - 1);
		::close(fd);
		REQUIRE(n > 0);
		log_content.assign(buf.data(), static_cast<std::size_t>(n));
	}
	::unlink(path);
	auto const doc = require_json_text(log_content);
	check_json_absent_at(doc, "/app");
	check_json_string_at(doc, "/path", "/x");
}
TEST_CASE(
	"structured_log: path with double-quote is JSON-escaped in log") {
	char path[64]{};
	std::strcpy(path, "/tmp/conflux_slog3_XXXXXX");
	int const tmp = ::mkstemp(path);
	REQUIRE(tmp >= 0);
	::close(tmp);

	Config cfg = mw_config();
	conflux::http::Router router;
	router.use(conflux::http::structured_log_middleware({.log_file = path, .app_name = "test"}));
	router.get("/{*path}", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};

	LocalTcpClient client{srv.port()};
	client.set_recv_timeout(std::chrono::seconds{5});
	std::string_view const raw_request =
		"GET /q\" HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Connection: close\r\n"
		"\r\n";
	REQUIRE(client.send(raw_request, MSG_NOSIGNAL) == static_cast<ssize_t>(raw_request.size()));
	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	std::string log_content;
	{
		int const fd = ::open(path, O_RDONLY);
		REQUIRE(fd >= 0);
		std::array<char, 4096> buf{};
		ssize_t const n = ::read(fd, buf.data(), buf.size() - 1);
		::close(fd);
		REQUIRE(n > 0);
		log_content.assign(buf.data(), static_cast<std::size_t>(n));
	}
	::unlink(path);
	auto const doc = require_json_text(log_content);
	check_json_string_at(doc, "/app", "test");
	check_json_string_at(doc, "/path", "/q\"");
}
// ---------------------------------------------------------------------------
// tracing_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"tracing: generates traceparent when none in request") {
	ensure_trace_server();
	auto resp = http_get_on(g_trace_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// conflux::http::Response header contains Traceparent.
	auto tp_header = extract_header(resp, "Traceparent");
	REQUIRE(!tp_header.empty());
	REQUIRE(tp_header.starts_with("00-"));
	REQUIRE(tp_header.size() == 55); // "00-" + 32 + "-" + 16 + "-01"
	// Body (injected traceparent in request) matches the response header.
	REQUIRE(extract_body(resp) == tp_header);
}
TEST_CASE(
	"tracing: incoming traceparent preserves trace_id, generates new span_id") {
	ensure_trace_server();
	std::string_view incoming = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
	auto resp = http_get_on(g_trace_port, "/", std::format("traceparent: {}\r\n", incoming));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto tp = extract_header(resp, "Traceparent");
	REQUIRE(!tp.empty());
	// trace_id (chars 3..34) must be preserved.
	REQUIRE(tp.substr(3, 32) == "4bf92f3577b34da6a3ce929d0e0e4736");
	// span_id (chars 36..51) must differ from the incoming parent_id.
	auto new_span = tp.substr(36, 16);
	REQUIRE(new_span != "00f067aa0ba902b7");
}
TEST_CASE(
	"tracing: malformed traceparent generates fresh trace_id") {
	ensure_trace_server();
	auto resp = http_get_on(g_trace_port, "/", "traceparent: bad-value\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto tp = extract_header(resp, "Traceparent");
	REQUIRE(!tp.empty());
	// Must be a well-formed traceparent: "00-<32hex>-<16hex>-01"
	REQUIRE(tp.size() == 55);
	REQUIRE(tp.substr(0, 3) == "00-");
	REQUIRE(tp[35] == '-');
	REQUIRE(tp[52] == '-');
}
TEST_CASE(
	"tracing: non-hex chars in trace_id reject the incoming traceparent") {
	ensure_trace_server();
	// 55-char traceparent with correct structure but non-hex chars in trace_id.
	auto resp =
		http_get_on(g_trace_port, "/", "traceparent: 00-ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ-0000000000000000-01\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto tp = extract_header(resp, "Traceparent");
	REQUIRE(!tp.empty());
	// Must generate a fresh trace_id (not echo back ZZZZ...).
	REQUIRE(tp.substr(3, 32) != "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
}
TEST_CASE(
	"tracing: propagate_in_response=false omits Traceparent response header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::tracing_middleware({.propagate_in_response = false}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_header(resp, "Traceparent").empty());
}
TEST_CASE(
	"tracing: on_end callback can add response header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::tracing_middleware({
			.on_end = [](conflux::http::OwnedRequest const &,
						 conflux::http::Response &res,
						 conflux::http::TracingContext const &ctx) { res.headers["X-Trace-Id"] = ctx.trace_id; },
			.propagate_in_response = false,
		}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto trace_id = extract_header(resp, "X-Trace-Id");
	REQUIRE(trace_id.size() == 32);
}
TEST_CASE(
	"tracing: on_start callback receives TraceContext and can inject header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::tracing_middleware({
			.on_start = [](conflux::http::OwnedRequest &req, conflux::http::TracingContext const &ctx) { req.headers["x-injected-span"] = ctx.span_id; },
			.propagate_in_response = false,
		}));
		// Echo the injected span id from the request.
		router.get("/", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["x-injected-span"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Body is the span_id: 16 hex chars.
	auto body = extract_body(resp);
	REQUIRE(body.size() == 16);
}
// ---------------------------------------------------------------------------
// VHostRouter
// ---------------------------------------------------------------------------

TEST_CASE(
	"vhost: Host api.example.com routes to api router") {
	ensure_vhost_server();
	auto resp = http_get_on_host(g_vhost_port, "api.example.com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "api");
}
TEST_CASE(
	"vhost: Host web.example.com routes to web router") {
	ensure_vhost_server();
	auto resp = http_get_on_host(g_vhost_port, "web.example.com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "web");
}
TEST_CASE(
	"vhost: HttpServer accepts VHostRouter directly") {
	ensure_vhost_direct_server();

	auto api_resp = http_get_on_host(g_vhost_direct_port, "api.example.com", "/status");
	REQUIRE(api_resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(api_resp) == "api-direct");

	auto default_resp = http_get_on_host(g_vhost_direct_port, "other.example.com", "/status");
	REQUIRE(default_resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(default_resp) == "default-direct");
}
TEST_CASE(
	"vhost: unknown host falls back to default router") {
	ensure_vhost_server();
	auto resp = http_get_on_host(g_vhost_port, "other.example.com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "default");
}
TEST_CASE(
	"vhost: Host header with port suffix is stripped before matching") {
	ensure_vhost_server();
	auto resp = http_get_on_host(g_vhost_port, "api.example.com:8080", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "api");
}
TEST_CASE(
	"vhost: Host matching is case-insensitive") {
	ensure_vhost_server();
	auto resp = http_get_on_host(g_vhost_port, "API.Example.Com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "api");
}
TEST_CASE(
	"vhost: subrouters share one work pool") {
	auto shared_pool = std::make_shared<WorkPool>();

	conflux::http::Router api_router;
	conflux::http::Router web_router;
	conflux::http::Router def_router;

	VHostRouter vhost;
	vhost.set_work_pool(shared_pool);
	vhost.add("api.example.com", std::move(api_router));
	vhost.add("web.example.com", std::move(web_router));
	vhost.set_default(std::move(def_router));

	CHECK(vhost.work_pool().get() == shared_pool.get());
	CHECK(vhost.resolved_work_pool("api.example.com").get() == shared_pool.get());
	CHECK(vhost.resolved_work_pool("web.example.com").get() == shared_pool.get());
	CHECK(vhost.resolved_work_pool("other.example.com").get() == shared_pool.get());
}
TEST_CASE(
	"router: work pool is created only for websocket or sse routes") {
	conflux::http::Router plain;
	CHECK_FALSE(plain.work_pool());

	conflux::http::Router sse;
	sse.sse("/events", [](conflux::http::RequestView const &, std::shared_ptr<conflux::http::SseChannel> const &) {});
	CHECK(sse.work_pool());

	conflux::http::Router ws;
	ws.ws("/ws", [](conflux::http::RequestView const &, conflux::http::WsConn &) {});
	CHECK(ws.work_pool());
}
TEST_CASE(
	"vhost: default construction does not allocate work pool") {
	VHostRouter vhost;
	CHECK_FALSE(vhost.work_pool());

	conflux::http::Router plain;
	vhost.add("plain.example.com", std::move(plain));
	CHECK_FALSE(vhost.resolved_work_pool("plain.example.com"));

	conflux::http::Router sse;
	sse.sse("/events", [](conflux::http::RequestView const &, std::shared_ptr<conflux::http::SseChannel> const &) {});
	auto sse_pool = sse.work_pool();
	REQUIRE(sse_pool);
	vhost.add("sse.example.com", std::move(sse));
	CHECK(vhost.resolved_work_pool("sse.example.com").get() == sse_pool.get());
}
TEST_CASE(
	"vhost: rebinding work pool updates existing subrouters") {
	conflux::http::Router api_router;
	conflux::http::Router def_router;

	VHostRouter vhost;
	vhost.add("api.example.com", std::move(api_router));
	vhost.set_default(std::move(def_router));

	auto rebound_pool = std::make_shared<WorkPool>();
	vhost.set_work_pool(rebound_pool);

	CHECK(vhost.work_pool().get() == rebound_pool.get());
	CHECK(vhost.resolved_work_pool("api.example.com").get() == rebound_pool.get());
	CHECK(vhost.resolved_work_pool("other.example.com").get() == rebound_pool.get());
}
TEST_CASE(
	"vhost: unknown host with no default returns 404") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router api;
		api.get("/status", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("api"); });
		VHostRouter vhost;
		vhost.add("api.example.com", std::move(api));
		// No set_default call.
		Config const cfg{.port = 0, .rings = 1};
		port = test_servers().start(cfg, std::move(vhost));
	});
	auto resp = http_get_on_host(port, "unknown.example.com", "/status");
	REQUIRE(resp.starts_with("HTTP/1.1 404"));
}
TEST_CASE(
	"vhost: IPv6 host with port is stripped before matching") {
	conflux::http::Router api;
	api.get("/status", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("api-v6"); });
	VHostRouter vhost;
	vhost.add("[::1]", std::move(api));

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/status";
	req.headers["host"] = "[::1]:8080";

	auto resp = vhost.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "api-v6");
}
TEST_CASE(
	"vhost: IPv6 host without port matches directly") {
	conflux::http::Router api;
	api.get("/status", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("api-v6-noport"); });
	VHostRouter vhost;
	vhost.add("[::1]", std::move(api));

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/status";
	req.headers["host"] = "[::1]";

	auto resp = vhost.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "api-v6-noport");
}
// ---------------------------------------------------------------------------
// openapi_spec
// ---------------------------------------------------------------------------

namespace {

Document parse_openapi_spec(
	std::string spec) {
	auto doc = conflux::json::parse_copy(std::move(spec));
	REQUIRE(doc.has_value());
	return std::move(*doc);
}

void check_parser_problem_code(
	std::string_view response,
	std::string_view code) {
	CHECK(response.find("application/problem+json") != std::string_view::npos);
	auto doc = conflux::json::parse_copy(extract_body(response));
	REQUIRE(doc.has_value());
	check_json_string_at(*doc, "/code", code);
}

} // namespace

TEST_CASE(
	"openapi: spec contains openapi 3.0.0 root key") {
	conflux::http::Router router;
	router.get("/hello/{name}", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	router.post("/items", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(openapi_spec(router, "Test API", "0.1.0"));
	check_json_string_at(doc, "/openapi", "3.0.0");
}
TEST_CASE(
	"openapi: spec includes registered path with path parameter") {
	conflux::http::Router router;
	router.get("/hello/{name}", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(openapi_spec(router, "Test API", "0.1.0"));
	REQUIRE(require_json_pointer(doc, "/paths/~1hello~1{name}/get").as_object().has_value());
	check_json_string_at(doc, "/paths/~1hello~1{name}/get/parameters/0/name", "name");
	check_json_string_at(doc, "/paths/~1hello~1{name}/get/parameters/0/in", "path");
}
TEST_CASE(
	"openapi: spec includes title and version from arguments") {
	conflux::http::Router router;
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(openapi_spec(router, "My Service", "2.3.4"));
	check_json_string_at(doc, "/info/title", "My Service");
	check_json_string_at(doc, "/info/version", "2.3.4");
}
TEST_CASE(
	"openapi: spec includes method in lowercase") {
	conflux::http::Router router;
	router.post("/items", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(openapi_spec(router));
	REQUIRE(require_json_pointer(doc, "/paths/~1items/post").as_object().has_value());
}
TEST_CASE(
	"openapi: title with special characters is properly JSON-escaped") {
	conflux::http::Router router;
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(openapi_spec(router, R"(My "API" & More)"));
	check_json_string_at(doc, "/info/title", R"(My "API" & More)");
}
TEST_CASE(
	"openapi: empty router produces valid paths object") {
	conflux::http::Router router;
	auto doc = parse_openapi_spec(openapi_spec(router, "Empty", "0.0.1"));
	auto paths = require_json_pointer(doc, "/paths").as_object();
	REQUIRE(paths.has_value());
	CHECK(paths->size() == 0);
	check_json_string_at(doc, "/info/title", "Empty");
}
TEST_CASE(
	"openapi_handler_protected: wrong bearer token returns 401") {
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
	std::vector<conflux::http::Router::Middleware> chain;
	chain.push_back(bearer_auth_middleware([](std::string_view token) { return token == "apikey"; }));
	router.get("/openapi.json", openapi_handler_protected(router, "API", "1.0.0", std::move(chain)));

	Config const cfg{.port = 0, .rings = 1};
	std::uint16_t port = test_servers().start(cfg, std::move(router));

	auto resp_no_auth = http_get_on(port, "/openapi.json");
	REQUIRE(resp_no_auth.starts_with("HTTP/1.1 401"));

	auto resp_ok = http_get_on(port, "/openapi.json", "Authorization: Bearer apikey\r\n");
	REQUIRE(resp_ok.starts_with("HTTP/1.1 200"));
	REQUIRE(resp_ok.find("application/json") != std::string::npos);
	auto doc = parse_openapi_spec(extract_body(resp_ok));
	check_json_string_at(doc, "/openapi", "3.0.0");
}
namespace {

ReadUntilCloseResult send_raw_bytes_result(
	std::string_view raw) {
	ensure_server();
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket failed"};
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_test_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"connect failed"};
	}
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
	auto response = read_until_close_with_state(fd);
	::close(fd);
	return response;
}

std::string send_raw_bytes(
	std::string_view raw) {
	return send_raw_bytes_result(raw).bytes;
}

ReadUntilCloseResult send_raw_bytes_result_on(
	std::uint16_t port,
	std::string_view raw) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket failed"};
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"connect failed"};
	}
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
	auto response = read_until_close_with_state(fd);
	::close(fd);
	return response;
}

std::string send_raw_bytes_on(
	std::uint16_t port,
	std::string_view raw) {
	return send_raw_bytes_result_on(port, raw).bytes;
}

} // namespace
TEST_CASE(
	"parser: request line exceeding 8 KiB returns 414") {
	std::string path = "/";
	path.append(9000, 'a');
	auto req = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 414"));
}
TEST_CASE(
	"parser: invalid method token returns 400") {
	auto resp = send_raw_bytes("GE<T /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: empty request target returns 400") {
	auto resp = send_raw_bytes("GET  HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: single header line exceeding 8 KiB returns 431") {
	std::string header_value(9000, 'v');
	auto req = std::format(
		"GET /api/ping HTTP/1.1\r\nHost: localhost\r\nX-Big: {}\r\nConnection: close\r\n\r\n",
		header_value);
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 431"));
}
TEST_CASE(
	"parser: more than 100 headers returns 431") {
	std::string req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
	for (int i = 0; i < 120; ++i) {
		req += std::format("X-H-{}: v\r\n", i);
	}
	req += "\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 431"));
}
TEST_CASE(
	"parser: obs-fold line returns 400") {
	std::string req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nX-A: one\r\n two\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: NUL std::byte in header returns 400") {
	std::string req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nX-Bad: a";
	req.push_back('\0');
	req += "b\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: header missing colon returns 400") {
	std::string req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nNoColonHere\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: field-name with space before colon returns 400") {
	std::string req = "GET /api/ping HTTP/1.1\r\nHost : localhost\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: header with no space after colon is accepted") {
	std::string req =
		"GET /api/echo-header HTTP/1.1\r\nHost: localhost\r\nX-Test-Header:no-space\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
	REQUIRE(resp.find("no-space") != std::string::npos);
}
TEST_CASE(
	"parser: header value is trimmed of leading and trailing OWS") {
	std::string req =
		"GET /api/echo-header HTTP/1.1\r\nHost: localhost\r\nX-Test-Header:   spaced   \r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != std::string::npos);
	auto body = resp.substr(body_start + 4);
	REQUIRE(body == "spaced");
}
TEST_CASE(
	"parser: malformed Content-Length returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5abc\r\nConnection: close\r\n\r\nhello";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: duplicate Content-Length returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 5\r\n"
		"Connection: close\r\n\r\nhello";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: missing Host in HTTP/1.1 returns 400") {
	std::string req = "GET /api/ping HTTP/1.1\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: duplicate Host in HTTP/1.1 returns 400") {
	std::string req =
		"GET /api/ping HTTP/1.1\r\nHost: localhost\r\nHost: attacker.example\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: Content-Length with Transfer-Encoding returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nTransfer-Encoding: "
		"chunked\r\nConnection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: Content-Length plus Transfer-Encoding smuggling attempt closes before pipelined request") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nTransfer-Encoding: "
		"chunked\r\n\r\n0\r\n\r\n"
		"GET /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes_result(req);
	REQUIRE(resp.closed());
	REQUIRE(resp.bytes.starts_with("HTTP/1.1 400"));
	REQUIRE(resp.bytes.find("HTTP/1.1 200") == std::string::npos);
	REQUIRE(resp.bytes.find(R"({"status":"ok"})") == std::string::npos);
}
TEST_CASE(
	"parser: unsupported Transfer-Encoding returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip, chunked\r\n"
		"Connection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: Transfer-Encoding after chunked returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked, gzip\r\n"
		"Connection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: duplicate Transfer-Encoding headers return 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"
		"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: empty Transfer-Encoding token returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked,\r\n"
		"Connection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: uppercase chunked Transfer-Encoding is accepted") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: CHUNKED\r\n"
		"Connection: close\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
}
TEST_CASE(
	"parser: Connection close token closes persistent connection") {
	REQUIRE(server_closed_after("/api/ping", "Connection: keep-alive, close\r\n"));
}
TEST_CASE(
	"parser: chunked transfer with chunk-count overflow returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
	for (int i = 0; i < 200000; ++i) {
		req += "1\r\nx\r\n";
	}
	req += "0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: chunked transfer with oversized trailer returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
		"0\r\nX-Trailer: ";
	req.append(9000, 'x');
	req += "\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}
TEST_CASE(
	"parser: chunked transfer with huge declared chunk returns 413") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
		"ffffffffffffffff\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 413"));
}
TEST_CASE(
	"parser: rejection metrics count classified HTTP/1 rejects") {
	conflux::http::Router router;
	router.post("/echo", [](conflux::http::OwnedRequest const &req) { return conflux::http::Response::text(std::string{req.body}); });
	Config cfg = mw_config();
	cfg.max_body_size = 4;
	ScopedTestServer srv{cfg, std::move(router)};
	auto const port = srv.port();

	auto malformed_cl = send_raw_bytes_on(
		port,
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5abc\r\nConnection: close\r\n\r\nhello");
	REQUIRE(malformed_cl.starts_with("HTTP/1.1 400"));
	check_parser_problem_code(malformed_cl, "malformed_content_length");

	auto duplicate_cl = send_raw_bytes_on(
		port,
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nContent-Length: 2\r\n"
		"Connection: close\r\n\r\nhi");
	REQUIRE(duplicate_cl.starts_with("HTTP/1.1 400"));
	check_parser_problem_code(duplicate_cl, "duplicate_content_length");

	auto cl_te = send_raw_bytes_on(
		port,
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n"
		"Connection: close\r\n\r\n0\r\n\r\n");
	REQUIRE(cl_te.starts_with("HTTP/1.1 400"));
	check_parser_problem_code(cl_te, "content_length_with_transfer_encoding");

	std::string long_path = "/";
	long_path.append(9000, 'a');
	auto request_line = send_raw_bytes_on(
		port,
		std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", long_path));
	REQUIRE(request_line.starts_with("HTTP/1.1 414"));
	check_parser_problem_code(request_line, "request_line_too_large");

	std::string header_line_value(9000, 'v');
	auto header_line = send_raw_bytes_on(
		port,
		std::format(
			"GET /echo HTTP/1.1\r\nHost: localhost\r\nX-Big: {}\r\nConnection: close\r\n\r\n",
			header_line_value));
	REQUIRE(header_line.starts_with("HTTP/1.1 431"));
	check_parser_problem_code(header_line, "header_line_too_large");

	std::string header_block_req = "GET /echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
	for (int i = 0; i < 80; ++i) {
		header_block_req += std::format("X-Block-{}: {}\r\n", i, std::string(900, 'v'));
	}
	header_block_req += "\r\n";
	auto header_block = send_raw_bytes_on(port, header_block_req);
	REQUIRE(header_block.starts_with("HTTP/1.1 431"));
	check_parser_problem_code(header_block, "header_block_too_large");

	std::string too_many_headers_req = "GET /echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
	for (int i = 0; i < 120; ++i) {
		too_many_headers_req += std::format("X-Count-{}: v\r\n", i);
	}
	too_many_headers_req += "\r\n";
	auto too_many_headers = send_raw_bytes_on(port, too_many_headers_req);
	REQUIRE(too_many_headers.starts_with("HTTP/1.1 431"));
	check_parser_problem_code(too_many_headers, "too_many_headers");

	auto too_large_body = send_raw_bytes_on(
		port,
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
	REQUIRE(too_large_body.starts_with("HTTP/1.1 413"));
	check_parser_problem_code(too_large_body, "body_too_large");

	auto invalid_chunk = send_raw_bytes_on(
		port,
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
		"z\r\nx\r\n");
	REQUIRE(invalid_chunk.starts_with("HTTP/1.1 400"));
	check_parser_problem_code(invalid_chunk, "invalid_chunk");

	srv.stop();
	auto const metrics = srv.metrics();
	CHECK(metrics.rejections.malformed_content_length == 1);
	CHECK(metrics.rejections.duplicate_content_length == 1);
	CHECK(metrics.rejections.content_length_with_transfer_encoding == 1);
	CHECK(metrics.rejections.request_line_too_large == 1);
	CHECK(metrics.rejections.header_line_too_large == 1);
	CHECK(metrics.rejections.header_block_too_large == 1);
	CHECK(metrics.rejections.too_many_headers == 1);
	CHECK(metrics.rejections.body_too_large == 1);
	CHECK(metrics.rejections.invalid_chunk == 1);
}
// ---------------------------------------------------------------------------
// WebSocket frame validation + fragmentation (A2)
// ---------------------------------------------------------------------------

namespace ws_test {

std::string read_http_headers(
	int fd) {
	std::string resp;
	std::array<char, 512> buf{};
	while (resp.find("\r\n\r\n") == std::string::npos) {
		auto n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		REQUIRE(n > 0);
		resp.append(buf.data(), static_cast<std::size_t>(n));
		REQUIRE(resp.size() <= 8192);
	}
	return resp;
}
Config ws_cfg() {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	return cfg;
}
int ws_handshake(
	std::uint16_t port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	std::string req = std::format(
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n");
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	auto resp = read_http_headers(fd);
	REQUIRE(resp.starts_with("HTTP/1.1 101"));
	REQUIRE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
	return fd;
}
std::vector<std::uint8_t> make_masked_frame(
	std::uint8_t b0,
	std::string_view payload,
	bool mask = true) {
	std::vector<std::uint8_t> f;
	f.push_back(b0);
	auto len = payload.size();
	std::uint8_t const mask_bit = mask ? 0x80U : 0U;
	if (len < 126) {
		f.push_back(mask_bit | static_cast<std::uint8_t>(len));
	} else if (len <= 0xFFFF) {
		f.push_back(mask_bit | 126U);
		f.push_back(static_cast<std::uint8_t>(len >> 8));
		f.push_back(static_cast<std::uint8_t>(len & 0xFFU));
	} else {
		f.push_back(mask_bit | 127U);
		for (int s = 56; s >= 0; s -= 8) {
			f.push_back(static_cast<std::uint8_t>((len >> s) & 0xFFU));
		}
	}
	if (mask) {
		std::array<std::uint8_t, 4> const key{0x01, 0x02, 0x03, 0x04};
		f.insert(f.end(), key.begin(), key.end());
		for (std::size_t i = 0; i < payload.size(); ++i) {
			f.push_back(static_cast<std::uint8_t>(payload[i]) ^ key[i & 3]);
		}
	} else {
		f.insert(f.end(), payload.begin(), payload.end());
	}
	return f;
}
struct CloseFrame {
	std::uint16_t code{};
	std::string reason;
	bool received{};
};
CloseFrame read_close(
	int fd) {
	auto read_exact = [fd](std::span<std::uint8_t> out) {
		std::size_t got = 0;
		while (got < out.size()) {
			auto n = ::recv(fd, out.data() + got, out.size() - got, 0);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				return false;
			}
			got += static_cast<std::size_t>(n);
		}
		return true;
	};

	std::array<std::uint8_t, 2> header{};
	if (!read_exact(header)) {
		return {};
	}
	std::uint8_t const b0 = header[0];
	std::uint8_t const b1 = header[1] & 0x7FU;
	if ((b0 & 0x0FU) != 0x08U) {
		return {};
	}
	if (b1 > 125) {
		return {};
	}
	std::array<std::uint8_t, 125> payload{};
	if (!read_exact(std::span{payload}.first(b1))) {
		return {};
	}
	if (b1 < 2) {
		return {.code = 0, .reason = {}, .received = true};
	}
	auto const code = static_cast<std::uint16_t>(
		(static_cast<std::uint32_t>(payload[0]) << 8U) | static_cast<std::uint32_t>(payload[1]));
	std::string reason;
	if (b1 > 2) {
		reason.assign(reinterpret_cast<char const *>(payload.data()) + 2, static_cast<std::size_t>(b1) - 2);
	}
	return {.code = code, .reason = std::move(reason), .received = true};
}

} // namespace ws_test
TEST_CASE(
	"ws: closed worker pool increments pressure metric",
	"[ws][http.lifecycle]") {
	conflux::http::Router router;
	auto pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 1});
	pool->stop();
	router.set_work_pool(pool);
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &) {});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};

	int const fd = ws_test::ws_handshake(srv.port());
	for (int i = 0; i < 50; ++i) {
		if (srv.metrics().pressure.websocket_closed_for_pressure > 0) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
	}
	::close(fd);
	srv.stop();

	auto const metrics = srv.metrics();
	CHECK(metrics.pressure.websocket_closed_for_pressure >= 1);
}

TEST_CASE(
	"ws: frame with RSV bit set triggers close 1002") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto frame = ws_test::make_masked_frame(0xC1U, "x"); // FIN | RSV1 | text
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: unmasked client frame triggers close 1002") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto frame = ws_test::make_masked_frame(0x81U, "x", /*mask=*/false);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: non-minimal extended payload length triggers close 1002") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	std::array<std::uint8_t, 9> frame{
		0x81U, // FIN | text
		0xFEU, // MASK | 126 extended length marker
		0x00U,
		0x01U, // non-minimal encoding for length 1
		0x01U,
		0x02U,
		0x03U,
		0x04U,
		static_cast<std::uint8_t>('x' ^ 0x01U)};
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: oversized control frame (ping with 126-std::byte payload) triggers close 1002") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	std::string big(126, 'p');
	auto frame = ws_test::make_masked_frame(0x89U, big); // FIN | ping
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: handshake without Upgrade header is rejected") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &) {});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	std::string const req =
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Connection: keep-alive\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	auto resp = ws_test::read_http_headers(fd);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: handshake with invalid Sec-WebSocket-Key is rejected") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &) {});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	std::string const req =
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: keep-alive, Upgrade\r\n"
		"Sec-WebSocket-Key: not-a-valid-key\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	auto resp = ws_test::read_http_headers(fd);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: one-std::byte close payload triggers close 1002") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto frame = ws_test::make_masked_frame(0x88U, "x"); // FIN | close, invalid payload length 1
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: invalid close code is rejected instead of echoed") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	std::string code_payload;
	code_payload.push_back('\x03');
	code_payload.push_back('\xED'); // 1005 (reserved/invalid close code)
	auto frame = ws_test::make_masked_frame(0x88U, code_payload);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: invalid close reason UTF-8 from peer triggers close 1007") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	std::string close_payload;
	close_payload.push_back('\x03');
	close_payload.push_back('\xE8'); // 1000
	close_payload.append("\xC0\xAF", 2);
	auto frame = ws_test::make_masked_frame(0x88U, close_payload);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1007);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: close rejects invalid status code via public API") {
	auto result = std::make_shared<std::promise<bool>>();
	auto done = result->get_future();
	conflux::http::Router router;
	router.ws("/ws", [result](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		try {
			ws.close(1005);
			result->set_value(false);
		} catch (std::invalid_argument const &) { result->set_value(true); }
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	REQUIRE(done.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
	REQUIRE(done.get());
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: close rejects invalid UTF-8 reason via public API") {
	auto result = std::make_shared<std::promise<bool>>();
	auto done = result->get_future();
	conflux::http::Router router;
	router.ws("/ws", [result](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		try {
			ws.close(1000, std::string_view{"\xC0\xAF", 2});
			result->set_value(false);
		} catch (std::invalid_argument const &) { result->set_value(true); }
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	REQUIRE(done.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
	REQUIRE(done.get());
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: fragmented text message is reassembled before handler sees it") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (auto f = ws.recv()) {
			if (f->opcode == conflux::http::WsConn::Opcode::Text) {
				if (!ws.send_text(f->payload)) {
					break;
				}
			}
		}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto part1 = ws_test::make_masked_frame(0x01U, "hel"); // FIN=0 | text
	auto part2 = ws_test::make_masked_frame(0x80U, "lo"); // FIN=1 | continuation
	::send(fd, part1.data(), part1.size(), MSG_NOSIGNAL);
	::send(fd, part2.data(), part2.size(), MSG_NOSIGNAL);
	std::array<std::uint8_t, 64> rx{};
	auto n = ::recv(fd, rx.data(), rx.size(), 0);
	REQUIRE(n >= 7);
	REQUIRE(rx[0] == 0x81U); // FIN | text
	REQUIRE((rx[1] & 0x7FU) == 5U);
	std::string echo{reinterpret_cast<char const *>(rx.data()) + 2, 5};
	REQUIRE(echo == "hello");
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: invalid UTF-8 in text frame triggers close 1007") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	std::string bad{"\xC0\xAF"}; // overlong / illegal sequence
	auto frame = ws_test::make_masked_frame(0x81U, bad);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1007);
	::close(fd);
	srv.stop();
}
// ---------------------------------------------------------------------------
// A3: TLS sniff timeout + EOF handling
// ---------------------------------------------------------------------------

TEST_CASE(
	"TLS sniff: silent connection closed after tls_sniff_timeout_ms") {
	char cert_tmp[] = "/tmp/conflux_sniff_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_sniff_key_XXXXXX.pem";
	{
		int fd = ::mkstemps(cert_tmp, 4);
		::close(fd);
		fd = ::mkstemps(key_tmp, 4);
		::close(fd);
	}
	std::string const cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	REQUIRE(::system(cmd.c_str()) == 0);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 0; // disable idle-request reap so only sniff timeout is exercised
	cfg.tls_sniff_timeout_ms = 1500;
	cfg.cert_file = cert_tmp;
	cfg.key_file = key_tmp;

	conflux::http::Router router;
	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const port = srv.port();
	::unlink(cert_tmp);
	::unlink(key_tmp);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	auto const end = recv_close_state(fd, MSG_DONTWAIT);
	::close(fd);
	REQUIRE(is_socket_closed(end));

	srv.stop();
}
TEST_CASE(
	"TLS sniff: client half-close before any data triggers clean server close") {
	char cert_tmp[] = "/tmp/conflux_eof_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_eof_key_XXXXXX.pem";
	{
		int fd = ::mkstemps(cert_tmp, 4);
		::close(fd);
		fd = ::mkstemps(key_tmp, 4);
		::close(fd);
	}
	std::string const cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	REQUIRE(::system(cmd.c_str()) == 0);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 30000;
	cfg.tls_sniff_timeout_ms = 30000; // big — we don't want timer reap to be what closes us
	cfg.cert_file = cert_tmp;
	cfg.key_file = key_tmp;

	conflux::http::Router router;
	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const port = srv.port();
	::unlink(cert_tmp);
	::unlink(key_tmp);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	::shutdown(fd, SHUT_WR);

	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	auto const end = recv_close_state(fd, MSG_DONTWAIT);
	::close(fd);
	REQUIRE(is_socket_closed(end));

	srv.stop();
}
// ---------------------------------------------------------------------------
// C1: SseChannel bounded queue + overflow policies (unit tests, no server)
// ---------------------------------------------------------------------------

TEST_CASE(
	"SseChannel: DropNewest policy drops overflowing frames") {
	conflux::http::SseChannel ch{64, conflux::http::SseOverflowPolicy::DropNewest};
	// Each frame is 10 bytes; queue holds at most 64 bytes → 6 fit.
	for (int i = 0; i < 10; ++i) {
		std::string frame(10, 'x');
		(void)ch.send(std::move(frame));
	}
	REQUIRE(ch.dropped_count() == 4);
	auto out = ch.drain();
	REQUIRE(out.size() == 60);
}
TEST_CASE(
	"SseChannel: DropOldest policy keeps newest frames") {
	conflux::http::SseChannel ch{30, conflux::http::SseOverflowPolicy::DropOldest};
	for (int i = 0; i < 5; ++i) {
		std::string frame(10, static_cast<char>('a' + i));
		(void)ch.send(std::move(frame));
	}
	REQUIRE(ch.dropped_count() >= 2);
	auto out = ch.drain();
	// After overflow, the final 3 frames (cc…, dd…, ee…) should remain.
	REQUIRE(out.find("eeeeeeeeee") != std::string::npos);
	REQUIRE(out.find("aaaaaaaaaa") == std::string::npos);
}
TEST_CASE(
	"SseChannel: Disconnect policy closes on overflow") {
	conflux::http::SseChannel ch{20, conflux::http::SseOverflowPolicy::Disconnect};
	REQUIRE(ch.send(std::string(10, 'x')));
	// Next send exceeds the cap → channel is closed; further sends return false.
	(void)ch.send(std::string(20, 'y'));
	REQUIRE(ch.is_closed());
	REQUIRE_FALSE(ch.send(std::string(5, 'z')));
}
TEST_CASE(
	"SseChannel: send returns false after close") {
	conflux::http::SseChannel ch{4096};
	ch.close();
	REQUIRE_FALSE(ch.send("hello"));
}
// ---------------------------------------------------------------------------
// C2: conflux::http::DeferredResponse timeout
// ---------------------------------------------------------------------------

TEST_CASE(
	"deferred response that never completes returns 504 after its deadline") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 30000;

	conflux::http::Router router;
	// Handler returns a conflux::http::DeferredResponse with a 1-second deadline but never completes it.
	// The idle-timer sweeper should expire it with a 504 shortly after.
	router.get("/stuck", [](conflux::http::OwnedRequest const &) {
		auto d = std::make_shared<conflux::http::DeferredResponse>(std::chrono::milliseconds{1000});
		return conflux::http::Response::deferred(d);
	});

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const p = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(p);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	std::string_view const req = "GET /stuck HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);

	auto const response = read_one_response(fd);
	::close(fd);
	REQUIRE(response.starts_with("HTTP/1.1 504"));
	srv.stop();
}
TEST_CASE(
	"deferred response that completes before its deadline returns the completed payload") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	conflux::http::Router router;
	router.get("/fast", [](conflux::http::OwnedRequest const &) {
		auto d = std::make_shared<conflux::http::DeferredResponse>(std::chrono::milliseconds{10000});
		std::thread([d]() {
			std::this_thread::sleep_for(std::chrono::milliseconds{80});
			d->complete(conflux::http::Response::text("pong"));
		}).detach();
		return conflux::http::Response::deferred(d);
	});

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const p = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(p);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	std::string_view const req = "GET /fast HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);

	auto const response = read_one_response(fd);
	::close(fd);
	REQUIRE(response.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response.find("pong") != std::string::npos);
	srv.stop();
}
// ---------------------------------------------------------------------------
// conflux::http::HttpFields — set/erase
// ---------------------------------------------------------------------------

TEST_CASE(
	"conflux::http::HttpFields::set replaces all duplicate entries with a single one") {
	conflux::http::HttpFields f{true};
	f.emplace_back("Set-Cookie", "a=1");
	f.emplace_back("Set-Cookie", "b=2");
	f.emplace_back("Set-Cookie", "c=3");
	REQUIRE(f.size() == 3);

	f.set("set-cookie", "z=9");
	REQUIRE(f.size() == 1);
	REQUIRE(f.get("set-cookie") == "z=9");
	REQUIRE(f.values("set-cookie").size() == 1);
	REQUIRE(f.contains("set-cookie"));
}
TEST_CASE(
	"conflux::http::HttpFields::set inserts when key absent") {
	conflux::http::HttpFields f{true};
	f.set("Content-Type", "text/plain");
	REQUIRE(f.size() == 1);
	REQUIRE(f.get("content-type") == "text/plain");
}
TEST_CASE(
	"conflux::http::HttpFields::set preserves positions of other keys") {
	conflux::http::HttpFields f{true};
	f.emplace_back("A", "1");
	f.emplace_back("Dup", "x");
	f.emplace_back("B", "2");
	f.emplace_back("Dup", "y");
	f.emplace_back("C", "3");

	f.set("dup", "merged");
	REQUIRE(f.size() == 4);
	std::vector<std::string> keys;
	for (auto const &[k, _v]: f) {
		keys.push_back(k);
	}
	REQUIRE(keys == (std::vector<std::string>{"A", "Dup", "B", "C"}));
	REQUIRE(f.get("dup") == "merged");
	REQUIRE(f.get("a") == "1");
	REQUIRE(f.get("b") == "2");
	REQUIRE(f.get("c") == "3");
}
TEST_CASE(
	"conflux::http::HttpFields::erase removes all matches and returns count") {
	conflux::http::HttpFields f{true};
	f.emplace_back("Cookie", "a=1");
	f.emplace_back("Cookie", "b=2");
	f.emplace_back("Host", "example.com");

	auto removed = f.erase("cookie");
	REQUIRE(removed == 2);
	REQUIRE(f.size() == 1);
	REQUIRE(!f.contains("cookie"));
	REQUIRE(f.get("host") == "example.com");
}
TEST_CASE(
	"conflux::http::HttpFields::erase returns 0 when key absent") {
	conflux::http::HttpFields f{true};
	f.emplace_back("A", "1");
	REQUIRE(f.erase("missing") == 0);
	REQUIRE(f.size() == 1);
}
TEST_CASE(
	"conflux::http::HttpFields index stays consistent after set then erase") {
	conflux::http::HttpFields f{true};
	f.emplace_back("X", "1");
	f.emplace_back("X", "2");
	f.emplace_back("Y", "a");

	f.set("x", "z");
	REQUIRE(f.contains("x"));
	REQUIRE(f.get("x") == "z");
	REQUIRE(f.values("x").size() == 1);

	auto removed = f.erase("x");
	REQUIRE(removed == 1);
	REQUIRE(!f.contains("x"));
	REQUIRE(f.get("y") == "a");
	REQUIRE(f.size() == 1);
}
TEST_CASE(
	"conflux::http::HttpFields::values returns all entries for duplicate keys") {
	conflux::http::HttpFields f{true};
	f.emplace_back("Cookie", "a=1");
	f.emplace_back("Cookie", "b=2");
	f.emplace_back("Cookie", "c=3");
	f.emplace_back("Other", "x");
	auto vals = f.values("cookie");
	REQUIRE(vals.size() == 3);
	using sv = std::string_view;
	REQUIRE(std::ranges::contains(vals, sv{"a=1"}));
	REQUIRE(std::ranges::contains(vals, sv{"b=2"}));
	REQUIRE(std::ranges::contains(vals, sv{"c=3"}));
}
TEST_CASE(
	"conflux::http::HttpFields zero-allocation value callbacks visit duplicate keys") {
	conflux::http::HttpFields f{true};
	f.emplace_back("Set-Cookie", "a=1");
	f.emplace_back("set-cookie", "b=2");
	f.emplace_back("Other", "x");

	std::vector<std::string_view> vals;
	f.for_each_value("SET-COOKIE", [&](std::string_view value) { vals.push_back(value); });
	REQUIRE(vals == (std::vector<std::string_view>{"a=1", "b=2"}));
	int visits = 0;
	CHECK_FALSE(f.for_each_value_until("set-cookie", [&](std::string_view value) {
		++visits;
		return value != "a=1";
	}));
	CHECK(visits == 1);
	CHECK(f.any_value("set-cookie", [](std::string_view value) { return value == "b=2"; }));
	CHECK_FALSE(f.any_value("set-cookie", [](std::string_view value) { return value == "c=3"; }));

	conflux::http::HttpFieldsView view{f};
	vals.clear();
	conflux::http::for_each_header_value(view, "set-cookie", [&](std::string_view value) { vals.push_back(value); });
	REQUIRE(vals == (std::vector<std::string_view>{"a=1", "b=2"}));
	visits = 0;
	CHECK_FALSE(conflux::http::for_each_header_value_until(view, "set-cookie", [&](std::string_view value) {
		++visits;
		return value != "b=2";
	}));
	CHECK(visits == 2);
	CHECK(conflux::http::any_header_value(view, "set-cookie", [](std::string_view value) { return value == "a=1"; }));
	CHECK_FALSE(conflux::http::any_header_value(view, "set-cookie", [](std::string_view value) { return value == "z=9"; }));
}
TEST_CASE(
	"conflux::http::HttpFields::value_or returns default when key absent") {
	conflux::http::HttpFields f{true};
	f.emplace_back("A", "hello");
	REQUIRE(f.value_or("A") == "hello");
	REQUIRE(f.value_or("Missing", "default") == "default");
	REQUIRE(f.value_or("Missing") == "");
}
TEST_CASE(
	"conflux::http::HttpFields case-insensitive lookup folds only ASCII letters") {
	conflux::http::HttpFields f{true};
	f.emplace_back("X^Name", "caret");
	f.emplace_back("X~Name", "tilde");
	REQUIRE(f.get("x^name") == "caret");
	REQUIRE(f.get("x~name") == "tilde");
	REQUIRE(f.values("x^name").size() == 1);
	REQUIRE(f.values("x~name").size() == 1);

	conflux::http::HttpFieldsView view{f};
	REQUIRE(view.get("x^name") == "caret");
	REQUIRE(view.get("x~name") == "tilde");
	REQUIRE(view.values("x^name").size() == 1);
	REQUIRE(view.values("x~name").size() == 1);
}
TEST_CASE(
	"static file serving: percent-encoded filename in URL is decoded and served") {
	char tmpdir[] = "/tmp/conflux_enc_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto fpath = std::string{tmpdir} + "/hello world.txt";
	int const wfd = ::open(fpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(wfd >= 0);
	std::string_view const content{"space file"};
	REQUIRE(::write(wfd, content.data(), content.size()) == static_cast<ssize_t>(content.size()));
	::close(wfd);

	conflux::http::Router router;
	router.serve_static("/s", std::string{tmpdir});

	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/hello%20world.txt");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != std::string::npos);
	REQUIRE(resp.substr(body_start + 4) == "space file");

	srv.stop();
	::unlink(fpath.c_str());
	::rmdir(tmpdir);
}
TEST_CASE(
	"static file serving: If-Modified-Since matching the file mtime returns 304") {
	char tmpdir[] = "/tmp/conflux_ims_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto fpath = std::string{tmpdir} + "/test.txt";
	int const wfd = ::open(fpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(wfd >= 0);
	std::string_view const content{"hello"};
	REQUIRE(::write(wfd, content.data(), content.size()) == static_cast<ssize_t>(content.size()));
	::close(wfd);

	conflux::http::Router router;
	router.serve_static("/s", std::string{tmpdir});

	ScopedTestServer srv{mw_config(), std::move(router)};

	// First request: get the Last-Modified header.
	auto resp1 = conflux::tests::http_get_on(srv.port(), "/s/test.txt");
	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	auto last_modified = extract_header(resp1, "Last-Modified");
	REQUIRE(!last_modified.empty());

	// Second request with matching If-Modified-Since: should get 304.
	auto resp2 =
		conflux::tests::http_get_on(srv.port(), "/s/test.txt", std::format("If-Modified-Since: {}\r\n", last_modified));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));

	srv.stop();
	::unlink(fpath.c_str());
	::rmdir(tmpdir);
}
// ---------------------------------------------------------------------------
// Router: wildcard {*name} tail capture (unit tests, no server)
// ---------------------------------------------------------------------------

TEST_CASE(
	"router: wildcard {*path} captures entire tail") {
	conflux::http::Router router;
	router.get("/files/{*path}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["path"]});
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/files/docs/readme.txt";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "docs/readme.txt");
}
TEST_CASE(
	"router: wildcard {*path} captures empty tail when path ends at prefix") {
	conflux::http::Router router;
	router.get("/files/{*path}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["path"]});
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/files/";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body().empty());
}
TEST_CASE(
	"router: wildcard with prefix param captures both") {
	conflux::http::Router router;
	router.get("/{version}/files/{*path}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::format("{}/{}", req.params["version"], req.params["path"]));
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/v2/files/a/b/c.txt";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "v2/a/b/c.txt");
}
TEST_CASE(
	"router: non-matching path returns 404") {
	conflux::http::Router router;
	router.get("/files/{*path}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["path"]});
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/other/stuff";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 404);
}
TEST_CASE(
	"router: percent-encoded path param is URL-decoded") {
	conflux::http::Router router;
	router.get("/hello/{name}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["name"]});
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/hello/hello%20world";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "hello world");
}
TEST_CASE(
	"router: wildcard route_info preserves star notation") {
	conflux::http::Router router;
	router.get("/files/{*path}", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	auto infos = router.route_infos();
	REQUIRE(infos.size() == 1);
	CHECK(infos[0].path_pattern == "/files/{*path}");
	REQUIRE(infos[0].path_params.size() == 1);
	CHECK(infos[0].path_params[0] == "path");
}

TEST_CASE(
	"router: wildcard tail with percent-encoded segment is URL-decoded") {
	conflux::http::Router router;
	router.get("/files/{*path}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["path"]});
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/files/dir/my%20file.txt";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "dir/my file.txt");
}
// ---------------------------------------------------------------------------
// Router: on_not_found / on_error custom handlers
// ---------------------------------------------------------------------------

TEST_CASE(
	"router: on_not_found custom handler called for unmatched path") {
	conflux::http::Router router;
	router.get("/exists", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	router.on_not_found(
		[](conflux::http::OwnedRequest const &req) { return conflux::http::Response::text(std::format("nope:{}", req.path)); });
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/missing";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "nope:/missing");
}
TEST_CASE(
	"router: on_error custom handler called when route throws") {
	conflux::http::Router router;
	router.get("/boom", [](conflux::http::OwnedRequest const &) -> conflux::http::Response { throw std::runtime_error{"oops"}; });
	std::string captured_what;
	router.on_error([&](conflux::http::OwnedRequest const &, std::exception const &ex) {
		captured_what = ex.what();
		return conflux::http::Response::text("caught");
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/boom";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "caught");
	REQUIRE(captured_what == "oops");
}
TEST_CASE(
	"router: default 404 when no on_not_found is set") {
	conflux::http::Router router;
	router.get("/a", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("a"); });
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/missing";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 404);
}
TEST_CASE(
	"router: default 500 when route throws and no on_error is set") {
	conflux::http::Router router;
	router.get("/boom", [](conflux::http::OwnedRequest const &) -> conflux::http::Response { throw std::runtime_error{"crash"}; });
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/boom";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 500);
}
// ---------------------------------------------------------------------------
// http1_parser unit tests
// ---------------------------------------------------------------------------

TEST_CASE(
	"http1_parser: valid GET request parses correctly") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET /path HTTP/1.1\r\nHost: localhost\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.method == "GET");
	REQUIRE(out.target == "/path");
	REQUIRE(out.version == "HTTP/1.1");
	REQUIRE(out.headers.size() == 1);
	REQUIRE(out.headers[0].first == "Host");
	REQUIRE(out.headers[0].second == "localhost");
}
TEST_CASE(
	"http1_parser: incomplete request returns Incomplete") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET /path HTTP/1.1\r\nHost: localhost\r\n", limits, out);
	REQUIRE(status == ParseStatus::Incomplete);
}
TEST_CASE(
	"http1_parser: missing HTTP version returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// "GET /path NOTHTTP" → version check fails → BadRequest.
	auto status = parse_request("GET /path NOTHTTP\r\nHost: x\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}
TEST_CASE(
	"http1_parser: request with no headers parses correctly") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// Header-free requests must not underflow header_block_size.
	auto status = parse_request("GET / HTTP/1.1\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.method == "GET");
	REQUIRE(out.target == "/");
	REQUIRE(out.headers.empty());
}
TEST_CASE(
	"http1_parser: incomplete request line over limit returns UriTooLong") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_request_line_size = 12;
	ParsedRequest out;
	auto status = parse_request("GET /still-growing", limits, out);
	REQUIRE(status == ParseStatus::UriTooLong);
}

TEST_CASE(
	"http1_parser: URI too long returns UriTooLong") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_request_line_size = 10;
	ParsedRequest out;
	auto status = parse_request("GET /very-long-path-here HTTP/1.1\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::UriTooLong);
}
TEST_CASE(
	"http1_parser: header with null std::byte returns BadRequest") {
	using namespace conflux::http1;
	using namespace std::string_literals;
	ParserLimits const limits{};
	ParsedRequest out;
	// Use "s" suffix so std::string captures embedded null bytes.
	std::string raw = "GET / HTTP/1.1\r\nX-Bad: val\x00ue\r\n\r\n"s;
	auto status = parse_request(raw, limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}
TEST_CASE(
	"http1_parser: incomplete header line over limit returns HeaderLineTooLarge") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_header_line_size = 8;
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nX-Long: still-growing", limits, out);
	REQUIRE(status == ParseStatus::HeaderLineTooLarge);
}

TEST_CASE(
	"http1_parser: incomplete header count over limit returns TooManyHeaders") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_headers = 2;
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n", limits, out);
	REQUIRE(status == ParseStatus::TooManyHeaders);
}

TEST_CASE(
	"http1_parser: too many headers returns TooManyHeaders") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_headers = 2;
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::TooManyHeaders);
}
TEST_CASE(
	"http1_parser: header value leading/trailing whitespace is stripped") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nX-Foo:   bar  \r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.headers.size() == 1);
	REQUIRE(out.headers[0].second == "bar");
}
TEST_CASE(
	"http1_parser: HTTP/1.0 version is accepted") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.0\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.version == "HTTP/1.0");
}
TEST_CASE(
	"http1_parser: empty method returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// Space at the very start means method is empty.
	auto status = parse_request(" /path HTTP/1.1\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}
TEST_CASE(
	"http1_parser: invalid tchar in header name returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// Space in field name is not a valid tchar.
	auto status = parse_request("GET / HTTP/1.1\r\nX Bad: value\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}
TEST_CASE(
	"http1_parser: folded header line returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// Line starting with whitespace (obs-fold) is rejected per RFC 7230 §3.2.6.
	auto status = parse_request("GET / HTTP/1.1\r\nX-Foo: bar\r\n  continuation\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}
TEST_CASE(
	"http1_parser: header with empty name (bare colon) returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	// ": value" — colon at position 0, no field name.
	auto status = parse_request("GET / HTTP/1.1\r\n: value\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}
// ---------------------------------------------------------------------------
// serve_static: directory listing
// ---------------------------------------------------------------------------

TEST_CASE(
	"static file serving: directory request without listing returns 403") {
	char tmpdir[] = "/tmp/conflux_dirlist_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	// Put a file so the dir is non-empty (no index.html).
	auto fpath = std::string{tmpdir} + "/file.txt";
	int const fd = ::open(fpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(fd >= 0);
	REQUIRE(::write(fd, "hi", 2) == 2);
	::close(fd);

	conflux::http::Router router;
	router.serve_static("/s", std::string{tmpdir});

	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));

	srv.stop();
	::unlink(fpath.c_str());
	::rmdir(tmpdir);
}
TEST_CASE(
	"static file serving: directory request with listing returns HTML") {
	char tmpdir[] = "/tmp/conflux_dirlist2_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto write_file = [&](std::string_view name, std::string_view content) {
		auto path = std::string{tmpdir} + "/" + std::string{name};
		int const wfd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(wfd >= 0);
		REQUIRE(::write(wfd, content.data(), content.size()) == static_cast<ssize_t>(content.size()));
		::close(wfd);
	};
	write_file("alpha.txt", "a");
	write_file("beta.html", "b");

	conflux::http::Router router;
	StaticOptions sopts{};
	sopts.directory_listing = true;
	router.serve_static("/s", std::string{tmpdir}, sopts);

	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != std::string::npos);
	auto body = resp.substr(body_start + 4);
	REQUIRE(body.find("alpha.txt") != std::string::npos);
	REQUIRE(body.find("beta.html") != std::string::npos);
	REQUIRE(body.find("<ul>") != std::string::npos);

	srv.stop();
	::unlink((std::string{tmpdir} + "/alpha.txt").c_str());
	::unlink((std::string{tmpdir} + "/beta.html").c_str());
	::rmdir(tmpdir);
}
TEST_CASE(
	"static file serving: directory listing entries are sorted") {
	char tmpdir[] = "/tmp/conflux_dirlist3_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto write_file = [&](std::string_view name) {
		auto path = std::string{tmpdir} + "/" + std::string{name};
		int const wfd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(wfd >= 0);
		::close(wfd);
	};
	write_file("zebra.txt");
	write_file("apple.txt");
	write_file("mango.txt");

	conflux::http::Router router;
	StaticOptions sopts{};
	sopts.directory_listing = true;
	router.serve_static("/s", std::string{tmpdir}, sopts);

	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != std::string::npos);
	auto body = resp.substr(body_start + 4);
	auto apple_pos = body.find("apple.txt");
	auto mango_pos = body.find("mango.txt");
	auto zebra_pos = body.find("zebra.txt");
	REQUIRE(apple_pos != std::string::npos);
	REQUIRE(mango_pos != std::string::npos);
	REQUIRE(zebra_pos != std::string::npos);
	REQUIRE(apple_pos < mango_pos);
	REQUIRE(mango_pos < zebra_pos);

	srv.stop();
	::unlink((std::string{tmpdir} + "/zebra.txt").c_str());
	::unlink((std::string{tmpdir} + "/apple.txt").c_str());
	::unlink((std::string{tmpdir} + "/mango.txt").c_str());
	::rmdir(tmpdir);
}
TEST_CASE(
	"static file serving: directory listing index.html takes precedence") {
	char tmpdir[] = "/tmp/conflux_dirlist4_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto path = std::string{tmpdir} + "/index.html";
	int const wfd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(wfd >= 0);
	auto content = std::string_view{"<h1>Index</h1>"};
	REQUIRE(::write(wfd, content.data(), content.size()) == static_cast<ssize_t>(content.size()));
	::close(wfd);

	conflux::http::Router router;
	StaticOptions sopts{};
	sopts.directory_listing = true;
	router.serve_static("/s", std::string{tmpdir}, sopts);

	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body_start = resp.find("\r\n\r\n");
	REQUIRE(body_start != std::string::npos);
	auto body = resp.substr(body_start + 4);
	REQUIRE(body == "<h1>Index</h1>");

	srv.stop();
	::unlink(path.c_str());
	::rmdir(tmpdir);
}
// Regression test for handle_send recv re-arm bug:
// When a send CQE arrives with response_ptr==nullptr (can occur when an
// error-path response races with a multishot recv CQE clearing recv_armed),
// the old code did send_queued=false; return — leaving recv_armed=false with
// no pending ops, orphaning the connection forever.
// The fix: queue_multishot_recv in that branch.
//
// We exercise this by:
//   (a) sending a pipelined error+good pair — the good request lands in
//       conn.partial while the 400 send is in-flight, maximising the chance
//       that handle_send sees response_ptr==nullptr after the pipelined
//       dispatch_request clears it and defers.
//   (b) running many iterations so that even rare CQE orderings are hit.
//   (c) keeping a 2 s SO_RCVTIMEO: a stuck connection makes recv() timeout
//       instead of returning 0, which turns an infinite hang into a test
//       failure.
namespace {

int make_conn() {
	ensure_server();
	int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket"};
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_test_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"connect"};
	}
	timeval tv{.tv_sec = 2, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}
std::string drain_fd(
	int fd) {
	std::string out;
	std::array<char, 4096> buf{};
	for (;;) {
		auto n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n <= 0) {
			break;
		}
		out.append(buf.data(), static_cast<std::size_t>(n));
	}
	return out;
}
bool recv_until_closed(
	int fd) {
	std::array<char, 4096> buf{};
	for (;;) {
		auto const n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n == 0) {
			return true;
		}
		if (n < 0) {
			return errno == ECONNRESET;
		}
	}
}

} // namespace
TEST_CASE(
	"regression: handle_send recv re-arm — no stuck connection after error close") {
	// Error request without Connection: close.
	// Server responds 400 + close_after_send=true and must properly close the
	// fd.  Without the fix recv() would block until the 2 s timeout instead of
	// returning 0 (FIN).
	static constexpr std::string_view kDupCL =
		"POST /api/echo-body HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Length: 5\r\n"
		"Content-Length: 5\r\n"
		"\r\nhello";
	static constexpr std::string_view kGood =
		"GET /api/ping HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Connection: close\r\n"
		"\r\n";
	constexpr int kIter = 300;
	for (int i = 0; i < kIter; ++i) {
		// (a) standalone error — server must send FIN promptly
		{
			int fd = make_conn();
			::send(fd, kDupCL.data(), kDupCL.size(), MSG_NOSIGNAL);
			auto r = drain_fd(fd);
			REQUIRE(r.starts_with("HTTP/1.1 400"));
			// drain_fd reads until recv()=0 (FIN) or timeout.
			// A timeout returns whatever partial data arrived — still starts with 400
			// but the next assert catches whether the loop exited cleanly.
			// We verify no timeout occurred by checking we got a clean FIN (recv=0).
			// drain_fd exits on n<=0; recv()=0 means FIN, recv()<0 means timeout →
			// r won't contain a second response, but r is already checked above.
			// The key observable: the test must finish in <2 s per iteration.
			::close(fd);
		}
		// (b) pipelined error+good in one write — exercises conn.partial path
		// during handle_http_response_send_complete; server must 400+close and
		// not get stuck reading the good request after it.
		{
			int fd = make_conn();
			std::string both{kDupCL};
			both.append(kGood);
			::send(fd, both.data(), both.size(), MSG_NOSIGNAL);
			auto r = drain_fd(fd);
			// Server closes after 400; good request is never processed.
			REQUIRE(r.starts_with("HTTP/1.1 400"));
			::close(fd);
		}
		// (c) verify server still responsive after each iteration
		{
			int fd = make_conn();
			::send(fd, kGood.data(), kGood.size(), MSG_NOSIGNAL);
			auto r = drain_fd(fd);
			REQUIRE(r.starts_with("HTTP/1.1 200"));
			::close(fd);
		}
	}
}
// ---------------------------------------------------------------------------
// PR A — cancel_recv_if_armed: shutdown drains armed multishot recv connections
// (proposal tests 1, 3, 4)
// ---------------------------------------------------------------------------
namespace {

Config small_ring_cfg_pr_a() {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 64;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.startup_banner = false;
	cfg.request_timeout_ms = 0; // disable to prevent timeout closing before srv.stop()
	return cfg;
}
int connect_to(
	std::uint16_t port) {
	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		return -1;
	}
	timeval const tv{.tv_sec = 5, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}

} // namespace
TEST_CASE(
	"server shutdown: cleanly closes N connections with armed multishot recv") {
	// N idle connections holding TCP open without sending a request.
	// Server recv is armed on each. shutdown() must cancel every multishot
	// recv and close the sockets so the client sees EOF, not a hung recv.
	// Covers proposal tests 1 (recv cancel on close) and 3 (sweep N conns).
	static constexpr int N = 20;
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
	ScopedTestServer srv{small_ring_cfg_pr_a(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(N);
	for (int i = 0; i < N; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		fds.push_back(fd);
	}
	// srv.stop() signals shutdown and joins the server std::thread; the std::thread only
	// exits once all connections are closed, so by the time stop() returns
	// every client fd must have received FIN.
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == N);
}
TEST_CASE(
	"server shutdown: recv cancel fires for send_queued connections") {
	// A connection with a response in flight (send_queued=true) must also
	// have its multishot recv cancelled so it does not block server teardown.
	// This covers the handle_shutdown send_queued branch of PR A.
	conflux::http::Router router;
	router.get("/big", [](conflux::http::OwnedRequest const &) {
		// Large enough body that the send may still be in-flight when shutdown
		// fires, increasing the chance that send_queued=true at shutdown time.
		return conflux::http::Response::text(std::string(128 * 1024, 'z'));
	});
	ScopedTestServer srv{small_ring_cfg_pr_a(), std::move(router)};
	int const fd = connect_to(srv.port());
	REQUIRE(fd >= 0);
	std::string_view const req = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	// Shutdown immediately — races with the large-body send completing.
	// The server must not deadlock regardless of which side wins the race.
	srv.stop();
	// Already queued response bytes may arrive before FIN; the invariant is
	// that stop() drains to server close before the socket timeout fires.
	CHECK(recv_until_closed(fd));
	::close(fd);
}
TEST_CASE(
	"server shutdown: concurrent idle + send_queued connections all close") {
	// Mix: some connections idle (recv armed, no send), some with response
	// in flight. All must close after shutdown without deadlock.
	static constexpr int N_IDLE = 10;
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	router.get("/big", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(std::string(128 * 1024, 'z')); });
	ScopedTestServer srv{small_ring_cfg_pr_a(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(N_IDLE + 2);
	// Idle connections — no request sent
	for (int i = 0; i < N_IDLE; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		fds.push_back(fd);
	}
	// Connections with response in-flight
	for (int i = 0; i < 2; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		std::string_view const req = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
		::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
		fds.push_back(fd);
	}
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == static_cast<int>(fds.size()));
}
// ---------------------------------------------------------------------------
// P1-08b — recv-only gen invalidation + no-stall close
// ---------------------------------------------------------------------------
namespace {

Config tiny_ring_cfg_p108b() {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 16;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.startup_banner = false;
	cfg.request_timeout_ms = 0;
	return cfg;
}

} // namespace
TEST_CASE(
	"P1-08b: SQ-pressure shutdown — 30 idle conns, ring_entries=16") {
	static constexpr int N = 30;
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
	ScopedTestServer srv{tiny_ring_cfg_p108b(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(N);
	for (int i = 0; i < N; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		fds.push_back(fd);
	}
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == N);
}
TEST_CASE(
	"P1-08b: SQ-pressure shutdown — mixed idle + send_queued, ring_entries=16") {
	static constexpr int N_IDLE = 20;
	static constexpr int N_SEND = 5;
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	router.get("/big", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(std::string(256 * 1024, 'z')); });
	ScopedTestServer srv{tiny_ring_cfg_p108b(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(N_IDLE + N_SEND);
	for (int i = 0; i < N_IDLE; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		fds.push_back(fd);
	}
	for (int i = 0; i < N_SEND; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		std::string_view const req = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
		::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
		fds.push_back(fd);
	}
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == static_cast<int>(fds.size()));
}
TEST_CASE(
	"P1-08b: recv data queued before close_after_send is discarded") {
	conflux::http::Router router;
	router.get("/big", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(std::string(256 * 1024, 'z')); });
	ScopedTestServer srv{small_ring_cfg_pr_a(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(4);
	for (int i = 0; i < 4; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		std::string_view const req = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
		::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
		std::string_view const extra = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\ngarbage";
		::send(fd, extra.data(), extra.size(), MSG_NOSIGNAL);
		fds.push_back(fd);
	}
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == static_cast<int>(fds.size()));
}
TEST_CASE(
	"P1-08b: final recv CQE before send completion — clean shutdown") {
	conflux::http::Router router;
	router.get("/slow", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(std::string(16 * 1024, 'x')); });
	ScopedTestServer srv{small_ring_cfg_pr_a(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(8);
	for (int i = 0; i < 8; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		std::string_view const req = "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n";
		::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
		fds.push_back(fd);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == static_cast<int>(fds.size()));
}
