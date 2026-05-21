// CSRF protection via the double-submit cookie pattern.
// On the first request (or when the token cookie is absent), generate a token,
// set it as a cookie, and also expose it in X-CSRF-Token response header for
// JavaScript to read.
// On mutating requests (POST/PUT/PATCH/DELETE), compare the cookie value with
// the token submitted in the request header or form field.
export module conflux.net.csrf;
import std;
import conflux.types;
import conflux.crypto;
import conflux.net.http.types;
import conflux.net.router;
import conflux.utils;
namespace csrf_detail {

std::string generate_token() {
	std::array<unsigned char, 24> bytes{};
	crypto_random_bytes(bytes);
	return base64url_encode(bytes);
}

} // namespace csrf_detail
export struct CsrfOptions {
	// Name of the cookie that stores the CSRF token.
	std::string cookie_name{"csrf_token"};
	// Request header the client must echo the token in.
	std::string header_name{"X-CSRF-Token"};
	// Form field the client may echo the token in (checked if header absent).
	std::string form_field{"csrf_token"};
	// Cookie attributes. Includes Secure by default; remove for plain-HTTP dev environments.
	std::string cookie_attrs{"Path=/; Secure; SameSite=Strict"};
	// HTTP methods that require a valid CSRF token.
	std::vector<std::string> protected_methods{"POST", "PUT", "PATCH", "DELETE"};
};
// Middleware factory implementing the double-submit cookie pattern.
// Safe methods (GET, HEAD, OPTIONS) are passed through; the CSRF cookie is
// set/refreshed on every response.  Protected methods are rejected with 403
// when the submitted token does not match the cookie.
export Router::Middleware csrf_middleware(
	CsrfOptions opts = {}) {
	std::string lower_cookie = ascii_lower(opts.cookie_name);
	std::string lower_header = ascii_lower(opts.header_name);
	std::string lower_field = ascii_lower(opts.form_field);

	return [opts = std::move(opts),
			lower_cookie = std::move(lower_cookie),
			lower_header = std::move(lower_header),
			lower_field = std::move(lower_field)](RequestView const &req, Router::Handler const &next) -> Response {
		auto is_protected =
			std::ranges::any_of(opts.protected_methods, [&](std::string const &m) { return m == req.method; });

		if (is_protected) {
			// Read cookie token.
			auto cookie_token = std::string{req.cookies[lower_cookie]};
			if (cookie_token.empty()) {
				Response r;
				r.status = 403;
				r.status_text = "Forbidden";
				r.content_type = "text/plain; charset=utf-8";
				r.set_text_body("CSRF token missing");
				return r;
			}
			// Read submitted token (header takes precedence over form field).
			std::string submitted{req.headers[lower_header]};
			if (submitted.empty()) {
				submitted = std::string{req.form[lower_field]};
			}
			if (!constant_time_eq(cookie_token, submitted)) {
				Response r;
				r.status = 403;
				r.status_text = "Forbidden";
				r.content_type = "text/plain; charset=utf-8";
				r.set_text_body("CSRF token invalid");
				return r;
			}
		}

		auto resp = next(req);

		// Refresh/set the CSRF cookie on every response.
		std::string token{req.cookies[lower_cookie]};
		if (token.empty()) {
			token = csrf_detail::generate_token();
		}
		resp.set_cookie(opts.cookie_name, token, opts.cookie_attrs);
		// Expose the token in a response header for SPA JavaScript.
		resp.headers[opts.header_name] = token;
		return resp;
	};
}
