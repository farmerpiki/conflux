// Intentionally invalid: the explicit async-middleware helper is extended-only.
#include <coroutine>

import conflux.http;
import conflux.work;

namespace http = conflux::http;

void invalid_use_async_helper() {
	auto app = http::app();
	http::use_async(
		app,
		[](http::RequestView const &,
		   http::RequestContext const &,
		   auto const &) -> conflux::work::Task<http::Response> { co_return http::text("bad"); });
}
