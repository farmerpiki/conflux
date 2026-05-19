// Intentionally invalid: extracted handlers must wrap string responses explicitly.
import std;
import conflux.http;

namespace http = conflux::http;

struct RawStringState {};

void invalid_raw_string_extractor_handler() {
	auto app = http::app();
	app.state(std::make_shared<RawStringState>());
	app.get("/bad", [](http::State<RawStringState>) { return std::string{"bad"}; });
}
