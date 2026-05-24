// libFuzzer driver for HTTP policy/security middleware.
// Invariants:
//   - CORS/CSRF/forwarded/cache/ETag/security middlewares never crash on
//     bounded header-like inputs.
//   - trusted-proxy handling strips spoofed forwarding headers from untrusted
//     peers and canonicalizes trusted IP headers where possible.
//   - CSRF double-submit accepts matching token pairs and rejects mismatches.
//   - cache-control does not overwrite explicit handler values.

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.cache_control;
import conflux.net.cors;
import conflux.net.csrf;
import conflux.net.etag;
import conflux.net.forwarded;
import conflux.net.security;

using namespace std;

namespace {

struct Cursor {
	std::uint8_t const *data{};
	std::size_t size{};
	std::size_t pos{};

	[[nodiscard]] std::uint8_t byte() noexcept {
		if (pos >= size) {
			return 0;
		}
		return data[pos++];
	}

	[[nodiscard]] bool bit() noexcept { return (byte() & 1U) != 0; }
};

[[nodiscard]] char safe_char(
	std::uint8_t b) noexcept {
	static constexpr string_view alphabet =
		"abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"0123456789"
		"-_.:/,;= *\"W";
	return alphabet[b % alphabet.size()];
}

[[nodiscard]] string safe_string(
	Cursor &cur,
	std::size_t max_len) {
	auto const n = static_cast<std::size_t>(cur.byte()) % (max_len + 1U);
	string out;
	out.reserve(n);
	for (std::size_t i = 0; i < n; ++i) {
		out.push_back(safe_char(cur.byte()));
	}
	return out;
}

[[nodiscard]] string ipish(
	Cursor &cur) {
	switch (cur.byte() % 8U) {
	case 0 : return "127.0.0.1";
	case 1 : return "203.0.113.9";
	case 2 : return "198.51.100.7";
	case 3 : return "10.0.0.5";
	case 4 : return "::1";
	case 5 : return "not-an-ip";
	case 6 : return std::format("{}.{}.{}.{}", cur.byte(), cur.byte(), cur.byte(), cur.byte());
	default: return safe_string(cur, 32);
	}
}

[[nodiscard]] Request make_request(
	Cursor &cur,
	string method,
	string remote = "127.0.0.1") {
	Request req;
	req.method = std::move(method);
	req.path = "/fuzz";
	req.version = "HTTP/1.1";
	req.remote_addr = std::move(remote);
	req.is_tls = cur.bit();
	req.headers = HttpFields(true);
	req.params = HttpFields{};
	req.query = HttpFields{};
	req.form = HttpFields{};
	req.cookies = HttpFields{};
	req.body = safe_string(cur, 128);
	return req;
}

[[nodiscard]] Response base_response(
	Cursor &cur) {
	Response resp = Response::text(safe_string(cur, 128));
	resp.content_type = cur.bit() ? string{"text/plain; charset=utf-8"} : string{"application/json; charset=utf-8"};
	if (cur.bit()) {
		resp.headers["Cache-Control"] = "private, max-age=7";
	}
	if (cur.bit()) {
		resp.headers["ETag"] = "\"handler-set\"";
	}
	return resp;
}

void check_forwarded(
	Cursor &cur) {
	auto remote = cur.bit() ? string{"127.0.0.1"} : ipish(cur);
	auto req = make_request(cur, "GET", std::move(remote));
	auto xff = cur.bit() ? string{"203.0.113.9, 198.51.100.7"} : ipish(cur);
	auto xri = ipish(cur);
	req.headers["X-Forwarded-For"] = xff;
	req.headers["X-Real-IP"] = xri;

	bool const trust_loopback = cur.bit();
	auto mw = forwarded_middleware({
		.trusted_proxies = trust_loopback ? vector<string>{"127.0.0.1/32", "::1/128"}
            : vector<string>{},
		.use_x_forwarded_for = cur.bit(),
		.use_x_real_ip = true,
		.strict_mode = true,
	});
	NextHandler next = [](RequestView const &seen) {
		Response resp = Response::text(string{seen.remote_addr});
		resp.headers["X-Seen-XFF"] = string{seen.headers["x-forwarded-for"]};
		resp.headers["X-Seen-XRI"] = string{seen.headers["x-real-ip"]};
		return resp;
	};
	auto resp = mw(RequestView{req}, next);

	if (!trust_loopback || req.remote_addr != "127.0.0.1") {
		if (!resp.headers["X-Seen-XFF"].empty() || !resp.headers["X-Seen-XRI"].empty()) {
			__builtin_trap();
		}
	}
}

void check_cors(
	Cursor &cur) {
	auto req = make_request(cur, cur.bit() ? "OPTIONS" : "GET");
	auto origin = cur.bit() ? string{"https://example.test"} : safe_string(cur, 96);
	req.headers["Origin"] = origin;
	if (req.method == "OPTIONS" || cur.bit()) {
		req.headers["Access-Control-Request-Method"] = cur.bit() ? "POST" : safe_string(cur, 24);
	}

	CorsOptions opts;
	opts.allowed_origins = cur.bit() ? vector<string>{"*"} : vector<string>{"https://example.test"};
	opts.allow_credentials = cur.bit();
	opts.allowed_methods = {"GET", "POST", "OPTIONS"};
	opts.allowed_headers = {"Content-Type", "Authorization", "X-CSRF-Token"};
	opts.expose_headers = cur.bit() ? vector<string>{"ETag"} : vector<string>{};
	opts.max_age = static_cast<unsigned>(cur.byte()) * 60U;
	auto mw = cors_middleware(std::move(opts));
	NextHandler next = [&cur](RequestView const &) { return base_response(cur); };
	auto resp = mw(RequestView{req}, next);

	if (req.method == "OPTIONS" && !req.headers["access-control-request-method"].empty()) {
		if (resp.status != kHttpNoContent) {
			__builtin_trap();
		}
	}
	if (!resp.headers["Access-Control-Allow-Origin"].empty()
		&& resp.headers["Vary"].find("Origin") == string_view::npos) {
		__builtin_trap();
	}
}

void check_csrf(
	Cursor &cur) {
	auto req = make_request(cur, cur.bit() ? "POST" : "DELETE");
	auto cookie_token = cur.bit() ? string{"fixed-token"} : safe_string(cur, 48);
	auto submitted = cur.bit() ? cookie_token : safe_string(cur, 48);
	if (cur.bit()) {
		req.cookies["csrf_token"] = cookie_token;
	}
	if (cur.bit()) {
		req.headers["X-CSRF-Token"] = submitted;
	} else {
		req.form["csrf_token"] = submitted;
	}
	auto expect_ok = !req.cookies["csrf_token"].empty() && submitted == string{req.cookies["csrf_token"]};
	auto mw = csrf_middleware({
		.cookie_attrs = "Path=/; SameSite=Strict",
	});
	NextHandler next = [](RequestView const &) { return Response::text("ok"); };
	auto resp = mw(RequestView{req}, next);
	if (expect_ok) {
		if (resp.status == 403 || resp.headers["X-CSRF-Token"] != cookie_token) {
			__builtin_trap();
		}
	} else if (resp.status != 403) {
		__builtin_trap();
	}
}

void check_cache_and_etag(
	Cursor &cur) {
	auto req = make_request(cur, "GET");
	if (cur.bit()) {
		req.headers["If-None-Match"] = cur.bit() ? "*" : safe_string(cur, 64);
	}
	CacheControlOptions opts{
		.rules =
			{
					CacheRule{.mime_prefix = "text/", .directive = "max-age=60, public"},
					CacheRule{.mime_prefix = "application/json", .directive = "no-store"},
					},
		.default_directive = "max-age=5",
	};
	auto cache = cache_control_middleware(std::move(opts));
	auto etag = etag_middleware({.weak = cur.bit()});
	NextHandler handler = [&cur](RequestView const &) { return base_response(cur); };
	auto cached = cache(RequestView{req}, handler);
	if (cached.headers["Cache-Control"].empty() && cached.is_text()) {
		__builtin_trap();
	}
	NextHandler cached_handler = [cached](RequestView const &) { return cached; };
	auto tagged = etag(RequestView{req}, cached_handler);
	if (cached.headers["ETag"] == "\"handler-set\"" && tagged.headers["ETag"] != "\"handler-set\"") {
		__builtin_trap();
	}
	if (req.headers["If-None-Match"] == "*"
		&& cached.headers["ETag"].empty()
		&& cached.is_text()
		&& !cached.text_body().empty()) {
		if (tagged.status != kHttpNotModified) {
			__builtin_trap();
		}
	}
}

void check_security_headers(
	Cursor &cur) {
	auto req = make_request(cur, "GET");
	SecurityOptions opts;
	opts.hsts_max_age = cur.bit() ? 0U : static_cast<unsigned>(1U + cur.byte());
	opts.hsts_include_subdomains = cur.bit();
	opts.hsts_only_on_tls = cur.bit();
	opts.frame_options = cur.bit() ? string{} : string{"DENY"};
	opts.nosniff = cur.bit();
	opts.xss_protection = cur.bit() ? string{} : string{"0"};
	opts.referrer_policy = cur.bit() ? string{} : string{"strict-origin-when-cross-origin"};
	opts.permissions_policy = cur.bit() ? string{} : string{"geolocation=()"};
	opts.csp = cur.bit() ? string{} : string{"default-src 'none'"};
	auto mw = security_headers_middleware(std::move(opts));
	NextHandler next = [&cur](RequestView const &) { return base_response(cur); };
	auto resp = mw(RequestView{req}, next);
	if (!req.is_tls && opts.hsts_only_on_tls && !resp.headers["Strict-Transport-Security"].empty()) {
		__builtin_trap();
	}
	if (opts.nosniff && resp.headers["X-Content-Type-Options"] != "nosniff") {
		__builtin_trap();
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	if (size > 64U * 1024U) {
		return 0;
	}
	Cursor cur{.data = data, .size = size};
	check_forwarded(cur);
	check_cors(cur);
	check_csrf(cur);
	check_cache_and_etag(cur);
	check_security_headers(cur);
	return 0;
}
