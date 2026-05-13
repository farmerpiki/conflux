module;
#include <ctime>

export module conflux.net.http_server_helpers;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http.response;
import conflux.utils;

export constexpr SZ kMaxChunkHexDigits = 16;
export constexpr SZ kMaxChunkSizeLineBytes = 256;
export constexpr SZ kMaxChunkTrailerLines = 64;
export constexpr SZ kMaxChunkTrailerBytes = 8192;

export [[nodiscard]] bool is_valid_header_name(SV name) noexcept {
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

export SV http_date_now() {
	static thread_local S cached;
	static thread_local time_t cached_epoch = 0;
	auto const now = ::time(nullptr);
	if (now != cached_epoch) {
		cached_epoch = now;
		tm gmt{};
		if (::gmtime_r(&now, &gmt) == nullptr) {
			cached = "Thu, 01 Jan 1970 00:00:00 GMT";
			return cached;
		}
		A<char, 32> buf{};
		SZ const n = ::strftime(buf.data(), buf.size(), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
		cached = n != 0 ? S{buf.data(), n} : S{"Thu, 01 Jan 1970 00:00:00 GMT"};
	}
	return cached;
}

[[nodiscard]] static bool is_valid_header_value(SV value) noexcept {
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

[[nodiscard]] static bool is_framing_header(SV name) noexcept {
	auto lower_eq = [](SV a, SV b) {
		if (a.size() != b.size()) {
			return false;
		}
		for (SZ i = 0; i < a.size(); ++i) {
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

[[nodiscard]] static bool must_not_have_body(int status) noexcept {
	return (status >= 100 && status < 200) || status == 204 || status == 304;
}

[[nodiscard]] static bool is_valid_reason_phrase(SV value) noexcept {
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

export S format_response(HttpResponse const &r, SV alt_svc = {}, bool close = false) {
	if (r.is_ws_upgrade() && r.ws_upgrade_ptr()) {
		return format(
			"HTTP/1.1 101 Switching Protocols\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Accept: {}\r\n\r\n",
			r.ws_upgrade_ptr()->accept_key);
	}
	bool const status_no_body = must_not_have_body(r.status);
	bool const suppress_body = status_no_body || r.head_only;
	S out = format(
		"HTTP/1.1 {} {}\r\n"
		"Date: {}\r\n",
		r.status,
		is_valid_reason_phrase(r.status_text) ? SV{r.status_text} : SV{},
		http_date_now());
	if (!r.content_type.empty()) {
		out += format("Content-Type: {}\r\n", r.content_type);
	}
	if (r.status == 304) {
		if (r.content_length_hint != 0) {
			out += format("Content-Length: {}\r\n", r.content_length_hint);
		}
	} else if (!status_no_body) {
		out += format("Content-Length: {}\r\n", r.content_length());
	}
	for (auto const &[k, v]: r.headers) {
		if (is_framing_header(k)) {
			continue;
		}
		if (!is_valid_header_name(k) || !is_valid_header_value(v)) {
			continue;
		}
		out += format("{}: {}\r\n", k, v);
	}
	for (auto const &sc: r.set_cookies) {
		if (!is_valid_header_value(sc)) {
			continue;
		}
		out += format("Set-Cookie: {}\r\n", sc);
	}
	if (!alt_svc.empty()) {
		out += format("Alt-Svc: {}\r\n", alt_svc);
	}
	out += close ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
	if (!suppress_body && !r.is_mapped_file() && !r.is_streamed_file()) {
		out += r.text_body();
	}
	return out;
}

export SV format_sse_headers(bool close) {
	static constexpr SV kKeepAlive =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-cache\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: keep-alive\r\n"
		"\r\n";
	static constexpr SV kClose =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-cache\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: close\r\n"
		"\r\n";
	return close ? kClose : kKeepAlive;
}

export [[nodiscard]] S format_http_chunk(SV payload) {
	return format("{:x}\r\n{}\r\n", payload.size(), payload);
}

[[nodiscard]] static SV extract_param(SV header, SV param_name) {
	auto pos = header.find(param_name);
	if (pos == SV::npos) {
		return {};
	}
	pos += param_name.size();
	if (pos >= header.size() || header[pos] != '=') {
		return {};
	}
	++pos;
	if (pos >= header.size()) {
		return {};
	}
	if (header[pos] == '"') {
		++pos;
		auto end = header.find('"', pos);
		return end == SV::npos ? header.substr(pos) : header.substr(pos, end - pos);
	}
	auto end = header.find_first_of(";\r\n ", pos);
	return end == SV::npos ? header.substr(pos) : header.substr(pos, end - pos);
}

export void parse_cookies(SV cookie_header, HttpFieldsView &out) {
	SZ pos = 0;
	while (pos < cookie_header.size()) {
		while (pos < cookie_header.size() && cookie_header[pos] == ' ') {
			++pos;
		}
		auto sep = cookie_header.find(';', pos);
		auto P = sep == SV::npos ? cookie_header.substr(pos) : cookie_header.substr(pos, sep - pos);
		while (!P.empty() && P.back() == ' ') {
			P.remove_suffix(1);
		}
		if (auto eq = P.find('='); eq != SV::npos) {
			out.emplace_back(P.substr(0, eq), P.substr(eq + 1));
		} else if (!P.empty()) {
			out.emplace_back(P, {});
		}
		if (sep == SV::npos) {
			break;
		}
		pos = sep + 1;
	}
}

export [[nodiscard]] bool has_connection_token(HttpFieldsView const &headers, SV wanted) {
	for (auto const header_value: headers.values("connection")) {
		SZ pos = 0;
		while (pos <= header_value.size()) {
			auto const comma = header_value.find(',', pos);
			auto token = trim(comma == SV::npos ? header_value.substr(pos) : header_value.substr(pos, comma - pos));
			if (!token.empty() && conflux::http::ascii_iequals(token, wanted)) {
				return true;
			}
			if (comma == SV::npos) {
				break;
			}
			pos = comma + 1;
		}
	}
	return false;
}

export enum class ExpectState : u8 {
	none,
	continue_100,
	unsupported,
};

export [[nodiscard]] ExpectState parse_expect_header(HttpFieldsView const &headers) {
	bool saw_continue = false;
	for (auto const header_value: headers.values("expect")) {
		SZ pos = 0;
		while (pos <= header_value.size()) {
			auto const comma = header_value.find(',', pos);
			auto token = trim(comma == SV::npos ? header_value.substr(pos) : header_value.substr(pos, comma - pos));
			if (!token.empty()) {
				if (!conflux::http::ascii_iequals(token, "100-continue")) {
					return ExpectState::unsupported;
				}
				saw_continue = true;
			}
			if (comma == SV::npos) {
				break;
			}
			pos = comma + 1;
		}
	}
	return saw_continue ? ExpectState::continue_100 : ExpectState::none;
}

export [[nodiscard]] bool has_valid_chunked_transfer_encoding(HttpFieldsView const &headers) {
	SZ token_count = 0;
	for (auto const header_value: headers.values("transfer-encoding")) {
		SZ pos = 0;
		while (pos <= header_value.size()) {
			auto const comma = header_value.find(',', pos);
			auto token = trim(comma == SV::npos ? header_value.substr(pos) : header_value.substr(pos, comma - pos));
			if (token.empty()) {
				return false;
			}
			++token_count;
			if (!conflux::http::ascii_iequals(token, "chunked")) {
				return false;
			}
			if (comma == SV::npos) {
				break;
			}
			pos = comma + 1;
		}
	}
	return token_count == 1;
}

struct MultipartBoundaryMatch {
	SZ delim_pos{};
	SZ content_end{};
};

[[nodiscard]] static Opt<MultipartBoundaryMatch> find_multipart_boundary_line(SV body, SV delim, SZ from) noexcept {
	SZ search = from;
	while (search < body.size()) {
		auto const pos = body.find(delim, search);
		if (pos == SV::npos) {
			return nullopt;
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
	return nullopt;
}

export void parse_multipart(SV body, SV boundary, HttpFieldsView &form, V<UploadedFile> &files) {
	S const delim = format("--{}", boundary);
	auto first = find_multipart_boundary_line(body, delim, 0);
	if (!first) {
		return;
	}
	SZ pos = first->delim_pos;

	static constexpr SZ kMaxMultipartParts = 1000;
	static constexpr SZ kMaxPartHeaderBytes = SZ{16} * 1024;
	SZ part_count = 0;
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
		if (headers_end == SV::npos) {
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

		SV disposition;
		SV part_ct = "text/plain";
		SZ h = 0;
		while (h < part_headers_sv.size()) {
			auto le = part_headers_sv.find("\r\n", h);
			auto line = le == SV::npos ? part_headers_sv.substr(h) : part_headers_sv.substr(h, le - h);
			if (auto colon = line.find(':'); colon != SV::npos) {
				S const key = ascii_lower(line.substr(0, colon));
				auto val = trim(line.substr(colon + 1));
				if (key == "content-disposition") {
					disposition = val;
				} else if (key == "content-type") {
					part_ct = val;
				}
			}
			if (le == SV::npos) {
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

[[gnu::pure]] [[nodiscard]] static bool needs_url_decode(SV s) noexcept {
	return s.find('%') != SV::npos || s.find('+') != SV::npos;
}

export void parse_urlencoded(SV data, HttpFieldsView &out) {
	SZ pos = 0;
	while (pos <= data.size()) {
		auto amp = data.find('&', pos);
		auto P = (amp == SV::npos) ? data.substr(pos) : data.substr(pos, amp - pos);
		if (auto eq = P.find('='); eq != SV::npos) {
			auto key = P.substr(0, eq);
			auto field_value = P.substr(eq + 1);
			if (!needs_url_decode(key) && !needs_url_decode(field_value)) {
				out.emplace_back(key, field_value);
			} else {
				out.emplace_back_owned(url_decode(key), url_decode(field_value));
			}
		} else if (!P.empty()) {
			if (!needs_url_decode(P)) {
				out.emplace_back(P, {});
			} else {
				out.emplace_back_owned(url_decode(P), S{});
			}
		}
		if (amp == SV::npos) {
			break;
		}
		pos = amp + 1;
	}
}

export i64 decode_chunked(SV data, SZ max_body_size, SZ max_chunks, S &body) {
	body.clear();
	SZ pos = 0;
	SZ chunks_seen = 0;
	while (true) {
		if (++chunks_seen > max_chunks) {
			return -1;
		}
		auto crlf = data.find("\r\n", pos);
		if (crlf == SV::npos) {
			return 0;
		}

		auto size_line_raw = data.substr(pos, crlf - pos);
		if (size_line_raw.size() > kMaxChunkSizeLineBytes) {
			return -1;
		}
		auto size_digits = size_line_raw;
		if (auto semi = size_digits.find(';'); semi != SV::npos) {
			size_digits = size_digits.substr(0, semi);
		}
		if (size_digits.empty()) {
			return -1;
		}

		if (size_digits.size() > kMaxChunkHexDigits) {
			return -1;
		}
		SZ chunk_size = 0;
		for (char const c: size_digits) {
			int const d = hex_char_to_int(c);
			if (d < 0) {
				return -1;
			}
			auto const digit = static_cast<SZ>(d);
			SZ shifted = 0;
			if (__builtin_mul_overflow(chunk_size, SZ{16}, &shifted)) {
				return -1;
			}
			if (__builtin_add_overflow(shifted, digit, &chunk_size)) {
				return -1;
			}
		}
		pos = crlf + 2;

		if (chunk_size == 0) {
			SZ trailer_lines = 0;
			SZ trailer_bytes = 0;
			while (true) {
				auto next = data.find("\r\n", pos);
				if (next == SV::npos) {
					return 0;
				}
				if (next == pos) {
					return static_cast<i64>(pos + 2);
				}
				if (++trailer_lines > kMaxChunkTrailerLines) {
					return -1;
				}
				auto const line_bytes = next - pos + 2;
				if (line_bytes > kMaxChunkTrailerBytes || trailer_bytes > kMaxChunkTrailerBytes - line_bytes) {
					return -1;
				}
				trailer_bytes += line_bytes;
				pos = next + 2;
			}
		}

		if (body.size() > max_body_size || chunk_size > max_body_size - body.size()) {
			return -2;
		}
		auto const remaining_wire = data.size() - pos;
		if (chunk_size > remaining_wire || remaining_wire - chunk_size < 2) {
			return 0;
		}
		body.append(data.substr(pos, chunk_size));
		if (data[pos + chunk_size] != '\r' || data[pos + chunk_size + 1] != '\n') {
			return -1;
		}
		pos += chunk_size + 2;
	}
}

export enum class ChunkedDecodePhase : u8 {
	SizeLine,
	Data,
	DataCrlf,
	Trailers,
	Complete,
};

export struct ChunkedDecodeState {
	bool active{};
	SZ body_start{};
	SZ pos{};
	SZ chunks_seen{};
	SZ current_chunk_size{};
	SZ remaining{};
	SZ trailer_lines{};
	SZ trailer_bytes{};
	ChunkedDecodePhase phase{ChunkedDecodePhase::SizeLine};
	S body{};
	void reset() {
		active = false;
		body_start = 0;
		pos = 0;
		chunks_seen = 0;
		current_chunk_size = 0;
		remaining = 0;
		trailer_lines = 0;
		trailer_bytes = 0;
		phase = ChunkedDecodePhase::SizeLine;
		body.clear();
	}
};

export [[nodiscard]] i64 decode_chunked_incremental(
	SV raw,
	SZ body_start,
	SZ max_body_size,
	SZ max_chunks,
	ChunkedDecodeState &st) {
	if (!st.active || st.body_start != body_start) {
		st.reset();
		st.active = true;
		st.body_start = body_start;
		st.pos = body_start;
	}

	while (true) {
		switch (st.phase) {
		case ChunkedDecodePhase::SizeLine:
			{
				auto const crlf = raw.find("\r\n", st.pos);
				if (crlf == SV::npos) {
					return 0;
				}
				if (++st.chunks_seen > max_chunks) {
					return -1;
				}

				auto size_line_raw = raw.substr(st.pos, crlf - st.pos);
				if (size_line_raw.size() > kMaxChunkSizeLineBytes) {
					return -1;
				}
				auto size_digits = size_line_raw;
				if (auto semi = size_digits.find(';'); semi != SV::npos) {
					size_digits = size_digits.substr(0, semi);
				}
				if (size_digits.empty()) {
					return -1;
				}

				if (size_digits.size() > kMaxChunkHexDigits) {
					return -1;
				}
				SZ chunk_size = 0;
				for (char const c: size_digits) {
					int const d = hex_char_to_int(c);
					if (d < 0) {
						return -1;
					}
					auto const digit = static_cast<SZ>(d);
					SZ shifted = 0;
					if (__builtin_mul_overflow(chunk_size, SZ{16}, &shifted)) {
						return -1;
					}
					if (__builtin_add_overflow(shifted, digit, &chunk_size)) {
						return -1;
					}
				}

				st.pos = crlf + 2;
				st.current_chunk_size = chunk_size;
				if (chunk_size == 0) {
					st.phase = ChunkedDecodePhase::Trailers;
					break;
				}
				if (st.body.size() > max_body_size || chunk_size > max_body_size - st.body.size()) {
					return -2;
				}
				st.remaining = chunk_size;
				st.phase = ChunkedDecodePhase::Data;
				break;
			}
		case ChunkedDecodePhase::Data:
			{
				if (st.pos >= raw.size()) {
					return 0;
				}
				auto const available = min(st.remaining, raw.size() - st.pos);
				if (available > 0) {
					st.body.append(raw.substr(st.pos, available));
					st.pos += available;
					st.remaining -= available;
				}
				if (st.remaining > 0) {
					return 0;
				}
				st.phase = ChunkedDecodePhase::DataCrlf;
				break;
			}
		case ChunkedDecodePhase::DataCrlf:
			if (raw.size() - st.pos < 2) {
				return 0;
			}
			if (raw[st.pos] != '\r' || raw[st.pos + 1] != '\n') {
				return -1;
			}
			st.pos += 2;
			st.phase = ChunkedDecodePhase::SizeLine;
			break;
		case ChunkedDecodePhase::Trailers:
			{
				auto const next = raw.find("\r\n", st.pos);
				if (next == SV::npos) {
					return 0;
				}
				if (next == st.pos) {
					st.pos += 2;
					st.phase = ChunkedDecodePhase::Complete;
					return static_cast<i64>(st.pos - body_start);
				}
				if (++st.trailer_lines > kMaxChunkTrailerLines) {
					return -1;
				}
				auto const line_bytes = next - st.pos + 2;
				if (line_bytes > kMaxChunkTrailerBytes || st.trailer_bytes > kMaxChunkTrailerBytes - line_bytes) {
					return -1;
				}
				st.trailer_bytes += line_bytes;
				st.pos = next + 2;
				break;
			}
		case ChunkedDecodePhase::Complete: return static_cast<i64>(st.pos - body_start);
		}
	}
}
