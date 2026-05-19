// Intentionally invalid: async middleware is not normalized yet.
#include <coroutine>

import conflux.http;
import conflux.work;

namespace http = conflux::http;

void invalid_async_middleware() {
	auto app = http::app();
	app.use(
		[](http::Request const &, http::Next const &) -> http::Task<http::Response> { co_return http::text("bad"); });
}
