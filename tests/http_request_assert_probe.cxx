/* Standalone binary: triggers ClientRequest::Builder debug body-set assert paths.
 * argv[1] selects the probe:
 * body_after_body — body() followed by body_view() must assert in debug builds
 * json_after_body — body() followed by body_json_raw() must assert in debug builds
 * form_after_body — body() followed by body_form() must assert in debug builds
 */
#include <csignal>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.request;

int main(
	int argc,
	char *argv[]) {
	::signal(SIGABRT, [](int) { ::_exit(42); });
	if (argc < 2) {
		return 1;
	}

	SV probe{argv[1]};
	if (probe == "body_after_body") {
		auto builder = conflux::http::ClientRequest::post("http://example.test/submit");
		builder.body("one").body_view("two");
		return 0;
	}
	if (probe == "json_after_body") {
		auto builder = conflux::http::ClientRequest::post("http://example.test/submit");
		builder.body("one").body_json_raw(S{"{\"two\":true}"});
		return 0;
	}
	if (probe == "form_after_body") {
		HttpFields fields{true};
		fields.emplace_back("two", "true");
		auto builder = conflux::http::ClientRequest::post("http://example.test/submit");
		builder.body("one").body_form(fields);
		return 0;
	}
	return 1;
}
