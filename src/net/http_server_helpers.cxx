module;
#include <ctime>

export module conflux.net.http_server_helpers;

export import conflux.net.http.parse_helpers;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http.response;
import conflux.utils;

export [[nodiscard]] Response make_rejection_response(
	conflux::http::HttpRejectReason reason) {
	return Response::problem_json(
		std::format(
			R"({{"code":"{}","diagnostic_code":"{}","detail":"{}"}})",
			conflux::http::reject_reason_code(reason),
			conflux::http::reject_reason_diagnostic_code(reason),
			conflux::http::reject_reason_detail(reason)),
		conflux::http::reject_reason_status(reason));
}

export void note_rejection(
	conflux::http::HttpRejectionMetrics &metrics,
	conflux::http::HttpRejectReason reason) noexcept {
	switch (reason) {
	case conflux::http::HttpRejectReason::malformed_request       : ++metrics.malformed_request; break;
	case conflux::http::HttpRejectReason::request_line_too_large  : ++metrics.request_line_too_large; break;
	case conflux::http::HttpRejectReason::header_line_too_large   : ++metrics.header_line_too_large; break;
	case conflux::http::HttpRejectReason::header_block_too_large  : ++metrics.header_block_too_large; break;
	case conflux::http::HttpRejectReason::too_many_headers        : ++metrics.too_many_headers; break;
	case conflux::http::HttpRejectReason::missing_host            : ++metrics.missing_host; break;
	case conflux::http::HttpRejectReason::duplicate_host          : ++metrics.duplicate_host; break;
	case conflux::http::HttpRejectReason::malformed_content_length: ++metrics.malformed_content_length; break;
	case conflux::http::HttpRejectReason::duplicate_content_length: ++metrics.duplicate_content_length; break;
	case conflux::http::HttpRejectReason::content_length_with_transfer_encoding:
		++metrics.content_length_with_transfer_encoding;
		break;
	case conflux::http::HttpRejectReason::unsupported_transfer_encoding: ++metrics.unsupported_transfer_encoding; break;
	case conflux::http::HttpRejectReason::invalid_transfer_encoding    : ++metrics.invalid_transfer_encoding; break;
	case conflux::http::HttpRejectReason::invalid_chunk                : ++metrics.invalid_chunk; break;
	case conflux::http::HttpRejectReason::body_too_large               : ++metrics.body_too_large; break;
	case conflux::http::HttpRejectReason::expectation_failed           : ++metrics.expectation_failed; break;
	case conflux::http::HttpRejectReason::header_timeout               : ++metrics.header_timeout; break;
	case conflux::http::HttpRejectReason::body_timeout                 : ++metrics.body_timeout; break;
	case conflux::http::HttpRejectReason::none                         : break;
	}
}

export [[nodiscard]] bool is_valid_header_name(
	std::string_view name) noexcept {
	if (name.empty()) {
		return false;
	}
	for (auto c: name) {
		auto const u = static_cast<unsigned char>(c);
		if (u >= 0x80U) {
			return false;
		}
		bool const ok = (c >= '0' && c <= '9')
					 || (c >= 'a' && c <= 'z')
					 || (c >= 'A' && c <= 'Z')
					 || c == '!'
					 || c == '#'
					 || c == '$'
					 || c == '%'
					 || c == '&'
					 || c == '\''
					 || c == '*'
					 || c == '+'
					 || c == '-'
					 || c == '.'
					 || c == '^'
					 || c == '_'
					 || c == '`'
					 || c == '|'
					 || c == '~';
		if (!ok) {
			return false;
		}
	}
	return true;
}

export std::string_view http_date_now() {
	static thread_local std::string cached;
	static thread_local time_t cached_epoch = 0;
	auto const now = ::time(nullptr);
	if (now != cached_epoch) {
		cached_epoch = now;
		cached = conflux::http::http_date(now);
	}
	return cached;
}

