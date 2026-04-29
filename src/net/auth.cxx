export module conflux.net.auth;
import std;
import conflux.types;
import conflux.crypto;
import conflux.net.router;
import conflux.utils;

namespace auth_detail {

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

Opt<SV> credentials_for_scheme(
	SV auth,
	SV scheme) noexcept {
	if (auth.size() <= scheme.size() || auth[scheme.size()] != ' ') {
		return std::nullopt;
	}
	if (!ascii_iequals(auth.substr(0, scheme.size()), scheme)) {
		return std::nullopt;
	}
	return auth.substr(scheme.size() + 1);
}

HttpResponse unauthorized(
	SV www_auth) {
	HttpResponse r;
	r.status = kHttpUnauthorized;
	r.status_text = "Unauthorized";
	r.content_type = "text/plain; charset=utf-8";
	r.set_text_body("Unauthorized");
	r.headers["WWW-Authenticate"] = S{www_auth};
	return r;
}

} // namespace auth_detail

// Middleware factory: HTTP Basic Authentication guard.
// validator(username, password) → true = allow, false = 401.
export template<typename Validator>
Router::Middleware basic_auth_middleware(
	Validator &&validator,
	S realm = "Restricted") {
	return [v = std::decay_t<Validator>(forward<Validator>(validator)),
			realm = move(realm)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto auth = req.headers["authorization"];
		auto credentials = auth_detail::credentials_for_scheme(auth, "Basic");
		if (!credentials) {
			return auth_detail::unauthorized(format("Basic realm=\"{}\"", realm));
		}
		auto decoded = base64_decode(*credentials);
		auto colon = decoded.find(':');
		if (colon == S::npos) {
			return auth_detail::unauthorized(format("Basic realm=\"{}\"", realm));
		}
		SV const sv{decoded};
		SV const user = sv.substr(0, colon);
		SV const pass = sv.substr(colon + 1);
		if (!v(user, pass)) {
			return auth_detail::unauthorized(format("Basic realm=\"{}\"", realm));
		}
		return next(req);
	};
}

// Middleware factory: Bearer token Authentication guard.
// validator(token) → true = allow, false = 401.
export template<typename Validator>
Router::Middleware bearer_auth_middleware(
	Validator &&validator) {
	return [v = std::decay_t<Validator>(forward<Validator>(
				validator))](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto auth = req.headers["authorization"];
		auto credentials = auth_detail::credentials_for_scheme(auth, "Bearer");
		if (!credentials) {
			return auth_detail::unauthorized("Bearer");
		}
		auto token = trim(*credentials);
		if (!v(token)) {
			return auth_detail::unauthorized("Bearer");
		}
		return next(req);
	};
}
