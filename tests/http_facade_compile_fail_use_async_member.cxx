// Intentionally invalid: curated App has one middleware registration path; use app.use(...).
#include <coroutine>

import conflux.http;
import conflux.work;

namespace http = conflux::http;

void invalid_use_async_member() {
	auto app = http::app();
	app.use_async(
		[](http::RequestView const &,
		   http::RequestContext const &,
		   auto const &next) -> conflux::work::Task<http::Response> { co_return http::text("bad"); });
}
