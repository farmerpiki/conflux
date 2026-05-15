module;

export module conflux.net.http.parse_helpers;

export import conflux.types;
export import conflux.net.http.types;

import std;
import conflux.utils;

export constexpr SZ kMaxChunkHexDigits = 16;
export constexpr SZ kMaxChunkSizeLineBytes = 256;
export constexpr SZ kMaxChunkTrailerLines = 64;
export constexpr SZ kMaxChunkTrailerBytes = 8192;

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

export std::int64_t decode_chunked(SV data, SZ max_body_size, SZ max_chunks, S &body) {
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

export [[nodiscard]] std::int64_t decode_chunked_incremental(
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
