// OpenAPI 3.0 spec generator.
// Introspects Router routes to produce a minimal JSON spec.
// Only routes registered via Router::add/get/post/... are included.
// SSE and WebSocket upgrade routes are included with their registered methods.
export module conflux.net.openapi;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.app.openapi;
// Generate an OpenAPI 3.0 JSON spec from the routes registered on `router`.
// title and version are used for the info object.
// Returns a JSON string (not pretty-printed).
export std::string openapi_spec(
	Router const &router,
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
