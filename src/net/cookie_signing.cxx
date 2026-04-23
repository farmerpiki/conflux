// Cookie signing: HMAC-SHA256 in pure C++ (no OpenSSL dep).
// Signed format: "value.BASE64URL(HMAC-SHA256(secret, value))".
module;
#include <cstdint>

export module conflux.net.cookie_signing;
import std;
import conflux.crypto;
import conflux.net.router;
import conflux.utils;
using namespace std;

namespace {

// Constant-time compare to prevent timing attacks.
bool constant_time_eq(
	string_view a,
	string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	unsigned char acc = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		acc = static_cast<unsigned char>(acc | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
	}
	return acc == 0;
}

} // namespace

namespace {

string mac_b64(
	string_view value,
	string_view secret) {
	auto key = span{reinterpret_cast<unsigned char const *>(secret.data()), secret.size()};
	auto msg = span{reinterpret_cast<unsigned char const *>(value.data()), value.size()};
	return base64url_encode(hmac_sha256(key, msg));
}

} // namespace

// Sign a cookie value. Returns "value.BASE64URL(HMAC-SHA256(secret, value))".
export string sign_cookie(
	string_view value,
	string_view secret) {
	return string{value} + '.' + mac_b64(value, secret);
}

// Verify a signed cookie. Returns the original value on success, nullopt on failure.
export optional<string> verify_cookie(
	string_view signed_value,
	string_view secret) {
	auto dot = signed_value.rfind('.');
	if (dot == string_view::npos) {
		return nullopt;
	}
	auto value = signed_value.substr(0, dot);
	auto sig = signed_value.substr(dot + 1);
	if (!constant_time_eq(sig, mac_b64(value, secret))) {
		return nullopt;
	}
	return string{value};
}

export struct CookieSigningOptions {
	string secret;
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
		throw invalid_argument{"cookie_signing_middleware: secret must be at least 16 bytes"};
	}
	return [opts = move(opts)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto modified = req.to_owned();
		for (auto &[name, value]: modified.cookies) {
			if (value.find('.') == string::npos) {
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
