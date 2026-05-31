module;
#include <cstdint>

export module conflux.net.jwt;
import std;
import conflux.types;
import std.compat;
import conflux.crypto;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;
import conflux.utils;
import conflux.net.config;
import conflux.json;
// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

export struct JwtClaims {
	std::string sub{};
	std::string iss{};
	std::string jti{};
	std::int64_t exp{};
	std::int64_t nbf{};
	std::int64_t iat{};
	std::string raw{}; // full decoded payload JSON (for custom claims)
};
export struct JwtOptions {
	conflux::http::ResolvedSecretRotation secrets{}; // active HMAC-SHA256 secret plus previous rotation secrets
	std::string issuer{}; // std::expected iss claim; "" = skip check
	std::string audience{}; // std::expected aud claim; "" = skip check
	bool verify_exp{true}; // reject expired tokens when an exp claim is present
	bool verify_nbf{true}; // reject not-yet-valid tokens when an nbf claim is present
	bool require_exp{false}; // reject tokens without an exp claim
	bool require_iat{false}; // reject tokens without an iat claim
	bool require_jti{false}; // reject tokens without a jti claim
	std::chrono::seconds clock_skew{}; // tolerance for exp/nbf comparisons
	std::chrono::seconds max_token_lifetime{}; // 0 = disabled; otherwise requires exp+iat and caps exp-iat
	std::function<bool(std::string_view)> revoked_jti{}; // optional revocation lookup; true = reject token
	std::size_t max_token_bytes{16U * 1024U}; // 0 = disabled; bounds bearer-token abuse before decoding
};
// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

std::string jwt_json_error(
	JsonError const &err,
	std::string_view segment) {
	static constexpr std::string_view kDuplicatePrefix = "duplicate member: ";
	if (err.code == JsonIssueCode::duplicate_member) {
		if (err.member_name) {
			return std::format("duplicate {} claim", *err.member_name);
		}
		if (err.message.starts_with(kDuplicatePrefix)) {
			return std::format("duplicate {} claim", std::string_view{err.message}.substr(kDuplicatePrefix.size()));
		}
		return "duplicate claim";
	}
	return std::format("invalid {} JSON: {}", segment, err.message);
}
std::expected<Document, std::string> parse_jwt_json(
	std::string_view input,
	std::string_view segment) {
	auto parsed = conflux::json::parse(input);
	if (!parsed) {
		return std::unexpected{jwt_json_error(parsed.error(), segment)};
	}
	if (!parsed->root().as_object()) {
		return std::unexpected{std::format("{} must be a JSON object", segment)};
	}
	return std::expected<Document, std::string>{std::in_place, std::move(*parsed)};
}
std::string jwt_string_claim(
	ObjectView const &obj,
	std::string_view key) {
	auto value = optional_string(obj, key);
	if (!value || !*value) {
		return {};
	}
	return std::move(**value);
}
std::expected<std::optional<std::int64_t>, std::string> jwt_numeric_date_claim(
	ObjectView const &obj,
	std::string_view key,
	std::string_view error) {
	auto value = obj.optional<std::int64_t>(key);
	if (!value || (*value && **value < 0)) {
		return std::unexpected{std::string{error}};
	}
	return *value;
}
bool json_array_contains_string(
	ObjectView const &obj,
	std::string_view key,
	std::string_view value) {
	auto member = obj.find_member(key);
	if (!member) {
		return false;
	}
	auto array = member->as_array();
	if (!array) {
		return false;
	}
	for (auto element: array->elements()) {
		auto str = element.as_string();
		if (str && *str == value) {
			return true;
		}
	}
	return false;
}
// Constant-time std::byte comparison (avoids timing side-channel on signature).
bool ct_equal(
	std::span<unsigned char const> a,
	std::span<unsigned char const> b) {
	if (a.size() != b.size()) {
		return false;
	}
	unsigned char diff = 0;
	for (std::size_t i = 0; i < a.size(); ++i) {
		diff = static_cast<unsigned char>(diff | (a[i] ^ b[i]));
	}
	return diff == 0;
}
[[nodiscard]] bool token_expired(
	std::int64_t exp,
	std::int64_t now,
	std::int64_t skew) noexcept {
	if (skew >= now) {
		return false;
	}
	return exp <= now - skew;
}
[[nodiscard]] bool token_not_yet_valid(
	std::int64_t nbf,
	std::int64_t now,
	std::int64_t skew) noexcept {
	if (skew > std::numeric_limits<std::int64_t>::max() - now) {
		return false;
	}
	return nbf > now + skew;
}

} // namespace
namespace jwt_detail {

[[nodiscard]] std::expected<void, std::string> validate_jwt_secrets(
	conflux::http::ResolvedSecretRotation const &secrets) {
	return conflux::http::validate_secret_bytes(secrets.active, "jwt", secrets.min_secret_bytes);
}

[[nodiscard]] std::array<unsigned char, 32> jwt_signature(
	std::string_view secret,
	std::string_view signing_input) {
	return hmac_sha256(to_unsigned_span(secret), to_unsigned_span(signing_input));
}

[[nodiscard]] bool signature_matches(
	std::string_view signing_input,
	std::string const &sig_claimed,
	conflux::http::ResolvedSecretRotation const &secrets) {
	auto expected_signature = jwt_signature(secrets.active, signing_input);
	if (ct_equal(to_unsigned_span(sig_claimed), std::span{expected_signature.data(), expected_signature.size()})) {
		return true;
	}
	for (auto const &previous: secrets.previous) {
		expected_signature = jwt_signature(previous, signing_input);
		if (ct_equal(to_unsigned_span(sig_claimed), std::span{expected_signature.data(), expected_signature.size()})) {
			return true;
		}
	}
	return false;
}

} // namespace jwt_detail
// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

