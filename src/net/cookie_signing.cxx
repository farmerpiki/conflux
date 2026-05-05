// Cookie signing: HMAC-SHA256 in pure C++ (no OpenSSL dep).
// Signed format: "value.BASE64URL(HMAC-SHA256(secret, value))".
module;
#include <cstdint>

export module conflux.net.cookie_signing;
import std;
import conflux.types;
import conflux.crypto;
import conflux.net.router;
import conflux.utils;

namespace {

S mac_b64(
	SV value,
	SV secret) {
	auto key = to_unsigned_span(secret);
	auto msg = to_unsigned_span(value);
	return base64url_encode(hmac_sha256(key, msg));
}

} // namespace

// Sign a cookie value. Returns "value.BASE64URL(HMAC-SHA256(secret, value))".
export S sign_cookie(
	SV value,
	SV secret) {
	return S{value} + '.' + mac_b64(value, secret);
}

// Verify a signed cookie. Returns the original value on success, std::nullopt on failure.
export Opt<S> verify_cookie(
	SV signed_value,
	SV secret) {
	auto dot = signed_value.rfind('.');
	if (dot == SV::npos) {
		return std::nullopt;
	}
	auto value = signed_value.substr(0, dot);
	auto sig = signed_value.substr(dot + 1);
	if (!constant_time_eq(sig, mac_b64(value, secret))) {
		return std::nullopt;
	}
	return S{value};
}

export struct CookieSigningOptions {
	S secret;
	// When true, cookies arriving with invalid signatures are stripped (set to empty).
	bool strip_invalid{true};
};

// Middleware: for every incoming cookie, attempt to verify its signature.
// Cookies in "value.SIG" format are verified; on success the plain value is injected back.
// Cookies without a "." are passed through unchanged (not all cookies are signed).
// On failure: if strip_invalid=true the cookie is cleared; otherwise it is passed as-is.
export Router::Middleware cookie_signing_middleware(
	CookieSigningOptions opts = {}) {
	if (opts.secret.size() < 16) {
		throw std::invalid_argument{"cookie_signing_middleware: secret must be at least 16 bytes"};
	}
	return [opts = move(opts)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto modified = req.to_owned();
		for (auto &[name, value]: modified.cookies) {
			if (value.find('.') == S::npos) {
				continue;
			} // unsigned cookie
			auto plain = verify_cookie(value, opts.secret);
			if (plain) {
				value = move(*plain);
			} else if (opts.strip_invalid) {
				value.clear();
			}
		}
		return next(modified);
	};
}
