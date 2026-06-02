module;

export module conflux.net.http.parse_helpers;

export import conflux.types;
export import conflux.net.http.types;

import std;
import conflux.utils;

namespace conflux::http {

using conflux::utils::hex_char_to_int;
using conflux::utils::url_decode;
using conflux::utils::url_needs_component_decode;

export constexpr std::size_t kMaxChunkHexDigits = 16;
export constexpr std::size_t kMaxChunkSizeLineBytes = 256;
export constexpr std::size_t kMaxChunkTrailerLines = 64;
export constexpr std::size_t kMaxChunkTrailerBytes = 8192;

[[nodiscard]] bool parse_chunk_size_line(
	std::string_view size_line_raw,
	std::size_t &chunk_size) {
	if (size_line_raw.size() > kMaxChunkSizeLineBytes) {
		return false;
	}
	auto size_digits = size_line_raw;
	if (auto semi = size_digits.find(';'); semi != std::string_view::npos) {
		size_digits = size_digits.substr(0, semi);
	}
	if (size_digits.empty() || size_digits.size() > kMaxChunkHexDigits) {
		return false;
	}

	chunk_size = 0;
	for (char const c: size_digits) {
		int const d = hex_char_to_int(c);
		if (d < 0) {
			return false;
		}
		auto const digit = static_cast<std::size_t>(d);
		std::size_t shifted = 0;
		if (__builtin_mul_overflow(chunk_size, std::size_t{16}, &shifted)) {
			return false;
		}
		if (__builtin_add_overflow(shifted, digit, &chunk_size)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool accept_chunk_trailer_line(
	std::size_t line_bytes,
	std::size_t &trailer_lines,
	std::size_t &trailer_bytes) {
	if (++trailer_lines > kMaxChunkTrailerLines) {
		return false;
	}
	if (line_bytes > kMaxChunkTrailerBytes || trailer_bytes > kMaxChunkTrailerBytes - line_bytes) {
		return false;
	}
	trailer_bytes += line_bytes;
	return true;
}

export [[nodiscard]] std::string_view content_type_media_type(
	std::string_view content_type) noexcept {
	auto const semi = content_type.find(';');
	auto const media_type = semi == std::string_view::npos ? content_type : content_type.substr(0, semi);
	return conflux::utils::trim(media_type);
}

export [[nodiscard]] bool content_type_matches(
	std::string_view content_type,
	std::string_view expected) noexcept {
	return conflux::http::ascii_iequals(content_type_media_type(content_type), expected);
}

export [[nodiscard]] bool content_type_is_form_urlencoded(
	std::string_view content_type) noexcept {
	return content_type_matches(content_type, "application/x-www-form-urlencoded");
}

export [[nodiscard]] bool content_type_is_multipart_form_data(
	std::string_view content_type) noexcept {
	return content_type_matches(content_type, "multipart/form-data");
}

export [[nodiscard]] bool content_type_is_json_like(
	std::string_view content_type) noexcept {
	auto const media_type = content_type_media_type(content_type);
	return conflux::http::ascii_iequals(media_type, "application/json")
		|| (media_type.ends_with("+json") && media_type.find('/') != std::string_view::npos);
}

export void parse_urlencoded(
	std::string_view data,
	conflux::http::HttpFieldsView &out) {
	if (!data.empty()) {
		std::size_t fields = 1;
		for (auto const c: data) {
			if (c == '&') {
				++fields;
			}
		}
		out.reserve(out.size() + fields);
	}
	std::size_t pos = 0;
	while (pos <= data.size()) {
		auto amp = data.find('&', pos);
		auto P = (amp == std::string_view::npos) ? data.substr(pos) : data.substr(pos, amp - pos);
		if (auto eq = P.find('='); eq != std::string_view::npos) {
			auto key = P.substr(0, eq);
			auto field_value = P.substr(eq + 1);
			if (!url_needs_component_decode(key) && !url_needs_component_decode(field_value)) {
				out.emplace_back(key, field_value);
			} else {
				out.emplace_back_owned(url_decode(key), url_decode(field_value));
			}
		} else if (!P.empty()) {
			if (!url_needs_component_decode(P)) {
				out.emplace_back(P, {});
			} else {
				out.emplace_back_owned(url_decode(P), std::string{});
			}
		}
		if (amp == std::string_view::npos) {
			break;
		}
		pos = amp + 1;
	}
}

export std::int64_t decode_chunked(
	std::string_view data,
	std::size_t max_body_size,
	std::size_t max_chunks,
	std::string &body) {
	body.clear();
	std::size_t pos = 0;
	std::size_t chunks_seen = 0;
	while (true) {
		if (++chunks_seen > max_chunks) {
			return -1;
		}
		auto crlf = data.find("\r\n", pos);
		if (crlf == std::string_view::npos) {
			return 0;
		}

		std::size_t chunk_size = 0;
		if (!parse_chunk_size_line(data.substr(pos, crlf - pos), chunk_size)) {
			return -1;
		}
		pos = crlf + 2;

		if (chunk_size == 0) {
			std::size_t trailer_lines = 0;
			std::size_t trailer_bytes = 0;
			while (true) {
				auto next = data.find("\r\n", pos);
				if (next == std::string_view::npos) {
					return 0;
				}
				if (next == pos) {
					return static_cast<std::int64_t>(pos + 2);
				}
				auto const line_bytes = next - pos + 2;
				if (!accept_chunk_trailer_line(line_bytes, trailer_lines, trailer_bytes)) {
					return -1;
				}
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
		body.append(data.data() + pos, chunk_size);
		if (data[pos + chunk_size] != '\r' || data[pos + chunk_size + 1] != '\n') {
			return -1;
		}
		pos += chunk_size + 2;
	}
}

export enum class ChunkedDecodePhase : std::uint8_t {
	SizeLine,
	Data,
	DataCrlf,
	Trailers,
	Complete,
};

export struct ChunkedDecodeState {
	bool active{};
	std::size_t body_start{};
	std::size_t pos{};
	std::size_t chunks_seen{};
	std::size_t current_chunk_size{};
	std::size_t remaining{};
	std::size_t trailer_lines{};
	std::size_t trailer_bytes{};
	ChunkedDecodePhase phase{ChunkedDecodePhase::SizeLine};
	std::string body{};
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

export [[nodiscard]] std::int64_t decode_chunked_incremental(
	std::string_view raw,
	std::size_t body_start,
	std::size_t max_body_size,
	std::size_t max_chunks,
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
				if (crlf == std::string_view::npos) {
					return 0;
				}
				if (++st.chunks_seen > max_chunks) {
					return -1;
				}

				std::size_t chunk_size = 0;
				if (!parse_chunk_size_line(raw.substr(st.pos, crlf - st.pos), chunk_size)) {
					return -1;
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
				auto const available = std::min(st.remaining, raw.size() - st.pos);
				if (available > 0) {
					st.body.append(raw.data() + st.pos, available);
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
				if (next == std::string_view::npos) {
					return 0;
				}
				if (next == st.pos) {
					st.pos += 2;
					st.phase = ChunkedDecodePhase::Complete;
					return static_cast<std::int64_t>(st.pos - body_start);
				}
				auto const line_bytes = next - st.pos + 2;
				if (!accept_chunk_trailer_line(line_bytes, st.trailer_lines, st.trailer_bytes)) {
					return -1;
				}
				st.pos = next + 2;
				break;
			}
		case ChunkedDecodePhase::Complete: return static_cast<std::int64_t>(st.pos - body_start);
		}
	}
}

} // namespace conflux::http
