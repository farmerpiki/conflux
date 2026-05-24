// Intentionally invalid: curated App has one middleware registration path; use app.use(...).
#include <coroutine>

import conflux.http;

namespace http = conflux::http;

void invalid_use_async_member() {
	auto app = http::app();
	app.use_async(
		[](http::Request const &,
		   http::RequestContext const &,
		   http::AsyncNext const &next) -> http::Task<http::Response> { co_return http::text("bad"); });
}
