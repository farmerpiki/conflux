export module conflux.http;

export import :problem;
export import conflux.net.app;
export import conflux.net.app.defer;
export import conflux.net.http.server_types;
import conflux.net.http.response;
export import conflux.net.http.request;
export import conflux.net.http.realtime;
export import conflux.json;
import conflux.net.http.native_json;
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

using Response = ::Response;

template<class T>
using Task = conflux::work::Task<T>;
template<class T>
using Result = std::expected<T, Problem>;
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

[[nodiscard]] ::Router::Middleware tracing(
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

[[nodiscard]] Response text(
	char const *body) {
	return text(std::string_view{body});
}

[[nodiscard]] Response text(
	std::string body) {
	return Response::text(std::move(body));
}

[[nodiscard]] Response owned_text(
	std::string body) {
	return text(std::move(body));
}

[[nodiscard]] Response html(
	std::string_view body) {
	return Response::html(std::string{body});
}

[[nodiscard]] Response html(
	char const *body) {
	return html(std::string_view{body});
}

[[nodiscard]] Response html(
	std::string body) {
	return Response::html(std::move(body));
}

[[nodiscard]] Response owned_html(
	std::string body) {
	return html(std::move(body));
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
	std::shared_ptr<conflux::http::SseChannel> channel) {
	return Response::sse(std::move(channel));
}

[[nodiscard]] CookieBuilder cookie(
	std::string_view name,
	std::string_view value) {
	return CookieBuilder{name, value};
}

struct StreamSink {
	std::string body;

	void write(
		std::string_view chunk) {
		body.append(chunk);
	}
};

template<class F>
[[nodiscard]] Response buffered_stream(
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

[[nodiscard]] Response created(
	char const *body,
	std::string_view content_type = "text/plain; charset=utf-8") {
	return created(std::string_view{body}, content_type);
}

[[nodiscard]] Response created(
	std::string body,
	std::string content_type = "text/plain; charset=utf-8") {
	return Response::with_body(std::move(body), std::move(content_type), kHttpCreated);
}

[[nodiscard]] Response owned_created(
	std::string body,
	std::string content_type = "text/plain; charset=utf-8") {
	return created(std::move(body), std::move(content_type));
}

template<class T>
[[nodiscard]] Json<T> json(
	T value) {
	return Json<T>{std::move(value)};
}

template<class T>
[[nodiscard]] CreatedBody<T> created(
	Json<T> const &body) {
	return CreatedBody<T>{codec::json::response_or_internal_error(body.value, kHttpCreated, "Created")};
}

template<class T>
[[nodiscard]] CreatedBody<T> created(
	T const &value)
	requires conflux::json::boundary::JsonWritableProvider<
		codec::json::DefaultJsonProvider,
		std::remove_cvref_t<T>,
		codec::json::detail::ResponseBodySink &>
{
	return CreatedBody<T>{codec::json::response_or_internal_error(value, kHttpCreated, "Created")};
}

} // namespace conflux::http

export namespace conflux::http::codec::json {

[[nodiscard]] inline AppJsonRoutes<DefaultJsonProvider> routes(
	App &app) {
	return AppJsonRoutes<DefaultJsonProvider>{app};
}

} // namespace conflux::http::codec::json
