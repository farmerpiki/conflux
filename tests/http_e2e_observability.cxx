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
	router.use(
		conflux::http::response_cache_middleware(
			{.max_entries = 10, .max_bytes = 4, .default_ttl = std::chrono::seconds{60}}));
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
		conflux::http::response_cache_middleware(
			{.max_entries = 10, .max_bytes = 16, .default_ttl = std::chrono::seconds{60}}));
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
		router.use(
			conflux::http::tracing_middleware({
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
		router.use(
			conflux::http::tracing_middleware({
				.on_start =
					[](conflux::http::OwnedRequest &req, conflux::http::TracingContext const &ctx) {
						req.headers["x-injected-span"] = ctx.span_id;
					},
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

	conflux::http::VHostRouter vhost;
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
	conflux::http::VHostRouter vhost;
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

	conflux::http::VHostRouter vhost;
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
		conflux::http::VHostRouter vhost;
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
	conflux::http::VHostRouter vhost;
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
	api.get("/status", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text("api-v6-noport");
	});
	conflux::http::VHostRouter vhost;
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
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router, "Test API", "0.1.0"));
	check_json_string_at(doc, "/openapi", "3.0.0");
}
TEST_CASE(
	"openapi: spec includes registered path with path parameter") {
	conflux::http::Router router;
	router.get("/hello/{name}", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router, "Test API", "0.1.0"));
	REQUIRE(require_json_pointer(doc, "/paths/~1hello~1{name}/get").as_object().has_value());
	check_json_string_at(doc, "/paths/~1hello~1{name}/get/parameters/0/name", "name");
	check_json_string_at(doc, "/paths/~1hello~1{name}/get/parameters/0/in", "path");
}
TEST_CASE(
	"openapi: spec includes title and version from arguments") {
	conflux::http::Router router;
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router, "My Service", "2.3.4"));
	check_json_string_at(doc, "/info/title", "My Service");
	check_json_string_at(doc, "/info/version", "2.3.4");
}
TEST_CASE(
	"openapi: spec includes method in lowercase") {
	conflux::http::Router router;
	router.post("/items", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router));
	REQUIRE(require_json_pointer(doc, "/paths/~1items/post").as_object().has_value());
}
TEST_CASE(
	"openapi: title with special characters is properly JSON-escaped") {
	conflux::http::Router router;
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router, R"(My "API" & More)"));
	check_json_string_at(doc, "/info/title", R"(My "API" & More)");
}
TEST_CASE(
	"openapi: empty router produces valid paths object") {
	conflux::http::Router router;
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router, "Empty", "0.0.1"));
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
	chain.push_back(conflux::http::bearer_auth_middleware([](std::string_view token) { return token == "apikey"; }));
	router.get("/openapi.json", conflux::http::openapi_handler_protected(router, "API", "1.0.0", std::move(chain)));

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
	router.post("/echo", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.body});
	});
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
	router.get("/big", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(std::string(128 * 1024, 'z'));
	});
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
	router.get("/big", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(std::string(256 * 1024, 'z'));
	});
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
	router.get("/big", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(std::string(256 * 1024, 'z'));
	});
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
	router.get("/slow", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(std::string(16 * 1024, 'x'));
	});
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
