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
#if CONFLUX_HAS_JSON
import conflux.json;
#endif
// Generate an OpenAPI 3.0 JSON spec from the routes registered on `router`.
// title and version are used for the info object.
// Returns a JSON string (not pretty-printed).
export std::string openapi_spec(
	Router const &router,
	std::string_view title = "API",
	std::string_view version = "1.0.0") {
	auto infos = router.route_infos();

	// Group infos by path pattern (preserve insertion order).
	std::vector<std::string> path_order;
	std::unordered_map<std::string, std::vector<RouteInfo>> by_path;

	for (auto const &info: infos) {
		auto [it, inserted] = by_path.try_emplace(info.path_pattern);
		if (inserted) {
			path_order.push_back(info.path_pattern);
		}
		it->second.push_back(info);
	}

	auto json_str = [](std::string_view value) -> std::string {
#if CONFLUX_HAS_JSON
		auto dumped = dump_direct(value);
		if (dumped) {
			return std::move(*dumped);
		}
#endif
		return json_string_fallback(value);
	};

	std::string out;
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
			std::string method_lower = ascii_lower(info.method);

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
				out += R"(,"in":"path","required":true,"schema":{"type":"string"}})";
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
	std::string_view title = "API",
	std::string_view version = "1.0.0") {
	auto spec = openapi_spec(router, title, version);
	return [spec = std::move(spec)](RequestView const &) -> Response { return Response::json(spec); };
}
// Route handler wrapped with the supplied middleware chain (e.g. bearer_auth).
// Each middleware is applied in order: chain[0] runs first, chain.back() last.
export Router::Handler openapi_handler_protected(
	Router const &router,
	std::string_view title,
	std::string_view version,
	std::vector<Router::Middleware> chain) {
	Router::Handler current = openapi_handler(router, title, version);
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		Router::Middleware mw = std::move(*it);
		Router::Handler next = std::move(current);
		current = [mw = std::move(mw), next = std::move(next)](RequestView const &req) -> Response {
			return mw(req, next);
		};
	}
	return current;
}
