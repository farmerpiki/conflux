// Intentionally invalid: async extracted handlers must not take a borrowed RequestView.
#include <coroutine>

import conflux.http;
import conflux.work;
import std;

namespace http = conflux::http;

void invalid_async_borrowed_request_extracted_handler() {
	auto app = http::app();
	std::string state = "state";
	app.state(state);
	app.get("/bad", [](http::RequestView const &, http::State<std::string>) -> http::Task<http::Response> {
		co_return http::text("bad");
	});
}
