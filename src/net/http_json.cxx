export module conflux.net.http.json;

import std;
import conflux.types;
import conflux.net.http.types;
export import conflux.net.http.request;
export import conflux.json.native_provider;
export namespace conflux::http::json {

using DefaultJsonProvider = conflux::json::boundary::NativeJsonProvider;

template<class Provider = DefaultJsonProvider, class T>
inline HttpRequest::Builder &set_body(
	HttpRequest::Builder &b,
	T const &value,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonDumpProvider<Provider, T>
{
	auto dumped = conflux::json::boundary::dump_with<Provider>(value, opts);
	if (dumped) {
		b.body(move(*dumped));
	}
	return b.content_type(conflux::json::boundary::kContentType);
}

template<class Provider = DefaultJsonProvider, class T>
inline HttpRequest::Builder &&set_body(
	HttpRequest::Builder &&b,
	T const &value,
	conflux::json::boundary::DumpOptions const &opts = {})
	requires conflux::json::boundary::JsonDumpProvider<Provider, T>
{
	return move(set_body<Provider>(b, value, opts));
}

} // namespace conflux::http::json
