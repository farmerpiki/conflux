// OpenAPI 3.0 spec generator.
// Introspects Router routes to produce a minimal JSON spec.
// Only routes registered via Router::add/get/post/... are included.
// SSE and WebSocket upgrade routes are included with their registered methods.
export module conflux.net.openapi;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;
import conflux.net.app.openapi;

export namespace conflux::http {

// Generate an OpenAPI 3.0 JSON spec from the routes registered on `router`.
// title and version are used for the info object.
// Returns a JSON string (not pretty-printed).
std::string openapi_spec(
	conflux::http::Router const &router,
	std::string_view title = "API",
	std::string_view version = "1.0.0") {
	auto infos = router.route_infos();
	std::vector<conflux::http::detail::AppOpenApiRoute> routes;
	routes.reserve(infos.size());
	for (auto const &info: infos) {
		routes.push_back(
			conflux::http::detail::AppOpenApiRoute{
				.method = info.method,
				.path = info.path_pattern,
				.path_params = info.path_params,
				.success_status = kHttpOk});
	}
	return conflux::http::detail::render_openapi_spec(routes, title, version);
}
// Route handler: serve the OpenAPI spec as JSON.
// Usage: router.get("/openapi.json", openapi_handler(router, "My API", "1.0"));
// The spec is snapshotted at factory time so it survives moving the Router
// into the HttpServer. Routes added later are not reflected.
// WARNING: the plain handler is unauthenticated — avoid on public listeners;
// prefer openapi_handler_protected or a network-level ACL.
conflux::http::Router::Handler openapi_handler(
	conflux::http::Router const &router,
	std::string_view title = "API",
	std::string_view version = "1.0.0") {
	auto spec = openapi_spec(router, title, version);
	return [spec = std::move(spec)](conflux::http::RequestView const &) -> conflux::http::Response {
		return conflux::http::Response::json(spec);
	};
}
// Route handler wrapped with the supplied middleware chain (e.g. bearer_auth).
// Each middleware is applied in order: chain[0] runs first, chain.back() last.
conflux::http::Router::Handler openapi_handler_protected(
	conflux::http::Router const &router,
	std::string_view title,
	std::string_view version,
	std::vector<conflux::http::Router::Middleware> chain) {
	conflux::http::Router::Handler current = openapi_handler(router, title, version);
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		conflux::http::Router::Middleware mw = std::move(*it);
		conflux::http::Router::Handler next = std::move(current);
		current = [mw = std::move(mw), next = std::move(next)](conflux::http::RequestView const &req) -> conflux::http::Response {
			return mw(req, next);
		};
	}
	return current;
}

} // namespace conflux::http
