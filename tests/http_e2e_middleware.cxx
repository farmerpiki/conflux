// ---------------------------------------------------------------------------
// WebSocket upgrade
// ---------------------------------------------------------------------------

TEST_CASE(
	"WebSocket upgrade performs handshake, echoes text frames, closes cleanly") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		// Echo all text frames until connection closes.
		while (auto frame = ws.recv()) {
			if (frame->opcode == conflux::http::WsConn::Opcode::Text) {
				if (!ws.send_text(frame->payload)) {
					break;
				}
			}
		}
	});

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const ws_port = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ws_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	// --- Handshake ---
	// RFC 6455 §1.3 test V: key → accept.
	std::string_view const ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
	std::string upgrade_req = std::format(
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: {}\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n",
		ws_key);
	::send(fd, upgrade_req.data(), upgrade_req.size(), 0);

	std::array<char, 4096> buf{};
	auto n = ::recv(fd, buf.data(), buf.size(), 0);
	REQUIRE(n > 0);
	std::string_view const resp{buf.data(), static_cast<std::size_t>(n)};
	REQUIRE(resp.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
	REQUIRE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string_view::npos);

	// --- Send masked text frame "hello" ---
	// FIN=1, opcode=1; MASK=1, len=5; mask=0x01020304; masked payload.
	std::array<std::uint8_t, 4> const mask = {0x01, 0x02, 0x03, 0x04};
	std::string_view const msg = "hello";
	std::array<std::uint8_t, 11> tx_frame{};
	tx_frame[0] = 0x81; // FIN | text
	tx_frame[1] = 0x80 | 0x05; // MASK | len=5
	tx_frame[2] = mask[0];
	tx_frame[3] = mask[1];
	tx_frame[4] = mask[2];
	tx_frame[5] = mask[3];
	for (std::size_t i = 0; i < msg.size(); ++i) {
		tx_frame[6 + i] = static_cast<std::uint8_t>(msg[i]) ^ mask[i & 3];
	}
	::send(fd, tx_frame.data(), tx_frame.size(), 0);

	// --- Receive unmasked echo frame ---
	std::array<std::uint8_t, 64> rx_buf{};
	auto rn = ::recv(fd, rx_buf.data(), rx_buf.size(), 0);
	REQUIRE(rn >= 7); // 2 header + 5 payload
	REQUIRE(rx_buf[0] == 0x81); // FIN | text
	REQUIRE((rx_buf[1] & 0x80U) == 0U); // NOT masked (server→client)
	REQUIRE((rx_buf[1] & 0x7FU) == 5U); // payload length = 5
	std::string echo{reinterpret_cast<char const *>(rx_buf.data() + 2), 5};
	REQUIRE(echo == "hello");

	// --- Send close frame (masked, status 1000) ---
	std::uint16_t const status = 1000;
	std::array<std::uint8_t, 8> close_frame{};
	close_frame[0] = 0x88; // FIN | close
	close_frame[1] = 0x80 | 0x02; // MASK | len=2
	close_frame[2] = 0xAA;
	close_frame[3] = 0xBB;
	close_frame[4] = 0xCC;
	close_frame[5] = 0xDD; // mask key
	close_frame[6] = static_cast<std::uint8_t>((status >> 8) ^ close_frame[2]);
	close_frame[7] = static_cast<std::uint8_t>((status & 0xFF) ^ close_frame[3]);
	::send(fd, close_frame.data(), close_frame.size(), 0);

	// Server echoes a close frame then closes.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	::close(fd);

	srv.stop();
}
// ---------------------------------------------------------------------------
// compress_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"compress: large body with Accept-Encoding gzip is compressed") {
	ensure_compress_server();
	auto resp = http_get_on(g_compress_port, "/big", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") != std::string::npos);
	REQUIRE(resp.find("Vary: Accept-Encoding") != std::string::npos);
	// Body decompresses back to 512 A's.
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	auto body = resp.substr(hdr_end + 4);
	auto decompressed = gzip_decompress(body);
	REQUIRE(decompressed == std::string(512, 'A'));
}
TEST_CASE(
	"compress: large body without Accept-Encoding is not compressed") {
	ensure_compress_server();
	auto resp = http_get_on(g_compress_port, "/big");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") == std::string::npos);
}
TEST_CASE(
	"compress: body smaller than min_body_size is not compressed") {
	ensure_compress_server();
	auto resp = http_get_on(g_compress_port, "/small", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") == std::string::npos);
}
TEST_CASE(
	"compress: non-compressible MIME type is not compressed") {
	ensure_compress_server();
	auto resp = http_get_on(g_compress_port, "/bin", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") == std::string::npos);
}
// ---------------------------------------------------------------------------
// conflux::http::security_headers_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"security: default options inject HSTS header") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("Strict-Transport-Security:") != std::string::npos);
	REQUIRE(resp.find("max-age=") != std::string::npos);
}
TEST_CASE(
	"security: default options inject X-Frame-Options DENY") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("X-Frame-Options: DENY") != std::string::npos);
}
TEST_CASE(
	"security: default options inject X-Content-Type-Options nosniff") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("X-Content-Type-Options: nosniff") != std::string::npos);
}
TEST_CASE(
	"security: default options inject Referrer-Policy") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("Referrer-Policy:") != std::string::npos);
}
TEST_CASE(
	"security: default X-XSS-Protection is 0 (OWASP-recommended)") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("X-XSS-Protection: 0") != std::string::npos);
}
TEST_CASE(
	"security: default options inject Permissions-Policy") {
	ensure_security_server();
	auto resp = http_get_on(g_security_port, "/");
	REQUIRE(resp.find("Permissions-Policy:") != std::string::npos);
}
TEST_CASE(
	"security: custom CSP is injected") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	conflux::http::SecurityOptions sopts{};
	sopts.csp = "default-src 'self'";

	conflux::http::Router router;
	router.use(conflux::http::security_headers_middleware(sopts));
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });

	ScopedTestServer srv{cfg, std::move(router)};
	auto resp = http_get_on(srv.port(), "/");
	REQUIRE(resp.find("Content-Security-Policy: default-src 'self'") != std::string::npos);
}
TEST_CASE(
	"security: hsts with no subdomains omits includeSubDomains") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	conflux::http::SecurityOptions sopts{};
	sopts.hsts_include_subdomains = false;
	sopts.hsts_only_on_tls = false;

	conflux::http::Router router;
	router.use(conflux::http::security_headers_middleware(sopts));
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });

	ScopedTestServer srv{cfg, std::move(router)};
	auto resp = http_get_on(srv.port(), "/");
	REQUIRE(resp.find("Strict-Transport-Security:") != std::string::npos);
	REQUIRE(resp.find("includeSubDomains") == std::string::npos);
}
TEST_CASE(
	"security: hsts_max_age=0 disables HSTS header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::security_headers_middleware({.hsts_max_age = 0}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.find("Strict-Transport-Security") == std::string::npos);
}
TEST_CASE(
	"security: empty frame_options disables X-Frame-Options header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::security_headers_middleware({.frame_options = ""}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.find("X-Frame-Options") == std::string::npos);
}
// ---------------------------------------------------------------------------
// conflux::http::cors_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"cors: preflight with matching origin returns 204 and ACAO") {
	ensure_cors_server();
	auto resp = http_options_on(
		g_cors_port,
		"/api",
		"Origin: https://test.example\r\n"
		"Access-Control-Request-Method: GET\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 204"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://test.example") != std::string::npos);
	REQUIRE(resp.find("Access-Control-Allow-Methods:") != std::string::npos);
}
TEST_CASE(
	"cors: preflight with non-matching origin has no ACAO") {
	ensure_cors_server();
	auto resp = http_options_on(
		g_cors_port,
		"/api",
		"Origin: https://evil.com\r\n"
		"Access-Control-Request-Method: GET\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 204"));
	REQUIRE(resp.find("Access-Control-Allow-Origin:") == std::string::npos);
}
TEST_CASE(
	"cors: GET with matching origin receives ACAO header") {
	ensure_cors_server();
	auto resp = http_get_on(g_cors_port, "/api", "Origin: https://test.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://test.example") != std::string::npos);
}
TEST_CASE(
	"cors: GET with matching origin appends Origin to existing Vary") {
	ensure_cors_server();
	auto resp = http_get_on(g_cors_port, "/vary", "Origin: https://test.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Vary: Accept-Encoding, Origin") != std::string::npos);
}
TEST_CASE(
	"cors: GET without Origin header has no ACAO header") {
	ensure_cors_server();
	auto resp = http_get_on(g_cors_port, "/api");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin:") == std::string::npos);
}
TEST_CASE(
	"cors: allow_credentials reflects origin instead of wildcard") {
	ensure_cors_cred_server();
	auto resp = http_get_on(g_cors_cred_port, "/api", "Origin: https://foo.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://foo.example") != std::string::npos);
	REQUIRE(resp.find("Access-Control-Allow-Credentials: true") != std::string::npos);
}
TEST_CASE(
	"cors: expose_headers present in non-preflight response") {
	ensure_cors_cred_server();
	auto resp = http_get_on(g_cors_cred_port, "/api", "Origin: https://foo.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Expose-Headers:") != std::string::npos);
	REQUIRE(resp.find("X-Custom-Header") != std::string::npos);
	REQUIRE(resp.find("X-Request-Id") != std::string::npos);
}
TEST_CASE(
	"cors: preflight with allow_credentials reflects origin and sets credentials header") {
	ensure_cors_cred_server();
	auto resp = http_options_on(
		g_cors_cred_port,
		"/api",
		"Origin: https://foo.example\r\n"
		"Access-Control-Request-Method: POST\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 204"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://foo.example") != std::string::npos);
	REQUIRE(resp.find("Access-Control-Allow-Credentials: true") != std::string::npos);
}
TEST_CASE(
	"cors: wildcard origin without credentials returns ACAO: *") {
	ensure_cors_wildcard_server();
	auto resp = http_get_on(g_cors_wildcard_port, "/api", "Origin: https://any.example\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Access-Control-Allow-Origin: *") != std::string::npos);
	REQUIRE(resp.find("Access-Control-Allow-Credentials:") == std::string::npos);
}
TEST_CASE(
	"cors: OPTIONS without Access-Control-Request-Method is not a preflight") {
	ensure_cors_server();
	auto resp = http_options_on(g_cors_port, "/api", "Origin: https://test.example\r\n");
	// Not a preflight — passes to next handler, which may 405 or 200 depending on router.
	// Either way, CORS headers are still injected for the origin.
	REQUIRE(resp.find("Access-Control-Allow-Origin: https://test.example") != std::string::npos);
	REQUIRE(resp.find("Access-Control-Allow-Methods:") == std::string::npos);
}
// ---------------------------------------------------------------------------
// basic_auth_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"basic_auth: missing Authorization returns 401 with WWW-Authenticate") {
	ensure_auth_server();
	auto resp = http_get_on(g_auth_port, "/protected");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find("WWW-Authenticate: Basic") != std::string::npos);
}
TEST_CASE(
	"basic_auth: wrong credentials return 401") {
	ensure_auth_server();
	// "baduser:badpass" in base64
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: Basic YmFkdXNlcjpiYWRwYXNz\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"basic_auth: correct credentials return 200") {
	ensure_auth_server();
	// "testuser:testpass" in base64 = dGVzdHVzZXI6dGVzdHBhc3M=
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: Basic dGVzdHVzZXI6dGVzdHBhc3M=\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "secret");
}
TEST_CASE(
	"basic_auth: base64 credential without colon returns 401") {
	ensure_auth_server();
	// base64("nocolon") — no colon separator
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: Basic bm9jb2xvbg==\r\n"); // "nocolon"
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"basic_auth: custom realm appears in WWW-Authenticate header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::basic_auth_middleware([](std::string_view, std::string_view) { return false; }, "My Realm"));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find(R"(WWW-Authenticate: Basic realm="My Realm")") != std::string::npos);
}

