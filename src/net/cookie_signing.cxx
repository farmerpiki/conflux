// Cookie signing: HMAC-SHA256 in pure C++ (no OpenSSL dep).
// Signed std::format: "value.BASE64URL(HMAC-SHA256(secret, value))".
module;
#include <cstdint>

export module conflux.net.cookie_signing;
import std;
import conflux.types;
import conflux.crypto;
import conflux.net.config;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;
import conflux.utils;
export namespace conflux::http {

namespace cookie_signing_detail {

std::string mac_b64(
	std::string_view value,
	std::string_view secret) {
	auto key = conflux::crypto::to_unsigned_span(secret);
	auto msg = conflux::crypto::to_unsigned_span(value);
	return conflux::crypto::base64url_encode(conflux::crypto::hmac_sha256(key, msg));
}

[[nodiscard]] bool signed_value_matches(
	std::string_view value,
	std::string_view sig,
	std::string_view secret) {
	return conflux::crypto::constant_time_eq(sig, mac_b64(value, secret));
}

} // namespace cookie_signing_detail
// Sign a cookie value. Returns "value.BASE64URL(HMAC-SHA256(secret, value))".
std::string sign_cookie(
	std::string_view value,
	std::string_view secret) {
	auto mac = cookie_signing_detail::mac_b64(value, secret);
	std::string out;
	out.reserve(value.size() + 1 + mac.size());
	out.append(value.data(), value.size());
	out.push_back('.');
	out += mac;
	return out;
}
std::expected<std::string, std::string> sign_cookie(
	std::string_view value,
	conflux::http::ResolvedSecretRotation const &secrets) {
	if (auto valid = conflux::http::validate_secret_bytes(secrets.active, "cookie", secrets.min_secret_bytes); !valid) {
		return std::unexpected{valid.error()};
	}
	return sign_cookie(value, secrets.active);
}
// Verify a signed cookie. Returns the original value on success, std::nullopt on failure.
std::optional<std::string> verify_cookie(
	std::string_view signed_value,
	std::string_view secret) {
	auto dot = signed_value.rfind('.');
	if (dot == std::string_view::npos) {
		return std::nullopt;
	}
	auto value = signed_value.substr(0, dot);
	auto sig = signed_value.substr(dot + 1);
	if (!cookie_signing_detail::signed_value_matches(value, sig, secret)) {
		return std::nullopt;
	}
	return std::string{value};
}
std::optional<std::string> verify_cookie(
	std::string_view signed_value,
	conflux::http::ResolvedSecretRotation const &secrets) {
	auto dot = signed_value.rfind('.');
	if (dot == std::string_view::npos) {
		return std::nullopt;
	}
	auto value = signed_value.substr(0, dot);
	auto sig = signed_value.substr(dot + 1);
	if (cookie_signing_detail::signed_value_matches(value, sig, secrets.active)) {
		return std::string{value};
	}
	for (auto const &previous: secrets.previous) {
		if (cookie_signing_detail::signed_value_matches(value, sig, previous)) {
			return std::string{value};
		}
	}
	return std::nullopt;
}
struct CookieSigningOptions {
	conflux::http::ResolvedSecretRotation secrets{};
	// When true, cookies arriving with invalid signatures are stripped (set to empty).
	bool strip_invalid{true};
};

[[nodiscard]] std::expected<CookieSigningOptions, std::string> cookie_signing_options_from_config(
	conflux::http::AuthSecretsConfig const &cfg,
	CookieSigningOptions base = {},
	bool required = true) {
	auto secrets = conflux::http::resolve_secret_rotation(cfg.cookie, "cookie", required);
	if (!secrets) {
		return std::unexpected{secrets.error()};
	}
	base.secrets = std::move(*secrets);
	return base;
}

[[nodiscard]] std::expected<CookieSigningOptions, std::string> cookie_signing_options_from_config(
	conflux::http::Config const &cfg,
	CookieSigningOptions base = {},
	bool required = true) {
	return cookie_signing_options_from_config(cfg.auth_secrets, std::move(base), required);
}

// Middleware: for every incoming cookie, attempt to verify its signature.
// Cookies in "value.SIG" std::format are verified; on success the plain value is injected back.
// Cookies without a "." are passed through unchanged (not all cookies are signed).
// On failure: if strip_invalid=true the cookie is cleared; otherwise it is passed as-is.
Router::Middleware cookie_signing_middleware(
	CookieSigningOptions opts) {
	if (auto valid = conflux::http::validate_secret_bytes(opts.secrets.active, "cookie", opts.secrets.min_secret_bytes);
		!valid) {
		throw std::invalid_argument{valid.error()};
	}
	return [opts = std::move(opts)](
			   conflux::http::RequestView const &req,
			   conflux::http::Router::Handler const &next) -> conflux::http::Response {
		auto modified = req.to_owned();
		for (auto &[name, value]: modified.cookies) {
			if (value.find('.') == std::string::npos) {
				continue;
			} // unsigned cookie
			auto plain = verify_cookie(value, opts.secrets);
			if (plain) {
				value = std::move(*plain);
			} else if (opts.strip_invalid) {
				value.clear();
			}
		}
		return next(modified);
	};
}

} // namespace conflux::http
