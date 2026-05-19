export module conflux.http:problem;

import std;
import conflux.net.http.response;
import conflux.net.app;

export namespace conflux::http::problem {

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

} // namespace conflux::http::problem
