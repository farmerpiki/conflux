module;
#include <cstdint>

export module conflux.net.jwt;
import std;
import conflux.types;
import std.compat;
import conflux.crypto;
import conflux.net.http.types;
import conflux.net.router;
import conflux.utils;
import conflux.net.config;
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
	ResolvedSecretRotation secrets{}; // active HMAC-SHA256 secret plus previous rotation secrets
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

bool is_json_ws(
	char c) noexcept {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
std::size_t skip_json_ws(
	std::string_view json,
	std::size_t pos) {
	auto rest = json.substr(pos);
	auto it = std::ranges::find_if_not(rest, is_json_ws);
	return pos + static_cast<std::size_t>(std::ranges::distance(rest.begin(), it));
}
// Locate the position of the value after `"key":` in a JSON object, skipping
// whitespace after the colon. Returns std::string_view::npos on miss.
std::size_t json_value_pos(
	std::string_view json,
	std::string_view key) {
	std::size_t pos = 0;
	while (true) {
		pos = json.find('"', pos);
		if (pos == std::string_view::npos) {
			return std::string_view::npos;
		}
		auto const key_begin = pos + 1;
		auto const key_end = key_begin + key.size();
		if (key_end < json.size() && json[key_end] == '"' && json.substr(key_begin, key.size()) == key) {
			pos = key_end + 1;
			break;
		}
		pos = key_begin;
	}
	pos = skip_json_ws(json, pos);
	if (pos >= json.size() || json[pos] != ':') {
		return std::string_view::npos;
	}
	++pos;
	return skip_json_ws(json, pos);
}
// Minimal JSON std::string extractor: find the std::string value of `"key"` in a JSON object.
// Handles basic escaping but not full Unicode escapes — sufficient for JWT claims.
std::string_view json_string(
	std::string_view json,
	std::string_view key) {
	auto pos = json_value_pos(json, key);
	if (pos >= json.size() || json[pos] != '"') {
		return {};
	}
	++pos;
	auto end = pos;
	while (end < json.size()) {
		if (json[end] == '"') {
			return json.substr(pos, end - pos);
		}
		if (json[end] == '\\') {
			++end;
			if (end >= json.size()) {
				return {};
			}
		}
		++end;
	}
	return {};
}
// Minimal JSON number extractor: find std::int64_t value of `"key"`.
std::optional<std::int64_t> json_int_at(
	std::string_view json,
	std::size_t pos) {
	if (pos >= json.size()) {
		return std::nullopt;
	}
	std::int64_t val{};
	auto const *jend = std::ranges::next(json.data(), ssize(json));
	auto const *jpos = json.data() + pos;
	auto [ptr, ec] = std::from_chars(jpos, jend, val);
	if (ec != std::errc{} || ptr == jpos) {
		return std::nullopt;
	}
	return val;
}
bool json_array_contains_string(
	std::string_view json,
	std::string_view key,
	std::string_view value) {
	auto pos = json_value_pos(json, key);
	if (pos >= json.size() || json[pos] != '[') {
		return false;
	}
	++pos;
	bool first = true;
	bool matched = false;
	while (pos < json.size()) {
		pos = skip_json_ws(json, pos);
		if (pos >= json.size()) {
			return false;
		}
		if (json[pos] == ']') {
			return matched;
		}
		if (!first) {
			if (json[pos] != ',') {
				return false;
			}
			++pos;
			pos = skip_json_ws(json, pos);
			if (pos >= json.size() || json[pos] == ']') {
				return false;
			}
		}
		if (json[pos] != '"') {
			return false;
		}
		first = false;
		++pos;
		auto const start = pos;
		while (pos < json.size() && json[pos] != '"') {
			if (json[pos] == '\\') {
				++pos;
			}
			++pos;
		}
		if (pos >= json.size()) {
			return false;
		}
		if (json.substr(start, pos - start) == value) {
			matched = true;
		}
		++pos;
	}
	return false;
}
std::size_t json_top_level_key_count(
	std::string_view json,
	std::string_view key) {
	auto const first = skip_json_ws(json, 0);
	if (first >= json.size() || json[first] != '{') {
		return 0;
	}
	std::size_t count = 0;
	std::size_t depth = 0;
	char previous_sig = 0;
	for (std::size_t pos = first; pos < json.size(); ++pos) {
		char const c = json[pos];
		if (c == '"') {
			auto const str_start = pos + 1;
			auto end = str_start;
			while (end < json.size()) {
				if (json[end] == '"') {
					break;
				}
				if (json[end] == '\\') {
					++end;
				}
				++end;
			}
			if (end >= json.size()) {
				return count;
			}
			auto const after = skip_json_ws(json, end + 1);
			if (depth == 1
				&& (previous_sig == '{' || previous_sig == ',')
				&& after < json.size()
				&& json[after] == ':'
				&& json.substr(str_start, end - str_start) == key) {
				++count;
			}
			pos = end;
			previous_sig = '"';
			continue;
		}
		if (is_json_ws(c)) {
			continue;
		}
		if (c == '{' || c == '[') {
			++depth;
		} else if (c == '}' || c == ']') {
			if (depth == 0) {
				return count;
			}
			--depth;
		}
		previous_sig = c;
	}
	return count;
}
std::optional<std::string_view> first_duplicate_jwt_claim(
	std::string_view json) {
	static constexpr std::array<std::string_view, 7> kClaims = {"sub", "iss", "jti", "exp", "nbf", "iat", "aud"};
	for (auto claim: kClaims) {
		if (json_top_level_key_count(json, claim) > 1) {
			return claim;
		}
	}
	return std::nullopt;
}
// Constant-time std::byte comparison (avoids timing side-channel on signature).
bool ct_equal(
	std::span<unsigned char const> a,
	std::span<unsigned char const> b) {
	if (a.size() != b.size()) {
		return false;
	}
	unsigned char diff = 0;
	for (auto [x, y]: std::views::zip(a, b)) {
		diff = static_cast<unsigned char>(diff | (x ^ y));
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
	ResolvedSecretRotation const &secrets) {
	return validate_secret_bytes(secrets.active, "jwt", secrets.min_secret_bytes);
}

[[nodiscard]] std::array<unsigned char, 32> jwt_signature(
	std::string_view secret,
	std::string_view signing_input) {
	return hmac_sha256(to_unsigned_span(secret), to_unsigned_span(signing_input));
}

[[nodiscard]] bool signature_matches(
	std::string_view signing_input,
	std::string const &sig_claimed,
	ResolvedSecretRotation const &secrets) {
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
	AuthSecretsConfig const &cfg,
	JwtOptions base = {},
	bool required = true) {
	auto secrets = resolve_secret_rotation(cfg.jwt, "jwt", required);
	if (!secrets) {
		return std::unexpected{secrets.error()};
	}
	base.secrets = std::move(*secrets);
	return base;
}

export [[nodiscard]] std::expected<JwtOptions, std::string> jwt_options_from_config(
	Config const &cfg,
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
	if (json_top_level_key_count(header, "alg") > 1) {
		return std::unexpected{"duplicate alg claim"};
	}
	if (json_string(header, "alg") != "HS256") {
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
	if (auto dup = first_duplicate_jwt_claim(payload)) {
		return std::unexpected{std::format("duplicate {} claim", *dup)};
	}

	// Extract standard claims.
	JwtClaims claims{};
	claims.raw = payload;
	claims.sub = std::string{json_string(payload, "sub")};
	claims.iss = std::string{json_string(payload, "iss")};
	claims.jti = std::string{json_string(payload, "jti")};
	auto const exp_pos = json_value_pos(payload, "exp");
	auto const nbf_pos = json_value_pos(payload, "nbf");
	auto const iat_pos = json_value_pos(payload, "iat");
	auto exp = json_int_at(payload, exp_pos);
	auto nbf = json_int_at(payload, nbf_pos);
	auto iat = json_int_at(payload, iat_pos);
	if (exp_pos != std::string_view::npos && (!exp || *exp < 0)) {
		return std::unexpected{"invalid exp claim"};
	}
	if (nbf_pos != std::string_view::npos && (!nbf || *nbf < 0)) {
		return std::unexpected{"invalid nbf claim"};
	}
	if (iat_pos != std::string_view::npos && (!iat || *iat < 0)) {
		return std::unexpected{"invalid iat claim"};
	}
	claims.exp = exp.value_or(0);
	claims.nbf = nbf.value_or(0);
	claims.iat = iat.value_or(0);

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
		auto aud_str = json_string(payload, "aud");
		bool aud_match = aud_str == opts.audience;
		if (!aud_match) {
			aud_match = json_array_contains_string(payload, "aud", opts.audience);
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
export Router::Middleware jwt_middleware(
	JwtOptions opts) {
	return [opts = std::move(opts)](RequestView const &req, Router::Handler const &next) -> Response {
		auto unauthorized = [](std::string_view www_auth) {
			Response r;
			r.status = kHttpUnauthorized;
			r.status_text = "Unauthorized";
			r.content_type = "text/plain; charset=utf-8";
			r.set_text_body("Unauthorized");
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
