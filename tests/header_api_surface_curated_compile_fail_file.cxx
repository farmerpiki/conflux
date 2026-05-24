// Intentionally invalid: blocking file helper is not part of curated header surface.
#include <conflux/curated.hxx>

int main() {
	(void)conflux::http::file("index.html");
}
