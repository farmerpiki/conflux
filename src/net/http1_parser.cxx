module;

export module conflux.net.http1_parser;

import std;
import conflux.types;
import conflux.net.config;

export namespace conflux::http1 {

enum class ParseStatus : u8 {
	Ok,
	Incomplete,
	BadRequest,
	UriTooLong,
	HeaderFieldsTooLarge,
};

struct ParsedRequest {
	SV method;
	SV target;
	SV version;
	V<P<SV, SV>> headers;
	SZ header_end_offset = 0;
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

} // namespace

export namespace conflux::http1 {

ParseStatus parse_request(
	SV raw,
	ParserLimits const &limits,
	ParsedRequest &out) {
	out.headers.clear();

	auto header_end = raw.find("\r\n\r\n");
	if (header_end == SV::npos) {
		if (raw.size() > limits.max_header_block_size + limits.max_request_line_size + 2) {
			return ParseStatus::HeaderFieldsTooLarge;
		}
		return ParseStatus::Incomplete;
	}

	auto eol = raw.find("\r\n");
	auto req_line = raw.substr(0, eol);
	if (req_line.size() > limits.max_request_line_size) {
		return ParseStatus::UriTooLong;
	}

	auto const post_req_line = eol + 2;
	auto const header_block_size = (header_end > post_req_line) ? header_end - post_req_line : SZ{0};
	if (header_block_size > limits.max_header_block_size) {
		return ParseStatus::HeaderFieldsTooLarge;
	}

	auto sp1 = req_line.find(' ');
	if (sp1 == SV::npos) {
		return ParseStatus::BadRequest;
	}

	if (sp1 == 0) {
		return ParseStatus::BadRequest;
	}
	out.method = req_line.substr(0, sp1);
	if (!ranges::all_of(out.method, is_tchar)) {
		return ParseStatus::BadRequest;
	}
	auto rest = req_line.substr(sp1 + 1);
	auto sp2 = rest.find(' ');
	out.target = sp2 != SV::npos ? rest.substr(0, sp2) : rest;
	if (out.target.empty()) {
		return ParseStatus::BadRequest;
	}
	out.version = sp2 != SV::npos ? rest.substr(sp2 + 1) : SV{};
	if (out.version != "HTTP/1.0" && out.version != "HTTP/1.1") {
		return ParseStatus::BadRequest;
	}

	SZ header_count = 0;
	auto pos = eol + 2;
	while (pos < header_end) {
		auto line_end = raw.find("\r\n", pos);
		if (line_end == SV::npos || line_end > header_end) {
			return ParseStatus::BadRequest;
		}
		auto line = raw.substr(pos, line_end - pos);
		if (line.size() > limits.max_header_line_size) {
			return ParseStatus::HeaderFieldsTooLarge;
		}
		if (++header_count > limits.max_headers) {
			return ParseStatus::HeaderFieldsTooLarge;
		}
		if (line.empty() || line.front() == ' ' || line.front() == '\t') {
			return ParseStatus::BadRequest;
		}
		if (line.find('\0') != SV::npos || line.find('\r') != SV::npos) {
			return ParseStatus::BadRequest;
		}
		auto colon = line.find(':');
		if (colon == SV::npos || colon == 0) {
			return ParseStatus::BadRequest;
		}
		auto name = line.substr(0, colon);
		if (!ranges::all_of(name, is_tchar)) {
			return ParseStatus::BadRequest;
		}
		auto field_value = line.substr(colon + 1);
		while (!field_value.empty() && (field_value.front() == ' ' || field_value.front() == '\t')) {
			field_value.remove_prefix(1);
		}
		while (!field_value.empty() && (field_value.back() == ' ' || field_value.back() == '\t')) {
			field_value.remove_suffix(1);
		}
		out.headers.emplace_back(name, field_value);
		pos = line_end + 2;
	}

	out.header_end_offset = header_end;
	return ParseStatus::Ok;
}

} // namespace conflux::http1
