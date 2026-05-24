// Compile-only API snapshot for the extended HTTP facade.
import std;
import conflux.http.extended;

namespace http_extended_snapshot {

namespace http = conflux::http;

void openapi_handler_spelling_compiles() {
	auto app = http::app();
	app.get("/health", [] { return http::text("ok"); }).name("health.check");
	(void)http::openapi_handler(app, "API", "1.0.0");
}

void offload_spelling_compiles(
	std::shared_ptr<WorkPool> pool) {
	(void)http::offload(pool, [] { return http::text("ok"); });
}

void offload_ref_spelling_compiles(
	WorkPool &pool) {
	(void)http::offload(pool, [] { return http::text("ok"); });
}

} // namespace http_extended_snapshot
