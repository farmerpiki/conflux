// Intentionally invalid: handlers must wrap string responses explicitly.
import std;
import conflux.http;

namespace http = conflux::http;

void invalid_raw_string_handler() {
	auto app = http::app();
	app.get("/bad", [] { return std::string{"bad"}; });
}
