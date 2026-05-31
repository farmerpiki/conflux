module;
#include <cerrno>
#include <chrono>
#include <memory>

export module conflux.http.extended;

export import conflux.http;
import conflux.work;
import std;
import conflux.net.app.defer;
import conflux.net.router;
import conflux.file_io_sync;
import conflux.types;

export namespace conflux::http {

using Router = ::Router;
using WorkPool = ::WorkPool;
using WorkPoolOptions = ::WorkPoolOptions;
using WorkPoolQueueMode = ::WorkPoolQueueMode;
using WorkPoolQueueStats = ::WorkPoolQueueStats;

template<class F>
concept ViewMiddleware = ::ViewMiddleware<F>;
template<class F>
concept RequestMiddleware = ::RequestMiddleware<F>;
template<class F>
concept AsyncMiddleware = ::AsyncMiddleware<F>;
template<class F>
concept Middleware = ::Middleware<F>;

[[nodiscard]] Response blocking_file_response(
	std::filesystem::path const &path,
	std::string content_type = "application/octet-stream") {
	auto path_string = path.string();
	auto body = blocking_read_text_file(path_string, std::numeric_limits<std::size_t>::max());
	if (!body) {
		auto const err = errnum(body);
		if (err == ENOENT || err == ENOTDIR) {
			return Response::not_found(path.string());
		}
		return Response::internal_error("failed to read file");
	}
	return Response::with_body(std::move(*body), std::move(content_type));
}

[[nodiscard]] Router::Handler openapi_handler(
	App const &app,
	std::string_view title = "API",
	std::string_view version = "1.0.0") {
	auto spec = app.openapi_spec(title, version);
	return [spec = std::move(spec)](RequestView const &) -> Response { return Response::json(spec); };
}

template<typename F>
App &use_async(
	App &app,
	F &&middleware) {
	return app.use(std::forward<F>(middleware));
}

template<typename F>
	requires(std::invocable<F &> && IntoResponse<std::invoke_result_t<F &>>)
[[nodiscard]] Response offload(
	std::shared_ptr<WorkPool> const &pool,
	F &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	return defer(pool, std::forward<F>(fn), timeout);
}

template<typename F>
	requires(std::invocable<F &> && IntoResponse<std::invoke_result_t<F &>>)
[[nodiscard]] Response offload(
	WorkPool &pool,
	F &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	return defer(pool, std::forward<F>(fn), timeout);
}

} // namespace conflux::http
