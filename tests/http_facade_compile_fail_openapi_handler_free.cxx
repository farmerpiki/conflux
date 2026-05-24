// Intentionally invalid: openapi_handler is extended HTTP API.
import std;
import conflux.http;

int main() {
	auto app = http::app();
	(void)http::openapi_handler(app, "API", "1.0.0");
}
