module;

export module conflux.net.http1_parser;

import std;
import conflux.types;
import conflux.net.config;
export namespace conflux::http1 {

enum class ParseStatus : std::uint8_t {
	Ok,
	Incomplete,
	BadRequest,
	UriTooLong,
	HeaderLineTooLarge,
	HeaderBlockTooLarge,
	TooManyHeaders,
};
struct ParsedRequest {
	std::string_view method;
	std::string_view target;
	std::string_view version;
	std::vector<std::pair<std::string_view, std::string_view>> headers;
	std::size_t header_end_offset = 0;
};

} // namespace conflux::http1
namespace {

constexpr bool is_tchar(
	char c) noexcept {
	auto const u = static_cast<unsigned char>(c);
	if (u >= 0x80U) {
		return false;
	}
	switch (c) {
	case '!':
	case '#':
	case '$':
	case '%':
	case '&':
	case '\'':
	case '*':
	case '+':
	case '-':
	case '.':
	case '^':
	case '_':
	case '`':
	case '|':
	case '~' : return true;
	default  : return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}
}

struct HeaderBounds {
	std::size_t request_line_end{};
	std::size_t header_start{};
	std::size_t header_end{};
};

struct HeaderBlockResult {
	conflux::http1::ParseStatus status{conflux::http1::ParseStatus::Ok};
	HeaderBounds bounds{};
};

[[nodiscard]] HeaderBlockResult find_complete_http1_header_block(
	std::string_view raw,
	conflux::http::ParserLimits const &limits) {
	auto eol = raw.find("\r\n");
	if (eol == std::string_view::npos) {
		if (raw.size() > limits.max_request_line_size) {
			return HeaderBlockResult{.status = conflux::http1::ParseStatus::UriTooLong};
		}
		return HeaderBlockResult{.status = conflux::http1::ParseStatus::Incomplete};
	}

	if (eol > limits.max_request_line_size) {
		return HeaderBlockResult{.status = conflux::http1::ParseStatus::UriTooLong};
	}

	auto const header_start = eol + 2;
	auto header_end = raw.find("\r\n\r\n", eol);
	if (header_end != std::string_view::npos) {
		auto const header_block_size = (header_end > header_start) ? header_end - header_start : std::size_t{0};
		if (header_block_size > limits.max_header_block_size) {
			return HeaderBlockResult{.status = conflux::http1::ParseStatus::HeaderBlockTooLarge};
		}
		return HeaderBlockResult{
			.status = conflux::http1::ParseStatus::Ok,
			.bounds = HeaderBounds{.request_line_end = eol, .header_start = header_start, .header_end = header_end}
        };
	}

	if (raw.size() > header_start && raw.size() - header_start > limits.max_header_block_size) {
		return HeaderBlockResult{.status = conflux::http1::ParseStatus::HeaderBlockTooLarge};
	}

	std::size_t header_count = 0;
	std::size_t pos = header_start;
	while (pos < raw.size()) {
		auto const line_end = raw.find("\r\n", pos);
		auto const line_size = line_end == std::string_view::npos ? raw.size() - pos : line_end - pos;
		if (line_size > limits.max_header_line_size) {
			return HeaderBlockResult{.status = conflux::http1::ParseStatus::HeaderLineTooLarge};
		}
		if (line_end == std::string_view::npos) {
			return HeaderBlockResult{.status = conflux::http1::ParseStatus::Incomplete};
		}
		if (++header_count > limits.max_headers) {
			return HeaderBlockResult{.status = conflux::http1::ParseStatus::TooManyHeaders};
		}
		pos = line_end + 2;
	}
	return HeaderBlockResult{.status = conflux::http1::ParseStatus::Incomplete};
}

[[nodiscard]] conflux::http1::ParseStatus parse_http1_request_line(
	std::string_view req_line,
	conflux::http1::ParsedRequest &out) {
	auto sp1 = req_line.find(' ');
	if (sp1 == std::string_view::npos || sp1 == 0) {
		return conflux::http1::ParseStatus::BadRequest;
	}

	out.method = req_line.substr(0, sp1);
	if (!std::ranges::all_of(out.method, is_tchar)) {
		return conflux::http1::ParseStatus::BadRequest;
	}

	auto rest = req_line.substr(sp1 + 1);
	auto sp2 = rest.find(' ');
	out.target = sp2 != std::string_view::npos ? rest.substr(0, sp2) : rest;
	if (out.target.empty()) {
		return conflux::http1::ParseStatus::BadRequest;
	}
	out.version = sp2 != std::string_view::npos ? rest.substr(sp2 + 1) : std::string_view{};
	if (out.version != "HTTP/1.0" && out.version != "HTTP/1.1") {
		return conflux::http1::ParseStatus::BadRequest;
	}

	return conflux::http1::ParseStatus::Ok;
}

[[nodiscard]] bool is_valid_http1_field_value(
	std::string_view field_value) noexcept {
	for (auto c: field_value) {
		auto const u = static_cast<unsigned char>(c);
		if ((u < 0x20 && u != '\t') || u == 0x7F) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::string_view trim_http1_ows(
	std::string_view field_value) noexcept {
	while (!field_value.empty() && (field_value.front() == ' ' || field_value.front() == '\t')) {
		field_value.remove_prefix(1);
	}
	while (!field_value.empty() && (field_value.back() == ' ' || field_value.back() == '\t')) {
		field_value.remove_suffix(1);
	}
	return field_value;
}

[[nodiscard]] conflux::http1::ParseStatus append_http1_header(
	std::string_view line,
	conflux::http1::ParsedRequest &out) {
	if (line.empty() || line.front() == ' ' || line.front() == '\t') {
		return conflux::http1::ParseStatus::BadRequest;
	}
	if (line.find('\0') != std::string_view::npos || line.find('\r') != std::string_view::npos) {
		return conflux::http1::ParseStatus::BadRequest;
	}
	auto colon = line.find(':');
	if (colon == std::string_view::npos || colon == 0) {
		return conflux::http1::ParseStatus::BadRequest;
	}
	auto name = line.substr(0, colon);
	if (!std::ranges::all_of(name, is_tchar)) {
		return conflux::http1::ParseStatus::BadRequest;
	}
	auto field_value = trim_http1_ows(line.substr(colon + 1));
	if (!is_valid_http1_field_value(field_value)) {
		return conflux::http1::ParseStatus::BadRequest;
	}
	out.headers.emplace_back(name, field_value);
	return conflux::http1::ParseStatus::Ok;
}

} // namespace
export namespace conflux::http1 {

ParseStatus parse_request(
	std::string_view raw,
	conflux::http::ParserLimits const &limits,
	ParsedRequest &out) {
	out.headers.clear();

	auto const bounds = find_complete_http1_header_block(raw, limits);
	if (bounds.status != ParseStatus::Ok) {
		return bounds.status;
	}
	out.headers.reserve(std::min(limits.max_headers, std::size_t{16}));

	if (auto const status = parse_http1_request_line(raw.substr(0, bounds.bounds.request_line_end), out);
		status != ParseStatus::Ok) {
		return status;
	}

	std::size_t header_count = 0;
	auto pos = bounds.bounds.header_start;
	while (pos < bounds.bounds.header_end) {
		auto line_end = raw.find("\r\n", pos);
		if (line_end == std::string_view::npos || line_end > bounds.bounds.header_end) {
			return ParseStatus::BadRequest;
		}
		auto line = raw.substr(pos, line_end - pos);
		if (line.size() > limits.max_header_line_size) {
			return ParseStatus::HeaderLineTooLarge;
		}
		if (++header_count > limits.max_headers) {
			return ParseStatus::TooManyHeaders;
		}
		if (auto const status = append_http1_header(line, out); status != ParseStatus::Ok) {
			return status;
		}
		pos = line_end + 2;
	}

	out.header_end_offset = bounds.bounds.header_end;
	return ParseStatus::Ok;
}

} // namespace conflux::http1
