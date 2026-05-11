// OpenAPI 3.0 spec generator.
// Introspects Router routes to produce a minimal JSON spec.
// Only routes registered via Router::add/get/post/... are included.
// SSE and WebSocket upgrade routes are included with their registered methods.
export module conflux.net.openapi;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;
// Generate an OpenAPI 3.0 JSON spec from the routes registered on `router`.
// title and version are used for the info object.
// Returns a JSON S (not pretty-printed).
export S openapi_spec(
	Router const &router,
	SV title = "API",
	SV version = "1.0.0") {
	auto infos = router.route_infos();

	// Group infos by path pattern (preserve insertion order).
	V<S> path_order;
	UM<S, V<RouteInfo>> by_path;

	for (auto const &info: infos) {
		auto [it, inserted] = by_path.try_emplace(info.path_pattern);
		if (inserted) {
			path_order.push_back(info.path_pattern);
		}
		it->second.push_back(info);
	}

	// Build JSON S manually (no deps).
	auto json_str = [](SV s) -> S {
		S out = "\"";
		for (auto const byte: s) {
			auto const c = static_cast<unsigned char>(byte);
			if (c == '"') {
				out += "\\\"";
			} else if (c == '\\') {
				out += "\\\\";
			} else if (c == '\n') {
				out += "\\n";
			} else if (c == '\r') {
				out += "\\r";
			} else if (c == '\t') {
				out += "\\t";
			} else if (c == '\b') {
				out += "\\b";
			} else if (c == '\f') {
				out += "\\f";
			} else if (c < 0x20) {
				out += format("\\u{:04x}", c);
			} else {
				out += static_cast<char>(c);
			}
		}
		out += '"';
		return out;
	};

	S out;
	out += R"({"openapi":"3.0.0","info":{"title":)";
	out += json_str(title);
	out += R"(,"version":)";
	out += json_str(version);
	out += R"(},"paths":{)";

	bool first_path = true;
	for (auto const &path: path_order) {
		auto const &route_list = by_path.at(path);
		if (!first_path) {
			out += ',';
		}
		first_path = false;

		out += json_str(path);
		out += ":{";

		bool first_method = true;
		for (auto const &info: route_list) {
			if (!first_method) {
				out += ',';
			}
			first_method = false;

			// method key must be lowercase.
			S method_lower = ascii_lower(info.method);

			out += json_str(method_lower);
			out += R"(:{"parameters":[)";

			bool first_param = true;
			for (auto const &param: info.path_params) {
				if (!first_param) {
					out += ',';
				}
				first_param = false;
				out += R"({"name":)";
				out += json_str(param);
				out += R"(,"in":"path","required":true,"schema":{"type":"S"}})";
			}
			out += R"(],"responses":{"200":{"description":"OK"}}})";
		}
		out += '}';
	}
	out += "}}";
	return out;
}
// Route handler: serve the OpenAPI spec as JSON.
// Usage: router.get("/openapi.json", openapi_handler(router, "My API", "1.0"));
// The spec is snapshotted at factory time so it survives moving the Router
// into the HttpServer. Routes added later are not reflected.
// WARNING: the plain handler is unauthenticated — avoid on public listeners;
// prefer openapi_handler_protected or a network-level ACL.
export Router::Handler openapi_handler(
	Router const &router,
	SV title = "API",
	SV version = "1.0.0") {
	auto spec = openapi_spec(router, title, version);
	return [spec = move(spec)](HttpRequestView const &) -> HttpResponse {
		HttpResponse r;
		r.status = 200;
		r.status_text = "OK";
		r.content_type = "application/json";
		r.set_text_body(spec);
		return r;
	};
}
// Route handler wrapped with the supplied middleware chain (e.g. bearer_auth).
// Each middleware is applied in order: chain[0] runs first, chain.back() last.
export Router::Handler openapi_handler_protected(
	Router const &router,
	SV title,
	SV version,
	V<Router::Middleware> chain) {
	Router::Handler current = openapi_handler(router, title, version);
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		Router::Middleware mw = move(*it);
		Router::Handler next = move(current);
		current = [mw = move(mw), next = move(next)](HttpRequestView const &req) -> HttpResponse {
			return mw(req, next);
		};
	}
	return current;
}
