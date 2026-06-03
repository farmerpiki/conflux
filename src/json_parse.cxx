module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

std::expected<void, JsonError> JsonStringToken::append_decoded_to(
	std::string &out) const {
	if (unquoted_) {
		out.append(raw_lexeme_.data(), raw_lexeme_.size());
		return {};
	}
	if (raw_lexeme_.size() < 2) {
		return {};
	}
	std::string_view body = raw_lexeme_.substr(1, raw_lexeme_.size() - 2);
	if (!has_escapes_) {
		out.append(body.data(), body.size());
		return {};
	}
	auto res = detail::decode_str_body(
		body,
		[&](std::string_view chunk) { out.append(chunk.data(), chunk.size()); },
		max_string_size_);
	if (!res) {
		return std::unexpected(std::move(res).error());
	}
	return {};
}

std::expected<std::string_view, JsonError> JsonStringToken::decode_into(
	std::span<char> buf) const {
	if (unquoted_) {
		std::ranges::copy(raw_lexeme_, buf.data());
		return std::string_view{buf.data(), raw_lexeme_.size()};
	}
	if (raw_lexeme_.size() < 2) {
		return std::string_view{buf.data(), 0};
	}
	std::string_view body = raw_lexeme_.substr(1, raw_lexeme_.size() - 2);
	if (!has_escapes_) {
		std::ranges::copy(body, buf.data());
		return std::string_view{buf.data(), body.size()};
	}
	std::size_t written = 0;
	auto res = detail::decode_str_body(
		body,
		[&](std::string_view chunk) {
			std::ranges::copy(chunk, buf.data() + written);
			written += chunk.size();
		},
		max_string_size_);
	if (!res) {
		return std::unexpected(std::move(res).error());
	}
	return std::string_view{buf.data(), written};
}

