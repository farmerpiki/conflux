// Compile-only API snapshot for the extended HTTP facade.
import std;
import conflux.http.extended;

namespace http_extended_snapshot {

namespace http = conflux::http;

static_assert(
	http::AsyncMiddleware<
		decltype([](http::RequestView const &req, http::RequestContext const &ctx, http::AsyncNext const &next) -> http::Task<http::Response> {
			co_return co_await next(req, ctx);
		})>);

static_assert(
	http::Middleware<decltype([](http::RequestView const &req, http::Next const &next) { return next(req); })>);

void openapi_handler_spelling_compiles() {
	auto app = http::app();
	app.get("/health", [] { return http::text("ok"); }).name("health.check");
	(void)http::openapi_handler(app, "API", "1.0.0");
}

void use_async_spelling_compiles() {
	auto app = http::app();
	http::use_async(
		app,
		[](http::RequestView const &req,
		   http::RequestContext const &ctx,
		   http::AsyncNext const &next) -> http::Task<http::Response> { co_return co_await next(req, ctx); });
}

void router_escape_hatch_spelling_compiles() {
	auto app = http::app();
	app.get("/health", [] { return http::text("ok"); }).name("health.check");
	auto &router = http::router(app);
	static_assert(std::same_as<std::remove_reference_t<decltype(router)>, http::Router>);
	(void)http::route_infos(app);
}

void offload_spelling_compiles(
	std::shared_ptr<http::WorkPool> pool) {
	(void)http::offload(pool, [] { return http::text("ok"); });
}

void offload_ref_spelling_compiles(
	http::WorkPool &pool) {
	(void)http::offload(pool, [] { return http::text("ok"); });
}

} // namespace http_extended_snapshot
