export module conflux.net.http.native_json;

import std;
import conflux.types;
export import conflux.net.http.response_json;
export import conflux.json.native_provider;

export namespace conflux::http::json {

using DefaultJsonProvider = conflux::json::boundary::NativeJsonProvider;

template<class T>
inline ClientRequest::Builder &set_body(
	ClientRequest::Builder &b,
	T const &value,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonDumpProvider<DefaultJsonProvider, T>
{
	return set_body_with<DefaultJsonProvider>(b, value, opts);
}

template<class T>
inline ClientRequest::Builder &&set_body(
	ClientRequest::Builder &&b,
	T const &value,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonDumpProvider<DefaultJsonProvider, T>
{
	return move(set_body(b, value, opts));
}

template<class T>
[[nodiscard]] expected<std::remove_cvref_t<T>, conflux::json::boundary::Error> decode_body(
	ClientRequest const &req,
	conflux::json::boundary::DecodeOptions const &opts = {})
	requires conflux::json::boundary::JsonDecodeProvider<DefaultJsonProvider, std::remove_cvref_t<T>>
{
	return decode_body_with<DefaultJsonProvider, std::remove_cvref_t<T>>(req, opts);
}

template<class T>
[[nodiscard]] expected<HttpResponse, conflux::json::boundary::Error> try_response(
	T const &value,
	ResponseOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<
		DefaultJsonProvider,
		std::remove_cvref_t<T>,
		detail::ResponseBodySink &>
{
	return try_response_with<DefaultJsonProvider>(value, opts);
}

template<class T>
[[nodiscard]] expected<HttpResponse, conflux::json::boundary::Error> try_response(
	T const &value,
	int status,
	SV status_text,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<
		DefaultJsonProvider,
		std::remove_cvref_t<T>,
		detail::ResponseBodySink &>
{
	return try_response_with<DefaultJsonProvider>(value, status, status_text, opts);
}

template<class T>
[[nodiscard]] HttpResponse response_or_internal_error(
	T const &value,
	ResponseOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<
		DefaultJsonProvider,
		std::remove_cvref_t<T>,
		detail::ResponseBodySink &>
{
	return response_or_internal_error_with<DefaultJsonProvider>(value, opts);
}

template<class T>
[[nodiscard]] HttpResponse response_or_internal_error(
	T const &value,
	int status,
	SV status_text,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonWritableProvider<
		DefaultJsonProvider,
		std::remove_cvref_t<T>,
		detail::ResponseBodySink &>
{
	return response_or_internal_error_with<DefaultJsonProvider>(value, status, status_text, opts);
}

} // namespace conflux::http::json
