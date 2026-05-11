export module conflux.net.cors;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
import conflux.utils;

export constexpr unsigned kCorsDefaultMaxAge = 86400U; // 24 hours
export struct CorsOptions {
	V<S> allowed_origins{"*"};
	V<S> allowed_methods{"GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"};
	V<S> allowed_headers{"Content-Type", "Authorization", "Accept"};
	V<S> expose_headers{};
	unsigned max_age{kCorsDefaultMaxAge};
	bool allow_credentials{false};
};
namespace cors_detail {

// Join a V of strings with ", ".
S join(
	V<S> const &v) {
	S s;
	for (auto const &e: v) {
		if (!s.empty()) {
			s += ", ";
		}
		s += e;
	}
	return s;
}
// Resolve the Access-Control-Allow-Origin value for this request.
// Returns the matching origin S, or empty if the request origin is not allowed.
S resolve_origin(
	CorsOptions const &opts,
	SV request_origin) {
	if (opts.allowed_origins.size() == 1 && opts.allowed_origins[0] == "*") {
		// Wildcard: reflect origin when credentials are used, else return "*".
		return opts.allow_credentials ? S{request_origin} : S{"*"};
	}
	return ranges::contains(opts.allowed_origins, request_origin) ? S{request_origin} : S{};
}
void inject_cors_headers(
	CorsOptions const &opts,
	SV request_origin,
	HttpResponse &resp) {
	auto origin = resolve_origin(opts, request_origin);
	if (origin.empty()) {
		return;
	}
	resp.headers["Access-Control-Allow-Origin"] = move(origin);
	if (opts.allow_credentials) {
		resp.headers["Access-Control-Allow-Credentials"] = "true";
	}
	if (!opts.expose_headers.empty()) {
		resp.headers["Access-Control-Expose-Headers"] = join(opts.expose_headers);
	}
	resp.append_vary("Origin");
}

} // namespace cors_detail
// Middleware factory: handle CORS preflight and inject CORS headers.
// Register this before other middleware so OPTIONS preflights short-circuit
// before route matching attempts (first-registered = outermost wrapper).
export Router::Middleware cors_middleware(
	CorsOptions opts = {}) {
	return [opts = move(opts)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto request_origin = req.headers["origin"];

		// Preflight: OPTIONS + Origin + Access-Control-Request-Method
		if (req.method == "OPTIONS"
			&& !request_origin.empty()
			&& !req.headers["access-control-request-method"].empty()) {
			HttpResponse preflight{.status = kHttpNoContent, .status_text = "No Content", .content_type = "text/plain"};
			auto origin = cors_detail::resolve_origin(opts, request_origin);
			if (!origin.empty()) {
				preflight.headers["Access-Control-Allow-Origin"] = move(origin);
				preflight.headers["Access-Control-Allow-Methods"] = cors_detail::join(opts.allowed_methods);
				preflight.headers["Access-Control-Allow-Headers"] = cors_detail::join(opts.allowed_headers);
				preflight.headers["Access-Control-Max-Age"] = format("{}", opts.max_age);
				if (opts.allow_credentials) {
					preflight.headers["Access-Control-Allow-Credentials"] = "true";
				}
				preflight.headers["Vary"] = "Origin";
			}
			return preflight;
		}

		auto resp = next(req);
		if (!request_origin.empty()) {
			cors_detail::inject_cors_headers(opts, request_origin, resp);
		}
		return resp;
	};
}
