// Intentionally invalid: offload helpers are extended HTTP API.
#include <conflux/http.hxx>
#include <memory>

int main() {
	auto pool = std::make_shared<conflux::work::WorkPool>();
	(void)conflux::http::offload(pool, [] { return conflux::http::text("ok"); });
	return 0;
}