[[nodiscard]] static bool is_valid_header_value(
	std::string_view value) noexcept {
	for (auto c: value) {
		auto const u = static_cast<unsigned char>(c);
		if (u < 0x20 && u != '\t') {
			return false;
		}
		if (u == 0x7F) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] static bool is_framing_header(
	std::string_view name) noexcept {
	auto lower_eq = [](std::string_view a, std::string_view b) {
		if (a.size() != b.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i) {
			auto ca = static_cast<unsigned char>(a[i]);
			auto cb = static_cast<unsigned char>(b[i]);
			if (ca >= 'A' && ca <= 'Z') {
				ca += 32;
			}
			if (ca != cb) {
				return false;
			}
		}
		return true;
	};
	return lower_eq(name, "content-length")
		|| lower_eq(name, "transfer-encoding")
		|| lower_eq(name, "connection")
		|| lower_eq(name, "upgrade")
		|| lower_eq(name, "keep-alive")
		|| lower_eq(name, "te")
		|| lower_eq(name, "trailer");
}

[[nodiscard]] static bool must_not_have_body(
	int status) noexcept {
	return (status >= 100 && status < 200) || status == 204 || status == 304;
}

[[nodiscard]] static bool is_valid_reason_phrase(
	std::string_view value) noexcept {
	for (auto c: value) {
		auto const u = static_cast<unsigned char>(c);
		if (u < 0x20 && u != '\t') {
			return false;
		}
		if (u == 0x7F) {
			return false;
		}
	}
	return true;
}

static void append_sv(
	std::string &out,
	std::string_view value) {
	out.append(value.data(), value.size());
}

static void append_dec(
	std::string &out,
	auto value) {
	std::array<char, 32> buf{};
	auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
	if (ec == std::errc{}) {
		out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
	}
}

static void append_hex(
	std::string &out,
	std::size_t value) {
	std::array<char, 2 * sizeof(std::size_t)> buf{};
	auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value, 16);
	if (ec == std::errc{}) {
		out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
	}
}

[[nodiscard]] static std::size_t response_reserve_hint(
	Response const &r,
	std::string_view alt_svc,
	bool include_body) {
	std::size_t n = 128 + r.status_text.size() + r.content_type.size() + alt_svc.size();
	if (include_body) {
		n += r.text_body().size();
	}
	for (auto const &[k, v]: r.headers) {
		n += k.size() + v.size() + 4;
	}
	for (auto const &sc: r.set_cookies) {
		n += sc.size() + 14;
	}
	return n;
}

export std::string format_response(
	Response const &r,
	std::string_view alt_svc = {},
	bool close = false) {
	if (r.is_ws_upgrade() && r.ws_upgrade_ptr()) {
		auto const &accept = r.ws_upgrade_ptr()->accept_key;
		std::string out;
		out.reserve(
			sizeof("HTTP/1.1 101 Switching Protocols\r\n"
				   "Upgrade: websocket\r\n"
				   "Connection: Upgrade\r\n"
				   "Sec-WebSocket-Accept: ")
			+ accept.size()
			+ 4);
		out += "HTTP/1.1 101 Switching Protocols\r\n";
		out += "Upgrade: websocket\r\n";
		out += "Connection: Upgrade\r\n";
		out += "Sec-WebSocket-Accept: ";
		out += accept;
		out += "\r\n\r\n";
		return out;
	}
	bool const status_no_body = must_not_have_body(r.status);
	bool const suppress_body = status_no_body || r.head_only;
	bool const include_body = !suppress_body && !r.is_mapped_file() && !r.is_streamed_file();
	std::string out;
	out.reserve(response_reserve_hint(r, alt_svc, include_body));
	out += "HTTP/1.1 ";
	append_dec(out, r.status);
	out += ' ';
	if (is_valid_reason_phrase(r.status_text)) {
		out += r.status_text;
	}
	out += "\r\nDate: ";
	append_sv(out, http_date_now());
	out += "\r\n";
	if (!r.content_type.empty()) {
		out += "Content-Type: ";
		out += r.content_type;
		out += "\r\n";
	}
	if (r.status == 304) {
		if (r.content_length_hint != 0) {
			out += "Content-Length: ";
			append_dec(out, r.content_length_hint);
			out += "\r\n";
		}
	} else if (!status_no_body) {
		out += "Content-Length: ";
		append_dec(out, r.content_length());
		out += "\r\n";
	}
	for (auto const &[k, v]: r.headers) {
		if (is_framing_header(k)) {
			continue;
		}
		if (!is_valid_header_name(k) || !is_valid_header_value(v)) {
			continue;
		}
		out += k;
		out += ": ";
		out += v;
		out += "\r\n";
	}
	for (auto const &sc: r.set_cookies) {
		if (!is_valid_header_value(sc)) {
			continue;
		}
		out += "Set-Cookie: ";
		out += sc;
		out += "\r\n";
	}
	if (!alt_svc.empty()) {
		out += "Alt-Svc: ";
		append_sv(out, alt_svc);
		out += "\r\n";
	}
	out += close ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
	if (include_body) {
		append_sv(out, r.text_body());
	}
	return out;
}

