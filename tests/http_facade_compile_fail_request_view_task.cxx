// Intentionally invalid: async handlers must not take a borrowed RequestView.
#include <coroutine>

import conflux.http;
import conflux.work;

namespace http = conflux::http;

void invalid_async_borrowed_request_handler() {
	auto app = http::app();
	app.get("/bad", [](http::RequestView const &) -> http::Task<http::Response> { co_return http::text("bad"); });
}
