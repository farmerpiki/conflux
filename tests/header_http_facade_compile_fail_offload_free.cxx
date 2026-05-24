// Intentionally invalid: offload helpers are extended HTTP API.
#include <conflux/http.hxx>

int main() {
	(void)conflux::http::offload(nullptr, [] { return conflux::http::text("ok"); });
}