export std::string format_sse_headers(
	bool close) {
	static constexpr std::string_view kKeepAlive =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-cache\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: keep-alive\r\n"
		"\r\n";
	static constexpr std::string_view kClose =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-cache\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: close\r\n"
		"\r\n";
	return std::string{close ? kClose : kKeepAlive};
}

export [[nodiscard]] std::string format_http_chunk(
	std::string_view payload) {
	std::string out;
	out.reserve(2 * sizeof(std::size_t) + 4 + payload.size());
	append_hex(out, payload.size());
	out += "\r\n";
	append_sv(out, payload);
	out += "\r\n";
	return out;
}

export [[nodiscard]] std::string_view extract_param(
	std::string_view header,
	std::string_view param_name) {
	for (auto const param: conflux::http::header_params(header)) {
		if (!param.has_value || !conflux::http::ascii_iequals(param.name, param_name)) {
			continue;
		}
		auto value = trim(param.value);
		if (value.empty() || value.front() != '"') {
			return value;
		}
		value.remove_prefix(1);
		bool escaped = false;
		for (std::size_t i = 0; i < value.size(); ++i) {
			char const c = value[i];
			if (escaped) {
				escaped = false;
				continue;
			}
			if (c == '\\') {
				escaped = true;
				continue;
			}
			if (c == '"') {
				return value.substr(0, i);
			}
		}
		return value;
	}
	return {};
}

[[nodiscard]] std::string_view trim_cookie_ows(
	std::string_view s) noexcept {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
		s.remove_prefix(1);
	}
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
		s.remove_suffix(1);
	}
	return s;
}

export void parse_cookies(
	std::string_view cookie_header,
	HttpFieldsView &out) {
	std::size_t pos = 0;
	while (pos < cookie_header.size()) {
		auto sep = cookie_header.find(';', pos);
		auto P = trim_cookie_ows(
			sep == std::string_view::npos ? cookie_header.substr(pos) : cookie_header.substr(pos, sep - pos));
		if (auto eq = P.find('='); eq != std::string_view::npos) {
			auto name = trim_cookie_ows(P.substr(0, eq));
			auto value = trim_cookie_ows(P.substr(eq + 1));
			if (!name.empty()) {
				out.emplace_back(name, value);
			}
		} else if (!P.empty()) {
			out.emplace_back(P, {});
		}
		if (sep == std::string_view::npos) {
			break;
		}
		pos = sep + 1;
	}
}

export [[nodiscard]] bool has_connection_token(
	HttpFieldsView const &headers,
	std::string_view wanted) {
	return headers.any_value("connection", [&](std::string_view header_value) {
		return conflux::http::header_token_contains(header_value, wanted);
	});
}

export enum class ExpectState : std::uint8_t {
	none,
	continue_100,
	unsupported,
};

export [[nodiscard]] ExpectState parse_expect_header(
	HttpFieldsView const &headers) {
	bool saw_continue = false;
	bool unsupported = false;
	headers.for_each_value_until("expect", [&](std::string_view header_value) {
		for (auto const token: conflux::http::header_tokens(header_value)) {
			if (token.empty()) {
				continue;
			}
			if (!conflux::http::ascii_iequals(token, "100-continue")) {
				unsupported = true;
				break;
			}
			saw_continue = true;
		}
		if (unsupported) {
			return false;
		}
		return true;
	});
	if (unsupported) {
		return ExpectState::unsupported;
	}
	return saw_continue ? ExpectState::continue_100 : ExpectState::none;
}

export [[nodiscard]] bool has_valid_chunked_transfer_encoding(
	HttpFieldsView const &headers) {
	std::size_t token_count = 0;
	bool valid = true;
	headers.for_each_value_until("transfer-encoding", [&](std::string_view header_value) {
		for (auto const token: conflux::http::header_tokens(header_value)) {
			if (token.empty() || !conflux::http::ascii_iequals(token, "chunked")) {
				valid = false;
				break;
			}
			++token_count;
		}
		return valid;
	});
	return valid && token_count == 1;
}

struct MultipartBoundaryMatch {
	std::size_t delim_pos{};
	std::size_t content_end{};
};

