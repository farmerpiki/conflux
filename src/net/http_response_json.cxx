export module conflux.net.http.response_json;

import std;
import conflux.types;
import conflux.net.http.types;
export import conflux.net.http.response;
export import conflux.net.http.json;

export namespace conflux::http::json {

struct ResponseOptions {
	int status{kHttpOk};
	SV status_text{"OK"};
	conflux::json::boundary::DumpOptions dump{};
};

namespace detail {

struct ResponseBodySink {
	S *body{};

	void operator()(
		SV chunk) const {
		body->append(chunk);
	}
};

} // namespace detail

template<class Provider = DefaultJsonProvider, class T>
[[nodiscard]] expected<HttpResponse, conflux::json::boundary::Error> try_response(
	T const &value,
	ResponseOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<
		Provider,
		std::remove_cvref_t<T>,
		detail::ResponseBodySink &>
{
	S body;
	auto sink = detail::ResponseBodySink{.body = &body};
	auto written = conflux::json::boundary::write_with<Provider>(value, sink, opts.dump);
	if (!written) {
		return unexpected(written.error());
	}
	return HttpResponse::json(move(body), opts.status, S{opts.status_text});
}

template<class Provider = DefaultJsonProvider, class T>
[[nodiscard]] expected<HttpResponse, conflux::json::boundary::Error> try_response(
	T const &value,
	int status,
	SV status_text,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<
		Provider,
		std::remove_cvref_t<T>,
		detail::ResponseBodySink &>
{
	return try_response<Provider>(value, ResponseOptions{.status = status, .status_text = status_text, .dump = opts});
}

[[nodiscard]] inline HttpResponse serialization_error_response() {
	return HttpResponse::json(R"({"error":"json serialization failed"})", kHttpInternalServerError, "Internal Server Error");
}

template<class Provider = DefaultJsonProvider, class T>
[[nodiscard]] HttpResponse response_or_internal_error(
	T const &value,
	ResponseOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<
		Provider,
		std::remove_cvref_t<T>,
		detail::ResponseBodySink &>
{
	auto resp = try_response<Provider>(value, opts);
	if (resp) {
		return move(*resp);
	}
	return serialization_error_response();
}

template<class Provider = DefaultJsonProvider, class T>
[[nodiscard]] HttpResponse response_or_internal_error(
	T const &value,
	int status,
	SV status_text,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<
		Provider,
		std::remove_cvref_t<T>,
		detail::ResponseBodySink &>
{
	return response_or_internal_error<Provider>(
		value,
		ResponseOptions{.status = status, .status_text = status_text, .dump = opts});
}

} // namespace conflux::http::json
