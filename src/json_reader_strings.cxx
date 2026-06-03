module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

std::size_t utf8_seq_len(
	unsigned char lead) noexcept {
	// NOLINTBEGIN(readability-magic-numbers)
	if (lead < 0x80U) {
		return 1;
	}
	if (lead < 0xC2U) {
		return 0;
	}
	if (lead < 0xE0U) {
		return 2;
	}
	if (lead < 0xF0U) {
		return 3;
	}
	if (lead < 0xF5U) {
		return 4;
	}
	return 0;
	// NOLINTEND(readability-magic-numbers)
}

bool is_cont(
	unsigned char c) noexcept {
	return (c & 0xC0U) == 0x80U;
}

std::expected<void, JsonError> JsonReader::parse_str_into_token(
	LimitOption max_sz,
	JsonStringToken &tok_out) {
	std::size_t const raw_start = pos_ - 1;
	bool has_esc = false;
	while (pos_ < input_.size()) {
		std::size_t const remaining = input_.size() - pos_;
		std::size_t const skip = detail::simd::scan_str_until_special(input_.data() + pos_, remaining);
		pos_ += skip;
		col_ += skip;
		if (pos_ >= input_.size()) {
			break;
		}
		auto const c = static_cast<unsigned char>(input_[pos_]);
		if (c == '"') {
			adv();
			std::string_view raw_lex = input_.substr(raw_start, pos_ - raw_start);
			std::size_t const body_len = raw_lex.size() - 2;
			if (max_sz.exceeds(body_len, kDefaultMaxString)) [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::string_too_large, "string exceeds max_string_size"));
			}
			tok_out = JsonStringToken{raw_lex, has_esc, max_sz};
			return {};
		}
		if (c < 0x20U) {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "unescaped control character"));
		}
		if (c == '\\') {
			has_esc = true;
			adv();
			if (pos_ >= input_.size()) {
				return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in escape"));
			}
			char const esc = input_[pos_];
			if (esc == 'u') {
				adv();
				if (pos_ + 4 > input_.size()) {
					return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid \\uXXXX"));
				}
				// NOLINTBEGIN(readability-magic-numbers)
				auto cp_opt = detail::hex4_from_sv(input_, pos_);
				if (!cp_opt) {
					return std::unexpected(
						mk_err(JsonIssueCode::invalid_unicode_escape, "invalid hex digit in \\uXXXX"));
				}
				std::uint32_t cp = *cp_opt;
				adv(4);
				if (cp >= 0xD800U && cp <= 0xDBFFU) {
					if (pos_ + 6 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
						return std::unexpected(
							mk_err(JsonIssueCode::invalid_unicode_escape, "unpaired high surrogate"));
					}
					adv(2);
					auto lo_opt = detail::hex4_from_sv(input_, pos_);
					if (!lo_opt) {
						return std::unexpected(
							mk_err(JsonIssueCode::invalid_unicode_escape, "invalid hex digit in \\uXXXX"));
					}
					std::uint32_t const lo = *lo_opt;
					adv(4);
					if (lo < 0xDC00U || lo > 0xDFFFU) {
						return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid low surrogate"));
					}
				} else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
					return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "lone low surrogate"));
				}
				// NOLINTEND(readability-magic-numbers)
			} else {
				if (esc != '"'
					&& esc != '\\'
					&& esc != '/'
					&& esc != 'b'
					&& esc != 'f'
					&& esc != 'n'
					&& esc != 'r'
					&& esc != 't') {
					return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid escape"));
				}
				adv();
			}
			continue;
		}
		std::size_t const seq = utf8_seq_len(c);
		if (seq == 0) {
			return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 std::byte"));
		}
		if (pos_ + seq > input_.size()) {
			return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
		}
		for (std::size_t k = 1; k < seq; ++k) {
			if (!is_cont(static_cast<unsigned char>(input_[pos_ + k]))) {
				return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 continuation"));
			}
		}
		pos_ += seq;
		col_ += 1;
	}
	return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in string"));
}

std::expected<void, JsonError> JsonReader::parse_str_sq_into_token(
	LimitOption max_sz,
	JsonStringToken &tok_out) {
	std::size_t const raw_start = pos_ - 1;
	bool has_esc = false;
	while (pos_ < input_.size()) {
		auto const c = static_cast<unsigned char>(input_[pos_]);
		if (c == '\'') {
			adv();
			std::string_view raw_lex = input_.substr(raw_start, pos_ - raw_start);
			std::size_t const body_len = raw_lex.size() - 2;
			if (max_sz.exceeds(body_len, kDefaultMaxString)) {
				return std::unexpected(mk_err(JsonIssueCode::string_too_large, "string exceeds max_string_size"));
			}
			tok_out = JsonStringToken{raw_lex, has_esc, max_sz};
			return {};
		}
		if (c < 0x20U) {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "unescaped control character"));
		}
		if (c == '\\') {
			has_esc = true;
			adv();
			if (pos_ >= input_.size()) {
				return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in escape"));
			}
			char const esc = input_[pos_];
			if (esc == 'u') {
				adv();
				if (pos_ + 4 > input_.size()) {
					return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid \\uXXXX"));
				}
				auto cp_opt = detail::hex4_from_sv(input_, pos_);
				if (!cp_opt) {
					return std::unexpected(
						mk_err(JsonIssueCode::invalid_unicode_escape, "invalid hex digit in \\uXXXX"));
				}
				std::uint32_t cp = *cp_opt;
				adv(4);
				// NOLINTBEGIN(readability-magic-numbers)
				if (cp >= 0xD800U && cp <= 0xDBFFU) {
					if (pos_ + 6 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
						return std::unexpected(
							mk_err(JsonIssueCode::invalid_unicode_escape, "unpaired high surrogate"));
					}
					adv(2);
					auto lo_opt = detail::hex4_from_sv(input_, pos_);
					if (!lo_opt) {
						return std::unexpected(
							mk_err(JsonIssueCode::invalid_unicode_escape, "invalid hex digit in \\uXXXX"));
					}
					std::uint32_t const lo = *lo_opt;
					adv(4);
					if (lo < 0xDC00U || lo > 0xDFFFU) {
						return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "invalid low surrogate"));
					}
				} else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
					return std::unexpected(mk_err(JsonIssueCode::invalid_unicode_escape, "lone low surrogate"));
				}
				// NOLINTEND(readability-magic-numbers)
			} else {
				if (esc != '\''
					&& esc != '"'
					&& esc != '\\'
					&& esc != '/'
					&& esc != 'b'
					&& esc != 'f'
					&& esc != 'n'
					&& esc != 'r'
					&& esc != 't') {
					return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid escape"));
				}
				adv();
			}
			continue;
		}
		std::size_t const seq = utf8_seq_len(c);
		if (seq == 0) {
			return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 std::byte"));
		}
		if (pos_ + seq > input_.size()) {
			return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "truncated UTF-8"));
		}
		for (std::size_t k = 1; k < seq; ++k) {
			if (!is_cont(static_cast<unsigned char>(input_[pos_ + k]))) {
				return std::unexpected(mk_err(JsonIssueCode::invalid_utf8, "invalid UTF-8 continuation"));
			}
		}
		pos_ += seq;
		col_ += 1;
	}
	return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in string"));
}

} // namespace conflux::json
