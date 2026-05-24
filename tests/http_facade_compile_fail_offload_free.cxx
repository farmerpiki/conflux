// Intentionally invalid: offload helpers are extended HTTP API.
import std;
import conflux.http;

int main() {
	auto pool = std::make_shared<conflux::work::WorkPool>();
	(void)http::offload(pool, [] { return http::text("ok"); });
	return 0;
}
