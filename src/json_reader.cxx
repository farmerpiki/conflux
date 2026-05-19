module conflux.json;

import std;
import std.compat;
import conflux.types;

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

// ---------------------------------------------------------------------------
// JsonReader implementation
// ---------------------------------------------------------------------------

void JsonReader::set_error(
	JsonError e) noexcept {
	has_error_ = true;
	last_error_ = std::move(e);
}

JsonError JsonReader::mk_err(
	JsonIssueCode code,
	std::string msg) const {
	return {
		.stage = JsonStage::parse,
		.code = code,
		.source = JsonSourceLocation{.offset = pos_, .line = line_, .column = col_},
		.message = std::move(msg)
    };
}

void JsonReader::skip_ws() {
	for (;;) {
		while (pos_ < input_.size()) {
			char const c = input_[pos_];
			if (c == '\n') {
				++pos_;
				++line_;
				col_ = 1;
			} else if (c == ' ' || c == '\t' || c == '\r') {
				++pos_;
				++col_;
			} else {
				break;
			}
		}
		if (opts_.mode != ParseMode::json5 || pos_ + 1 >= input_.size() || input_[pos_] != '/') {
			return;
		}
		if (input_[pos_ + 1] == '/') {
			pos_ += 2;
			col_ += 2;
			while (pos_ < input_.size() && input_[pos_] != '\n') {
				++pos_;
				++col_;
			}
			continue;
		}
		if (input_[pos_ + 1] == '*') {
			std::size_t const comment_offset = pos_;
			std::size_t const comment_line = line_;
			std::size_t const comment_col = col_;
			pos_ += 2;
			col_ += 2;
			while (pos_ + 1 < input_.size()) {
				if (input_[pos_] == '*' && input_[pos_ + 1] == '/') {
					pos_ += 2;
					col_ += 2;
					goto next_reader_ws;
				}
				if (input_[pos_] == '\n') {
					++pos_;
					++line_;
					col_ = 1;
				} else {
					++pos_;
					++col_;
				}
			}
			pos_ = input_.size();
			set_error(
				JsonError{
					.stage = JsonStage::parse,
					.code = JsonIssueCode::unexpected_eof,
					.source = JsonSourceLocation{.offset = comment_offset, .line = comment_line, .column = comment_col},
					.message = "unterminated block comment"
            });
			return;
		}
		return;
next_reader_ws:;
	}
}

std::expected<void, JsonError> JsonReader::skip_ws_checked() {
	skip_ws();
	if (has_error_) {
		return std::unexpected(last_error_);
	}
	return {};
}

void JsonReader::adv(
	std::size_t n) noexcept {
	pos_ += n;
	col_ += n;
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

std::expected<void, JsonError> JsonReader::parse_number_into_val() {
	std::size_t const start = pos_;
	bool const neg = input_[pos_] == '-';
	if (neg) {
		adv();
	}
	if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
		return std::unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after sign"));
	}
	bool const starts_zero = input_[pos_] == '0';
	adv();
	if (starts_zero && pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
		return std::unexpected(mk_err(JsonIssueCode::syntax_error, "leading zeros forbidden"));
	}
	while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
		adv();
	}
	if (pos_ < input_.size() && input_[pos_] == '.') {
		adv();
		if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after '.'"));
		}
		while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
			adv();
		}
	}
	if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
		adv();
		if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
			adv();
		}
		if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "digit required in exponent"));
		}
		while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
			adv();
		}
	}
	if (pos_ - start > kMaxNumberLexemeLen) {
		return std::unexpected(mk_err(JsonIssueCode::invalid_number, "number lexeme exceeds maximum length"));
	}
	std::string_view const lex = input_.substr(start, pos_ - start);
	auto node = detail::build_number_node_from_lexeme(0, static_cast<std::uint32_t>(lex.size()), 0, lex);
	if (!node) {
		auto err = std::move(node).error();
		err.stage = JsonStage::parse;
		return std::unexpected(std::move(err));
	}
	num_val_ = JsonNumberView{lex, node->flags, node->_raw};
	return {};
}

