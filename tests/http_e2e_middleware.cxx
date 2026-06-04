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
