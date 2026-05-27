// Intentionally invalid: blocking file helper is not part of curated header surface.
#include <conflux/curated.hxx>

int main() {
	namespace http = conflux::http;
	static_assert(requires { http::file("index.html"); }, "conflux_api_surface_curated_unexpected_file_helper_visible");
}