TEST_CASE(
	"basic_auth: zero max_failed_clients clamps instead of corrupting limiter state",
	"[auth][security]") {
	conflux::http::Router router;
	unsigned calls = 0;
	router.use(
		conflux::http::basic_auth_middleware(
			[&calls](std::string_view, std::string_view) {
				++calls;
				return false;
			},
			conflux::http::BasicAuthOptions{
				.realm = "Clamp",
				.failed_attempts = 1,
				.failed_window = std::chrono::seconds{60},
				.max_failed_clients = 0,
			}));
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/";
	req.remote_addr = "127.0.0.1";
	req.headers.emplace_back("Authorization", "Basic YmFkOmNyZWRz");

	auto first = router.dispatch(req);
	REQUIRE(first.status == 401);
	CHECK(calls == 1);

	auto second = router.dispatch(req);
	REQUIRE(second.status == 429);
	CHECK(calls == 1);
}

TEST_CASE(
	"basic_auth: failed-attempt limit returns 429 before validator",
	"[auth][security]") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	static std::atomic<unsigned> calls{0};
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::basic_auth_middleware(
				[](std::string_view, std::string_view) {
					++calls;
					return false;
				},
				conflux::http::BasicAuthOptions{
					.realm = "Limited",
					.failed_attempts = 1,
					.failed_window = std::chrono::seconds{60},
					.max_failed_clients = 8,
				}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });
		port = start_mw_server(mw_config(), std::move(router));
	});

	auto first = http_get_on(port, "/", "Authorization: Basic YmFkOmNyZWRz\r\n");
	REQUIRE(first.starts_with("HTTP/1.1 401"));
	auto const before = calls.load();
	auto second = http_get_on(port, "/", "Authorization: Basic YmFkOmNyZWRz\r\n");
	REQUIRE(second.starts_with("HTTP/1.1 429"));
	CHECK(calls.load() == before);
}

