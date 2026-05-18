// Cookie signing: HMAC-SHA256 in pure C++ (no OpenSSL dep).
// Signed format: "value.BASE64URL(HMAC-SHA256(secret, value))".
module;
#include <cstdint>

export module conflux.net.cookie_signing;
import std;
import conflux.types;
import conflux.crypto;
import conflux.net.config;
import conflux.net.http.types;
import conflux.net.router;
import conflux.utils;
namespace {

std::string mac_b64(
	std::string_view value,
	std::string_view secret) {
	auto key = to_unsigned_span(secret);
	auto msg = to_unsigned_span(value);
	return base64url_encode(hmac_sha256(key, msg));
}

[[nodiscard]] bool signed_value_matches(
	std::string_view value,
	std::string_view sig,
	std::string_view secret) {
	return constant_time_eq(sig, mac_b64(value, secret));
}

} // namespace
// Sign a cookie value. Returns "value.BASE64URL(HMAC-SHA256(secret, value))".
export std::string sign_cookie(
	std::string_view value,
	std::string_view secret) {
	return std::string{value} + '.' + mac_b64(value, secret);
}
export expected<std::string, std::string> sign_cookie(
	std::string_view value,
	ResolvedSecretRotation const &secrets) {
	if (auto valid = validate_secret_bytes(secrets.active, "cookie", secrets.min_secret_bytes); !valid) {
		return unexpected{valid.error()};
	}
	return sign_cookie(value, secrets.active);
}
// Verify a signed cookie. Returns the original value on success, std::nullopt on failure.
export std::optional<std::string> verify_cookie(
	std::string_view signed_value,
	std::string_view secret) {
	auto dot = signed_value.rfind('.');
	if (dot == std::string_view::npos) {
		return nullopt;
	}
	auto value = signed_value.substr(0, dot);
	auto sig = signed_value.substr(dot + 1);
	if (!signed_value_matches(value, sig, secret)) {
		return nullopt;
	}
	return std::string{value};
}
export std::optional<std::string> verify_cookie(
	std::string_view signed_value,
	ResolvedSecretRotation const &secrets) {
	auto dot = signed_value.rfind('.');
	if (dot == std::string_view::npos) {
		return nullopt;
	}
	auto value = signed_value.substr(0, dot);
	auto sig = signed_value.substr(dot + 1);
	if (signed_value_matches(value, sig, secrets.active)) {
		return std::string{value};
	}
	for (auto const &previous: secrets.previous) {
		if (signed_value_matches(value, sig, previous)) {
			return std::string{value};
		}
	}
	return nullopt;
}
export struct CookieSigningOptions {
	ResolvedSecretRotation secrets{};
	// When true, cookies arriving with invalid signatures are stripped (set to empty).
	bool strip_invalid{true};
};

export [[nodiscard]] expected<CookieSigningOptions, std::string> cookie_signing_options_from_config(
	AuthSecretsConfig const &cfg,
	CookieSigningOptions base = {},
	bool required = true) {
	auto secrets = resolve_secret_rotation(cfg.cookie, "cookie", required);
	if (!secrets) {
		return unexpected{secrets.error()};
	}
	base.secrets = move(*secrets);
	return base;
}

export [[nodiscard]] expected<CookieSigningOptions, std::string> cookie_signing_options_from_config(
	Config const &cfg,
	CookieSigningOptions base = {},
	bool required = true) {
	return cookie_signing_options_from_config(cfg.auth_secrets, move(base), required);
}

// Middleware: for every incoming cookie, attempt to verify its signature.
// Cookies in "value.SIG" format are verified; on success the plain value is injected back.
// Cookies without a "." are passed through unchanged (not all cookies are signed).
// On failure: if strip_invalid=true the cookie is cleared; otherwise it is passed as-is.
export Router::Middleware cookie_signing_middleware(
	CookieSigningOptions opts) {
	if (auto valid = validate_secret_bytes(opts.secrets.active, "cookie", opts.secrets.min_secret_bytes); !valid) {
		throw std::invalid_argument{valid.error()};
	}
	return [opts = move(opts)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto modified = req.to_owned();
		for (auto &[name, value]: modified.cookies) {
			if (value.find('.') == std::string::npos) {
				continue;
			} // unsigned cookie
			auto plain = verify_cookie(value, opts.secrets);
			if (plain) {
				value = move(*plain);
			} else if (opts.strip_invalid) {
				value.clear();
			}
		}
		return next(modified);
	};
}
