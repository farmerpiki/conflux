// CSRF protection via the double-submit cookie pattern.
// On the first request (or when the token cookie is absent), generate a token,
// set it as a cookie, and also expose it in X-CSRF-Token response header for
// JavaScript to read.
// On mutating requests (POST/PUT/PATCH/DELETE), compare the cookie value with
// the token submitted in the request header or form field.
module;
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

export module conflux.net.csrf;
import std;
import conflux.crypto;
import conflux.net.router;
import conflux.utils;
using namespace std;

namespace csrf_detail {

string generate_token() {
	array<unsigned char, 24> bytes{};
	int const fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		throw system_error{errno, system_category(), "generate_token: open /dev/urandom"};
	}
	size_t total = 0;
	while (total < bytes.size()) {
		ssize_t const n = ::read(fd, bytes.data() + total, bytes.size() - total);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			::close(fd);
			throw system_error{errno, system_category(), "generate_token: read /dev/urandom"};
		}
		total += static_cast<size_t>(n);
	}
	::close(fd);
	return base64url_encode(bytes);
}

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

} // namespace csrf_detail

export struct CsrfOptions {
	// Name of the cookie that stores the CSRF token.
	string cookie_name{"csrf_token"};
	// Request header the client must echo the token in.
	string header_name{"X-CSRF-Token"};
	// Form field the client may echo the token in (checked if header absent).
	string form_field{"csrf_token"};
	// Cookie attributes. Includes Secure by default; remove for plain-HTTP dev environments.
	string cookie_attrs{"Path=/; Secure; SameSite=Strict"};
	// HTTP methods that require a valid CSRF token.
	vector<string> protected_methods{"POST", "PUT", "PATCH", "DELETE"};
};

// Middleware factory implementing the double-submit cookie pattern.
// Safe methods (GET, HEAD, OPTIONS) are passed through; the CSRF cookie is
// set/refreshed on every response.  Protected methods are rejected with 403
// when the submitted token does not match the cookie.
export Router::Middleware csrf_middleware(
	CsrfOptions opts = {}) {
	string lower_cookie = ascii_lower(opts.cookie_name);
	string lower_header = ascii_lower(opts.header_name);
	string lower_field = ascii_lower(opts.form_field);

	return [opts = move(opts),
			lower_cookie = move(lower_cookie),
			lower_header = move(lower_header),
			lower_field = move(lower_field)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto is_protected = ranges::any_of(opts.protected_methods, [&](string const &m) { return m == req.method; });

		if (is_protected) {
			// Read cookie token.
			auto cookie_token = string{req.cookies[lower_cookie]};
			if (cookie_token.empty()) {
				HttpResponse r;
				r.status = 403;
				r.status_text = "Forbidden";
				r.content_type = "text/plain; charset=utf-8";
				r.set_text_body("CSRF token missing");
				return r;
			}
			// Read submitted token (header takes precedence over form field).
			string submitted{req.headers[lower_header]};
			if (submitted.empty()) {
				submitted = string{req.form[lower_field]};
			}
			if (!csrf_detail::constant_time_eq(cookie_token, submitted)) {
				HttpResponse r;
				r.status = 403;
				r.status_text = "Forbidden";
				r.content_type = "text/plain; charset=utf-8";
				r.set_text_body("CSRF token invalid");
				return r;
			}
		}

		auto resp = next(req);

		// Refresh/set the CSRF cookie on every response.
		string token{req.cookies[lower_cookie]};
		if (token.empty()) {
			token = csrf_detail::generate_token();
		}
		resp.set_cookie(opts.cookie_name, token, opts.cookie_attrs);
		// Expose the token in a response header for SPA JavaScript.
		resp.headers[opts.header_name] = token;
		return resp;
	};
}
