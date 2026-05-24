// Compile-fail guard: OpenAPI handler generation is extended API, not a curated App member.
import conflux.http;

int main() {
	auto app = conflux::http::app();
	(void)app.openapi_handler("API", "1.0.0");
}