std::expected<JsonReader::Event, JsonError> JsonReader::parse_value_event() {
	if (opts_.max_depth.exceeds(stack_.size(), kDefaultMaxDepth)) {
		return std::unexpected(mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded"));
	}
	if (pos_ >= input_.size()) {
		return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "std::unexpected end of input"));
	}
	char const c = input_[pos_];
	if (c == '"') {
		adv();
		auto res = parse_str_into_token(opts_.max_string_size, str_token_);
		if (!res) {
			return std::unexpected(std::move(res).error());
		}
		return Event::string_value;
	}
	if (c == '\'' && opts_.mode == ParseMode::json5) {
		adv();
		auto res = parse_str_sq_into_token(opts_.max_string_size, str_token_);
		if (!res) {
			return std::unexpected(std::move(res).error());
		}
		return Event::string_value;
	}
	if (c == '{') {
		adv();
		stack_.push_back(StateFrame{.kind = StateFrame::Kind::object, .first = true, .awaiting_value = false});
		return Event::begin_object;
	}
	if (c == '[') {
		adv();
		stack_.push_back(StateFrame{.kind = StateFrame::Kind::array, .first = true, .awaiting_value = false});
		return Event::begin_array;
	}
	if (c == 't') {
		if (input_.substr(pos_, 4) != "true") {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
		}
		adv(4);
		bool_val_ = true;
		return Event::bool_value;
	}
	if (c == 'f') {
		if (input_.substr(pos_, 5) != "false") {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
		}
		adv(5);
		bool_val_ = false;
		return Event::bool_value;
	}
	if (c == 'n') {
		if (input_.substr(pos_, 4) != "null") {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
		}
		adv(4);
		return Event::null_value;
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		auto res = parse_number_into_val();
		if (!res) {
			return std::unexpected(std::move(res).error());
		}
		return Event::number_value;
	}
	return std::unexpected(mk_err(JsonIssueCode::syntax_error, std::format("std::unexpected character '{}'", c)));
}

JsonReader::Checkpoint JsonReader::checkpoint() const {
	return Checkpoint{
		.pos = pos_,
		.line = line_,
		.col = col_,
		.stack = stack_,
		.key_token = key_token_,
		.str_token = str_token_,
		.num_val = num_val_,
		.bool_val = bool_val_,
		.has_error = has_error_,
		.last_error = last_error_,
		.value_start = value_start_,
	};
}

void JsonReader::restore(
	JsonReader::Checkpoint checkpoint) {
	pos_ = checkpoint.pos;
	line_ = checkpoint.line;
	col_ = checkpoint.col;
	stack_ = std::move(checkpoint.stack);
	key_token_ = checkpoint.key_token;
	str_token_ = checkpoint.str_token;
	num_val_ = checkpoint.num_val;
	bool_val_ = checkpoint.bool_val;
	has_error_ = checkpoint.has_error;
	last_error_ = std::move(checkpoint.last_error);
	value_start_ = checkpoint.value_start;
}

void JsonReader::replace_input(
	std::string_view input) noexcept {
	input_ = input;
}

JsonReader::JsonReader(
	std::string_view input,
	JsonParseOptions const &opts)
	: input_{input}
	, opts_{opts} {}

JsonReader::JsonReader(
	std::span<std::byte const> input,
	JsonParseOptions const &opts)
	: input_{reinterpret_cast<char const *>(input.data()), input.size()}
	, opts_{opts} {}

