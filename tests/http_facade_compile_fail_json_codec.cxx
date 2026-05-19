// Intentionally invalid: Json<T> responses require a codec or JsonMembers<T>.
import conflux.http;

namespace http = conflux::http;

struct NotSerializable {
	int value{};
};

void invalid_json_response_handler() {
	auto app = http::app();
	app.get("/bad", [] { return http::Json{NotSerializable{.value = 7}}; });
}