// Phase 3 — Tokenizer owns input bytes / source coordinates and emits S
// + number lexemes; TreeBuilder consumes those + structural punctuation and
// builds Nodes. Splitting them keeps the std::byte-level scan layer reusable
// (SIMD prerequisite) without changing semantics.
struct Tokenizer {
	std::string_view src;
	std::size_t pos{};
	std::size_t line{1};
	std::size_t col{1};
	DocumentStorage &store;
	std::uint32_t bom_prefix_bytes;
	ParseMode mode{};
	bool unterminated_block_comment{};
	std::size_t unterminated_block_comment_pos{};
	std::size_t unterminated_block_comment_line{1};
	std::size_t unterminated_block_comment_col{1};
	[[nodiscard]] JsonError mk_err(
		JsonIssueCode code,
		std::string msg) const {
		// Source offsets are reported in raw input bytes including any
		// stripped BOM (Correction Q): bom_prefix_bytes is added here so
		// every error site sees v7-compatible coordinates.
		return {
			.stage = JsonStage::parse,
			.code = code,
			.source = JsonSourceLocation{.offset = pos + bom_prefix_bytes, .line = line, .column = col},
			.message = std::move(msg)
        };
	}
	[[nodiscard]] JsonError whitespace_error() const {
		return {
			.stage = JsonStage::parse,
			.code = JsonIssueCode::unexpected_eof,
			.source =
				JsonSourceLocation{
								   .offset = unterminated_block_comment_pos + bom_prefix_bytes,
								   .line = unterminated_block_comment_line,
								   .column = unterminated_block_comment_col},
			.message = "unterminated block comment"
        };
	}
	template<ParseMode Mode>
	void skip_ws() {
		if (unterminated_block_comment) {
			return;
		}
		for (;;) {
			while (pos < src.size()) {
				char const c = src[pos];
				if (c == '\n') {
					++pos;
					++line;
					col = 1;
				} else if (c == ' ' || c == '\t' || c == '\r') {
					++pos;
					++col;
				} else {
					break;
				}
			}
			if constexpr (Mode != ParseMode::json5) {
				return;
			}
			if (pos + 1 >= src.size() || src[pos] != '/') {
				return;
			}
			if (src[pos + 1] == '/') {
				pos += 2;
				col += 2;
				while (pos < src.size() && src[pos] != '\n') {
					++pos;
					++col;
				}
				continue;
			}
			if (src[pos + 1] == '*') {
				std::size_t const comment_pos = pos;
				std::size_t const comment_line = line;
				std::size_t const comment_col = col;
				pos += 2;
				col += 2;
				while (pos + 1 < src.size()) {
					if (src[pos] == '*' && src[pos + 1] == '/') {
						pos += 2;
						col += 2;
						goto next_ws;
					}
					if (src[pos] == '\n') {
						++pos;
						++line;
						col = 1;
					} else {
						++pos;
						++col;
					}
				}
				pos = src.size();
				unterminated_block_comment = true;
				unterminated_block_comment_pos = comment_pos;
				unterminated_block_comment_line = comment_line;
				unterminated_block_comment_col = comment_col;
				return;
			}
			return;
next_ws:;
		}
	}
	void adv(
		std::size_t n = 1) noexcept {
		pos += n;
		col += n;
	}
	template<class Str>
	static void append_utf8(
		std::uint32_t cp,
		Str &out) {
		// NOLINTBEGIN(readability-magic-numbers)
		if (cp < 0x80U) {
			out += static_cast<char>(cp);
		} else if (cp < 0x800U) {
			out += static_cast<char>(0xC0U | (cp >> 6U));
			out += static_cast<char>(0x80U | (cp & 0x3FU));
		} else if (cp < 0x10000U) {
			out += static_cast<char>(0xE0U | (cp >> 12U));
			out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
			out += static_cast<char>(0x80U | (cp & 0x3FU));
		} else {
			out += static_cast<char>(0xF0U | (cp >> 18U));
			out += static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
			out += static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
			out += static_cast<char>(0x80U | (cp & 0x3FU));
		}
		// NOLINTEND(readability-magic-numbers)
	}
	[[nodiscard]] bool hex4(
		std::uint32_t &out) noexcept {
		if (pos + 4 > src.size()) {
			return false;
		}
		out = 0;
		for (std::size_t i = 0; i < 4; ++i) {
			char const c = src[pos + i];
			std::uint32_t d = 0;
			constexpr std::uint32_t kA = 10;
			if (c >= '0' && c <= '9') {
				d = static_cast<std::uint32_t>(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				d = static_cast<std::uint32_t>(c - 'a') + kA;
			} else if (c >= 'A' && c <= 'F') {
				d = static_cast<std::uint32_t>(c - 'A') + kA;
			} else {
				return false;
			}
			out = (out << 4U) | d;
		}
		pos += 4;
		col += 4;
		return true;
	}
	struct ParsedStr {
		std::uint32_t off;
		std::uint32_t len;
		std::uint8_t flags; // kStorageInputView | kRawJsonSlice for zero-copy, 0 for escaped
	};
	void ensure_string_arena_decode_capacity(
		std::size_t additional_bytes) {
		constexpr std::size_t kInitialStringArenaReserve = 256;
		std::size_t const wanted = store.string_arena.size() + additional_bytes;
		if (store.string_arena.capacity() >= wanted) {
			return;
		}
		std::size_t target = store.string_arena.capacity();
		if (target < kInitialStringArenaReserve) {
			target = kInitialStringArenaReserve;
		}
		while (target < wanted) {
			if (target > std::numeric_limits<std::size_t>::max() / 2U) {
				target = wanted;
				break;
			}
			target *= 2U;
		}
		store.string_arena.reserve(target);
		store.parse_stats.string_arena_reserve_bytes = std::max(store.parse_stats.string_arena_reserve_bytes, target);
	}
	[[nodiscard]] static constexpr std::size_t utf8_encoded_len(
		std::uint32_t cp) noexcept {
		if (cp < 0x80U) {
			return 1;
		}
		if (cp < 0x800U) {
			return 2;
		}
		if (cp < 0x10000U) {
			return 3;
		}
		return 4;
	}
	// Phase 2: fast path scans for `"` or `\` without copying. On `\`, copies
	// the prefix to escape_arena and continues with the escape-decoding loop;
	// the result then lives in escape_arena with flags = 0.
	// Phase 8: SIMD-accelerated bulk-ASCII fast-forward via
	// detail::simd::scan_str_until_special; scalar fallback handles the
	// boundary std::byte (terminator / escape / control / UTF-8 lead).
	[[nodiscard]] std::expected<ParsedStr, JsonError> parse_str_body() {
		constexpr unsigned char kCtrlEnd = 0x20U;
		auto const start_pos = static_cast<std::uint32_t>(pos);
		while (pos < src.size()) {
			std::size_t const remaining = src.size() - pos;
			std::size_t const skip = detail::simd::scan_str_until_special(src.data() + pos, remaining);
			pos += skip;
			col += skip;
			if (pos >= src.size()) [[unlikely]] {
				break;
			}
			auto const c = static_cast<unsigned char>(src[pos]);
			if (c == '"') {
				auto const len = static_cast<std::uint32_t>(pos) - start_pos;
				adv();
				return ParsedStr{start_pos, len, static_cast<std::uint8_t>(kStorageInputView | kRawJsonSlice)};
			}
			if (c < kCtrlEnd) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "unescaped control character"));
			}
			if (c == '\\') {
				// Slow path: copy bytes seen so far to escape_arena, then keep decoding.
				ensure_string_arena_decode_capacity((pos - start_pos) + 32U);
				std::size_t const arena_off = store.string_arena.size();
				store.string_arena.append(src.data() + start_pos, pos - start_pos);
				return parse_str_decode_tail(arena_off);
			}
			std::size_t const seq = utf8_seq_len(c);
			if (seq == 0) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 std::byte"));
			}
			if (pos + seq > src.size()) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
			}
			for (std::size_t k = 1; k < seq; ++k) {
				if (!is_cont(static_cast<unsigned char>(src[pos + k]))) [[unlikely]] {
					return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 continuation"));
				}
			}
			pos += seq;
			col += 1;
		}
		return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in string"));
	}
	[[nodiscard]] std::expected<ParsedStr, JsonError> parse_str_decode_tail(
		std::size_t arena_off) {
		constexpr unsigned char kCtrlEnd = 0x20U;
		while (pos < src.size()) {
			auto const c = static_cast<unsigned char>(src[pos]);
			if (c == '"') {
				adv();
				std::size_t const len = store.string_arena.size() - arena_off;
				return ParsedStr{static_cast<std::uint32_t>(arena_off), static_cast<std::uint32_t>(len), 0};
			}
			if (c < kCtrlEnd) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "unescaped control character"));
			}
			if (c == '\\') {
				adv();
				if (pos >= src.size()) [[unlikely]] {
					return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in escape"));
				}
				switch (src[pos]) {
				case '"':
					store.string_arena += '"';
					adv();
					break;
				case '\\':
					store.string_arena += '\\';
					adv();
					break;
				case '/':
					store.string_arena += '/';
					adv();
					break;
				case 'b':
					store.string_arena += '\b';
					adv();
					break;
				case 'f':
					store.string_arena += '\f';
					adv();
					break;
				case 'n':
					store.string_arena += '\n';
					adv();
					break;
				case 'r':
					store.string_arena += '\r';
					adv();
					break;
				case 't':
					store.string_arena += '\t';
					adv();
					break;
				case 'u':
					{
						adv();
						std::uint32_t cp = 0;
						if (!hex4(cp)) [[unlikely]] {
							return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid \\uXXXX"));
						}
						// NOLINTBEGIN(readability-magic-numbers)
						if (cp >= 0xD800U && cp <= 0xDBFFU) {
							if (pos + 6 > src.size() || src[pos] != '\\' || src[pos + 1] != 'u') {
								return std::unexpected(
									mk_err(JsonIssueCode::invalid_unicode_escape, "unpaired high surrogate"));
							}
							adv(2);
							std::uint32_t lo = 0;
							if (!hex4(lo) || lo < 0xDC00U || lo > 0xDFFFU) [[unlikely]] {
								return std::unexpected(
									mk_err(JsonIssueCode::invalid_unicode_escape, "invalid low surrogate"));
							}
							cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
						} else if (cp >= 0xDC00U && cp <= 0xDFFFU) [[unlikely]] {
							return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "lone low surrogate"));
						}
						// NOLINTEND(readability-magic-numbers)
						append_utf8(cp, store.string_arena);
						break;
					}
				default: return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid escape"));
				}
				continue;
			}
			std::size_t const seq = utf8_seq_len(c);
			if (seq == 0) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 std::byte"));
			}
			if (pos + seq > src.size()) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
			}
			for (std::size_t k = 1; k < seq; ++k) {
				if (!is_cont(static_cast<unsigned char>(src[pos + k]))) [[unlikely]] {
					return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 continuation"));
				}
			}
			store.string_arena.append(src.data() + pos, seq);
			pos += seq;
			col += 1;
		}
		return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in string"));
	}
	// JSON5: single-quoted string. Scalar scan (no SIMD). Allows \' escape.
	// NOLINTNEXTLINE(readability-function-cognitive-complexity)
	[[nodiscard]] std::expected<ParsedStr, JsonError> parse_str_body_sq() {
		constexpr unsigned char kCtrlEnd = 0x20U;
		ensure_string_arena_decode_capacity(std::min<std::size_t>(src.size() - pos, 256U));
		std::size_t const arena_off = store.string_arena.size();
		while (pos < src.size()) {
			auto const c = static_cast<unsigned char>(src[pos]);
			if (c == '\'') {
				adv();
				std::size_t const len = store.string_arena.size() - arena_off;
				return ParsedStr{static_cast<std::uint32_t>(arena_off), static_cast<std::uint32_t>(len), 0};
			}
			if (c < kCtrlEnd) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "unescaped control character"));
			}
			if (c == '\\') {
				adv();
				if (pos >= src.size()) [[unlikely]] {
					return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in escape"));
				}
				switch (src[pos]) {
				case '\'':
					store.string_arena += '\'';
					adv();
					break;
				case '"':
					store.string_arena += '"';
					adv();
					break;
				case '\\':
					store.string_arena += '\\';
					adv();
					break;
				case '/':
					store.string_arena += '/';
					adv();
					break;
				case 'b':
					store.string_arena += '\b';
					adv();
					break;
				case 'f':
					store.string_arena += '\f';
					adv();
					break;
				case 'n':
					store.string_arena += '\n';
					adv();
					break;
				case 'r':
					store.string_arena += '\r';
					adv();
					break;
				case 't':
					store.string_arena += '\t';
					adv();
					break;
				case 'u':
					{
						adv();
						std::uint32_t cp = 0;
						if (!hex4(cp)) [[unlikely]] {
							return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid \\uXXXX"));
						}
						// NOLINTBEGIN(readability-magic-numbers)
						if (cp >= 0xD800U && cp <= 0xDBFFU) {
							if (pos + 6 > src.size() || src[pos] != '\\' || src[pos + 1] != 'u') {
								return std::unexpected(
									mk_err(JsonIssueCode::invalid_unicode_escape, "unpaired high surrogate"));
							}
							adv(2);
							std::uint32_t lo = 0;
							if (!hex4(lo) || lo < 0xDC00U || lo > 0xDFFFU) [[unlikely]] {
								return std::unexpected(
									mk_err(JsonIssueCode::invalid_unicode_escape, "invalid low surrogate"));
							}
							cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
						} else if (cp >= 0xDC00U && cp <= 0xDFFFU) [[unlikely]] {
							return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "lone low surrogate"));
						}
						// NOLINTEND(readability-magic-numbers)
						append_utf8(cp, store.string_arena);
						break;
					}
				default: return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid escape"));
				}
				continue;
			}
			std::size_t const seq = utf8_seq_len(c);
			if (seq == 0) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 std::byte"));
			}
			if (pos + seq > src.size()) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
			}
			for (std::size_t k = 1; k < seq; ++k) {
				if (!is_cont(static_cast<unsigned char>(src[pos + k]))) [[unlikely]] {
					return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 continuation"));
				}
			}
			store.string_arena.append(src.data() + pos, seq);
			pos += seq;
			col += 1;
		}
		return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in string"));
	}
	[[nodiscard]] std::expected<std::uint32_t, JsonError> skip_str_body_no_store(
		char quote) {
		constexpr unsigned char kCtrlEnd = 0x20U;
		std::size_t decoded_len = 0;
		while (pos < src.size()) {
			auto const c = static_cast<unsigned char>(src[pos]);
			if (c == static_cast<unsigned char>(quote)) {
				adv();
				return static_cast<std::uint32_t>(decoded_len);
			}
			if (c < kCtrlEnd) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "unescaped control character"));
			}
			if (c == '\\') {
				adv();
				if (pos >= src.size()) [[unlikely]] {
					return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in escape"));
				}
				switch (src[pos]) {
				case '\'':
					if (quote != '\'') [[unlikely]] {
						return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid escape"));
					}
					++decoded_len;
					adv();
					break;
				case '"':
				case '\\':
				case '/':
				case 'b':
				case 'f':
				case 'n':
				case 'r':
				case 't':
					++decoded_len;
					adv();
					break;
				case 'u':
					{
						adv();
						std::uint32_t cp = 0;
						if (!hex4(cp)) [[unlikely]] {
							return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid \\uXXXX"));
						}
						// NOLINTBEGIN(readability-magic-numbers)
						if (cp >= 0xD800U && cp <= 0xDBFFU) {
							if (pos + 6 > src.size() || src[pos] != '\\' || src[pos + 1] != 'u') {
								return std::unexpected(
									mk_err(JsonIssueCode::invalid_unicode_escape, "unpaired high surrogate"));
							}
							adv(2);
							std::uint32_t lo = 0;
							if (!hex4(lo) || lo < 0xDC00U || lo > 0xDFFFU) [[unlikely]] {
								return std::unexpected(
									mk_err(JsonIssueCode::invalid_unicode_escape, "invalid low surrogate"));
							}
							cp = 0x10000U + ((cp - 0xD800U) << 10U) + (lo - 0xDC00U);
						} else if (cp >= 0xDC00U && cp <= 0xDFFFU) [[unlikely]] {
							return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "lone low surrogate"));
						}
						// NOLINTEND(readability-magic-numbers)
						decoded_len += utf8_encoded_len(cp);
						break;
					}
				default: return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid escape"));
				}
				continue;
			}
			std::size_t const seq = utf8_seq_len(c);
			if (seq == 0) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 std::byte"));
			}
			if (pos + seq > src.size()) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
			}
			for (std::size_t k = 1; k < seq; ++k) {
				if (!is_cont(static_cast<unsigned char>(src[pos + k]))) [[unlikely]] {
					return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 continuation"));
				}
			}
			decoded_len += seq;
			pos += seq;
			col += 1;
		}
		return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in string"));
	}

	// JSON5: unquoted key — [A-Za-z_$][A-Za-z0-9_$]*
	[[nodiscard]] std::expected<ParsedStr, JsonError> parse_unquoted_key() {
		std::size_t const start = pos;
		char const first = src[pos];
		if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_' || first == '$')) {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected string key or identifier"));
		}
		adv();
		while (pos < src.size()) {
			char const ch = src[pos];
			if ((ch >= 'A' && ch <= 'Z')
				|| (ch >= 'a' && ch <= 'z')
				|| (ch >= '0' && ch <= '9')
				|| ch == '_'
				|| ch == '$') {
				adv();
			} else {
				break;
			}
		}
		auto const len = static_cast<std::uint32_t>(pos - start);
		return ParsedStr{
			static_cast<std::uint32_t>(start),
			len,
			static_cast<std::uint8_t>(kStorageInputView | kRawJsonSlice)};
	}
	// Scans a number lexeme per RFC 8259 grammar and returns the slice of `src`
	// covering it. Caller (TreeBuilder) classifies the value and stores the
	// node; the lexeme references input_view directly (Phase 1: zero-copy).
	// NOLINTNEXTLINE(readability-function-cognitive-complexity)
	[[nodiscard]] std::expected<std::string_view, JsonError> parse_number_lexeme() {
		std::size_t const start = pos;
		bool const neg = src[pos] == '-';
		if (neg) {
			adv();
		}
		if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after sign"));
		}
		bool const starts_zero = src[pos] == '0';
		adv();
		if (starts_zero && pos < src.size() && src[pos] >= '0' && src[pos] <= '9') [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "leading zeros forbidden"));
		}
		while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
			adv();
		}
		if (pos < src.size() && src[pos] == '.') {
			adv();
			if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after '.'"));
			}
			while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
				adv();
			}
		}
		if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
			adv();
			if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) {
				adv();
			}
			if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "digit required in exponent"));
			}
			while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
				adv();
			}
		}
		if (pos - start > kMaxNumberLexemeLen) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::invalid_number, "number lexeme exceeds maximum length"));
		}
		return src.substr(start, pos - start);
	}
};
template<ParseMode Mode>
struct TreeBuilder {
	Tokenizer tok;
	DocumentStorage &store;
	JsonParseOptions const &opts;

