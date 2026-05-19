export module conflux.net.http.response_json;

import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
export import conflux.net.http.response;
export import conflux.net.http.json;

export namespace conflux::http::json {

struct ResponseOptions {
	int status{kHttpOk};
	std::string_view status_text{"OK"};
	conflux::json::boundary::DumpOptions dump{};
};

namespace detail {

struct ResponseBodySink {
	std::string *body{};

	void operator ()(
		std::string_view chunk) const {
		body->append(chunk);
	}
};

} // namespace detail

template<class Provider, class T>
[[nodiscard]] std::expected<HttpResponse, conflux::json::boundary::Error> try_response_with(
	T const &value,
	ResponseOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<Provider, std::remove_cvref_t<T>, detail::ResponseBodySink &>
{
	std::string body;
	auto sink = detail::ResponseBodySink{.body = &body};
	auto written = conflux::json::boundary::write_with<Provider>(value, sink, opts.dump);
	if (!written) {
		return std::unexpected(written.error());
	}
	return HttpResponse::json(std::move(body), opts.status, std::string{opts.status_text});
}

template<class Provider, class T>
[[nodiscard]] std::expected<HttpResponse, conflux::json::boundary::Error> try_response_with(
	T const &value,
	int status,
	std::string_view status_text,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<Provider, std::remove_cvref_t<T>, detail::ResponseBodySink &>
{
	return try_response_with<Provider>(
		value,
		ResponseOptions{.status = status, .status_text = status_text, .dump = opts});
}

[[nodiscard]] inline HttpResponse serialization_error_response() {
	return HttpResponse::json(
		R"({"error":"json serialization failed"})",
		kHttpInternalServerError,
		"Internal Server Error");
}

template<class Provider, class T>
[[nodiscard]] HttpResponse response_or_internal_error_with(
	T const &value,
	ResponseOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<Provider, std::remove_cvref_t<T>, detail::ResponseBodySink &>
{
	auto resp = try_response_with<Provider>(value, opts);
	if (resp) {
		return std::move(*resp);
	}
	return serialization_error_response();
}

template<class Provider, class T>
[[nodiscard]] HttpResponse response_or_internal_error_with(
	T const &value,
	int status,
	std::string_view status_text,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<Provider, std::remove_cvref_t<T>, detail::ResponseBodySink &>
{
	return response_or_internal_error_with<Provider>(
		value,
		ResponseOptions{.status = status, .status_text = status_text, .dump = opts});
}

// Pre-release compatibility aliases. New boundary-first code should call the
// *_with form so provider selection is explicit at the HTTP layer.
template<class Provider, class T>
[[nodiscard]] std::expected<HttpResponse, conflux::json::boundary::Error> try_response(
	T const &value,
	ResponseOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<Provider, std::remove_cvref_t<T>, detail::ResponseBodySink &>
{
	return try_response_with<Provider>(value, opts);
}

template<class Provider, class T>
[[nodiscard]] std::expected<HttpResponse, conflux::json::boundary::Error> try_response(
	T const &value,
	int status,
	std::string_view status_text,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<Provider, std::remove_cvref_t<T>, detail::ResponseBodySink &>
{
	return try_response_with<Provider>(value, status, status_text, opts);
}

template<class Provider, class T>
[[nodiscard]] HttpResponse response_or_internal_error(
	T const &value,
	ResponseOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<Provider, std::remove_cvref_t<T>, detail::ResponseBodySink &>
{
	return response_or_internal_error_with<Provider>(value, opts);
}

template<class Provider, class T>
[[nodiscard]] HttpResponse response_or_internal_error(
	T const &value,
	int status,
	std::string_view status_text,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<Provider, std::remove_cvref_t<T>, detail::ResponseBodySink &>
{
	return response_or_internal_error_with<Provider>(value, status, status_text, opts);
}

} // namespace conflux::http::json