[[nodiscard]] static std::optional<MultipartBoundaryMatch> find_multipart_boundary_line(
	std::string_view body,
	std::string_view delim,
	std::size_t from) noexcept {
	std::size_t search = from;
	while (search < body.size()) {
		auto const pos = body.find(delim, search);
		if (pos == std::string_view::npos) {
			return std::nullopt;
		}
		bool const at_start = pos == 0;
		bool const after_crlf = pos >= 2 && body.substr(pos - 2, 2) == "\r\n";
		bool const after_lf = !after_crlf && pos >= 1 && body[pos - 1] == '\n';
		if (at_start || after_crlf || after_lf) {
			auto const after = pos + delim.size();
			bool const valid_end = after == body.size()
								|| body.substr(after, 2) == "--"
								|| body.substr(after, 2) == "\r\n"
								|| body[after] == '\n';
			if (valid_end) {
				return MultipartBoundaryMatch{
					.delim_pos = pos,
					.content_end = after_crlf ? pos - 2 : (after_lf ? pos - 1 : pos)};
			}
		}
		search = pos + 1;
	}
	return std::nullopt;
}

export void parse_multipart(
	std::string_view body,
	std::string_view boundary,
	HttpFieldsView &form,
	std::vector<UploadedFile> &files) {
	std::string delim;
	delim.reserve(boundary.size() + 2);
	delim += "--";
	delim += boundary;
	auto first = find_multipart_boundary_line(body, delim, 0);
	if (!first) {
		return;
	}
	std::size_t pos = first->delim_pos;

	static constexpr std::size_t kMaxMultipartParts = 1000;
	static constexpr std::size_t kMaxPartHeaderBytes = std::size_t{16} * 1024;
	std::size_t part_count = 0;
	while (true) {
		if (++part_count > kMaxMultipartParts) {
			break;
		}
		pos += delim.size();
		if (pos + 1 >= body.size()) {
			break;
		}
		if (body.substr(pos, 2) == "--") {
			break;
		}

		if (body.substr(pos, 2) == "\r\n") {
			pos += 2;
		} else if (body[pos] == '\n') {
			++pos;
		} else {
			break;
		}

		auto headers_end = body.find("\r\n\r\n", pos);
		if (headers_end == std::string_view::npos) {
			break;
		}
		if (headers_end < pos || headers_end - pos > kMaxPartHeaderBytes) {
			break;
		}

		auto part_headers_sv = body.substr(pos, headers_end - pos);
		auto content_start = headers_end + 4;

		auto next_boundary = find_multipart_boundary_line(body, delim, content_start);
		if (!next_boundary) {
			break;
		}

		auto content_end = next_boundary->content_end;
		if (content_end < content_start) {
			break;
		}
		auto content = body.substr(content_start, content_end - content_start);

		std::string_view disposition;
		std::string_view part_ct = "text/plain";
		std::size_t h = 0;
		while (h < part_headers_sv.size()) {
			auto le = part_headers_sv.find("\r\n", h);
			auto line = le == std::string_view::npos ? part_headers_sv.substr(h) : part_headers_sv.substr(h, le - h);
			if (auto colon = line.find(':'); colon != std::string_view::npos) {
				auto key = trim(line.substr(0, colon));
				auto val = trim(line.substr(colon + 1));
				if (conflux::http::ascii_iequals(key, "content-disposition")) {
					disposition = val;
				} else if (conflux::http::ascii_iequals(key, "content-type")) {
					part_ct = val;
				}
			}
			if (le == std::string_view::npos) {
				break;
			}
			h = le + 2;
		}

		auto name = extract_param(disposition, "name");
		auto filename = extract_param(disposition, "filename");
		if (!filename.empty()) {
			files.push_back(UploadedFile::borrowed(name, filename, part_ct, content));
		} else if (!name.empty()) {
			form.emplace_back(name, content);
		}
		pos = next_boundary->delim_pos;
	}
}

export void populate_request_parts(
	conflux::http::PathQueryView const &target,
	HttpFieldsView const &headers,
	std::string_view body,
	HttpFieldsView &query,
	HttpFieldsView &form,
	HttpFieldsView &cookies,
	std::vector<UploadedFile> &files) {
	if (!target.query_suffix.empty()) {
		parse_urlencoded(target.query, query);
	}

	auto const content_type = headers["content-type"];
	if (content_type_is_form_urlencoded(content_type)) {
		parse_urlencoded(body, form);
	}
	if (content_type_is_multipart_form_data(content_type)) {
		auto const boundary = extract_param(content_type, "boundary");
		if (!boundary.empty()) {
			parse_multipart(body, boundary, form, files);
		}
	}

	if (auto const cookie = headers["cookie"]; !cookie.empty()) {
		parse_cookies(cookie, cookies);
	}
}