export std::string jwt_sign(std::string_view payload_json, std::string_view secret);
export std::string jwt_sign(std::string_view header_json, std::string_view payload_json, std::string_view secret);

export [[nodiscard]] std::expected<JwtOptions, std::string> jwt_options_from_config(
	conflux::http::AuthSecretsConfig const &cfg,
	JwtOptions base = {},
	bool required = true) {
	auto secrets = conflux::http::resolve_secret_rotation(cfg.jwt, "jwt", required);
	if (!secrets) {
		return std::unexpected{secrets.error()};
	}
	base.secrets = std::move(*secrets);
	return base;
}

export [[nodiscard]] std::expected<JwtOptions, std::string> jwt_options_from_config(
	conflux::http::Config const &cfg,
	JwtOptions base = {},
	bool required = true) {
	return jwt_options_from_config(cfg.auth_secrets, std::move(base), required);
}

// Decode and verify a JWT.  Returns JwtClaims on success, error string on failure.
export std::expected<JwtClaims, std::string> jwt_decode(
	std::string_view token,
	JwtOptions const &opts) {
	if (auto valid = jwt_detail::validate_jwt_secrets(opts.secrets); !valid) {
		return std::unexpected{valid.error()};
	}
	if (opts.max_token_bytes > 0 && token.size() > opts.max_token_bytes) {
		return std::unexpected{"token too large"};
	}
	// Split header.payload.signature
	auto dot1 = token.find('.');
	if (dot1 == std::string_view::npos) {
		return std::unexpected{"malformed token: missing first dot"};
	}
	auto dot2 = token.find('.', dot1 + 1);
	if (dot2 == std::string_view::npos) {
		return std::unexpected{"malformed token: missing second dot"};
	}

	auto header_b64 = token.substr(0, dot1);
	auto payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
	auto signature_b64 = token.substr(dot2 + 1);

	// Decode header and check algorithm.
	auto header = base64url_decode(header_b64);
	if (header.empty()) {
		return std::unexpected{"invalid header encoding"};
	}
	auto header_doc = parse_jwt_json(header, "header");
	if (!header_doc) {
		return std::unexpected{header_doc.error()};
	}
	auto header_obj = header_doc->root().as_object();
	if (jwt_string_claim(*header_obj, "alg") != "HS256") {
		return std::unexpected{"unsupported algorithm (only HS256 supported)"};
	}

	// Decode payload.
	auto payload = base64url_decode(payload_b64);
	if (payload.empty()) {
		return std::unexpected{"invalid payload encoding"};
	}

	// Decode claimed signature.
	auto sig_claimed = base64url_decode(signature_b64);
	if (sig_claimed.empty()) {
		return std::unexpected{"invalid signature encoding"};
	}

	// Recompute std::expected signature over "header_b64.payload_b64".
	std::string_view const signing_input = token.substr(0, static_cast<std::size_t>(dot2));
	if (!jwt_detail::signature_matches(signing_input, sig_claimed, opts.secrets)) {
		return std::unexpected{"signature verification failed"};
	}
	auto payload_doc = parse_jwt_json(payload, "payload");
	if (!payload_doc) {
		return std::unexpected{payload_doc.error()};
	}
	auto payload_obj = payload_doc->root().as_object();

	// Extract standard claims.
	JwtClaims claims{};
	claims.raw = payload;
	claims.sub = jwt_string_claim(*payload_obj, "sub");
	claims.iss = jwt_string_claim(*payload_obj, "iss");
	claims.jti = jwt_string_claim(*payload_obj, "jti");
	auto exp = jwt_numeric_date_claim(*payload_obj, "exp", "invalid exp claim");
	if (!exp) {
		return std::unexpected{exp.error()};
	}
	auto nbf = jwt_numeric_date_claim(*payload_obj, "nbf", "invalid nbf claim");
	if (!nbf) {
		return std::unexpected{nbf.error()};
	}
	auto iat = jwt_numeric_date_claim(*payload_obj, "iat", "invalid iat claim");
	if (!iat) {
		return std::unexpected{iat.error()};
	}
	claims.exp = exp->value_or(0);
	claims.nbf = nbf->value_or(0);
	claims.iat = iat->value_or(0);

	// Validate claims.
	auto const now =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	auto const skew =
		std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(opts.clock_skew).count());
	auto const max_lifetime = std::chrono::duration_cast<std::chrono::seconds>(opts.max_token_lifetime).count();

	if (opts.require_exp && claims.exp == 0) {
		return std::unexpected{"missing exp claim"};
	}
	if (opts.require_iat && claims.iat == 0) {
		return std::unexpected{"missing iat claim"};
	}
	if (opts.require_jti && claims.jti.empty()) {
		return std::unexpected{"missing jti claim"};
	}
	if (opts.verify_exp && claims.exp != 0 && token_expired(claims.exp, now, skew)) {
		return std::unexpected{"token expired"};
	}
	if (opts.verify_nbf && claims.nbf != 0 && token_not_yet_valid(claims.nbf, now, skew)) {
		return std::unexpected{"token not yet valid"};
	}
	if (max_lifetime > 0) {
		if (claims.exp == 0) {
			return std::unexpected{"missing exp claim"};
		}
		if (claims.iat == 0) {
			return std::unexpected{"missing iat claim"};
		}
		if (claims.exp < claims.iat) {
			return std::unexpected{"invalid token lifetime"};
		}
		if (claims.exp - claims.iat > max_lifetime) {
			return std::unexpected{"token lifetime too long"};
		}
	}
	if (opts.revoked_jti && !claims.jti.empty() && opts.revoked_jti(claims.jti)) {
		return std::unexpected{"token revoked"};
	}
	if (!opts.issuer.empty() && claims.iss != opts.issuer) {
		return std::unexpected{std::format("issuer mismatch: got '{}', want '{}'", claims.iss, opts.issuer)};
	}
	if (!opts.audience.empty()) {
		auto aud_str = jwt_string_claim(*payload_obj, "aud");
		bool aud_match = aud_str == opts.audience;
		if (!aud_match) {
			aud_match = json_array_contains_string(*payload_obj, "aud", opts.audience);
		}
		if (!aud_match) {
			return std::unexpected{"audience mismatch"};
		}
	}

	return claims;
}
// Sign a payload JSON std::string and return a complete JWT.
// payload_json must be a valid JSON object std::string, e.g. R"({"sub":"user1","exp":9999999999})".
export std::expected<std::string, std::string> jwt_sign(
	std::string_view payload_json,
	JwtOptions const &opts) {
	if (auto valid = jwt_detail::validate_jwt_secrets(opts.secrets); !valid) {
		return std::unexpected{valid.error()};
	}
	return jwt_sign(payload_json, opts.secrets.active);
}
export std::string jwt_sign(
	std::string_view payload_json,
	std::string_view secret) {
	// Header: {"alg":"HS256","typ":"JWT"}
	static constexpr std::string_view kHeader = R"({"alg":"HS256","typ":"JWT"})";
	auto header_b64 = base64url_encode(to_unsigned_span(kHeader));
	auto payload_b64 = base64url_encode(to_unsigned_span(payload_json));

	std::string const signing_input = header_b64 + '.' + payload_b64;
	auto sig = hmac_sha256(to_unsigned_span(secret), to_unsigned_span(signing_input));
	auto sig_b64 = base64url_encode(std::span{sig.data(), sig.size()});

	return signing_input + '.' + sig_b64;
}
export std::expected<std::string, std::string> jwt_sign(
	std::string_view header_json,
	std::string_view payload_json,
	JwtOptions const &opts) {
	if (auto valid = jwt_detail::validate_jwt_secrets(opts.secrets); !valid) {
		return std::unexpected{valid.error()};
	}
	return jwt_sign(header_json, payload_json, opts.secrets.active);
}
export std::string jwt_sign(
	std::string_view header_json,
	std::string_view payload_json,
	std::string_view secret) {
	auto header_b64 = base64url_encode(to_unsigned_span(header_json));
	auto payload_b64 = base64url_encode(to_unsigned_span(payload_json));

	std::string const signing_input = header_b64 + '.' + payload_b64;
	auto sig = hmac_sha256(to_unsigned_span(secret), to_unsigned_span(signing_input));
	auto sig_b64 = base64url_encode(std::span{sig.data(), sig.size()});

	return signing_input + '.' + sig_b64;
}
// Middleware: verify the Bearer JWT in Authorization header.
// On success: injects jwt_sub, jwt_iss, jwt_payload into a copy of the request params.
// On failure: returns 401 with WWW-Authenticate: Bearer error=...
export conflux::http::Router::Middleware jwt_middleware(
	JwtOptions opts) {
	return [opts = std::move(
				opts)](conflux::http::RequestView const &req, conflux::http::Router::Handler const &next) -> conflux::http::Response {
		auto unauthorized = [](std::string_view www_auth) {
			auto r = conflux::http::Response::text("Unauthorized", kHttpUnauthorized, "Unauthorized");
			r.headers["WWW-Authenticate"] = std::string{www_auth};
			return r;
		};

		auto auth = req.headers["authorization"];
		auto token = conflux::http::credentials_for_auth_scheme(auth, "Bearer");
		if (!token) {
			return unauthorized("Bearer");
		}
		auto result = jwt_decode(*token, opts);
		if (!result) {
			return unauthorized(std::format(R"(Bearer error="invalid_token", error_description="{}")", result.error()));
		}
		auto const &claims = *result;
		// Copy request, inject claims as params so handlers can read them.
		auto modified = req.to_owned();
		modified.params.set("jwt_sub", claims.sub);
		modified.params.set("jwt_iss", claims.iss);
		modified.params.set("jwt_payload", claims.raw);
		return next(modified);
	};
}
