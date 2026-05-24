// Intentionally invalid: file helper is extended HTTP API.
#include <conflux/http.hxx>

int main() {
	(void)conflux::http::file("index.html");
}
