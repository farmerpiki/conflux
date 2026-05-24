// Intentionally invalid: raw route metadata is extended HTTP API.
#include <conflux/http.hxx>

int main() {
	auto app = conflux::http::app();
	(void)app.route_infos();
}