TEST_CASE(
	"basic_auth: lowercase scheme returns 200") {
	ensure_auth_server();
	auto resp = http_get_on(g_auth_port, "/protected", "Authorization: basic dGVzdHVzZXI6dGVzdHBhc3M=\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
// ---------------------------------------------------------------------------
// bearer_auth_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"bearer_auth: missing Authorization returns 401 with WWW-Authenticate") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find("WWW-Authenticate: Bearer") != std::string::npos);
}
TEST_CASE(
	"bearer_auth: invalid token returns 401") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected", "Authorization: Bearer wrong-token\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"bearer_auth: valid token returns 200") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected", "Authorization: Bearer valid-token-123\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "secret");
}
TEST_CASE(
	"bearer_auth: token with surrounding whitespace is trimmed and accepted") {
	ensure_bearer_server();
	// Extra space after "Bearer " — trim() should strip it before validation.
	auto resp = http_get_on(g_bearer_port, "/protected", "Authorization: Bearer  valid-token-123 \r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"bearer_auth: lowercase scheme returns 200") {
	ensure_bearer_server();
	auto resp = http_get_on(g_bearer_port, "/protected", "Authorization: bearer valid-token-123\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
// ---------------------------------------------------------------------------
// conflux::http::rate_limit_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"rate_limit: first two requests succeed, third returns 429") {
	ensure_rate_server();
	// 3 requests in sequence; bucket size = 2.
	auto r1 = http_get_on(g_rate_port, "/");
	auto r2 = http_get_on(g_rate_port, "/");
	auto r3 = http_get_on(g_rate_port, "/");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r3.starts_with("HTTP/1.1 429"));
	REQUIRE(r3.find("Retry-After:") != std::string::npos);
}
TEST_CASE(
	"rate_limit: burst allows extra requests beyond base rate") {
	ensure_rate_burst_server();
	// requests=1 + burst=2 → capacity=3. First 3 succeed; 4th returns 429.
	auto r1 = http_get_on(g_rate_burst_port, "/");
	auto r2 = http_get_on(g_rate_burst_port, "/");
	auto r3 = http_get_on(g_rate_burst_port, "/");
	auto r4 = http_get_on(g_rate_burst_port, "/");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r3.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r4.starts_with("HTTP/1.1 429"));
}
TEST_CASE(
	"rate_limit: 429 response includes Retry-After header") {
	// Use the existing 1-request rate server: exhaust it then check the header.
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::rate_limit_middleware({.requests = 1, .window = std::chrono::seconds{10}}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	http_get_on(port, "/"); // consume the one allowed request
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 429"));
	auto retry = extract_header(resp, "Retry-After");
	REQUIRE(!retry.empty());
	int retry_val = std::stoi(std::string{retry});
	REQUIRE(retry_val > 0);
	REQUIRE(retry_val <= 10);
}
TEST_CASE(
	"rate_limit: max_clients zero is clamped to one client") {
	ensure_rate_zero_clients_server();
	auto r1 = http_get_on(g_rate_zero_clients_port, "/");
	auto r2 = http_get_on(g_rate_zero_clients_port, "/");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 429"));
}
// ---------------------------------------------------------------------------
// TLS (HTTPS) shared infrastructure
// ---------------------------------------------------------------------------

#if CONFLUX_HAS_TLS
std::uint16_t g_tls_port = 0;
// Generate a self-signed cert+key P once, start a TLS server, delete the
// temp files (already loaded into SSL_CTX by the time port() returns).
void ensure_tls_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		char cert_tmp[] = "/tmp/conflux_tls_cert_XXXXXX.pem";
		char key_tmp[] = "/tmp/conflux_tls_key_XXXXXX.pem";
		{
			int fd = ::mkstemps(cert_tmp, 4);
			if (fd < 0) {
				throw std::runtime_error{"mkstemps cert"};
			}
			::close(fd);
			fd = ::mkstemps(key_tmp, 4);
			if (fd < 0) {
				throw std::runtime_error{"mkstemps key"};
			}
			::close(fd);
		}
		std::string const cmd = std::format(
			"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
			"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
			key_tmp,
			cert_tmp);
		if (::system(cmd.c_str()) != 0) {
			throw std::runtime_error{"openssl req failed"};
		}
		Config cfg = mw_config();
		cfg.cert_file = cert_tmp;
		cfg.key_file = key_tmp;

		conflux::http::Router router;
		router.get("/ping", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"tls":true})");
		});
		router.get("/hello/{name}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::format("hello {}", req.params["name"]));
		});
		router.post("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(req.body);
		});
		router.put("/put/{id}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(std::format(R"({{"id":"{}"}})", req.params["id"]));
		});
		router.get("/notfound-test", [](conflux::http::OwnedRequest const &) -> conflux::http::Response {
			// deliberately absent — router returns 404
			return conflux::http::Response::not_found("notfound-test");
		});

		g_tls_port = start_mw_server(cfg, std::move(router));
		// Cert+key are loaded; temp files no longer needed.
		::unlink(cert_tmp);
		::unlink(key_tmp);
	});
}
// Open one TLS connection to g_tls_port, send an arbitrary raw request,
// read back a complete HTTP response (via Content-Length), close.
std::string tls_raw(
	std::uint16_t port,
	std::string_view raw_request) {
	conflux::net_tls::UniqueSslCtx const ctx{SSL_CTX_new(TLS_client_method())};
	SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

	conflux::net_tls::UniqueSsl const ssl{SSL_new(ctx.get())};
	SSL_set_fd(ssl.get(), fd);
	SSL_connect(ssl.get());

	SSL_write(ssl.get(), raw_request.data(), static_cast<int>(raw_request.size()));

	std::string response;
	std::array<char, 4096> buf{};
	for (;;) {
		int const n = SSL_read(ssl.get(), buf.data(), static_cast<int>(buf.size()));
		if (n <= 0) {
			break;
		}
		response.append(buf.data(), static_cast<std::size_t>(n));
		auto hdr_end = response.find("\r\n\r\n");
		if (hdr_end == std::string::npos) {
			continue;
		}
		auto cl_pos = response.find("Content-Length: ");
		if (cl_pos == std::string::npos || cl_pos > hdr_end) {
			break;
		}
		cl_pos += 16;
		auto cl_end = response.find("\r\n", cl_pos);
		std::size_t body_len = 0;
		std::from_chars(response.data() + cl_pos, response.data() + cl_end, body_len);
		if (response.size() >= hdr_end + 4 + body_len) {
			break;
		}
	}

	SSL_shutdown(ssl.get());
	::close(fd);
	return response;
}
// Convenience wrappers.
std::string tls_get(
	std::string_view path,
	std::string_view extra = "") {
	ensure_tls_server();
	return tls_raw(
		g_tls_port,
		std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}\r\n", path, extra));
}
std::string tls_post(
	std::string_view path,
	std::string_view body,
	std::string_view ct = "text/plain") {
	ensure_tls_server();
	return tls_raw(
		g_tls_port,
		std::format(
			"POST {} HTTP/1.1\r\nHost: localhost\r\nContent-Type: {}\r\n"
			"Content-Length: {}\r\nConnection: close\r\n\r\n{}",
			path,
			ct,
			body.size(),
			body));
}
// ---------------------------------------------------------------------------
// TLS (HTTPS) tests
// ---------------------------------------------------------------------------

