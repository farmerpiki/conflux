// Intentionally invalid: the explicit async-middleware helper is extended-only.
#include <coroutine>

import conflux.http;

namespace http = conflux::http;

void invalid_use_async_helper() {
	auto app = http::app();
	http::use_async(
		app,
		[](http::Request const &, http::RequestContext const &, http::AsyncNext const &) -> http::Task<http::Response> {
			co_return http::text("bad");
		});
}