	// Phase 4: shared staging buffers across nested A/object frames. Each
	// frame's slice is [frame.children_start .. staging.size()) for arrays and
	// [frame.members_start .. staging_members.size()) for objects; on close
	// the slice is moved to store.array_children / store.object_members and
	// the staging buffer truncated back. This eliminates per-frame heap
	// allocation that the v7-style local vectors paid for each container.
	std::vector<std::uint32_t> staging;
	std::vector<MemberEntry> staging_members;
	[[nodiscard]] JsonError mk_err(
		JsonIssueCode code,
		std::string msg) const {
		return tok.mk_err(code, std::move(msg));
	}
	[[nodiscard]] std::expected<void, JsonError> skip_ws_checked() {
		tok.template skip_ws<Mode>();
		if (tok.unterminated_block_comment) [[unlikely]] {
			return std::unexpected(tok.whitespace_error());
		}
		return {};
	}
	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] std::expected<std::size_t, JsonError> parse_value(
		std::size_t depth) {
		if (auto ok = skip_ws_checked(); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		if (tok.pos >= tok.src.size()) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "std::unexpected end of input"));
		}
		if (opts.max_depth.exceeds(depth, kDefaultMaxDepth)) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded"));
		}

		char const c = tok.src[tok.pos];
		if (c == '"') {
			tok.adv();
			return parse_str_node();
		}
		if constexpr (Mode == ParseMode::json5) {
			if (c == '\'') {
				tok.adv();
				return parse_str_node_sq();
			}
		}
		if (c == '[') {
			return parse_array(depth);
		}
		if (c == '{') {
			return parse_object(depth);
		}
		if (c == 't') {
			if (tok.src.substr(tok.pos, 4) != "true") [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			tok.adv(4);
			store.nodes.push_back(detail::make_bool(true));
			return store.nodes.size() - 1;
		}
		if (c == 'f') {
			if (tok.src.substr(tok.pos, 5) != "false") [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			tok.adv(5);
			store.nodes.push_back(detail::make_bool(false));
			return store.nodes.size() - 1;
		}
		if (c == 'n') {
			if (tok.src.substr(tok.pos, 4) != "null") [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			tok.adv(4);
			store.nodes.push_back(detail::make_null());
			return store.nodes.size() - 1;
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			return parse_number();
		}
		return std::unexpected(mk_err(JsonIssueCode::syntax_error, std::format("std::unexpected character '{}'", c)));
	}
	// Syntax-only value skipper used by first_wins duplicate handling. It keeps
	// parser validation and depth/string limits, but avoids materializing ignored
	// duplicate subtrees into DocumentStorage.
	// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
	[[nodiscard]] std::expected<void, JsonError> skip_value(
		std::size_t depth) {
		if (auto ok = skip_ws_checked(); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		if (tok.pos >= tok.src.size()) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "std::unexpected end of input"));
		}
		if (opts.max_depth.exceeds(depth, kDefaultMaxDepth)) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded"));
		}

		char const c = tok.src[tok.pos];
		if (c == '"') {
			tok.adv();
			auto len = tok.skip_str_body_no_store('"');
			if (!len) [[unlikely]] {
				return std::unexpected(std::move(len).error());
			}
			if (opts.max_string_size.exceeds(*len, kDefaultMaxString)) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::string_too_large, "std::string exceeds max_string_size"));
			}
			return {};
		}
		if constexpr (Mode == ParseMode::json5) {
			if (c == '\'') {
				tok.adv();
				auto len = tok.skip_str_body_no_store('\'');
				if (!len) [[unlikely]] {
					return std::unexpected(std::move(len).error());
				}
				if (opts.max_string_size.exceeds(*len, kDefaultMaxString)) [[unlikely]] {
					return std::unexpected(
						mk_err(JsonIssueCode::string_too_large, "std::string exceeds max_string_size"));
				}
				return {};
			}
		}
		if (c == '[') {
			return skip_array(depth);
		}
		if (c == '{') {
			return skip_object(depth);
		}
		if (c == 't') {
			if (tok.src.substr(tok.pos, 4) != "true") [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			tok.adv(4);
			return {};
		}
		if (c == 'f') {
			if (tok.src.substr(tok.pos, 5) != "false") [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			tok.adv(5);
			return {};
		}
		if (c == 'n') {
			if (tok.src.substr(tok.pos, 4) != "null") [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
			}
			tok.adv(4);
			return {};
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			auto lex = tok.parse_number_lexeme();
			if (!lex) {
				return std::unexpected(std::move(lex).error());
			}
			return {};
		}
		return std::unexpected(mk_err(JsonIssueCode::syntax_error, std::format("std::unexpected character '{}'", c)));
	}
	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] std::expected<void, JsonError> skip_array(
		std::size_t depth) {
		tok.adv(); // '['
		if (auto ok = skip_ws_checked(); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		if (tok.pos < tok.src.size() && tok.src[tok.pos] == ']') {
			tok.adv();
			return {};
		}
		while (true) {
			if (auto child = skip_value(depth + 1); !child) [[unlikely]] {
				return child;
			}
			if (auto ok = skip_ws_checked(); !ok) {
				return std::unexpected(std::move(ok).error());
			}
			if (tok.pos >= tok.src.size()) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in array"));
			}
			if (tok.src[tok.pos] == ']') {
				tok.adv();
				return {};
			}
			if (tok.src[tok.pos] != ',') {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected ',' or ']'"));
			}
			tok.adv();
			if constexpr (Mode == ParseMode::json5) {
				if (auto ok = skip_ws_checked(); !ok) {
					return std::unexpected(std::move(ok).error());
				}
				if (tok.pos < tok.src.size() && tok.src[tok.pos] == ']') {
					tok.adv();
					return {};
				}
			}
		}
	}
	// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
	[[nodiscard]] std::expected<void, JsonError> skip_object(
		std::size_t depth) {
		tok.adv(); // '{'
		if (auto ok = skip_ws_checked(); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		if (tok.pos < tok.src.size() && tok.src[tok.pos] == '}') {
			tok.adv();
			return {};
		}
		while (true) {
			if (auto ok = skip_ws_checked(); !ok) {
				return std::unexpected(std::move(ok).error());
			}
			if (tok.pos >= tok.src.size()) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
			}
			char const key_ch = tok.src[tok.pos];
			if (key_ch == '"') {
				tok.adv();
				auto key = tok.skip_str_body_no_store('"');
				if (!key) [[unlikely]] {
					return std::unexpected(std::move(key).error());
				}
			} else [[unlikely]] {
				if constexpr (Mode == ParseMode::json5) {
					if (key_ch == '\'') {
						tok.adv();
						auto key = tok.skip_str_body_no_store('\'');
						if (!key) [[unlikely]] {
							return std::unexpected(std::move(key).error());
						}
					} else {
						auto key = tok.parse_unquoted_key();
						if (!key) [[unlikely]] {
							return std::unexpected(std::move(key).error());
						}
					}
				} else {
					return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected string key"));
				}
			}
			if (auto ok = skip_ws_checked(); !ok) {
				return std::unexpected(std::move(ok).error());
			}
			if (tok.pos >= tok.src.size() || tok.src[tok.pos] != ':') {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected ':'"));
			}
			tok.adv();
			if (auto val = skip_value(depth + 1); !val) [[unlikely]] {
				return val;
			}
			if (auto ok = skip_ws_checked(); !ok) {
				return std::unexpected(std::move(ok).error());
			}
			if (tok.pos >= tok.src.size()) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
			}
			if (tok.src[tok.pos] == '}') {
				tok.adv();
				return {};
			}
			if (tok.src[tok.pos] != ',') {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected ',' or '}'"));
			}
			tok.adv();
			if constexpr (Mode == ParseMode::json5) {
				if (auto ok = skip_ws_checked(); !ok) {
					return std::unexpected(std::move(ok).error());
				}
				if (tok.pos < tok.src.size() && tok.src[tok.pos] == '}') {
					tok.adv();
					return {};
				}
			}
		}
	}

	[[nodiscard]] std::expected<std::size_t, JsonError> parse_str_node() {
		auto parsed = tok.parse_str_body();
		if (!parsed) [[unlikely]] {
			return std::unexpected(std::move(parsed).error());
		}
		if (opts.max_string_size.exceeds(parsed->len, kDefaultMaxString)) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::string_too_large, "std::string exceeds max_string_size"));
		}
		store.nodes.push_back(detail::make_string(parsed->off, parsed->len, parsed->flags));
		return store.nodes.size() - 1;
	}
	[[nodiscard]] std::expected<std::size_t, JsonError> parse_str_node_sq() {
		auto parsed = tok.parse_str_body_sq();
		if (!parsed) [[unlikely]] {
			return std::unexpected(std::move(parsed).error());
		}
		if (opts.max_string_size.exceeds(parsed->len, kDefaultMaxString)) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::string_too_large, "std::string exceeds max_string_size"));
		}
		store.nodes.push_back(detail::make_string(parsed->off, parsed->len, parsed->flags));
		return store.nodes.size() - 1;
	}
	// NOLINTNEXTLINE(misc-no-recursion)
	[[nodiscard]] std::expected<std::size_t, JsonError> parse_array(
		std::size_t depth) {
		tok.adv(); // '['
		if (auto ok = skip_ws_checked(); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		if (tok.pos < tok.src.size() && tok.src[tok.pos] == ']') {
			tok.adv();
			std::size_t const cs = store.array_children.size();
			store.nodes.push_back(detail::node_array(static_cast<std::uint32_t>(cs), static_cast<std::uint32_t>(0)));
			return store.nodes.size() - 1;
		}
		// Phase 4: append child indices to shared staging[children_start..],
		// flush to array_children at close, then truncate staging.
		std::size_t const children_start = staging.size();
		while (true) {
			auto child = parse_value(depth + 1);
			if (!child) [[unlikely]] {
				return std::unexpected(std::move(child).error());
			}
			staging.push_back(static_cast<std::uint32_t>(*child));
			if (auto ok = skip_ws_checked(); !ok) {
				return std::unexpected(std::move(ok).error());
			}
			if (tok.pos >= tok.src.size()) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in array"));
			}
			if (tok.src[tok.pos] == ']') {
				tok.adv();
				std::size_t const len = staging.size() - children_start;
				std::size_t const cs = store.array_children.size();
				store.array_children.insert(
					store.array_children.end(),
					staging.begin() + static_cast<std::ptrdiff_t>(children_start),
					staging.end());
				staging.resize(children_start);
				store.nodes.push_back(
					detail::node_array(static_cast<std::uint32_t>(cs), static_cast<std::uint32_t>(len)));
				return store.nodes.size() - 1;
			}
			if (tok.src[tok.pos] != ',') {
				staging.resize(children_start);
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected ',' or ']'"));
			}
			tok.adv();
			if constexpr (Mode == ParseMode::json5) {
				if (auto ok = skip_ws_checked(); !ok) {
					return std::unexpected(std::move(ok).error());
				}
				if (tok.pos < tok.src.size() && tok.src[tok.pos] == ']') {
					tok.adv();
					std::size_t const len = staging.size() - children_start;
					std::size_t const cs = store.array_children.size();
					store.array_children.insert(
						store.array_children.end(),
						staging.begin() + static_cast<std::ptrdiff_t>(children_start),
						staging.end());
					staging.resize(children_start);
					store.nodes.push_back(
						detail::node_array(static_cast<std::uint32_t>(cs), static_cast<std::uint32_t>(len)));
					return store.nodes.size() - 1;
				}
			}
		}
	}
	// Phase 5: linear dedup for n <= 8 (no allocation), lazy PMR hash-index
	// promotion above the threshold. The promoted table stores stable string
	// descriptors plus the first staging index, so duplicate lookup and
	// last_wins replacement avoid a second linear scan.
	static constexpr std::size_t kDedupLinearMax = 8;
	struct MemberNameKey {
		std::uint32_t off{};
		std::uint32_t len{};
		std::uint8_t flags{};
	};
	struct MemberNameHash {
		DocumentStorage const *store{};
		[[nodiscard]] std::size_t operator ()(
			MemberNameKey key) const noexcept {
			return std::hash<std::string_view>{}(store->bytes_at(key.off, key.len, key.flags));
		}
	};
	struct MemberNameEq {
		DocumentStorage const *store{};
		[[nodiscard]] bool operator ()(
			MemberNameKey lhs,
			MemberNameKey rhs) const noexcept {
			return store->bytes_at(lhs.off, lhs.len, lhs.flags) == store->bytes_at(rhs.off, rhs.len, rhs.flags);
		}
	};
	using MemberNameIndexAlloc = std::pmr::polymorphic_allocator<std::pair<MemberNameKey const, std::size_t>>;
	using MemberNameIndex =
		std::unordered_map<MemberNameKey, std::size_t, MemberNameHash, MemberNameEq, MemberNameIndexAlloc>;

	[[nodiscard]] std::optional<std::size_t> dedup_member_index(
		std::size_t members_start,
		MemberNameKey name,
		std::optional<MemberNameIndex> const &seen_hash) const {
		if (seen_hash.has_value()) {
			auto const it = seen_hash->find(name);
			if (it != seen_hash->end()) {
				return it->second;
			}
			return std::nullopt;
		}
		for (std::size_t i = members_start; i < staging_members.size(); ++i) {
			auto const &m = staging_members[i];
			if (store.bytes_at(m.name_off, m.name_len, static_cast<std::uint8_t>(m.name_flags))
				== store.bytes_at(name.off, name.len, name.flags)) {
				return i;
			}
		}
		return std::nullopt;
	}

	[[nodiscard]] std::size_t finish_object(
		std::size_t members_start) {
		std::size_t const len = staging_members.size() - members_start;
		std::size_t const ms = store.object_members.size();
		store.object_members.insert(
			store.object_members.end(),
			staging_members.begin() + static_cast<std::ptrdiff_t>(members_start),
			staging_members.end());
		staging_members.resize(members_start);
		store.nodes.push_back(detail::node_object(static_cast<std::uint32_t>(ms), static_cast<std::uint32_t>(len)));
		std::size_t const obj_node_idx = store.nodes.size() - 1;
		if (opts.warm_threshold.has_value()
			&& len >= static_cast<std::size_t>(*opts.warm_threshold)
			&& len >= kHashThreshold) {
			std::uint32_t const cap = detail::clamped_capacity(static_cast<std::uint32_t>(len));
			if (cap > 0) {
				ObjHashTable *ht = ObjHashTable::create(
					cap,
					static_cast<std::uint32_t>(len),
					detail::make_hash_seed(),
					store.hash_mr_);
				if (ht != nullptr) {
					if (detail::build_table(*ht, &store, ms, len)) {
						store.nodes[obj_node_idx].hash_idx_raw =
							ht; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
					} else {
						ObjHashTable::destroy(ht);
						store.nodes[obj_node_idx].hash_idx_raw =
							kHashBuildFailedSentinel; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
					}
				}
			}
		}
		return obj_node_idx;
	}

	[[nodiscard]] std::expected<Tokenizer::ParsedStr, JsonError> parse_object_member_name() {
		char const key_ch = tok.src[tok.pos];
		if (key_ch == '"') {
			tok.adv();
			return tok.parse_str_body();
		}
		if constexpr (Mode == ParseMode::json5) {
			if (key_ch == '\'') {
				tok.adv();
				return tok.parse_str_body_sq();
			}
			return tok.parse_unquoted_key();
		} else {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected string key"));
		}
	}

	[[nodiscard]] std::expected<void, JsonError> parse_object_member(
		std::size_t members_start,
		std::optional<MemberNameIndex> &seen_hash,
		DuplicateKeyPolicy dup_policy,
		std::size_t depth) {
		if (auto ok = skip_ws_checked(); !ok) {
			staging_members.resize(members_start);
			return std::unexpected(std::move(ok).error());
		}
		if (tok.pos >= tok.src.size()) [[unlikely]] {
			staging_members.resize(members_start);
			return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
		}

		auto parsed_name = parse_object_member_name();
		if (!parsed_name) [[unlikely]] {
			staging_members.resize(members_start);
			return std::unexpected(std::move(parsed_name).error());
		}
		MemberNameKey const name_key{.off = parsed_name->off, .len = parsed_name->len, .flags = parsed_name->flags};
		std::string_view const name_sv = store.bytes_at(parsed_name->off, parsed_name->len, parsed_name->flags);
		auto const dup_index = dedup_member_index(members_start, name_key, seen_hash);
		bool const is_dup = dup_index.has_value();
		if (is_dup) {
			++store.parse_stats.duplicate_member_hits;
		}
		if (is_dup && dup_policy == DuplicateKeyPolicy::reject) {
			staging_members.resize(members_start);
			return std::unexpected(
				mk_err(JsonIssueCode::duplicate_member, std::format("duplicate member: {}", name_sv)));
		}

		if (auto ok = skip_ws_checked(); !ok) {
			staging_members.resize(members_start);
			return std::unexpected(std::move(ok).error());
		}
		if (tok.pos >= tok.src.size() || tok.src[tok.pos] != ':') {
			staging_members.resize(members_start);
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected ':'"));
		}
		tok.adv();

		if (is_dup && dup_policy == DuplicateKeyPolicy::first_wins) {
			if (auto skipped = skip_value(depth + 1); !skipped) {
				staging_members.resize(members_start);
				return std::unexpected(std::move(skipped).error());
			}
			++store.parse_stats.first_wins_rollbacks;
			return {};
		}

		auto val = parse_value(depth + 1);
		if (!val) {
			staging_members.resize(members_start);
			return std::unexpected(std::move(val).error());
		}
		if (is_dup) {
			auto &m = staging_members[*dup_index]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			m.val_node = static_cast<std::uint32_t>(*val);
			++store.parse_stats.last_wins_updates;
			return {};
		}

		std::size_t const new_member_index = staging_members.size();
		staging_members.push_back(
			{parsed_name->off, parsed_name->len, static_cast<std::uint32_t>(*val), parsed_name->flags});
		if (seen_hash.has_value()) {
			seen_hash->emplace(name_key, new_member_index);
			++store.parse_stats.duplicate_hash_inserts;
		}

		std::size_t const cur_count = staging_members.size() - members_start;
		if (!seen_hash.has_value() && cur_count > kDedupLinearMax) {
			seen_hash.emplace(0, MemberNameHash{&store}, MemberNameEq{&store}, MemberNameIndexAlloc{store.hash_mr_});
			++store.parse_stats.duplicate_hash_promotions;
			std::size_t reserve_count = cur_count;
			if (cur_count <= std::numeric_limits<std::size_t>::max() - cur_count) {
				reserve_count += cur_count;
			}
			seen_hash->reserve(reserve_count);
			for (std::size_t i = members_start; i < staging_members.size(); ++i) {
				auto const &m = staging_members[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
				seen_hash->emplace(MemberNameKey{m.name_off, m.name_len, static_cast<std::uint8_t>(m.name_flags)}, i);
				++store.parse_stats.duplicate_hash_inserts;
			}
		}
		return {};
	}

	// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
	[[nodiscard]] std::expected<std::size_t, JsonError> parse_object(
		std::size_t depth) {
		tok.adv(); // '{'
		if (auto ok = skip_ws_checked(); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		if (tok.pos < tok.src.size() && tok.src[tok.pos] == '}') {
			tok.adv();
			std::size_t const ms = store.object_members.size();
			store.nodes.push_back(detail::node_object(static_cast<std::uint32_t>(ms), static_cast<std::uint32_t>(0)));
			return store.nodes.size() - 1;
		}
		// Phase 4: members go to shared staging_members[members_start..],
		// flushed to object_members at close.
		std::size_t const members_start = staging_members.size();
		// Phase 5: dedup is linear until size > kDedupLinearMax, then a
		// PMR hash index is built once and reused for the remainder of this object.
		std::optional<MemberNameIndex> seen_hash;
		auto const dup_policy = opts.duplicate_key;
		while (true) {
			if (auto member = parse_object_member(members_start, seen_hash, dup_policy, depth); !member) {
				return std::unexpected(std::move(member).error());
			}

			if (auto ok = skip_ws_checked(); !ok) {
				staging_members.resize(members_start);
				return std::unexpected(std::move(ok).error());
			}
			if (tok.pos >= tok.src.size()) [[unlikely]] {
				staging_members.resize(members_start);
				return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
			}
			if (tok.src[tok.pos] == '}') {
				tok.adv();
				return finish_object(members_start);
			}
			if (tok.src[tok.pos] != ',') {
				staging_members.resize(members_start);
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected ',' or '}'"));
			}
			tok.adv();
			if constexpr (Mode == ParseMode::json5) {
				if (auto ok = skip_ws_checked(); !ok) {
					staging_members.resize(members_start);
					return std::unexpected(std::move(ok).error());
				}
				if (tok.pos < tok.src.size() && tok.src[tok.pos] == '}') {
					tok.adv();
					return finish_object(members_start);
				}
			}
		}
	}
	[[nodiscard]] std::expected<std::size_t, JsonError> parse_number() {
		std::size_t const start = tok.pos;
		auto lex_result = tok.parse_number_lexeme();
		if (!lex_result) {
			return std::unexpected(std::move(lex_result).error());
		}
		std::string_view const lex = *lex_result;
		// Phase 1: number lexemes reference input_view directly — zero-copy.
		auto node = detail::build_number_node_from_lexeme(
			static_cast<std::uint32_t>(start),
			static_cast<std::uint32_t>(lex.size()),
			static_cast<std::uint8_t>(kStorageInputView | kRawJsonSlice),
			lex);
		if (!node) {
			return std::unexpected(std::move(node).error());
		}
		store.nodes.push_back(*node);
		return store.nodes.size() - 1;
	}
};
// ---------------------------------------------------------------------------
// parse()
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::expected<void, JsonError> check_input_limits(
	std::size_t input_size,
	JsonParseOptions const &opts) noexcept {
	// 4 GiB hard ceiling — Fix F / Correction P. Unbypassable by
	// max_input_size = no_limit because Node::off / Node::len /
	// array_children entries are all u32.
	constexpr std::size_t kU32Ceiling = (std::size_t{1} << 32) - 1;
	if (input_size >= kU32Ceiling) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::input_too_large,
				.message = "input exceeds 4 GiB hard ceiling"});
	}
	if (opts.max_input_size.exceeds(input_size, kDefaultMaxInput)) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::input_too_large,
				.message = "input exceeds max_input_size"});
	}
	return {};
}
template<ParseMode Mode>
[[nodiscard]] inline std::expected<void, JsonError> parse_inplace_impl(
	DocumentStorage &store,
	JsonParseOptions const &opts) {
	store.parse_stats = {};
	std::size_t const reserve_n = std::max<std::size_t>(64, store.input_view.size() / 16 + 16);
	store.nodes.reserve(reserve_n);
	store.array_children.reserve(reserve_n);
	store.object_members.reserve(reserve_n);
	store.parse_stats.input_bytes = store.input_view.size();
	store.parse_stats.string_arena_reserve_bytes = 0;

	TreeBuilder<Mode> tb{
		.tok =
			Tokenizer{
					  .src = store.input_view,
					  .store = store,
					  .bom_prefix_bytes = store.bom_prefix_bytes,
					  .mode = opts.mode},
		.store = store,
		.opts = opts,
		.staging = {},
		.staging_members = {}
    };
	if (auto ok = tb.skip_ws_checked(); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	if (tb.tok.pos >= store.input_view.size()) {
		return std::unexpected(
			JsonError{.stage = JsonStage::parse, .code = JsonIssueCode::unexpected_eof, .message = "empty input"});
	}
	auto root = tb.parse_value(0);
	if (!root) {
		return std::unexpected(std::move(root).error());
	}
	store.root_node = static_cast<std::uint32_t>(*root);
	if (auto ok = tb.skip_ws_checked(); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	if (tb.tok.pos < store.input_view.size()) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::trailing_garbage,
				.source =
					JsonSourceLocation{
									   .offset = tb.tok.pos + store.bom_prefix_bytes,
									   .line = tb.tok.line,
									   .column = tb.tok.col},
				.message = "trailing content after value"
        });
	}
	return {};
}
[[nodiscard]] inline std::expected<void, JsonError> parse_inplace(
	DocumentStorage &store,
	JsonParseOptions const &opts) {
	if (opts.mode == ParseMode::strict) {
		return parse_inplace_impl<ParseMode::strict>(store, opts);
	}
	return parse_inplace_impl<ParseMode::json5>(store, opts);
}

