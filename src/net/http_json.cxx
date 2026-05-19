export module conflux.net.http.json;

import std;
import conflux.types;
import conflux.net.http.types;
export import conflux.net.http.request;
export import conflux.json.boundary;

export namespace conflux::http::codec::json {

template<class Provider, class T>
inline ClientRequest::Builder &set_body_with(
	ClientRequest::Builder &b,
	T const &value,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonDumpProvider<Provider, T>
{
	auto dumped = conflux::json::boundary::dump_with<Provider>(value, opts);
	if (dumped) {
		b.body(std::move(*dumped));
	}
	return b.content_type(conflux::json::boundary::kContentType);
}

template<class Provider, class T>
inline ClientRequest::Builder &&set_body_with(
	ClientRequest::Builder &&b,
	T const &value,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonDumpProvider<Provider, T>
{
	return std::move(set_body_with<Provider>(b, value, opts));
}

template<class Provider, class T>
[[nodiscard]] std::expected<std::remove_cvref_t<T>, conflux::json::boundary::Error> decode_body_with(
	ClientRequest const &req,
	conflux::json::boundary::DecodeOptions const &opts = {})
	requires conflux::json::boundary::JsonDecodeProvider<Provider, std::remove_cvref_t<T>>
{
	return conflux::json::boundary::decode_with<Provider, std::remove_cvref_t<T>>(std::string_view{req.body()}, opts);
}

} // namespace conflux::http::codec::json
