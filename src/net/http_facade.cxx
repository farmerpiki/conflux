export module conflux.http;

export import :problem;
import std;

export import conflux.net.config;
export import conflux.net.http.types;
export import conflux.net.http.server_types;
export import conflux.net.http.response;
export import conflux.net.http.request;
export import conflux.net.http.client;
export import conflux.net.http.app_json;
export import conflux.net.http.native_json;
export import conflux.json;
export import conflux.net.router;
export import conflux.net.app;
export import conflux.net.auth;
export import conflux.net.cache_control;
export import conflux.net.compress;
export import conflux.net.rate_limit;
export import conflux.net.request_id;
export import conflux.net.security;
export import conflux.net.tracing;
import conflux.work;
#if CONFLUX_HAS_METRICS
export import conflux.net.metrics;
#endif

export namespace conflux::http {

using Router = ::Router;
using Config = ::Config;
using SseChannel = ::SseChannel;
using WsConn = ::WsConn;
template<class T>
using Task = conflux::work::Task<T>;
using Next = Router::Handler;
using AsyncNext = Router::AsyncNext;

template<typename F>
	requires(std::invocable<F &> && std::same_as<std::invoke_result_t<F &>, Response>)
[[nodiscard]] Response offload(
	std::shared_ptr<WorkPool> const &pool,
	F &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	return defer(pool, std::forward<F>(fn), timeout);
}

template<typename F>
	requires(std::invocable<F &> && std::same_as<std::invoke_result_t<F &>, Response>)
[[nodiscard]] Response offload(
	WorkPool &pool,
	F &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	return defer(pool, std::forward<F>(fn), timeout);
}

[[nodiscard]] Response text(
	std::string_view body) {
	return Response::text(std::string{body});
}

[[nodiscard]] Response html(
	std::string_view body) {
	return Response::html(std::string{body});
}

[[nodiscard]] Response json_response(
	std::string_view body) {
	return Response::json(std::string{body});
}

[[nodiscard]] Response no_content() {
	return Response::no_content();
}

[[nodiscard]] Response redirect(
	std::string_view location,
	int status = kHttpFound) {
	return Response::redirect(location, status);
}

[[nodiscard]] Response file(
	std::filesystem::path const &path,
	std::string content_type = "application/octet-stream") {
	std::ifstream input{path, std::ios::binary};
	if (!input) {
		return Response::not_found(path.string());
	}
	std::ostringstream body;
	body << input.rdbuf();
	return Response::with_body(std::move(body).str(), std::move(content_type));
}

struct StreamSink {
	std::string body;

	void write(
		std::string_view chunk) {
		body.append(chunk);
	}
};

template<class F>
[[nodiscard]] Response stream(
	F &&writer,
	std::string content_type = "application/octet-stream") {
	StreamSink sink;
	std::invoke(std::forward<F>(writer), sink);
	return Response::with_body(std::move(sink.body), std::move(content_type));
}

[[nodiscard]] Response created(
	std::string_view body,
	std::string_view content_type = "text/plain; charset=utf-8") {
	return Response::with_body(std::string{body}, std::string{content_type}, kHttpCreated);
}

template<class T>
[[nodiscard]] Json<T> ok(
	T value) {
	return Json<T>{std::move(value)};
}

template<class T>
[[nodiscard]] Json<T> json(
	T value) {
	return Json<T>{std::move(value)};
}

template<class T>
[[nodiscard]] Created created(
	Json<T> const &body) {
	return Created{.response = codec::json::response_or_internal_error(body.value, kHttpCreated, "Created")};
}

template<class T>
[[nodiscard]] Created created(
	T const &value)
	requires conflux::json::boundary::JsonWritableProvider<
		codec::json::DefaultJsonProvider,
		std::remove_cvref_t<T>,
		codec::json::detail::ResponseBodySink &>
{
	return Created{.response = codec::json::response_or_internal_error(value, kHttpCreated, "Created")};
}

} // namespace conflux::http

export namespace conflux::http::codec::json {

[[nodiscard]] inline AppJsonRoutes<DefaultJsonProvider> routes(
	App &app) {
	return AppJsonRoutes<DefaultJsonProvider>{app};
}

} // namespace conflux::http::codec::json