template<ParseMode Mode>
[[nodiscard]] inline std::expected<Document, JsonError> parse_with_storage_impl(
	DocumentStorage &storage_ref,
	std::unique_ptr<DocumentStorage> storage,
	JsonParseOptions const &opts) {
	// R1 / Polish AA — pre-size the three growth vectors. JSON has roughly
	// one node per 8–16 bytes of input on typical payloads; reserving ahead
	// of the parse skips the geometric realloc cycle on >100 KB inputs.
	// Floor at 64 preserves the tiny-input baseline. A precise structural
	// prescan was tried and rejected — the branchful in-std::string scan
	// (~1 GB/s) cost more than the realloc copies it saved on the
	// 4 KB / 200 KB corpora in this bench.
	std::size_t const reserve_n = std::max<std::size_t>(64, storage_ref.input_view.size() / 16 + 16);
	storage_ref.parse_stats = {};
	storage->nodes.reserve(reserve_n);
	storage->array_children.reserve(reserve_n);
	storage->object_members.reserve(reserve_n);
	storage_ref.parse_stats.input_bytes = storage_ref.input_view.size();
	storage_ref.parse_stats.string_arena_reserve_bytes = 0;

	TreeBuilder<Mode> tb{
		.tok =
			Tokenizer{
					  .src = storage_ref.input_view,
					  .store = storage_ref,
					  .bom_prefix_bytes = storage_ref.bom_prefix_bytes,
					  .mode = opts.mode},
		.store = storage_ref,
		.opts = opts,
		.staging = {},
		.staging_members = {}
    };
	if (auto ok = tb.skip_ws_checked(); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	if (tb.tok.pos >= storage_ref.input_view.size()) {
		return std::unexpected(
			JsonError{.stage = JsonStage::parse, .code = JsonIssueCode::unexpected_eof, .message = "empty input"});
	}

	auto root = tb.parse_value(0);
	if (!root) {
		return std::unexpected(std::move(root).error());
	}
	storage->root_node = static_cast<std::uint32_t>(*root);

	if (auto ok = tb.skip_ws_checked(); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	if (tb.tok.pos < storage_ref.input_view.size()) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::trailing_garbage,
				.source =
					JsonSourceLocation{
									   .offset = tb.tok.pos + storage_ref.bom_prefix_bytes,
									   .line = tb.tok.line,
									   .column = tb.tok.col},
				.message = "trailing content after value"
        });
	}

	return make_document(std::move(storage));
}

