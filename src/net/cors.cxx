export module conflux.net.cors;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;

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
	SZ total = 0;
	for (auto const &e: v) {
		total += e.size();
	}
	if (v.size() > 1) {
		total += (v.size() - 1) * 2;
	}
	S s;
	s.reserve(total);
	for (auto const &e: v) {
		if (!s.empty()) {
			s += ", ";
		}
		s += e;
	}
	return s;
}
[[nodiscard]] S decimal_string(
	unsigned value) {
	A<char, 16> buf{};
	auto [ptr, ec] = to_chars(buf.data(), buf.data() + buf.size(), value);
	if (ec != errc{}) {
		return {};
	}
	return S{buf.data(), static_cast<SZ>(ptr - buf.data())};
}
struct PreparedCorsOptions {
	CorsOptions opts;
	S allowed_methods;
	S allowed_headers;
	S expose_headers;
	S max_age;
	bool wildcard_origin{false};

	explicit PreparedCorsOptions(
		CorsOptions options)
		: opts(move(options))
		, allowed_methods(join(opts.allowed_methods))
		, allowed_headers(join(opts.allowed_headers))
		, expose_headers(join(opts.expose_headers))
		, max_age(decimal_string(opts.max_age))
		, wildcard_origin(opts.allowed_origins.size() == 1 && opts.allowed_origins[0] == "*") {}
};
// Resolve the Access-Control-Allow-Origin value for this request.
// Returns the matching origin, or empty if the request origin is not allowed.
SV resolve_origin(
	PreparedCorsOptions const &policy,
	SV request_origin) {
	if (policy.wildcard_origin) {
		// Wildcard: reflect origin when credentials are used, else return "*".
		return policy.opts.allow_credentials ? request_origin : SV{"*"};
	}
	return ranges::contains(policy.opts.allowed_origins, request_origin) ? request_origin : SV{};
}
void inject_cors_headers(
	PreparedCorsOptions const &policy,
	SV request_origin,
	HttpResponse &resp) {
	auto origin = resolve_origin(policy, request_origin);
	if (origin.empty()) {
		return;
	}
	resp.headers["Access-Control-Allow-Origin"] = origin;
	if (policy.opts.allow_credentials) {
		resp.headers["Access-Control-Allow-Credentials"] = "true";
	}
	if (!policy.expose_headers.empty()) {
		resp.headers["Access-Control-Expose-Headers"] = policy.expose_headers;
	}
	resp.append_vary("Origin");
}

} // namespace cors_detail
// Middleware factory: handle CORS preflight and inject CORS headers.
// Register this before other middleware so OPTIONS preflights short-circuit
// before route matching attempts (first-registered = outermost wrapper).
export Router::Middleware cors_middleware(
	CorsOptions opts = {}) {
	auto policy = cors_detail::PreparedCorsOptions{move(opts)};
	return [policy = move(policy)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto request_origin = req.headers["origin"];

		// Preflight: OPTIONS + Origin + Access-Control-Request-Method
		if (req.method == "OPTIONS"
			&& !request_origin.empty()
			&& !req.headers["access-control-request-method"].empty()) {
			HttpResponse preflight{.status = kHttpNoContent, .status_text = "No Content", .content_type = "text/plain"};
			auto origin = cors_detail::resolve_origin(policy, request_origin);
			if (!origin.empty()) {
				preflight.headers["Access-Control-Allow-Origin"] = origin;
				preflight.headers["Access-Control-Allow-Methods"] = policy.allowed_methods;
				preflight.headers["Access-Control-Allow-Headers"] = policy.allowed_headers;
				preflight.headers["Access-Control-Max-Age"] = policy.max_age;
				if (policy.opts.allow_credentials) {
					preflight.headers["Access-Control-Allow-Credentials"] = "true";
				}
				preflight.headers["Vary"] = "Origin";
			}
			return preflight;
		}

		auto resp = next(req);
		if (!request_origin.empty()) {
			cors_detail::inject_cors_headers(policy, request_origin, resp);
		}
		return resp;
	};
}
