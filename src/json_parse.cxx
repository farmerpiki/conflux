module conflux.json;

import std;
import std.compat;
import conflux.types;

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
			if (mode != ParseMode::json5 || pos + 1 >= src.size() || src[pos] != '/') {
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
		tok.skip_ws();
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
		if (c == '\'' && opts.mode == ParseMode::json5) {
			tok.adv();
			return parse_str_node_sq();
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
			if (opts.mode == ParseMode::json5) {
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
	// Phase 5: linear dedup for n <= 8 (no allocation), lazy US
	// promotion above the threshold. The set is constructed only when the
	// object actually exceeds the linear-scan window — typical configs
	// (small flat objects) pay zero std::hash-table cost.
	static constexpr std::size_t kDedupLinearMax = 8;
	[[nodiscard]] bool dedup_member_present(
		std::size_t members_start,
		std::string_view name,
		std::optional<std::unordered_set<std::string_view>> const &seen_hash) const {
		if (seen_hash.has_value()) {
			return seen_hash->contains(name);
		}
		for (std::size_t i = members_start; i < staging_members.size(); ++i) {
			auto const &m = staging_members[i];
			if (store.bytes_at(m.name_off, m.name_len, static_cast<std::uint8_t>(m.name_flags)) == name) {
				return true;
			}
		}
		return false;
	}
	// Destroy std::hash tables in store.nodes[from..store.nodes.size()) before resize.
	void destroy_nodes_range(
		std::size_t from) noexcept {
		for (std::size_t i = from; i < store.nodes.size(); ++i) {
			auto const &n = store.nodes[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
				ObjHashTable::destroy(n.hash_idx_raw);
			}
		}
	}
	// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity)
	[[nodiscard]] std::expected<std::size_t, JsonError> parse_object(
		std::size_t depth) {
		struct StorageMark {
			std::size_t nodes;
			std::size_t string_arena;
			std::size_t array_children;
			std::size_t object_members;
		};
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
		// std::hash set is built once and reused for the remainder of this object.
		std::optional<std::unordered_set<std::string_view>> seen_hash;
		auto const dup_policy = opts.duplicate_key;
		while (true) {
			if (auto ok = skip_ws_checked(); !ok) {
				staging_members.resize(members_start);
				return std::unexpected(std::move(ok).error());
			}
			if (tok.pos >= tok.src.size()) [[unlikely]] {
				staging_members.resize(members_start);
				return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
			}
			char const key_ch = tok.src[tok.pos];
			std::expected<Tokenizer::ParsedStr, JsonError> parsed_name;
			if (key_ch == '"') {
				tok.adv();
				parsed_name = tok.parse_str_body();
			} else if (key_ch == '\'' && opts.mode == ParseMode::json5) {
				tok.adv();
				parsed_name = tok.parse_str_body_sq();
			} else if (opts.mode == ParseMode::json5) {
				parsed_name = tok.parse_unquoted_key();
			} else [[unlikely]] {
				staging_members.resize(members_start);
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected string key"));
			}
			if (!parsed_name) [[unlikely]] {
				staging_members.resize(members_start);
				return std::unexpected(std::move(parsed_name).error());
			}
			std::string_view const name_sv = store.bytes_at(parsed_name->off, parsed_name->len, parsed_name->flags);
			bool const is_dup = dedup_member_present(members_start, name_sv, seen_hash);
			if (is_dup) {
				++store.parse_stats.duplicate_member_hits;
			}
			if (is_dup && dup_policy == DuplicateKeyPolicy::reject) {
				staging_members.resize(members_start);
				return std::unexpected(
					mk_err(JsonIssueCode::duplicate_member, std::format("duplicate member: {}", name_sv)));
			}
			if (!is_dup && seen_hash.has_value()) {
				seen_hash->insert(name_sv);
				++store.parse_stats.duplicate_hash_inserts;
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

			// For first_wins, snapshot before parsing the duplicate value.
			StorageMark mark{};
			if (is_dup && dup_policy == DuplicateKeyPolicy::first_wins) {
				mark = StorageMark{
					store.nodes.size(),
					store.string_arena.size(),
					store.array_children.size(),
					store.object_members.size()};
			}

			auto val = parse_value(depth + 1);
			if (!val) {
				staging_members.resize(members_start);
				return std::unexpected(std::move(val).error());
			}

			if (is_dup) {
				if (dup_policy == DuplicateKeyPolicy::first_wins) {
					// Discard the newly parsed value; restore storage to mark.
					destroy_nodes_range(mark.nodes);
					store.nodes.resize(mark.nodes);
					store.string_arena.resize(mark.string_arena);
					store.array_children.resize(mark.array_children);
					store.object_members.resize(mark.object_members);
					++store.parse_stats.first_wins_rollbacks;
				} else {
					// last_wins: update the first occurrence's val_node.
					for (std::size_t i = members_start; i < staging_members.size(); ++i) {
						auto &m = staging_members[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
						if (store.bytes_at(m.name_off, m.name_len, static_cast<std::uint8_t>(m.name_flags))
							== name_sv) {
							m.val_node = static_cast<std::uint32_t>(*val);
							++store.parse_stats.last_wins_updates;
							break;
						}
					}
				}
			} else {
				staging_members.push_back(
					{parsed_name->off, parsed_name->len, static_cast<std::uint32_t>(*val), parsed_name->flags});

				// Promote linear → std::hash once we cross the threshold.
				std::size_t const cur_count = staging_members.size() - members_start;
				if (!seen_hash.has_value() && cur_count > kDedupLinearMax) {
					seen_hash.emplace();
					++store.parse_stats.duplicate_hash_promotions;
					std::size_t reserve_count = cur_count;
					if (cur_count <= std::numeric_limits<std::size_t>::max() - cur_count) {
						reserve_count += cur_count;
					}
					seen_hash->reserve(reserve_count);
					for (std::size_t i = members_start; i < staging_members.size(); ++i) {
						auto const &m = staging_members[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
						seen_hash->insert(
							store.bytes_at(m.name_off, m.name_len, static_cast<std::uint8_t>(m.name_flags)));
						++store.parse_stats.duplicate_hash_inserts;
					}
				}
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
				std::size_t const len = staging_members.size() - members_start;
				std::size_t const ms = store.object_members.size();
				store.object_members.insert(
					store.object_members.end(),
					staging_members.begin() + static_cast<std::ptrdiff_t>(members_start),
					staging_members.end());
				staging_members.resize(members_start);
				store.nodes.push_back(
					detail::node_object(static_cast<std::uint32_t>(ms), static_cast<std::uint32_t>(len)));
				std::size_t const obj_node_idx = store.nodes.size() - 1;
				// Auto-warm if warm_threshold is set and object is large enough.
				if (opts.warm_threshold.has_value()
					&& len >= static_cast<std::size_t>(*opts.warm_threshold)
					&& len >= kHashThreshold) {
					std::uint32_t const cap = detail::clamped_capacity(static_cast<std::uint32_t>(len));
					if (cap > 0) {
						ObjHashTable *ht = ObjHashTable::create(cap, static_cast<std::uint32_t>(len), store.hash_mr_);
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
			if (tok.src[tok.pos] != ',') {
				staging_members.resize(members_start);
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected ',' or '}'"));
			}
			tok.adv();
			if (opts.mode == ParseMode::json5) {
				if (auto ok = skip_ws_checked(); !ok) {
					staging_members.resize(members_start);
					return std::unexpected(std::move(ok).error());
				}
				if (tok.pos < tok.src.size() && tok.src[tok.pos] == '}') {
					tok.adv();
					std::size_t const len2 = staging_members.size() - members_start;
					std::size_t const ms2 = store.object_members.size();
					store.object_members.insert(
						store.object_members.end(),
						staging_members.begin() + static_cast<std::ptrdiff_t>(members_start),
						staging_members.end());
					staging_members.resize(members_start);
					store.nodes.push_back(
						detail::node_object(static_cast<std::uint32_t>(ms2), static_cast<std::uint32_t>(len2)));
					std::size_t const obj2 = store.nodes.size() - 1;
					if (opts.warm_threshold.has_value()
						&& len2 >= static_cast<std::size_t>(*opts.warm_threshold)
						&& len2 >= kHashThreshold) {
						std::uint32_t const cap2 = detail::clamped_capacity(static_cast<std::uint32_t>(len2));
						if (cap2 > 0) {
							ObjHashTable *ht2 =
								ObjHashTable::create(cap2, static_cast<std::uint32_t>(len2), store.hash_mr_);
							if (ht2 != nullptr) {
								if (detail::build_table(*ht2, &store, ms2, len2)) {
									store.nodes[obj2].hash_idx_raw =
										ht2; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
								} else {
									ObjHashTable::destroy(ht2);
									store.nodes[obj2].hash_idx_raw =
										kHashBuildFailedSentinel; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
								}
							}
						}
					}
					return obj2;
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
[[nodiscard]] inline std::expected<void, JsonError> parse_inplace(
	DocumentStorage &store,
	JsonParseOptions const &opts) {
	store.parse_stats = {};
	std::size_t const reserve_n = std::max<std::size_t>(64, store.input_view.size() / 16 + 16);
	store.nodes.reserve(reserve_n);
	store.array_children.reserve(reserve_n);
	store.object_members.reserve(reserve_n);
	store.string_arena.reserve(store.input_view.size());
	store.parse_stats.input_bytes = store.input_view.size();
	store.parse_stats.string_arena_reserve_bytes = store.input_view.size();

	TreeBuilder tb{
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
[[nodiscard]] inline std::expected<Document, JsonError> parse_with_storage(
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
	// Reserve string_arena up-front so it never reallocates mid-parse.
	// The dedup std::hash set in parse_object stores SVs into string_arena;
	// any realloc would dangle them (TSan UAF, json.cxx:2598). Decoded
	// strings are always ≤ input size (escapes only ever shrink), so the
	// input length is a safe upper bound.
	storage_ref.string_arena.reserve(storage_ref.input_view.size());
	storage_ref.parse_stats.input_bytes = storage_ref.input_view.size();
	storage_ref.parse_stats.string_arena_reserve_bytes = storage_ref.input_view.size();

	TreeBuilder tb{
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
std::expected<ArenaDocument, JsonError> JsonArena::parse_into(
	std::string_view input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	reset_storage_for_reuse();

	storage_->owned_input = std::make_unique<std::string>(input);
	std::string_view src = *storage_->owned_input;
	constexpr std::string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage_->bom_prefix_bytes = static_cast<std::uint32_t>(kBOM.size());
	}
	storage_->input_view = src;

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return std::unexpected(std::move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}

void JsonArena::reset_storage_for_reuse() noexcept {
	++generation_;
	for (auto &n: storage_->nodes) {
		if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
			ObjHashTable::destroy(n.hash_idx_raw);
			n.hash_idx_raw = nullptr;
		}
	}
	storage_->nodes.clear();
	storage_->string_arena.clear();
	storage_->array_children.clear();
	storage_->object_members.clear();
	storage_->owned_input.reset();
	storage_->root_node = 0;
	storage_->bom_prefix_bytes = 0;
}

std::expected<ArenaDocument, JsonError> JsonArena::parse_borrowed_into(
	std::string_view input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	reset_storage_for_reuse();

	std::string_view src = input;
	constexpr std::string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage_->bom_prefix_bytes = static_cast<std::uint32_t>(kBOM.size());
	}
	storage_->input_view = src;

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

	storage_->owned_input = std::make_unique<std::string>(std::move(input));
	std::string_view src = *storage_->owned_input;
	constexpr std::string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage_->bom_prefix_bytes = static_cast<std::uint32_t>(kBOM.size());
	}
	storage_->input_view = src;

	auto r = parse_inplace(*storage_, opts);
	if (!r) {
		return std::unexpected(std::move(r).error());
	}
	return ArenaDocument{storage_.get(), generation_, &generation_};
}
void JsonArena::reset() noexcept {
	++generation_;
	for (auto &n: storage_->nodes) {
		if (n.kind == NodeKind::object && n.hash_idx_raw != nullptr && n.hash_idx_raw != kHashBuildFailedSentinel) {
			ObjHashTable::destroy(n.hash_idx_raw);
			n.hash_idx_raw = nullptr;
		}
	}
	storage_ = nullptr; // ~DocumentStorage: pmr dealloc is no-op on monotonic
	mbr_.release(); // actually frees the slab
	storage_ = std::make_unique<DocumentStorage>(&mbr_);
}
namespace conflux::json {

// Explicit owning parse: copies input into the Document's owned buffer.
// Number lexemes index directly into that buffer (zero-copy on read paths).
std::expected<Document, JsonError> parse_copy(
	std::string_view input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}

	auto storage = std::make_unique<DocumentStorage>();
	storage->owned_input = std::make_unique<std::string>(input);
	std::string_view src = *storage->owned_input;
	constexpr std::string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<std::uint32_t>(kBOM.size());
	}
	storage->input_view = src;

	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, std::move(storage), opts);
}
// Move-in owning overload: avoids the input copy. Keep this a concrete
// std::string rvalue overload so unrelated string-like temporaries continue to
// select parse_copy(string_view) instead of trying to become owned storage.
std::expected<Document, JsonError> parse_copy(
	std::string &&input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}

	auto storage = std::make_unique<DocumentStorage>();
	storage->owned_input = std::make_unique<std::string>(std::move(input));
	std::string_view src = *storage->owned_input;
	constexpr std::string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<std::uint32_t>(kBOM.size());
	}
	storage->input_view = src;

	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, std::move(storage), opts);
}
// Borrow-only overload: caller guarantees the bytes outlive the Document.
// Rvalue overload is deleted to prevent obvious lifetime mistakes.
std::expected<Document, JsonError> parse_borrowed(
	std::string_view input,
	JsonParseOptions const &opts) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}

	auto storage = std::make_unique<DocumentStorage>();
	std::string_view src = input;
	constexpr std::string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<std::uint32_t>(kBOM.size());
	}
	storage->input_view = src;

	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, std::move(storage), opts);
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
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	auto storage = std::make_unique<DocumentStorage>(resource);
	storage->owned_input = std::make_unique<std::string>(input);
	std::string_view src = *storage->owned_input;
	constexpr std::string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<std::uint32_t>(kBOM.size());
	}
	storage->input_view = src;
	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, std::move(storage), opts);
}
std::expected<Document, JsonError> parse_borrowed(
	std::string_view input,
	JsonParseOptions const &opts,
	std::pmr::memory_resource *resource) {
	if (auto ok = check_input_limits(input.size(), opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	auto storage = std::make_unique<DocumentStorage>(resource);
	std::string_view src = input;
	constexpr std::string_view kBOM = "\xEF\xBB\xBF";
	if (src.starts_with(kBOM)) {
		src.remove_prefix(kBOM.size());
		storage->bom_prefix_bytes = static_cast<std::uint32_t>(kBOM.size());
	}
	storage->input_view = src;
	auto &storage_ref = *storage;
	return parse_with_storage(storage_ref, std::move(storage), opts);
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

} // namespace conflux::json

namespace conflux::json::detail {

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

} // namespace conflux::json::detail

namespace conflux::json {

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
