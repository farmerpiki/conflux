module;
#include <cerrno>
#include <chrono>
#include <memory>

export module conflux.http.extended;

export import conflux.http;
import conflux.work;
import std;
import conflux.net.app.defer;
import conflux.file_io_sync;
import conflux.types;
import conflux.net.http.response;

export namespace conflux::http {

using conflux::errnum;
using WorkPool = conflux::work::WorkPool;
using WorkPoolOptions = conflux::work::WorkPoolOptions;
using WorkPoolQueueMode = conflux::work::WorkPoolQueueMode;
using WorkPoolQueueStats = conflux::work::WorkPoolQueueStats;
using Router = std::remove_reference_t<decltype(router(std::declval<App &>()))>;

template<class F>
concept ViewMiddleware = requires(std::decay_t<F> &fn, conflux::http::RequestView const &req, Next const &next) {
	{ std::invoke(fn, req, next) } -> std::same_as<Response>;
};

template<class F>
concept RequestMiddleware = requires(std::decay_t<F> &fn, conflux::http::OwnedRequest const &req, Next const &next) {
	{ std::invoke(fn, req, next) } -> std::same_as<Response>;
};

template<class F>
concept AsyncMiddleware = requires(
	std::decay_t<F> &fn,
	conflux::http::RequestView const &req,
	RequestContext const &ctx,
	AsyncNext const &next) {
	{ std::invoke(fn, req, ctx, next) } -> std::same_as<conflux::work::root::Task<Response>>;
};

template<class F>
concept Middleware = ViewMiddleware<F> || RequestMiddleware<F> || AsyncMiddleware<F>;

[[nodiscard]] Response blocking_file_response(
	std::filesystem::path const &path,
	std::string content_type = "application/octet-stream") {
	auto path_string = path.string();
	auto body = conflux::file_io_sync::blocking_read_text_file(path_string, std::numeric_limits<std::size_t>::max());
	if (!body) {
		auto const err = errnum(body);
		if (err == ENOENT || err == ENOTDIR) {
			return Response::not_found(path.string());
		}
		return Response::internal_error("failed to read file");
	}
	return Response::with_body(std::move(*body), std::move(content_type));
}

[[nodiscard]] Next openapi_handler(
	App const &app,
	std::string_view title = "API",
	std::string_view version = "1.0.0") {
	auto spec = app.openapi_spec(title, version);
	return [spec = std::move(spec)](conflux::http::RequestView const &) -> Response { return Response::json(spec); };
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
