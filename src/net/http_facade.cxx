export module conflux.http;

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
[[nodiscard]] Created created(
	Json<T> const &body) {
	return Created{.response = json::response_or_internal_error(body.value, kHttpCreated, "Created")};
}

template<class T>
[[nodiscard]] Created created(
	T const &value)
	requires conflux::json::boundary::
		JsonWritableProvider<json::DefaultJsonProvider, std::remove_cvref_t<T>, json::detail::ResponseBodySink &>
{
	return Created{.response = json::response_or_internal_error(value, kHttpCreated, "Created")};
}

namespace problem {

[[nodiscard]] std::string json_escape(
	std::string_view value) {
	std::string out;
	out.reserve(value.size());
	for (char ch: value) {
		switch (ch) {
		case '"' : out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default  : out += ch; break;
		}
	}
	return out;
}

[[nodiscard]] Problem make(
	int status,
	std::string_view status_text,
	std::string_view code,
	std::string_view detail) {
	auto body = std::format(R"({{"code":"{}","detail":"{}"}})", json_escape(code), json_escape(detail));
	return Problem{
		.response = Response::json(std::move(body), status, std::string{status_text}),
		.code = std::string{code},
		.detail = std::string{detail}};
}

[[nodiscard]] Problem bad_request(
	std::string_view detail = {}) {
	return Problem{.response = Response::bad_request(detail), .detail = std::string{detail}};
}

[[nodiscard]] Problem bad_request(
	std::string_view code,
	std::string_view detail) {
	return make(kHttpBadRequest, "Bad Request", code, detail);
}

[[nodiscard]] Problem not_found(
	std::string_view detail = {}) {
	return Problem{
		.response = detail.empty() ? Response::not_found({}) : Response::not_found(detail),
		.detail = std::string{detail}};
}

[[nodiscard]] Problem not_found(
	std::string_view code,
	std::string_view detail) {
	return make(kHttpNotFound, "Not Found", code, detail);
}

} // namespace problem

} // namespace conflux::http

export namespace conflux::http::json {

[[nodiscard]] inline AppJsonRoutes<DefaultJsonProvider> routes(
	App &app) {
	return AppJsonRoutes<DefaultJsonProvider>{app};
}

} // namespace conflux::http::json
