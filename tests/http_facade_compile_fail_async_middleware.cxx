// Intentionally invalid: async middleware must not borrow a request view across suspension.
#include <coroutine>

import conflux.http;
import conflux.work;

namespace http = conflux::http;

void invalid_async_middleware() {
	auto app = http::app();
	app.use_async(
		[](http::RequestView const &, RequestContext const &, http::AsyncNext const &) -> http::Task<http::Response> {
			co_return http::text("bad");
		});
}