std::expected<std::optional<JsonReader::Event>, JsonError> JsonReader::next() {
	if (has_error_) {
		return std::unexpected(last_error_);
	}
	if (auto ok = skip_ws_checked(); !ok) {
		return std::unexpected(std::move(ok).error());
	}

	if (stack_.empty()) {
		if (pos_ >= input_.size()) {
			return std::optional<Event>{};
		}
		value_start_ = pos_;
		auto ev = parse_value_event();
		if (!ev) {
			set_error(ev.error());
			return std::unexpected(last_error_);
		}
		return std::optional<Event>{*ev};
	}

	auto &top = stack_.back();

	if (top.kind == StateFrame::Kind::array) {
		if (auto ok = skip_ws_checked(); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		if (pos_ < input_.size() && input_[pos_] == ']') {
			adv();
			stack_.pop_back();
			return std::optional<Event>{Event::end_array};
		}
		if (!top.first) {
			if (pos_ >= input_.size() || input_[pos_] != ',') {
				auto e = mk_err(JsonIssueCode::syntax_error, "std::expected ',' or ']'");
				set_error(e);
				return std::unexpected(last_error_);
			}
			adv();
			if (auto ok = skip_ws_checked(); !ok) {
				return std::unexpected(std::move(ok).error());
			}
			if (opts_.mode == ParseMode::json5 && pos_ < input_.size() && input_[pos_] == ']') {
				adv();
				stack_.pop_back();
				return std::optional<Event>{Event::end_array};
			}
		}
		top.first = false;
		value_start_ = pos_;
		auto ev = parse_value_event();
		if (!ev) {
			set_error(ev.error());
			return std::unexpected(last_error_);
		}
		return std::optional<Event>{*ev};
	}

	// object
	if (top.awaiting_value) {
		top.awaiting_value = false;
		if (auto ok = skip_ws_checked(); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		value_start_ = pos_;
		auto ev = parse_value_event();
		if (!ev) {
			set_error(ev.error());
			return std::unexpected(last_error_);
		}
		return std::optional<Event>{*ev};
	}

	if (auto ok = skip_ws_checked(); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	if (pos_ < input_.size() && input_[pos_] == '}') {
		adv();
		stack_.pop_back();
		return std::optional<Event>{Event::end_object};
	}
	if (!top.first) {
		if (pos_ >= input_.size() || input_[pos_] != ',') {
			auto e = mk_err(JsonIssueCode::syntax_error, "std::expected ',' or '}'");
			set_error(e);
			return std::unexpected(last_error_);
		}
		adv();
		if (auto ok = skip_ws_checked(); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		if (opts_.mode == ParseMode::json5 && pos_ < input_.size() && input_[pos_] == '}') {
			adv();
			stack_.pop_back();
			return std::optional<Event>{Event::end_object};
		}
	}
	top.first = false;
	if (pos_ >= input_.size()) {
		auto e = mk_err(JsonIssueCode::unexpected_eof, "EOF in object");
		set_error(e);
		return std::unexpected(last_error_);
	}
	std::expected<void, JsonError> str_res =
		std::unexpected(mk_err(JsonIssueCode::syntax_error, "std::expected string key"));
	if (input_[pos_] == '"') {
		adv();
		str_res = parse_str_into_token(opts_.max_string_size, key_token_);
	} else if (input_[pos_] == '\'' && opts_.mode == ParseMode::json5) {
		adv();
		str_res = parse_str_sq_into_token(opts_.max_string_size, key_token_);
	} else if (opts_.mode == ParseMode::json5) {
		std::size_t const key_start = pos_;
		char const fc = input_[pos_];
		if ((fc >= 'A' && fc <= 'Z') || (fc >= 'a' && fc <= 'z') || fc == '_' || fc == '$') {
			adv();
			while (pos_ < input_.size()) {
				char const ch = input_[pos_];
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
			std::string_view raw_lex = input_.substr(key_start, pos_ - key_start);
			key_token_ = JsonStringToken{raw_lex, false, opts_.max_string_size};
			key_token_.unquoted_ = true;
			str_res = {};
		}
	}
	if (!str_res) {
		set_error(str_res.error());
		return std::unexpected(last_error_);
	}
	if (auto ok = skip_ws_checked(); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	if (pos_ >= input_.size() || input_[pos_] != ':') {
		auto e = mk_err(JsonIssueCode::syntax_error, "std::expected ':'");
		set_error(e);
		return std::unexpected(last_error_);
	}
	adv();
	top.awaiting_value = true;
	return std::optional<Event>{Event::key};
}

JsonStringToken JsonReader::key_token() const noexcept {
	return key_token_;
}

JsonStringToken JsonReader::string_token() const noexcept {
	return str_token_;
}

JsonNumberView JsonReader::number_val() const noexcept {
	return num_val_;
}

bool JsonReader::bool_val() const noexcept {
	return bool_val_;
}

std::string_view JsonReader::input() const noexcept {
	return input_;
}

std::size_t JsonReader::depth() const noexcept {
	return stack_.size();
}

bool JsonReader::has_error() const noexcept {
	return has_error_;
}

std::size_t JsonReader::pos() const noexcept {
	return pos_;
}

std::size_t JsonReader::value_start_pos() const noexcept {
	return value_start_;
}

void JsonReader::reset() noexcept {
	pos_ = 0;
	line_ = 1;
	col_ = 1;
	stack_.clear();
	has_error_ = false;
	last_error_ = {};
}

std::expected<JsonByteRange, JsonError> JsonReader::skip_next_value() {
	if (has_error_) {
		return std::unexpected(last_error_);
	}
	if (auto ok = skip_ws_checked(); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	std::size_t const start = pos_;
	auto ev = next();
	if (!ev) {
		return std::unexpected(std::move(ev).error());
	}
	if (!*ev) {
		auto e = JsonError{
			.stage = JsonStage::parse,
			.code = JsonIssueCode::unexpected_eof,
			.source = JsonSourceLocation{.offset = start},
			.message = "std::unexpected end of input"};
		set_error(e);
		return std::unexpected(last_error_);
	}
	int depth = 1;
	if (**ev == Event::string_value
		|| **ev == Event::number_value
		|| **ev == Event::bool_value
		|| **ev == Event::null_value) {
		return JsonByteRange{start, pos_};
	}
	while (depth > 0) {
		auto ne = next();
		if (!ne) {
			return std::unexpected(std::move(ne).error());
		}
		if (!*ne) {
			auto e = mk_err(JsonIssueCode::unexpected_eof, "EOF while skipping");
			set_error(e);
			return std::unexpected(last_error_);
		}
		if (**ne == Event::begin_object || **ne == Event::begin_array) {
			++depth;
		} else if (**ne == Event::end_object || **ne == Event::end_array) {
			--depth;
		}
	}
	return JsonByteRange{start, pos_};
}
