export module conflux.http;

export import :problem;
export import conflux.net.app;
import conflux.net.http.server_types;
export import conflux.net.http.response;
export import conflux.net.http.request;
export import conflux.json;
export import conflux.net.http.native_json;
import std;

import conflux.net.config;
export import conflux.net.http.app_json;
import conflux.net.router;
import conflux.net.request_id;
import conflux.net.security;
import conflux.net.tracing;
import conflux.net.observability;
import conflux.types;
import conflux.work;
#if CONFLUX_HAS_METRICS
import conflux.net.metrics;
#endif

export namespace conflux::http {

using Config = ::Config;
using SseChannel = ::SseChannel;
using WsConn = ::WsConn;
using ::Request;
using ::RequestView;
using ::RunStatus;
using RequestContext = ::RequestContext;
template<class T>
using Task = conflux::work::Task<T>;
using Next = ::Router::Handler;
using AsyncNext = ::Router::AsyncNext;
[[nodiscard]] ::Router::Middleware request_id(
	RequestIdOptions opts = {}) {
	return request_id_middleware(std::move(opts));
}

[[nodiscard]] ::Router::Middleware trace_context(
	TracingOptions opts = {}) {
	return tracing_middleware(std::move(opts));
}

[[nodiscard]] ::Router::Middleware security_headers(
	SecurityOptions opts = {}) {
	return security_headers_middleware(std::move(opts));
}

[[nodiscard]] Response text(
	std::string_view body) {
	return Response::text(std::string{body});
}

[[nodiscard]] Response html(
	std::string_view body) {
	return Response::html(std::string{body});
}

[[nodiscard]] Response no_content() {
	return Response::no_content();
}

[[nodiscard]] Response redirect(
	std::string_view location,
	int status = kHttpFound) {
	return Response::redirect(location, status);
}

[[nodiscard]] Response sse(
	std::shared_ptr<SseChannel> channel) {
	return Response::sse(std::move(channel));
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