TEST_CASE(
	"TLS: GET returns JSON response") {
	auto resp = tls_get("/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"tls":true})");
}
TEST_CASE(
	"http client: HTTPS GET /ping returns parsed response") {
	ensure_tls_server();
	HttpClientOptions tls_opts{};
	tls_opts.verify_peer = false;
	HttpClient tls_client{std::move(tls_opts)};
	auto response = tls_client.blocking_send(
		chttp::ClientRequest::get(std::format("https://127.0.0.1:{}/ping", g_tls_port))
			.server_name("localhost")
			.build());
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]}.find("application/json") != std::string::npos);
	CHECK(response->body == R"({"tls":true})");
}
TEST_CASE(
	"TLS: GET with path parameter") {
	auto resp = tls_get("/hello/world");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "hello world");
}
TEST_CASE(
	"TLS: POST body is echoed back") {
	auto resp = tls_post("/echo", "hello TLS");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "hello TLS");
}
TEST_CASE(
	"TLS: POST with binary-safe body") {
	std::string body(256, '\x00');
	for (int i = 0; i < 256; ++i) {
		body[static_cast<std::size_t>(i)] = static_cast<char>(i);
	}
	auto resp = tls_post("/echo", body, "application/octet-stream");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == body);
}
TEST_CASE(
	"TLS: PUT with path param returns JSON") {
	ensure_tls_server();
	auto resp = tls_raw(
		g_tls_port,
		"PUT /put/42 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"id":"42"})");
}
TEST_CASE(
	"TLS: unknown route returns 404") {
	auto resp = tls_get("/does-not-exist");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}
