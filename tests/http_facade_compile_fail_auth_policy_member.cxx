// Intentionally invalid: bearer-token presence must use the explicit route API.
import conflux.http;

namespace http = conflux::http;

void invalid_auth_policy_member() {
	auto app = http::app();
	app.get("/private", [] { return http::no_content(); }).auth_policy("user");
}