[[nodiscard]] inline std::expected<Document, JsonError> parse_with_storage(
	DocumentStorage &storage_ref,
	std::unique_ptr<DocumentStorage> storage,
	JsonParseOptions const &opts) {
	if (opts.mode == ParseMode::strict) {
		return parse_with_storage_impl<ParseMode::strict>(storage_ref, std::move(storage), opts);
	}
	return parse_with_storage_impl<ParseMode::json5>(storage_ref, std::move(storage), opts);
}

void set_storage_input_view(
	DocumentStorage &storage,
	std::string_view src) noexcept {
	constexpr std::string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage.bom_prefix_bytes = static_cast<std::uint32_t>(kBOM.size());
	}
	storage.input_view = src;
}

void prepare_copied_input(
	DocumentStorage &storage,
	std::string_view input) {
	storage.owned_input.assign(input);
	set_storage_input_view(storage, storage.owned_input);
}

void prepare_moved_input(
	DocumentStorage &storage,
	std::string &&input) {
	storage.owned_input = std::move(input);
	set_storage_input_view(storage, storage.owned_input);
}

void prepare_borrowed_input(
	DocumentStorage &storage,
	std::string_view input) noexcept {
	set_storage_input_view(storage, input);
}

template<typename PrepareInput>
[[nodiscard]] inline std::expected<Document, JsonError> parse_document_storage(
	std::size_t input_size,
	std::unique_ptr<DocumentStorage> storage,
	JsonParseOptions const &opts,
	PrepareInput &&prepare_input) {
	if (auto ok = check_input_limits(input_size, opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	prepare_input(*storage);
	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, std::move(storage), opts);
}

std::expected<ArenaDocument, JsonError> JsonArena::parse_into(
	std::string_view input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	reset_storage_for_reuse();

	prepare_copied_input(*storage_, input);

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return std::unexpected(std::move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}

void JsonArena::reset_storage_for_reuse() noexcept {
	++generation_;
	storage_->reset();
}

std::expected<ArenaDocument, JsonError> JsonArena::parse_borrowed_into(
	std::string_view input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	reset_storage_for_reuse();

	prepare_borrowed_input(*storage_, input);

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return std::unexpected(std::move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}
std::expected<ArenaDocument, JsonError> JsonArena::parse_moved_into(
	std::string input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	reset_storage_for_reuse();

	prepare_moved_input(*storage_, std::move(input));

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return std::unexpected(std::move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}
void JsonArena::reset() noexcept {
	++generation_;
	storage_ = nullptr; // ~DocumentStorage: pmr dealloc is no-op on monotonic
	mbr_.release(); // actually frees the slab
	storage_ = std::make_unique<DocumentStorage>(&mbr_, hash_index_resource_);
}
// Explicit owning parse: copies input into the Document's owned buffer.
// Number lexemes index directly into that buffer (zero-copy on read paths).
std::expected<Document, JsonError> parse_copy(
	std::string_view input,
	JsonParseOptions const &opts) {
	return parse_document_storage(
		input.size(),
		std::make_unique<DocumentStorage>(),
		opts,
		[input](DocumentStorage &storage) { prepare_copied_input(storage, input); });
}
// Move-in owning overload: avoids the input copy. Keep this a concrete
// std::string rvalue overload so unrelated string-like temporaries continue to
// select parse_copy(string_view) instead of trying to become owned storage.
std::expected<Document, JsonError> parse_copy(
	std::string &&input,
	JsonParseOptions const &opts) {
	auto const input_size = input.size();
	return parse_document_storage(
		input_size,
		std::make_unique<DocumentStorage>(),
		opts,
		[input = std::move(input)](DocumentStorage &storage) mutable {
			prepare_moved_input(storage, std::move(input));
		});
}
// Borrow-only overload: caller guarantees the bytes outlive the Document.
// Rvalue overload is deleted to prevent obvious lifetime mistakes.
std::expected<Document, JsonError> parse_borrowed(
	std::string_view input,
	JsonParseOptions const &opts) {
	return parse_document_storage(
		input.size(),
		std::make_unique<DocumentStorage>(),
		opts,
		[input](DocumentStorage &storage) { prepare_borrowed_input(storage, input); });
}
std::expected<Document, JsonError> parse_borrowed_unsafe(
	std::string_view input,
	JsonParseOptions const &opts) {
	return parse_borrowed(input, opts);
}
std::expected<Document, JsonError> parse_view(
	std::string_view input,
	JsonParseOptions const &opts) {
	return parse_borrowed(input, opts);
}
// Performance-default parse: borrows/view-parses from stable caller-owned
// storage. Use parse_copy(...) when the returned Document must own the bytes.
std::expected<Document, JsonError> parse(
	std::string_view input,
	JsonParseOptions const &opts) {
	return parse_borrowed(input, opts);
}

// pmr-injecting overloads — caller supplies the memory resource.
// The resource must outlive every Document (and NodeRef) derived from it.
std::expected<Document, JsonError> parse_copy(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_document_storage(
		input.size(),
		std::make_unique<DocumentStorage>(resource),
		opts,
		[input](DocumentStorage &storage) { prepare_copied_input(storage, input); });
}
std::expected<Document, JsonError> parse_borrowed(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_document_storage(
		input.size(),
		std::make_unique<DocumentStorage>(resource),
		opts,
		[input](DocumentStorage &storage) { prepare_borrowed_input(storage, input); });
}
std::expected<Document, JsonError> parse_borrowed_unsafe(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_borrowed(input, opts, resource);
}
std::expected<Document, JsonError> parse_view(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_borrowed(input, opts, resource);
}
std::expected<Document, JsonError> parse(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	return parse_borrowed(input, opts, resource);
}

namespace detail {

[[nodiscard]] JsonError dom_policy_error(
	std::string_view message) {
	return JsonError{
		.stage = JsonStage::parse,
		.code = JsonIssueCode::constraint_violation,
		.message = std::string{message}};
}

[[nodiscard]] std::expected<void, JsonError> require_dom_storage(
	JsonDomPolicy const &policy,
	JsonDomStorageModel expected_storage,
	std::string_view api_name) {
	if (policy.storage == expected_storage) {
		return {};
	}
	return std::unexpected(
		dom_policy_error(std::format("{} called with incompatible JsonDomPolicy storage model", api_name)));
}

} // namespace detail

[[nodiscard]] std::expected<Document, JsonError> parse_dom(
	std::string_view input,
	JsonDomPolicy const &policy) {
	if (policy.storage == JsonDomStorageModel::caller_pmr_document) {
		return std::unexpected(
			detail::dom_policy_error(
				"parse_dom(string_view) needs the memory_resource overload for caller_pmr_document"));
	}
	if (auto ok =
			detail::require_dom_storage(policy, JsonDomStorageModel::standalone_document, "parse_dom(string_view)");
		!ok) {
		return std::unexpected(std::move(ok).error());
	}
	switch (policy.input) {
	case JsonDomInputOwnership::borrowed_view: return parse_view(input, policy.parse);
	case JsonDomInputOwnership::owned_copy   : return parse_copy(input, policy.parse);
	case JsonDomInputOwnership::owned_move:
		return std::unexpected(detail::dom_policy_error("owned_move requires parse_dom(std::string&&)"));
	}
	return std::unexpected(detail::dom_policy_error("unknown JsonDomInputOwnership"));
}

[[nodiscard]] std::expected<Document, JsonError> parse_dom(
	std::string &&input,
	JsonDomPolicy const &policy) {
	if (policy.storage == JsonDomStorageModel::caller_pmr_document) {
		return std::unexpected(
			detail::dom_policy_error(
				"parse_dom(std::string&&) needs the memory_resource overload for caller_pmr_document"));
	}
	if (auto ok =
			detail::require_dom_storage(policy, JsonDomStorageModel::standalone_document, "parse_dom(std::string&&)");
		!ok) {
		return std::unexpected(std::move(ok).error());
	}
	if (policy.input == JsonDomInputOwnership::borrowed_view) {
		return std::unexpected(detail::dom_policy_error("borrowed_view is unsafe for parse_dom(std::string&&)"));
	}
	return parse_copy(std::move(input), policy.parse);
}

[[nodiscard]] std::expected<Document, JsonError> parse_dom(
	std::string_view input,
	std::pmr::memory_resource *resource,
	JsonDomPolicy const &policy) {
	if (resource == nullptr) {
		return std::unexpected(detail::dom_policy_error("parse_dom(memory_resource*) requires a non-null resource"));
	}
	if (auto ok = detail::require_dom_storage(
			policy,
			JsonDomStorageModel::caller_pmr_document,
			"parse_dom(memory_resource*)");
		!ok) {
		return std::unexpected(std::move(ok).error());
	}
	switch (policy.input) {
	case JsonDomInputOwnership::borrowed_view: return parse_view(input, policy.parse, resource);
	case JsonDomInputOwnership::owned_copy   : return parse_copy(input, policy.parse, resource);
	case JsonDomInputOwnership::owned_move:
		return std::unexpected(
			detail::dom_policy_error(
				"owned_move requires a std::string&& overload; caller_pmr cannot move-own input today"));
	}
	return std::unexpected(detail::dom_policy_error("unknown JsonDomInputOwnership"));
}

[[nodiscard]] std::expected<ArenaDocument, JsonError> parse_dom(
	JsonArena &arena,
	std::string_view input,
	JsonDomPolicy const &policy) {
	if (auto ok = detail::require_dom_storage(
			policy,
			JsonDomStorageModel::reusable_arena,
			"parse_dom(JsonArena&, string_view)");
		!ok) {
		return std::unexpected(std::move(ok).error());
	}
	switch (policy.input) {
	case JsonDomInputOwnership::borrowed_view: return arena.parse_borrowed_into(input, policy.parse);
	case JsonDomInputOwnership::owned_copy   : return arena.parse_into(input, policy.parse);
	case JsonDomInputOwnership::owned_move:
		return std::unexpected(detail::dom_policy_error("owned_move requires parse_dom(JsonArena&, std::string&&)"));
	}
	return std::unexpected(detail::dom_policy_error("unknown JsonDomInputOwnership"));
}

[[nodiscard]] std::expected<ArenaDocument, JsonError> parse_dom(
	JsonArena &arena,
	std::string &&input,
	JsonDomPolicy const &policy) {
	if (auto ok = detail::require_dom_storage(
			policy,
			JsonDomStorageModel::reusable_arena,
			"parse_dom(JsonArena&, std::string&&)");
		!ok) {
		return std::unexpected(std::move(ok).error());
	}
	if (policy.input == JsonDomInputOwnership::borrowed_view) {
		return std::unexpected(
			detail::dom_policy_error("borrowed_view is unsafe for parse_dom(JsonArena&, std::string&&)"));
	}
	return arena.parse_moved_into(std::move(input), policy.parse);
}

} // namespace conflux::json
