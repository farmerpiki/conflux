export module conflux.net.cors;
import std;
import conflux.types;
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

bool ascii_iequals(
	SV lhs,
	SV rhs) noexcept {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (SZ i = 0; i < lhs.size(); ++i) {
		auto const l = static_cast<unsigned char>(lhs[i]);
		auto const r = static_cast<unsigned char>(rhs[i]);
		if ((l | 0x20U) != (r | 0x20U)) {
			return false;
		}
	}
	return true;
}

bool vary_contains(
	SV vary,
	SV token) noexcept {
	while (!vary.empty()) {
		auto comma = vary.find(',');
		auto part = trim((comma == SV::npos) ? vary : vary.substr(0, comma));
		if (ascii_iequals(part, token)) {
			return true;
		}
		if (comma == SV::npos) {
			break;
		}
		vary.remove_prefix(comma + 1);
	}
	return false;
}

void append_vary(
	HttpResponse &resp,
	SV token) {
	S const current{resp.headers["Vary"]};
	if (current.empty()) {
		resp.headers["Vary"] = S{token};
		return;
	}
	if (trim(current) == "*" || vary_contains(current, token)) {
		return;
	}
	resp.headers["Vary"] = format("{}, {}", current, token);
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
	append_vary(resp, "Origin");
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
