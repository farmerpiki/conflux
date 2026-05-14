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
	S sub{};
	S iss{};
	S jti{};
	i64 exp{};
	i64 nbf{};
	i64 iat{};
	S raw{}; // full decoded payload JSON (for custom claims)
};
export struct JwtOptions {
	ResolvedSecretRotation secrets{}; // active HMAC-SHA256 secret plus previous rotation secrets
	S issuer{}; // expected iss claim; "" = skip check
	S audience{}; // expected aud claim; "" = skip check
	bool verify_exp{true}; // reject expired tokens when an exp claim is present
	bool verify_nbf{true}; // reject not-yet-valid tokens when an nbf claim is present
	bool require_exp{false}; // reject tokens without an exp claim
	bool require_iat{false}; // reject tokens without an iat claim
	bool require_jti{false}; // reject tokens without a jti claim
	chrono::seconds clock_skew{}; // tolerance for exp/nbf comparisons
	chrono::seconds max_token_lifetime{}; // 0 = disabled; otherwise requires exp+iat and caps exp-iat
	std::function<bool(SV)> revoked_jti{}; // optional revocation lookup; true = reject token
};
// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

bool is_json_ws(
	char c) noexcept {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
SZ skip_json_ws(
	SV json,
	SZ pos) {
	auto rest = json.substr(pos);
	auto it = ranges::find_if_not(rest, is_json_ws);
	return pos + static_cast<SZ>(ranges::distance(rest.begin(), it));
}
// Locate the position of the value after `"key":` in a JSON object, skipping
// whitespace after the colon. Returns SV::npos on miss.
SZ json_value_pos(
	SV json,
	SV key) {
	SZ pos = 0;
	while (true) {
		pos = json.find('"', pos);
		if (pos == SV::npos) {
			return SV::npos;
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
		return SV::npos;
	}
	++pos;
	return skip_json_ws(json, pos);
}
// Minimal JSON S extractor: find the S value of `"key"` in a JSON object.
// Handles basic escaping but not full Unicode escapes — sufficient for JWT claims.
SV json_string(
	SV json,
	SV key) {
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
// Minimal JSON number extractor: find i64 value of `"key"`.
Opt<i64> json_int_at(
	SV json,
	SZ pos) {
	if (pos >= json.size()) {
		return nullopt;
	}
	i64 val{};
	auto const *jend = ranges::next(json.data(), ssize(json));
	auto const *jpos = json.data() + pos;
	auto [ptr, ec] = from_chars(jpos, jend, val);
	if (ec != errc{} || ptr == jpos) {
		return nullopt;
	}
	return val;
}
bool json_array_contains_string(
	SV json,
	SV key,
	SV value) {
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
// Constant-time byte comparison (avoids timing side-channel on signature).
bool ct_equal(
	span<unsigned char const> a,
	span<unsigned char const> b) {
	if (a.size() != b.size()) {
		return false;
	}
	unsigned char diff = 0;
	for (auto [x, y]: views::zip(a, b)) {
		diff = static_cast<unsigned char>(diff | (x ^ y));
	}
	return diff == 0;
}
[[nodiscard]] bool token_expired(
	i64 exp,
	i64 now,
	i64 skew) noexcept {
	if (skew >= now) {
		return false;
	}
	return exp <= now - skew;
}
[[nodiscard]] bool token_not_yet_valid(
	i64 nbf,
	i64 now,
	i64 skew) noexcept {
	if (skew > std::numeric_limits<i64>::max() - now) {
		return false;
	}
	return nbf > now + skew;
}

} // namespace
namespace jwt_detail {

Opt<SV> bearer_token(
	SV auth) noexcept {
	static constexpr SV kScheme = "Bearer";
	if (auth.size() <= kScheme.size() || auth[kScheme.size()] != ' ') {
		return nullopt;
	}
	if (!conflux::http::ascii_iequals(auth.substr(0, kScheme.size()), kScheme)) {
		return nullopt;
	}
	return auth.substr(kScheme.size() + 1);
}


[[nodiscard]] expected<void, S> validate_jwt_secrets(
	ResolvedSecretRotation const &secrets) {
	return validate_secret_bytes(secrets.active, "jwt", secrets.min_secret_bytes);
}

[[nodiscard]] A<unsigned char, 32> jwt_signature(
	SV secret,
	SV signing_input) {
	return hmac_sha256(to_unsigned_span(secret), to_unsigned_span(signing_input));
}

[[nodiscard]] bool signature_matches(
	SV signing_input,
	S const &sig_claimed,
	ResolvedSecretRotation const &secrets) {
	auto expected = jwt_signature(secrets.active, signing_input);
	if (ct_equal(to_unsigned_span(sig_claimed), span{expected.data(), expected.size()})) {
		return true;
	}
	for (auto const &previous: secrets.previous) {
		expected = jwt_signature(previous, signing_input);
		if (ct_equal(to_unsigned_span(sig_claimed), span{expected.data(), expected.size()})) {
			return true;
		}
	}
	return false;
}

} // namespace jwt_detail
// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

export std::string jwt_sign(
	SV payload_json,
	SV secret);
export std::string jwt_sign(
	SV header_json,
	SV payload_json,
	SV secret);


export [[nodiscard]] expected<JwtOptions, S> jwt_options_from_config(
	AuthSecretsConfig const &cfg,
	JwtOptions base = {},
	bool required = true) {
	auto secrets = resolve_secret_rotation(cfg.jwt, "jwt", required);
	if (!secrets) {
		return unexpected{secrets.error()};
	}
	base.secrets = move(*secrets);
	return base;
}

export [[nodiscard]] expected<JwtOptions, S> jwt_options_from_config(
	Config const &cfg,
	JwtOptions base = {},
	bool required = true) {
	return jwt_options_from_config(cfg.auth_secrets, move(base), required);
}

// Decode and verify a JWT.  Returns JwtClaims on success, error string on failure.
export expected<JwtClaims, std::string> jwt_decode(
	SV token,
	JwtOptions const &opts) {
	if (auto valid = jwt_detail::validate_jwt_secrets(opts.secrets); !valid) {
		return unexpected{valid.error()};
	}
	// Split header.payload.signature
	auto dot1 = token.find('.');
	if (dot1 == SV::npos) {
		return unexpected{"malformed token: missing first dot"};
	}
	auto dot2 = token.find('.', dot1 + 1);
	if (dot2 == SV::npos) {
		return unexpected{"malformed token: missing second dot"};
	}

	auto header_b64 = token.substr(0, dot1);
	auto payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
	auto signature_b64 = token.substr(dot2 + 1);

	// Decode header and check algorithm.
	auto header = base64url_decode(header_b64);
	if (header.empty()) {
		return unexpected{"invalid header encoding"};
	}
	if (json_string(header, "alg") != "HS256") {
		return unexpected{"unsupported algorithm (only HS256 supported)"};
	}

	// Decode payload.
	auto payload = base64url_decode(payload_b64);
	if (payload.empty()) {
		return unexpected{"invalid payload encoding"};
	}

	// Decode claimed signature.
	auto sig_claimed = base64url_decode(signature_b64);
	if (sig_claimed.empty()) {
		return unexpected{"invalid signature encoding"};
	}

	// Recompute expected signature over "header_b64.payload_b64".
	SV const signing_input = token.substr(0, static_cast<SZ>(dot2));
	if (!jwt_detail::signature_matches(signing_input, sig_claimed, opts.secrets)) {
		return unexpected{"signature verification failed"};
	}

	// Extract standard claims.
	JwtClaims claims{};
	claims.raw = payload;
	claims.sub = S{json_string(payload, "sub")};
	claims.iss = S{json_string(payload, "iss")};
	claims.jti = S{json_string(payload, "jti")};
	auto const exp_pos = json_value_pos(payload, "exp");
	auto const nbf_pos = json_value_pos(payload, "nbf");
	auto const iat_pos = json_value_pos(payload, "iat");
	auto exp = json_int_at(payload, exp_pos);
	auto nbf = json_int_at(payload, nbf_pos);
	auto iat = json_int_at(payload, iat_pos);
	if (exp_pos != SV::npos && (!exp || *exp < 0)) {
		return unexpected{"invalid exp claim"};
	}
	if (nbf_pos != SV::npos && (!nbf || *nbf < 0)) {
		return unexpected{"invalid nbf claim"};
	}
	if (iat_pos != SV::npos && (!iat || *iat < 0)) {
		return unexpected{"invalid iat claim"};
	}
	claims.exp = exp.value_or(0);
	claims.nbf = nbf.value_or(0);
	claims.iat = iat.value_or(0);

	// Validate claims.
	auto const now = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
	auto const skew = max<i64>(0, chrono::duration_cast<chrono::seconds>(opts.clock_skew).count());
	auto const max_lifetime = chrono::duration_cast<chrono::seconds>(opts.max_token_lifetime).count();

	if (opts.require_exp && claims.exp == 0) {
		return unexpected{"missing exp claim"};
	}
	if (opts.require_iat && claims.iat == 0) {
		return unexpected{"missing iat claim"};
	}
	if (opts.require_jti && claims.jti.empty()) {
		return unexpected{"missing jti claim"};
	}
	if (opts.verify_exp && claims.exp != 0 && token_expired(claims.exp, now, skew)) {
		return unexpected{"token expired"};
	}
	if (opts.verify_nbf && claims.nbf != 0 && token_not_yet_valid(claims.nbf, now, skew)) {
		return unexpected{"token not yet valid"};
	}
	if (max_lifetime > 0) {
		if (claims.exp == 0) {
			return unexpected{"missing exp claim"};
		}
		if (claims.iat == 0) {
			return unexpected{"missing iat claim"};
		}
		if (claims.exp < claims.iat) {
			return unexpected{"invalid token lifetime"};
		}
		if (claims.exp - claims.iat > max_lifetime) {
			return unexpected{"token lifetime too long"};
		}
	}
	if (opts.revoked_jti && !claims.jti.empty() && opts.revoked_jti(claims.jti)) {
		return unexpected{"token revoked"};
	}
	if (!opts.issuer.empty() && claims.iss != opts.issuer) {
		return unexpected{format("issuer mismatch: got '{}', want '{}'", claims.iss, opts.issuer)};
	}
	if (!opts.audience.empty()) {
		auto aud_str = json_string(payload, "aud");
		bool aud_match = aud_str == opts.audience;
		if (!aud_match) {
			aud_match = json_array_contains_string(payload, "aud", opts.audience);
		}
		if (!aud_match) {
			return unexpected{"audience mismatch"};
		}
	}

	return claims;
}
// Sign a payload JSON S and return a complete JWT.
// payload_json must be a valid JSON object S, e.g. R"({"sub":"user1","exp":9999999999})".
export expected<std::string, S> jwt_sign(
	SV payload_json,
	JwtOptions const &opts) {
	if (auto valid = jwt_detail::validate_jwt_secrets(opts.secrets); !valid) {
		return unexpected{valid.error()};
	}
	return jwt_sign(payload_json, opts.secrets.active);
}
export std::string jwt_sign(
	SV payload_json,
	SV secret) {
	// Header: {"alg":"HS256","typ":"JWT"}
	static constexpr SV kHeader = R"({"alg":"HS256","typ":"JWT"})";
	auto header_b64 = base64url_encode(to_unsigned_span(kHeader));
	auto payload_b64 = base64url_encode(to_unsigned_span(payload_json));

	S const signing_input = header_b64 + '.' + payload_b64;
	auto sig = hmac_sha256(to_unsigned_span(secret), to_unsigned_span(signing_input));
	auto sig_b64 = base64url_encode(span{sig.data(), sig.size()});

	return signing_input + '.' + sig_b64;
}
export expected<std::string, S> jwt_sign(
	SV header_json,
	SV payload_json,
	JwtOptions const &opts) {
	if (auto valid = jwt_detail::validate_jwt_secrets(opts.secrets); !valid) {
		return unexpected{valid.error()};
	}
	return jwt_sign(header_json, payload_json, opts.secrets.active);
}
export std::string jwt_sign(
	SV header_json,
	SV payload_json,
	SV secret) {
	auto header_b64 = base64url_encode(to_unsigned_span(header_json));
	auto payload_b64 = base64url_encode(to_unsigned_span(payload_json));

	S const signing_input = header_b64 + '.' + payload_b64;
	auto sig = hmac_sha256(to_unsigned_span(secret), to_unsigned_span(signing_input));
	auto sig_b64 = base64url_encode(span{sig.data(), sig.size()});

	return signing_input + '.' + sig_b64;
}
// Middleware: verify the Bearer JWT in Authorization header.
// On success: injects jwt_sub, jwt_iss, jwt_payload into a copy of the request params.
// On failure: returns 401 with WWW-Authenticate: Bearer error=...
export Router::Middleware jwt_middleware(
	JwtOptions opts) {
	return [opts = move(opts)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto unauthorized = [](SV www_auth) {
			HttpResponse r;
			r.status = kHttpUnauthorized;
			r.status_text = "Unauthorized";
			r.content_type = "text/plain; charset=utf-8";
			r.set_text_body("Unauthorized");
			r.headers["WWW-Authenticate"] = S{www_auth};
			return r;
		};

		auto auth = req.headers["authorization"];
		auto token = jwt_detail::bearer_token(auth);
		if (!token) {
			return unauthorized("Bearer");
		}
		auto result = jwt_decode(trim(*token), opts);
		if (!result) {
			return unauthorized(format(R"(Bearer error="invalid_token", error_description="{}")", result.error()));
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
