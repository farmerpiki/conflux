// Compile-only API snapshot for the extended HTTP facade.
import std;
import conflux.http.extended;

namespace http_extended_snapshot {

namespace http = conflux::http;

void offload_spelling_compiles(
	std::shared_ptr<WorkPool> pool) {
	(void)http::offload(pool, [] { return http::text("ok"); });
}

void offload_ref_spelling_compiles(
	WorkPool &pool) {
	(void)http::offload(pool, [] { return http::text("ok"); });
}

} // namespace http_extended_snapshot
