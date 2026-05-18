export module conflux.net.security;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
export struct SecurityOptions {
	// Strict-Transport-Security max-age in seconds; 0 disables the header.
	unsigned hsts_max_age{31536000}; // 1 year
	bool hsts_include_subdomains{true};

	// X-Frame-Options: "DENY", "SAMEORIGIN", or "" to disable.
	std::string frame_options{"DENY"};

	// X-Content-Type-Options: nosniff (true = send header).
	bool nosniff{true};

	// X-XSS-Protection value; empty disables. OWASP recommends "0" — the legacy
	// filter (1; mode=block) introduced XSS vectors in older browsers.
	std::string xss_protection{"0"};

	// Referrer-Policy value; empty std::string disables the header.
	std::string referrer_policy{"strict-origin-when-cross-origin"};

	// Permissions-Policy value; empty std::string disables the header.
	std::string permissions_policy{"geolocation=(), microphone=(), camera=()"};

	// Content-Security-Policy value; empty std::string disables the header.
	std::string csp;

	// When true, Strict-Transport-Security is only emitted on TLS connections.
	// Set to false only in tests that run plain-HTTP servers.
	bool hsts_only_on_tls{true};
};
// Middleware factory: inject security headers into every response.
export Router::Middleware security_headers_middleware(
	SecurityOptions opts = {}) {
	return [opts = move(opts)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto resp = next(req);

		if (opts.hsts_max_age > 0 && (!opts.hsts_only_on_tls || req.is_tls)) {
			auto hsts = format("max-age={}", opts.hsts_max_age);
			if (opts.hsts_include_subdomains) {
				hsts += "; includeSubDomains";
			}
			resp.headers["Strict-Transport-Security"] = move(hsts);
		}
		if (!opts.frame_options.empty()) {
			resp.headers["X-Frame-Options"] = opts.frame_options;
		}
		if (opts.nosniff) {
			resp.headers["X-Content-Type-Options"] = "nosniff";
		}
		if (!opts.xss_protection.empty()) {
			resp.headers["X-XSS-Protection"] = opts.xss_protection;
		}
		if (!opts.referrer_policy.empty()) {
			resp.headers["Referrer-Policy"] = opts.referrer_policy;
		}
		if (!opts.permissions_policy.empty()) {
			resp.headers["Permissions-Policy"] = opts.permissions_policy;
		}
		if (!opts.csp.empty()) {
			resp.headers["Content-Security-Policy"] = opts.csp;
		}
		return resp;
	};
}
