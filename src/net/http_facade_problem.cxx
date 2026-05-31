export module conflux.http:problem;

import std;
import conflux.net.http.response;
import conflux.net.app;
import conflux.utils;

export namespace conflux::http::problem {

[[nodiscard]] Problem make(
	int status,
	std::string_view status_text,
	std::string_view code,
	std::string_view detail) {
	auto response = Response::problem_json({}, status, std::string{status_text});
	auto problem = Problem{
		.response = std::move(response),
		.code = std::string{code},
		.title = std::string{status_text},
		.detail = std::string{detail}};
	return std::move(problem.rebuild());
}

[[nodiscard]] Problem bad_request(
	std::string_view detail = {}) {
	return make(kHttpBadRequest, "Bad conflux::http::OwnedRequest", {}, detail);
}

[[nodiscard]] Problem bad_request(
	std::string_view code,
	std::string_view detail) {
	return make(kHttpBadRequest, "Bad conflux::http::OwnedRequest", code, detail);
}

[[nodiscard]] Problem not_found(
	std::string_view detail = {}) {
	return make(kHttpNotFound, "Not Found", {}, detail);
}

[[nodiscard]] Problem not_found(
	std::string_view code,
	std::string_view detail) {
	return make(kHttpNotFound, "Not Found", code, detail);
}

[[nodiscard]] Problem unauthorized(
	std::string_view code,
	std::string_view detail) {
	return make(kHttpUnauthorized, "Unauthorized", code, detail);
}

[[nodiscard]] Problem forbidden(
	std::string_view code,
	std::string_view detail) {
	return make(kHttpForbidden, "Forbidden", code, detail);
}

[[nodiscard]] Problem unprocessable_entity(
	std::string_view code,
	std::string_view detail) {
	return make(kHttpUnprocessableEntity, "Unprocessable Entity", code, detail);
}

[[nodiscard]] Problem content_too_large(
	std::string_view code = "content_too_large",
	std::string_view detail = "request body is larger than the configured limit") {
	return make(kHttpRequestEntityTooLarge, "Content Too Large", code, detail);
}

[[nodiscard]] Problem uri_too_long(
	std::string_view code = "uri_too_long",
	std::string_view detail = "request target is too long") {
	return make(kHttpUriTooLong, "URI Too Long", code, detail);
}

[[nodiscard]] Problem header_fields_too_large(
	std::string_view code = "header_fields_too_large",
	std::string_view detail = "request headers are too large") {
	return make(kHttpRequestHeaderFieldsTooLarge, "conflux::http::OwnedRequest Header Fields Too Large", code, detail);
}

[[nodiscard]] Problem gateway_timeout(
	std::string_view code = "gateway_timeout",
	std::string_view detail = "request timed out") {
	return make(kHttpGatewayTimeout, "Gateway Timeout", code, detail);
}

[[nodiscard]] Problem internal_error(
	std::string_view code,
	std::string_view detail) {
	return make(kHttpInternalServerError, "Internal Server Error", code, detail);
}

} // namespace conflux::http::problem
