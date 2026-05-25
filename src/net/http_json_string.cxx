export module conflux.net.http.json_string;

import std;
import conflux.utils;

export namespace conflux::http::detail {

void append_json_string(
	std::string &out,
	std::string_view value) {
	append_json_string_fallback(out, value);
}

[[nodiscard]] std::string json_string(
	std::string_view value) {
	return json_string_fallback(value);
}

} // namespace conflux::http::detail