TEST_CASE(
	"TLS: pipelined requests on one connection both succeed") {
	ensure_tls_server();
	conflux::net_tls::UniqueSslCtx const ctx{SSL_CTX_new(TLS_client_method())};
	SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_tls_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	conflux::net_tls::UniqueSsl const ssl{SSL_new(ctx.get())};
	SSL_set_fd(ssl.get(), fd);
	REQUIRE(SSL_connect(ssl.get()) == 1);

	// Send two requests back-to-back before reading any response.
	std::string_view const r1 = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
	std::string_view const r2 = "GET /hello/pipe HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	SSL_write(ssl.get(), r1.data(), static_cast<int>(r1.size()));
	SSL_write(ssl.get(), r2.data(), static_cast<int>(r2.size()));

	// Read first response via Content-Length.
	auto read_one_tls = [&]() {
		std::string resp;
		std::array<char, 4096> buf{};
		while (true) {
			int const n = SSL_read(ssl.get(), buf.data(), static_cast<int>(buf.size()));
			if (n <= 0) {
				break;
			}
			resp.append(buf.data(), static_cast<std::size_t>(n));
			auto hdr_end = resp.find("\r\n\r\n");
			if (hdr_end == std::string::npos) {
				continue;
			}
			auto cl_pos = resp.find("Content-Length: ");
			if (cl_pos == std::string::npos || cl_pos > hdr_end) {
				break;
			}
			cl_pos += 16;
			auto cl_end = resp.find("\r\n", cl_pos);
			std::size_t body_len = 0;
			std::from_chars(resp.data() + cl_pos, resp.data() + cl_end, body_len);
			if (resp.size() >= hdr_end + 4 + body_len) {
				// Trim to exactly one response.
				resp.resize(hdr_end + 4 + body_len);
				break;
			}
		}
		return resp;
	};

	auto resp1 = read_one_tls();
	auto resp2 = read_one_tls();

	SSL_shutdown(ssl.get());
	::close(fd);

	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	auto h1 = resp1.find("\r\n\r\n");
	REQUIRE(resp1.substr(h1 + 4) == R"({"tls":true})");

	REQUIRE(resp2.starts_with("HTTP/1.1 200 OK"));
	auto h2 = resp2.find("\r\n\r\n");
	REQUIRE(resp2.substr(h2 + 4) == "hello pipe");
}
TEST_CASE(
	"same-port: HTTP and HTTPS on same port both serve correctly") {
	ensure_tls_server();
	// Plaintext GET to a TLS-capable port: first-std::byte sniff routes it as plain HTTP.
	auto plain_resp = http_get_on(g_tls_port, "/ping");
	REQUIRE(plain_resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = plain_resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(plain_resp.substr(hdr_end + 4) == R"({"tls":true})");

	// TLS GET to the same port: first-std::byte 0x16 sniff routes it as HTTPS.
	auto tls_resp = tls_get("/ping");
	REQUIRE(tls_resp.starts_with("HTTP/1.1 200 OK"));
	auto tls_hdr_end = tls_resp.find("\r\n\r\n");
	REQUIRE(tls_hdr_end != std::string::npos);
	REQUIRE(tls_resp.substr(tls_hdr_end + 4) == R"({"tls":true})");
}
// ---------------------------------------------------------------------------
// WebSocket over TLS (wss://)
// ---------------------------------------------------------------------------

TEST_CASE(
	"TLS: WebSocket upgrade over TLS (wss://) works end-to-end") {
	// wss:// is fully supported: server upgrades to WebSocket over TLS and echoes frames.
	// Verify: client receives 101, sends a text frame, gets it echoed back, then closes.
	char cert_tmp[] = "/tmp/conflux_wss_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_wss_key_XXXXXX.pem";
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

	Config cfg = mw_config();
	cfg.cert_file = cert_tmp;
	cfg.key_file = key_tmp;

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

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const wss_port = srv.port();
	::unlink(cert_tmp);
	::unlink(key_tmp);

	conflux::net_tls::UniqueSslCtx const ctx{SSL_CTX_new(TLS_client_method())};
	SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(wss_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	conflux::net_tls::UniqueSsl const ssl{SSL_new(ctx.get())};
	SSL_set_fd(ssl.get(), fd);
	REQUIRE(SSL_connect(ssl.get()) == 1);

	// Send a valid WebSocket upgrade request.
	std::string_view const upgrade =
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";
	REQUIRE(SSL_write(ssl.get(), upgrade.data(), static_cast<int>(upgrade.size())) > 0);

	// Read until we have the 101 response (no Content-Length; ends at \r\n\r\n).
	std::string resp;
	std::array<char, 4096> buf{};
	for (;;) {
		int const n = SSL_read(ssl.get(), buf.data(), static_cast<int>(buf.size()));
		if (n <= 0) {
			break;
		}
		resp.append(buf.data(), static_cast<std::size_t>(n));
		if (resp.find("\r\n\r\n") != std::string::npos) {
			break;
		}
	}

	REQUIRE(resp.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
	REQUIRE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);

	// Send a masked text frame carrying "hello".
	// WS client frames must be masked (RFC 6455 §5.3).
	std::string_view const payload = "hello";
	std::array<std::uint8_t, 4> mask_key{0xAB, 0xCD, 0xEF, 0x01};
	std::array<std::uint8_t, 2 + 4 + 5> frame_buf{};
	frame_buf[0] = 0x81U; // FIN + Text opcode
	frame_buf[1] = 0x80U | static_cast<std::uint8_t>(payload.size()); // MASK + len
	frame_buf[2] = mask_key[0];
	frame_buf[3] = mask_key[1];
	frame_buf[4] = mask_key[2];
	frame_buf[5] = mask_key[3];
	for (std::size_t i = 0; i < payload.size(); ++i) {
		frame_buf[6 + i] = static_cast<std::uint8_t>(payload[i]) ^ mask_key[i & 3];
	}
	REQUIRE(SSL_write(ssl.get(), frame_buf.data(), static_cast<int>(frame_buf.size())) > 0);

	// Read the server's echo frame (unmasked text, FIN=1, opcode=1).
	std::array<char, 32> echo_buf{};
	int const n = SSL_read(ssl.get(), echo_buf.data(), static_cast<int>(echo_buf.size()));
	REQUIRE(n >= 7); // 2 hdr + 5 payload
	REQUIRE((static_cast<std::uint8_t>(echo_buf[0]) & 0x8FU) == 0x81U); // FIN + Text
	REQUIRE(static_cast<std::uint8_t>(echo_buf[1]) == 5); // unmasked, len=5
	REQUIRE(std::string_view{echo_buf.data() + 2, 5} == "hello");

	// Send a close frame (code 1000).
	std::array<std::uint8_t, 2 + 4 + 2> close_frame{};
	close_frame[0] = 0x88U; // FIN + Close
	close_frame[1] = 0x82U; // MASK + 2 bytes
	close_frame[2] = 0x11;
	close_frame[3] = 0x22;
	close_frame[4] = 0x33;
	close_frame[5] = 0x44; // mask
	close_frame[6] = static_cast<std::uint8_t>(0x03U ^ 0x11U); // 1000 >> 8 XOR mask[0]
	close_frame[7] = static_cast<std::uint8_t>(0xE8U ^ 0x22U); // 1000 & 0xFF XOR mask[1]
	SSL_write(ssl.get(), close_frame.data(), static_cast<int>(close_frame.size()));

	// Drain until EOF (server echoes Close frame and then shuts down).
	for (int i = 0; i < 10; ++i) {
		char drain[64]{};
		int const dr = SSL_read(ssl.get(), drain, sizeof(drain));
		if (dr <= 0) {
			break;
		}
	}

	::close(fd);

	srv.stop();
}
#endif
// ---------------------------------------------------------------------------
// conflux::http::forwarded_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"forwarded: X-Forwarded-For from trusted proxy rewrites remote_addr") {
	ensure_forwarded_server();
	auto resp = http_get_on(g_fwd_port, "/addr", "X-Forwarded-For: 203.0.113.5\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "203.0.113.5");
}
TEST_CASE(
	"forwarded: X-Real-IP from trusted proxy rewrites remote_addr") {
	ensure_forwarded_server();
	auto resp = http_get_on(g_fwd_port, "/addr", "X-Real-IP: 198.51.100.7\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "198.51.100.7");
}
TEST_CASE(
	"forwarded: X-Forwarded-For chain uses leftmost entry") {
	ensure_forwarded_server();
	auto resp = http_get_on(g_fwd_port, "/addr", "X-Forwarded-For: 10.0.0.1, 172.16.0.1\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "10.0.0.1");
}
TEST_CASE(
	"forwarded: no forwarding header keeps original remote_addr") {
	ensure_forwarded_server();
	auto resp = http_get_on(g_fwd_port, "/addr");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	// peer is 127.0.0.1 (loopback), no XFF header — keep as-is
	REQUIRE(resp.substr(hdr_end + 4) == "127.0.0.1");
}
TEST_CASE(
	"forwarded: strict mode with empty trusted_proxies ignores X-Forwarded-For") {
	ensure_forwarded_strict_empty_server();
	auto resp = http_get_on(g_fwd_strict_empty_port, "/addr", "X-Forwarded-For: 203.0.113.9\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	// Default is strict: empty trust list trusts nobody, remote_addr stays loopback.
	REQUIRE(resp.substr(hdr_end + 4) == "127.0.0.1");
}
TEST_CASE(
	"forwarded: strict mode with empty trusted_proxies ignores X-Real-IP") {
	ensure_forwarded_strict_empty_server();
	auto resp = http_get_on(g_fwd_strict_empty_port, "/addr", "X-Real-IP: 198.51.100.99\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "127.0.0.1");
}
TEST_CASE(
	"forwarded: lax mode with empty trusted_proxies trusts all peers (legacy)") {
	ensure_forwarded_lax_empty_server();
	auto resp = http_get_on(g_fwd_lax_empty_port, "/addr", "X-Forwarded-For: 203.0.113.9\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "203.0.113.9");
}
TEST_CASE(
	"forwarded: use_x_forwarded_for=false falls back to X-Real-IP") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::forwarded_middleware({
				.trusted_proxies = {"127.0.0.1/32"},
				.use_x_forwarded_for = false,
				.use_x_real_ip = true,
			}));
		router.get("/addr", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.remote_addr});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	// X-Forwarded-For is set but should be ignored; X-Real-IP wins.
	auto resp = http_get_on(
		port,
		"/addr",
		"X-Forwarded-For: 1.2.3.4\r\n"
		"X-Real-IP: 5.6.7.8\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "5.6.7.8");
}
TEST_CASE(
	"forwarded: untrusted peer has X-Forwarded-For stripped before downstream") {
	ScopedTestServer srv{mw_config(), [] {
							 conflux::http::Router r;
							 r.use(conflux::http::forwarded_middleware({})); // strict: no trusted proxies
							 // Echo the header as-seen by the downstream handler.
							 r.get("/xff", [](conflux::http::OwnedRequest const &req) {
								 return conflux::http::Response::text(std::string{req.headers["x-forwarded-for"]});
							 });
							 return r;
						 }()};
	// Send a spoofed X-Forwarded-For from an untrusted peer (loopback).
	auto resp = http_get_on(srv.port(), "/xff", "X-Forwarded-For: 203.0.113.99\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	// Middleware must strip the header; downstream sees empty S.
	REQUIRE(extract_body(resp).empty());
}
// ---------------------------------------------------------------------------
// conflux::http::ip_filter_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"ip_filter: allowlist passes loopback request") {
	ensure_ipallow_server();
	auto resp = http_get_on(g_ipallow_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"ip_filter: blocklist blocks loopback request") {
	ensure_ipblock_server();
	auto resp = http_get_on(g_ipblock_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
}
TEST_CASE(
	"ip_filter: allowlist blocks non-matching IP") {
	ensure_ipallow_block_server();
	auto resp = http_get_on(g_ipallow_block_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
}
TEST_CASE(
	"ip_filter: blocklist passes non-matching IP") {
	ensure_ipblock_pass_server();
	auto resp = http_get_on(g_ipblock_pass_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"ip_filter: empty allowlist blocks all requests") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::ip_filter_middleware({.mode = conflux::http::IpFilterMode::allowlist, .cidrs = {}}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
}
// ---------------------------------------------------------------------------
// conflux::http::cache_control_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"cache_control: image/* gets immutable max-age rule") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/image");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Cache-Control: max-age=31536000, immutable") != std::string::npos);
}
TEST_CASE(
	"cache_control: text/css gets its specific rule") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/css");
	REQUIRE(resp.find("Cache-Control: max-age=86400, public") != std::string::npos);
}
TEST_CASE(
	"cache_control: application/json gets no-store") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/api");
	REQUIRE(resp.find("Cache-Control: no-store") != std::string::npos);
}
TEST_CASE(
	"cache_control: unmatched MIME gets default directive") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/html");
	REQUIRE(resp.find("Cache-Control: no-cache") != std::string::npos);
}
TEST_CASE(
	"cache_control: handler-set Cache-Control is not overwritten") {
	ensure_cache_server();
	auto resp = http_get_on(g_cache_port, "/custom");
	REQUIRE(resp.find("Cache-Control: max-age=999") != std::string::npos);
}
TEST_CASE(
	"cache_control: Content-Type with charset still matches mime prefix") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::cache_control_middleware({
				.rules = {{"text/html", "max-age=60"}},
			}));
		router.get("/", [](conflux::http::OwnedRequest const &) {
			conflux::http::Response r;
			r.content_type = "text/html; charset=utf-8";
			r.set_text_body("<p/>");
			return r;
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.find("Cache-Control: max-age=60") != std::string::npos);
}
TEST_CASE(
	"cache_control: empty mime_prefix rule matches everything") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::cache_control_middleware({
				.rules = {{"image/", "max-age=99999"}, {"", "no-store"}},
        }));
		router.get("/any", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/any");
	REQUIRE(resp.find("Cache-Control: no-store") != std::string::npos);
}
// ---------------------------------------------------------------------------
// conflux::http::trailing_slash_middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"trailing_slash: /foo/ redirects to /foo with 301") {
	ensure_ts_remove_server();
	auto resp = http_get_on(g_ts_remove_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	REQUIRE(resp.find("Location: /foo\r\n") != std::string::npos);
}
TEST_CASE(
	"trailing_slash: /foo without slash passes through") {
	ensure_ts_remove_server();
	auto resp = http_get_on(g_ts_remove_port, "/foo");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"trailing_slash: root / is never redirected") {
	ensure_ts_remove_server();
	// Router returns 404 for /, but it must not be a 301.
	auto resp = http_get_on(g_ts_remove_port, "/");
	REQUIRE(!resp.starts_with("HTTP/1.1 301"));
}
TEST_CASE(
	"trailing_slash: add mode redirects /bar to /bar/") {
	ensure_ts_add_server();
	auto resp = http_get_on(g_ts_add_port, "/bar");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	REQUIRE(resp.find("Location: /bar/\r\n") != std::string::npos);
}
TEST_CASE(
	"trailing_slash: add mode passes /bar/ through") {
	ensure_ts_add_server();
	auto resp = http_get_on(g_ts_add_port, "/bar/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"trailing_slash: redirect_status=308 emits 308") {
	ensure_ts_308_server();
	auto resp = http_get_on(g_ts_308_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 308"));
	REQUIRE(resp.find("Location: /foo\r\n") != std::string::npos);
}
TEST_CASE(
	"trailing_slash: redirect_status=307 emits 307 Temporary Redirect") {
	ensure_ts_307_server();
	auto resp = http_get_on(g_ts_307_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 307 Temporary Redirect"));
	REQUIRE(resp.find("Location: /foo\r\n") != std::string::npos);
}
TEST_CASE(
	"trailing_slash: redirect_status=308 emits 308 Permanent Redirect") {
	ensure_ts_308_server();
	auto resp = http_get_on(g_ts_308_port, "/foo/");
	REQUIRE(resp.starts_with("HTTP/1.1 308 Permanent Redirect"));
}
TEST_CASE(
	"trailing_slash: query std::string is preserved in redirect Location") {
	ensure_ts_remove_server();
	// /foo/?x=1&y=2 should redirect to /foo?x=1&y=2
	auto resp = http_get_on(g_ts_remove_port, "/foo/?x=1&y=2");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	auto loc = extract_header(resp, "Location");
	REQUIRE(loc.starts_with("/foo?"));
	REQUIRE(loc.find("x=1") != std::string::npos);
	REQUIRE(loc.find("y=2") != std::string::npos);
}
TEST_CASE(
	"trailing_slash: query std::string with spaces is percent-encoded in Location") {
	ensure_ts_remove_server();
	// /foo/?name=hello world should percent-encode the space
	auto resp = http_get_on(g_ts_remove_port, "/foo/?name=hello%20world");
	REQUIRE(resp.starts_with("HTTP/1.1 301"));
	auto loc = extract_header(resp, "Location");
	REQUIRE(loc.find("name=hello%20world") != std::string::npos);
}
// ---------------------------------------------------------------------------
// JWT
// ---------------------------------------------------------------------------

#if CONFLUX_HAS_TLS
namespace {

// Shared JWT test server (single instance, lazy-init).
std::uint16_t g_jwt_port = 0;
std::string g_jwt_secret = "test-secret-key-32bytes";
void ensure_jwt_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		router.use(
			conflux::http::jwt_middleware(conflux::http::JwtOptions{.secrets = single_secret_rotation(g_jwt_secret)}));
		router.get("/api/protected", [](conflux::http::OwnedRequest const &req) {
			auto sub = req.params["jwt_sub"];
			return conflux::http::Response::json(std::format(R"({{"sub":"{}"}})", sub));
		});
		g_jwt_port = test_servers().start(cfg, std::move(router));
	});
}
std::string make_jwt(
	std::string_view payload_json) {
	return conflux::http::jwt_sign_unchecked(payload_json, g_jwt_secret);
}
std::string make_jwt_with_header(
	std::string_view header_json,
	std::string_view payload_json,
	std::string_view secret) {
	auto header_b64 = conflux::crypto::base64url_encode(
		std::span{reinterpret_cast<unsigned char const *>(header_json.data()), header_json.size()});
	auto payload_b64 = conflux::crypto::base64url_encode(
		std::span{reinterpret_cast<unsigned char const *>(payload_json.data()), payload_json.size()});
	std::string const signing_input = header_b64 + '.' + payload_b64;
	auto sig = conflux::crypto::hmac_sha256(
		std::span{reinterpret_cast<unsigned char const *>(secret.data()), secret.size()},
		std::span{reinterpret_cast<unsigned char const *>(signing_input.data()), signing_input.size()});
	auto sig_b64 = conflux::crypto::base64url_encode(std::span{sig.data(), sig.size()});
	return signing_input + '.' + sig_b64;
}

} // namespace
TEST_CASE(
	"jwt: valid token returns 200 and injects sub claim") {
	ensure_jwt_server();
	auto now =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	auto token = make_jwt(std::format(R"({{"sub":"user42","exp":{}}})", now + 3600));
	auto resp =
		http_get_with_header_on(g_jwt_port, "/api/protected", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == R"({"sub":"user42"})");
}
TEST_CASE(
	"jwt: missing Authorization header returns 401") {
	ensure_jwt_server();
	auto resp = http_get_on(g_jwt_port, "/api/protected");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"jwt: wrong secret returns 401") {
	ensure_jwt_server();
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"bad","exp":9999999999})", "wrong-secret");
	auto resp =
		http_get_with_header_on(g_jwt_port, "/api/protected", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"jwt: expired token returns 401") {
	ensure_jwt_server();
	auto token = make_jwt(R"({"sub":"x","exp":1})"); // exp = 1970
	auto resp =
		http_get_with_header_on(g_jwt_port, "/api/protected", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"jwt: malformed token returns 401") {
	ensure_jwt_server();
	auto resp = http_get_with_header_on(g_jwt_port, "/api/protected", "Authorization: Bearer not.a.jwt\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}
TEST_CASE(
	"jwt: lowercase bearer scheme returns 200") {
	ensure_jwt_server();
	auto token = make_jwt(R"({"sub":"user42","exp":9999999999})");
	auto resp =
		http_get_with_header_on(g_jwt_port, "/api/protected", std::format("Authorization: bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
}
TEST_CASE(
	"jwt_middleware: injected claim params override route params") {
	Config const cfg{.port = 0, .rings = 1};
	conflux::http::Router router;
	router.use(
		conflux::http::jwt_middleware(
			conflux::http::JwtOptions{.secrets = single_secret_rotation("sec", 3), .verify_exp = false}));
	router.get("/api/protected/{jwt_sub}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::json(std::format(R"({{"sub":"{}"}})", req.params["jwt_sub"]));
	});
	auto port = test_servers().start(cfg, std::move(router));
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"victim"})", "sec");
	auto resp =
		http_get_with_header_on(port, "/api/protected/attacker", std::format("Authorization: Bearer {}\r\n", token));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.substr(hdr_end + 4) == R"({"sub":"victim"})");
}
TEST_CASE(
	"jwt_decode: valid token with no exp returns claims") {
	conflux::http::JwtOptions const opts{.secrets = single_secret_rotation("sec", 3), .verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"alice","iss":"test"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->sub == "alice");
	REQUIRE(result->iss == "test");
}
TEST_CASE(
	"jwt_decode: issuer mismatch returns error") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.issuer = "expected",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","iss":"other"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("issuer") != std::string::npos);
}
TEST_CASE(
	"jwt_decode: audience std::string match") {
	conflux::http::JwtOptions opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "myapp",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"u","aud":"myapp"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
}
TEST_CASE(
	"jwt_decode: audience mismatch returns error") {
	conflux::http::JwtOptions opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "myapp",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"u","aud":"other"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("audience") != std::string::npos);
}
TEST_CASE(
	"jwt_decode: audience A match") {
	conflux::http::JwtOptions opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "myapp",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"u","aud":["svc","myapp"]})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
}
TEST_CASE(
	"jwt_decode: accepts JSON whitespace around claim separators") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "myapp",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(
		"{\n\t\"sub\"\t:\t\"u\",\r\n\t\"aud\"\n:\n[\n\t\"svc\",\r\n\t\"myapp\"\n]\n}",
		"sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->sub == "u");
}
TEST_CASE(
	"jwt_decode: audience A matches only aud claim") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "expected",
		.verify_exp = false};
	auto good = conflux::http::jwt_sign_unchecked(R"({"sub":"x","aud":["other","expected"]})", "sec");
	auto bad = conflux::http::jwt_sign_unchecked(R"({"sub":"expected","aud":["other"]})", "sec");
	REQUIRE(conflux::http::jwt_decode(good, opts).has_value());
	auto result = conflux::http::jwt_decode(bad, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "audience mismatch");
}
TEST_CASE(
	"jwt_decode: malformed audience array is rejected as invalid JSON") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.audience = "expected",
		.verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","aud":[,"expected"]})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().starts_with("invalid payload JSON:"));
}
TEST_CASE(
	"jwt_decode: malformed header JSON is rejected") {
	conflux::http::JwtOptions const opts{.secrets = single_secret_rotation("sec", 3), .verify_exp = false};
	auto token = make_jwt_with_header(R"({"alg":"HS256)", R"({"sub":"x"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().starts_with("invalid header JSON:"));
}
TEST_CASE(
	"jwt_decode: malformed numeric claims are rejected") {
	conflux::http::JwtOptions const opts{.secrets = single_secret_rotation("sec", 3)};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","exp":"soon"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "invalid exp claim");
}
TEST_CASE(
	"jwt_decode: malformed nbf claim is rejected") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.verify_exp = false,
		.verify_nbf = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","nbf":"later"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "invalid nbf claim");
}
TEST_CASE(
	"jwt_decode: malformed iat claim is rejected") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.verify_exp = false,
		.verify_nbf = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","iat":"earlier"})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "invalid iat claim");
}
TEST_CASE(
	"jwt_decode: exp equal to now is expired") {
	auto now =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	conflux::http::JwtOptions const opts{.secrets = single_secret_rotation("sec", 3)};
	auto token = conflux::http::jwt_sign_unchecked(std::format(R"({{"sub":"x","exp":{}}})", now), "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error() == "token expired");
}
TEST_CASE(
	"jwt_decode: nbf in future returns not-yet-valid error") {
	auto far_future =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
		+ 9999;
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.verify_exp = false,
		.verify_nbf = true};
	auto token = conflux::http::jwt_sign_unchecked(std::format(R"({{"sub":"x","nbf":{}}})", far_future), "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("not yet valid") != std::string::npos);
}
TEST_CASE(
	"jwt_decode: nbf in past is accepted") {
	conflux::http::JwtOptions const opts{
		.secrets = single_secret_rotation("sec", 3),
		.verify_exp = false,
		.verify_nbf = true};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","nbf":1})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
}
TEST_CASE(
	"jwt_decode: verify_exp=false allows expired token") {
	conflux::http::JwtOptions const opts{.secrets = single_secret_rotation("sec", 3), .verify_exp = false};
	auto token = conflux::http::jwt_sign_unchecked(R"({"sub":"x","exp":1})", "sec");
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(result.has_value());
	REQUIRE(result->sub == "x");
}
TEST_CASE(
	"jwt_decode: non-HS256 algorithm returns unsupported error") {
	// Pre-computed base64url (no padding):
	// {"alg":"RS256","typ":"JWT"} → eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9
	// {"sub":"x"}                 → eyJzdWIiOiJ4In0
	std::string_view token = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJ4In0.ZmFrZXNpZw";
	conflux::http::JwtOptions opts{.secrets = single_secret_rotation("sec", 3)};
	auto result = conflux::http::jwt_decode(token, opts);
	REQUIRE(!result.has_value());
	REQUIRE(result.error().find("HS256") != std::string::npos);
}
#endif
