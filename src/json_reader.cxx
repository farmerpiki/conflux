module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

namespace {

[[nodiscard]] bool is_digit_char(
	char c) noexcept {
	return c >= '0' && c <= '9';
}

[[nodiscard]] std::size_t scan_digits_fast(
	char const *p,
	std::size_t n) noexcept {
	std::size_t i = 0;
	if constexpr (std::endian::native == std::endian::little) {
		constexpr std::uint64_t kLow = 0x3030303030303030ULL;
		constexpr std::uint64_t kHigh = 0x3939393939393939ULL;
		constexpr std::uint64_t kMsb = 0x8080808080808080ULL;
		while (i + sizeof(std::uint64_t) <= n) {
			std::uint64_t word{};
			std::memcpy(&word, p + i, sizeof(word));
			std::uint64_t const below = (word - kLow) & ~word & kMsb;
			std::uint64_t const above = ((word + (0x7f7f7f7f7f7f7f7fULL - kHigh)) | word) & kMsb;
			std::uint64_t const bad = below | above;
			if (bad != 0U) {
				return i + static_cast<std::size_t>(__builtin_ctzll(bad) >> 3U);
			}
			i += sizeof(std::uint64_t);
		}
	}
	while (i < n && is_digit_char(p[i])) {
		++i;
	}
	return i;
}

} // namespace

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

template<ParseMode Mode>
void JsonReader::skip_ws_impl() {
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
		if constexpr (Mode == ParseMode::json5) {
			if (pos_ + 1 < input_.size() && input_[pos_] == '/') [[unlikely]] {
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
					bool closed = false;
					while (pos_ + 1 < input_.size()) {
						if (input_[pos_] == '*' && input_[pos_ + 1] == '/') {
							pos_ += 2;
							col_ += 2;
							closed = true;
							break;
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
					if (closed) {
						continue;
					}
					pos_ = input_.size();
					set_error(
						JsonError{
							.stage = JsonStage::parse,
							.code = JsonIssueCode::unexpected_eof,
							.source =
								JsonSourceLocation{
												   .offset = comment_offset,
												   .line = comment_line,
												   .column = comment_col},
							.message = "unterminated block comment",
                    });
					return;
				}
			}
		}
		return;
	}
}

void JsonReader::skip_ws() {
	if (opts_.mode == ParseMode::strict) {
		skip_ws_impl<ParseMode::strict>();
		return;
	}
	skip_ws_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::skip_ws_checked_impl() {
	skip_ws_impl<Mode>();
	if (has_error_) [[unlikely]] {
		return std::unexpected(last_error_);
	}
	return {};
}

std::expected<void, JsonError> JsonReader::skip_ws_checked() {
	if (opts_.mode == ParseMode::strict) {
		return skip_ws_checked_impl<ParseMode::strict>();
	}
	return skip_ws_checked_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::skip_ws_checked_fast_impl() {
	if constexpr (Mode == ParseMode::strict) {
		if (pos_ < input_.size() && static_cast<unsigned char>(input_[pos_]) > static_cast<unsigned char>(' ')) {
			return {};
		}
	}
	return skip_ws_checked_impl<Mode>();
}

std::expected<void, JsonError> JsonReader::skip_ws_checked_fast() {
	if (opts_.mode == ParseMode::strict) {
		return skip_ws_checked_fast_impl<ParseMode::strict>();
	}
	return skip_ws_checked_fast_impl<ParseMode::json5>();
}

void JsonReader::adv(
	std::size_t n) noexcept {
	pos_ += n;
	col_ += n;
}

std::expected<std::string_view, JsonError> JsonReader::parse_number_lexeme() {
	std::size_t const start = pos_;
	std::size_t p = pos_;
	auto commit = [&] {
		col_ += p - pos_;
		pos_ = p;
	};

	if (p < input_.size() && input_[p] == '-') {
		++p;
	}
	if (p >= input_.size() || !is_digit_char(input_[p])) [[unlikely]] {
		commit();
		return std::unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after sign"));
	}
	bool const starts_zero = input_[p] == '0';
	++p;
	if (starts_zero && p < input_.size() && is_digit_char(input_[p])) [[unlikely]] {
		commit();
		return std::unexpected(mk_err(JsonIssueCode::syntax_error, "leading zeros forbidden"));
	}
	p += scan_digits_fast(input_.data() + p, input_.size() - p);
	if (p < input_.size() && input_[p] == '.') {
		++p;
		if (p >= input_.size() || !is_digit_char(input_[p])) [[unlikely]] {
			commit();
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "digit required after '.'"));
		}
		p += scan_digits_fast(input_.data() + p, input_.size() - p);
	}
	if (p < input_.size() && (input_[p] == 'e' || input_[p] == 'E')) {
		++p;
		if (p < input_.size() && (input_[p] == '+' || input_[p] == '-')) {
			++p;
		}
		if (p >= input_.size() || !is_digit_char(input_[p])) [[unlikely]] {
			commit();
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "digit required in exponent"));
		}
		p += scan_digits_fast(input_.data() + p, input_.size() - p);
	}
	if (p - start > kMaxNumberLexemeLen) [[unlikely]] {
		commit();
		return std::unexpected(mk_err(JsonIssueCode::invalid_number, "number lexeme exceeds maximum length"));
	}
	commit();
	return input_.substr(start, pos_ - start);
}

std::expected<void, JsonError> JsonReader::parse_number_into_val() {
	auto lex_res = parse_number_lexeme();
	if (!lex_res) [[unlikely]] {
		return std::unexpected(std::move(lex_res).error());
	}
	std::string_view const lex = *lex_res;
	auto node = detail::build_number_node_from_lexeme(0, static_cast<std::uint32_t>(lex.size()), 0, lex);
	if (!node) [[unlikely]] {
		auto err = std::move(node).error();
		err.stage = JsonStage::parse;
		return std::unexpected(std::move(err));
	}
	num_val_ = JsonNumberView{lex, node->flags, node->_raw};
	return {};
}

std::expected<void, JsonError> JsonReader::skip_json5_identifier_key() {
	if (pos_ >= input_.size()) [[unlikely]] {
		return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
	}
	char const fc = input_[pos_];
	if ((fc < 'A' || fc > 'Z') && (fc < 'a' || fc > 'z') && fc != '_' && fc != '$') [[unlikely]] {
		return std::unexpected(mk_err(JsonIssueCode::syntax_error, "expected string key"));
	}
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
	return {};
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::skip_value_raw_impl(
	std::size_t raw_depth) {
	if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
		return std::unexpected(std::move(ok).error());
	}
	if (pos_ >= input_.size()) [[unlikely]] {
		return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "unexpected end of input"));
	}
	char const c = input_[pos_];
	if (c == '"') {
		adv();
		JsonStringToken ignored{};
		return parse_str_into_token(opts_.max_string_size, ignored);
	}
	if constexpr (Mode == ParseMode::json5) {
		if (c == '\'') {
			adv();
			JsonStringToken ignored{};
			return parse_str_sq_into_token(opts_.max_string_size, ignored);
		}
	}
	if (c == '{') {
		if (opts_.max_depth.exceeds(stack_.size() + raw_depth, kDefaultMaxDepth)) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded"));
		}
		adv();
		return skip_object_body_raw_impl<Mode>(raw_depth + 1U);
	}
	if (c == '[') {
		if (opts_.max_depth.exceeds(stack_.size() + raw_depth, kDefaultMaxDepth)) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded"));
		}
		adv();
		return skip_array_body_raw_impl<Mode>(raw_depth + 1U);
	}
	if (c == 't') {
		if (input_.substr(pos_, 4) != "true") [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
		}
		adv(4);
		return {};
	}
	if (c == 'f') {
		if (input_.substr(pos_, 5) != "false") [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
		}
		adv(5);
		return {};
	}
	if (c == 'n') {
		if (input_.substr(pos_, 4) != "null") [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
		}
		adv(4);
		return {};
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		auto lex = parse_number_lexeme();
		if (!lex) [[unlikely]] {
			return std::unexpected(std::move(lex).error());
		}
		return {};
	}
	return std::unexpected(mk_err(JsonIssueCode::syntax_error, std::format("unexpected character '{}'", c)));
}

std::expected<void, JsonError> JsonReader::skip_value_raw(
	std::size_t raw_depth) {
	if (opts_.mode == ParseMode::strict) {
		return skip_value_raw_impl<ParseMode::strict>(raw_depth);
	}
	return skip_value_raw_impl<ParseMode::json5>(raw_depth);
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::skip_array_body_raw_impl(
	std::size_t raw_depth) {
	bool first = true;
	while (true) {
		if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
			return std::unexpected(std::move(ok).error());
		}
		if (pos_ < input_.size() && input_[pos_] == ']') {
			adv();
			return {};
		}
		if (!first) {
			if (pos_ >= input_.size() || input_[pos_] != ',') [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "expected ',' or ']'"));
			}
			adv();
			if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
				return std::unexpected(std::move(ok).error());
			}
			if constexpr (Mode == ParseMode::json5) {
				if (pos_ < input_.size() && input_[pos_] == ']') {
					adv();
					return {};
				}
			}
		}
		first = false;
		if (auto skipped = skip_value_raw_impl<Mode>(raw_depth); !skipped) [[unlikely]] {
			return std::unexpected(std::move(skipped).error());
		}
	}
}

std::expected<void, JsonError> JsonReader::skip_array_body_raw(
	std::size_t raw_depth) {
	if (opts_.mode == ParseMode::strict) {
		return skip_array_body_raw_impl<ParseMode::strict>(raw_depth);
	}
	return skip_array_body_raw_impl<ParseMode::json5>(raw_depth);
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::skip_object_body_raw_impl(
	std::size_t raw_depth) {
	bool first = true;
	while (true) {
		if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
			return std::unexpected(std::move(ok).error());
		}
		if (pos_ < input_.size() && input_[pos_] == '}') {
			adv();
			return {};
		}
		if (!first) {
			if (pos_ >= input_.size() || input_[pos_] != ',') [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "expected ',' or '}'"));
			}
			adv();
			if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
				return std::unexpected(std::move(ok).error());
			}
			if constexpr (Mode == ParseMode::json5) {
				if (pos_ < input_.size() && input_[pos_] == '}') {
					adv();
					return {};
				}
			}
		}
		first = false;
		if (pos_ >= input_.size()) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
		}
		if (input_[pos_] == '"') {
			adv();
			JsonStringToken ignored{};
			if (auto key = parse_str_into_token(opts_.max_string_size, ignored); !key) [[unlikely]] {
				return std::unexpected(std::move(key).error());
			}
		} else {
			if constexpr (Mode == ParseMode::json5) {
				if (input_[pos_] == '\'') {
					adv();
					JsonStringToken ignored{};
					if (auto key = parse_str_sq_into_token(opts_.max_string_size, ignored); !key) [[unlikely]] {
						return std::unexpected(std::move(key).error());
					}
				} else if (auto key = skip_json5_identifier_key(); !key) [[unlikely]] {
					return std::unexpected(std::move(key).error());
				}
			} else {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "expected string key"));
			}
		}
		if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
			return std::unexpected(std::move(ok).error());
		}
		if (pos_ >= input_.size() || input_[pos_] != ':') [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "expected ':'"));
		}
		adv();
		if (auto skipped = skip_value_raw_impl<Mode>(raw_depth); !skipped) [[unlikely]] {
			return std::unexpected(std::move(skipped).error());
		}
	}
}

std::expected<void, JsonError> JsonReader::skip_object_body_raw(
	std::size_t raw_depth) {
	if (opts_.mode == ParseMode::strict) {
		return skip_object_body_raw_impl<ParseMode::strict>(raw_depth);
	}
	return skip_object_body_raw_impl<ParseMode::json5>(raw_depth);
}

template<ParseMode Mode>
std::expected<std::size_t, JsonError> JsonReader::count_array_elements_raw_impl() {
	bool first = stack_.empty() ? true : stack_.back().first;
	std::size_t count = 0;
	while (true) {
		if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
			return std::unexpected(std::move(ok).error());
		}
		if (pos_ < input_.size() && input_[pos_] == ']') {
			return count;
		}
		if (!first) {
			if (pos_ >= input_.size() || input_[pos_] != ',') [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "expected ',' or ']'"));
			}
			adv();
			if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
				return std::unexpected(std::move(ok).error());
			}
			if constexpr (Mode == ParseMode::json5) {
				if (pos_ < input_.size() && input_[pos_] == ']') {
					return count;
				}
			}
		}
		first = false;
		if (auto skipped = skip_value_raw_impl<Mode>(0); !skipped) [[unlikely]] {
			return std::unexpected(std::move(skipped).error());
		}
		++count;
	}
}

std::expected<std::size_t, JsonError> JsonReader::count_array_elements_raw() {
	if (opts_.mode == ParseMode::strict) {
		return count_array_elements_raw_impl<ParseMode::strict>();
	}
	return count_array_elements_raw_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<std::size_t, JsonError> JsonReader::count_object_members_raw_impl() {
	bool first = stack_.empty() ? true : stack_.back().first;
	bool awaiting_value = !stack_.empty() && stack_.back().awaiting_value;
	std::size_t count = 0;
	if (awaiting_value) {
		if (auto skipped = skip_value_raw_impl<Mode>(0); !skipped) [[unlikely]] {
			return std::unexpected(std::move(skipped).error());
		}
		first = false;
	}
	while (true) {
		if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
			return std::unexpected(std::move(ok).error());
		}
		if (pos_ < input_.size() && input_[pos_] == '}') {
			return count;
		}
		if (!first) {
			if (pos_ >= input_.size() || input_[pos_] != ',') [[unlikely]] {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "expected ',' or '}'"));
			}
			adv();
			if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
				return std::unexpected(std::move(ok).error());
			}
			if constexpr (Mode == ParseMode::json5) {
				if (pos_ < input_.size() && input_[pos_] == '}') {
					return count;
				}
			}
		}
		first = false;
		if (pos_ >= input_.size()) [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::unexpected_eof, "EOF in object"));
		}
		if (input_[pos_] == '"') {
			adv();
			JsonStringToken ignored{};
			if (auto key = parse_str_into_token(opts_.max_string_size, ignored); !key) [[unlikely]] {
				return std::unexpected(std::move(key).error());
			}
		} else {
			if constexpr (Mode == ParseMode::json5) {
				if (input_[pos_] == '\'') {
					adv();
					JsonStringToken ignored{};
					if (auto key = parse_str_sq_into_token(opts_.max_string_size, ignored); !key) [[unlikely]] {
						return std::unexpected(std::move(key).error());
					}
				} else if (auto key = skip_json5_identifier_key(); !key) [[unlikely]] {
					return std::unexpected(std::move(key).error());
				}
			} else {
				return std::unexpected(mk_err(JsonIssueCode::syntax_error, "expected string key"));
			}
		}
		if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
			return std::unexpected(std::move(ok).error());
		}
		if (pos_ >= input_.size() || input_[pos_] != ':') [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "expected ':'"));
		}
		adv();
		if (auto skipped = skip_value_raw_impl<Mode>(0); !skipped) [[unlikely]] {
			return std::unexpected(std::move(skipped).error());
		}
		++count;
	}
}

std::expected<std::size_t, JsonError> JsonReader::count_object_members_raw() {
	if (opts_.mode == ParseMode::strict) {
		return count_object_members_raw_impl<ParseMode::strict>();
	}
	return count_object_members_raw_impl<ParseMode::json5>();
}

std::size_t JsonReader::initial_array_reserve_hint(
	std::size_t element_size) noexcept {
	if (element_size == 0) {
		return 0;
	}
	constexpr std::size_t kInitialArrayReserveElements = 4U;
	constexpr std::size_t kMaxInitialArrayReserveBytes = 4096U;
	std::size_t const max_elems_by_bytes = std::max<std::size_t>(1, kMaxInitialArrayReserveBytes / element_size);
	return std::min(kInitialArrayReserveElements, max_elems_by_bytes);
}

std::expected<void, JsonError> JsonReader::push_frame(
	StateFrame frame) {
	try {
		stack_.push_back(frame);
	} catch (std::bad_alloc const &) {
		return std::unexpected(mk_err(JsonIssueCode::resource_exhausted, "OOM growing reader state stack"));
	}
	return {};
}

template<ParseMode Mode>
std::expected<JsonReader::Event, JsonError> JsonReader::parse_value_event_impl() {
	if (opts_.max_depth.exceeds(stack_.size(), kDefaultMaxDepth)) [[unlikely]] {
		return std::unexpected(mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded"));
	}
	if (pos_ >= input_.size()) [[unlikely]] {
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
	if constexpr (Mode == ParseMode::json5) {
		if (c == '\'') {
			adv();
			auto res = parse_str_sq_into_token(opts_.max_string_size, str_token_);
			if (!res) {
				return std::unexpected(std::move(res).error());
			}
			return Event::string_value;
		}
	}
	if (c == '{') {
		adv();
		if (auto pushed =
				push_frame(StateFrame{.kind = StateFrame::Kind::object, .first = true, .awaiting_value = false});
			!pushed) {
			return std::unexpected(std::move(pushed).error());
		}
		return Event::begin_object;
	}
	if (c == '[') {
		adv();
		if (auto pushed =
				push_frame(StateFrame{.kind = StateFrame::Kind::array, .first = true, .awaiting_value = false});
			!pushed) {
			return std::unexpected(std::move(pushed).error());
		}
		return Event::begin_array;
	}
	if (c == 't') {
		if (input_.substr(pos_, 4) != "true") [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
		}
		adv(4);
		bool_val_ = true;
		return Event::bool_value;
	}
	if (c == 'f') {
		if (input_.substr(pos_, 5) != "false") [[unlikely]] {
			return std::unexpected(mk_err(JsonIssueCode::syntax_error, "invalid token"));
		}
		adv(5);
		bool_val_ = false;
		return Event::bool_value;
	}
	if (c == 'n') {
		if (input_.substr(pos_, 4) != "null") [[unlikely]] {
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

std::expected<JsonReader::Event, JsonError> JsonReader::parse_value_event() {
	if (opts_.mode == ParseMode::strict) {
		return parse_value_event_impl<ParseMode::strict>();
	}
	return parse_value_event_impl<ParseMode::json5>();
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

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::parse_object_key_token_impl() {
	if (pos_ >= input_.size()) [[unlikely]] {
		auto e = mk_err(JsonIssueCode::unexpected_eof, "EOF in object");
		set_error(e);
		return std::unexpected(last_error_);
	}
	if (input_[pos_] == '"') {
		adv();
		if (auto str_res = parse_str_into_token(opts_.max_string_size, key_token_); !str_res) [[unlikely]] {
			set_error(str_res.error());
			return std::unexpected(last_error_);
		}
		return {};
	}
	if constexpr (Mode == ParseMode::json5) {
		if (input_[pos_] == '\'') {
			adv();
			if (auto str_res = parse_str_sq_into_token(opts_.max_string_size, key_token_); !str_res) [[unlikely]] {
				set_error(str_res.error());
				return std::unexpected(last_error_);
			}
			return {};
		}
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
			return {};
		}
	}
	auto e = mk_err(JsonIssueCode::syntax_error, "std::expected string key");
	set_error(e);
	return std::unexpected(last_error_);
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::finish_object_key_parse_impl(
	StateFrame &top) {
	if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
		return std::unexpected(std::move(ok).error());
	}
	if (pos_ >= input_.size() || input_[pos_] != ':') [[unlikely]] {
		auto e = mk_err(JsonIssueCode::syntax_error, "std::expected ':'");
		set_error(e);
		return std::unexpected(last_error_);
	}
	adv();
	top.awaiting_value = true;
	return {};
}

template<ParseMode Mode>
std::expected<std::optional<JsonReader::Event>, JsonError> JsonReader::next_impl() {
	if (has_error_) [[unlikely]] {
		return std::unexpected(last_error_);
	}
	if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
		return std::unexpected(std::move(ok).error());
	}

	if (stack_.empty()) {
		if (pos_ >= input_.size()) {
			return std::optional<Event>{};
		}
		value_start_ = pos_;
		auto ev = parse_value_event_impl<Mode>();
		if (!ev) {
			set_error(ev.error());
			return std::unexpected(last_error_);
		}
		return std::optional<Event>{*ev};
	}

	auto &top = stack_.back();

	if (top.kind == StateFrame::Kind::array) {
		if (pos_ < input_.size() && input_[pos_] == ']') {
			adv();
			stack_.pop_back();
			return std::optional<Event>{Event::end_array};
		}
		if (!top.first) {
			if (pos_ >= input_.size() || input_[pos_] != ',') [[unlikely]] {
				auto e = mk_err(JsonIssueCode::syntax_error, "std::expected ',' or ']'");
				set_error(e);
				return std::unexpected(last_error_);
			}
			adv();
			if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
				return std::unexpected(std::move(ok).error());
			}
			if constexpr (Mode == ParseMode::json5) {
				if (pos_ < input_.size() && input_[pos_] == ']') {
					adv();
					stack_.pop_back();
					return std::optional<Event>{Event::end_array};
				}
			}
		}
		top.first = false;
		value_start_ = pos_;
		auto ev = parse_value_event_impl<Mode>();
		if (!ev) {
			set_error(ev.error());
			return std::unexpected(last_error_);
		}
		return std::optional<Event>{*ev};
	}

	// object
	if (top.awaiting_value) {
		top.awaiting_value = false;
		value_start_ = pos_;
		auto ev = parse_value_event_impl<Mode>();
		if (!ev) {
			set_error(ev.error());
			return std::unexpected(last_error_);
		}
		return std::optional<Event>{*ev};
	}

	if (pos_ < input_.size() && input_[pos_] == '}') {
		adv();
		stack_.pop_back();
		return std::optional<Event>{Event::end_object};
	}
	if (!top.first) {
		if (pos_ >= input_.size() || input_[pos_] != ',') [[unlikely]] {
			auto e = mk_err(JsonIssueCode::syntax_error, "std::expected ',' or '}'");
			set_error(e);
			return std::unexpected(last_error_);
		}
		adv();
		if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
			return std::unexpected(std::move(ok).error());
		}
		if constexpr (Mode == ParseMode::json5) {
			if (pos_ < input_.size() && input_[pos_] == '}') {
				adv();
				stack_.pop_back();
				return std::optional<Event>{Event::end_object};
			}
		}
	}
	top.first = false;
	if (auto key = parse_object_key_token_impl<Mode>(); !key) [[unlikely]] {
		return std::unexpected(std::move(key).error());
	}
	if (auto key = finish_object_key_parse_impl<Mode>(top); !key) [[unlikely]] {
		return std::unexpected(std::move(key).error());
	}
	return std::optional<Event>{Event::key};
}

std::expected<std::optional<JsonReader::Event>, JsonError> JsonReader::next() {
	if (opts_.mode == ParseMode::strict) {
		return next_impl<ParseMode::strict>();
	}
	return next_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<JsonReader::Event, JsonError> JsonReader::next_object_value_event_impl() {
	auto prepared = prepare_object_value_impl<Mode>();
	if (!prepared) [[unlikely]] {
		return std::unexpected(std::move(prepared).error());
	}
	auto ev = parse_value_event_impl<Mode>();
	if (!ev) {
		set_error(ev.error());
		return std::unexpected(last_error_);
	}
	return *ev;
}

std::expected<JsonReader::Event, JsonError> JsonReader::next_object_value_event() {
	if (opts_.mode == ParseMode::strict) {
		return next_object_value_event_impl<ParseMode::strict>();
	}
	return next_object_value_event_impl<ParseMode::json5>();
}

#if defined(__GNUC__) || defined(__clang__)
	#define CONFLUX_JSON_READER_SECTION(name) __attribute__((section(name)))
#else
	#define CONFLUX_JSON_READER_SECTION(name)
#endif

#define CONFLUX_JSON_READER_INSTANTIATE_MODE(mode_name, mode_value)                                      \
	template void JsonReader::skip_ws_impl<mode_value>();                                                \
	template std::expected<void, JsonError> JsonReader::skip_ws_checked_fast_impl<mode_value>();          \
	template std::expected<std::size_t, JsonError> JsonReader::count_array_elements_raw_impl<mode_value>(); \
	template CONFLUX_JSON_READER_SECTION(".text.conflux.json.reader." mode_name ".parse-value")         \
	std::expected<JsonReader::Event, JsonError> JsonReader::parse_value_event_impl<mode_value>();        \
	template CONFLUX_JSON_READER_SECTION(".text.conflux.json.reader." mode_name ".next")                \
	std::expected<std::optional<JsonReader::Event>, JsonError> JsonReader::next_impl<mode_value>();      \
	template CONFLUX_JSON_READER_SECTION(".text.conflux.json.reader." mode_name ".object-value")        \
	std::expected<JsonReader::Event, JsonError> JsonReader::next_object_value_event_impl<mode_value>()

CONFLUX_JSON_READER_INSTANTIATE_MODE("strict", ParseMode::strict);
CONFLUX_JSON_READER_INSTANTIATE_MODE("json5", ParseMode::json5);

#undef CONFLUX_JSON_READER_INSTANTIATE_MODE
#undef CONFLUX_JSON_READER_SECTION

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

JsonParseOptions const &JsonReader::parse_options() const noexcept {
	return opts_;
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

template<ParseMode Mode>
std::expected<std::size_t, JsonError> JsonReader::count_remaining_array_elements_impl() {
	if (stack_.empty() || stack_.back().kind != StateFrame::Kind::array) {
		return std::unexpected(mk_err(JsonIssueCode::wrong_kind, "reader is not positioned inside an array"));
	}
	auto checkpoint_state = checkpoint();
	auto count = count_array_elements_raw_impl<Mode>();
	if (!count) {
		auto err = std::move(count).error();
		restore(std::move(checkpoint_state));
		return std::unexpected(std::move(err));
	}
	std::size_t const value = *count;
	restore(std::move(checkpoint_state));
	return value;
}

std::expected<std::size_t, JsonError> JsonReader::count_remaining_array_elements() {
	if (opts_.mode == ParseMode::strict) {
		return count_remaining_array_elements_impl<ParseMode::strict>();
	}
	return count_remaining_array_elements_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<std::size_t, JsonError> JsonReader::count_remaining_object_members_impl() {
	if (stack_.empty() || stack_.back().kind != StateFrame::Kind::object) {
		return std::unexpected(mk_err(JsonIssueCode::wrong_kind, "reader is not positioned inside an object"));
	}
	auto checkpoint_state = checkpoint();
	auto count = count_object_members_raw_impl<Mode>();
	if (!count) {
		auto err = std::move(count).error();
		restore(std::move(checkpoint_state));
		return std::unexpected(std::move(err));
	}
	std::size_t const value = *count;
	restore(std::move(checkpoint_state));
	return value;
}

std::expected<std::size_t, JsonError> JsonReader::count_remaining_object_members() {
	if (opts_.mode == ParseMode::strict) {
		return count_remaining_object_members_impl<ParseMode::strict>();
	}
	return count_remaining_object_members_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::skip_remaining_value_impl(
	Event event) {
	if (event == Event::string_value
		|| event == Event::number_value
		|| event == Event::bool_value
		|| event == Event::null_value) {
		return {};
	}
	if (event == Event::begin_array) {
		if (auto skipped = skip_array_body_raw_impl<Mode>(0); !skipped) {
			set_error(skipped.error());
			return std::unexpected(last_error_);
		}
		stack_.pop_back();
		return {};
	}
	if (event == Event::begin_object) {
		if (auto skipped = skip_object_body_raw_impl<Mode>(0); !skipped) {
			set_error(skipped.error());
			return std::unexpected(last_error_);
		}
		stack_.pop_back();
		return {};
	}
	return std::unexpected(mk_err(JsonIssueCode::wrong_kind, "event is not a value"));
}

std::expected<void, JsonError> JsonReader::skip_remaining_value(
	Event event) {
	if (opts_.mode == ParseMode::strict) {
		return skip_remaining_value_impl<ParseMode::strict>(event);
	}
	return skip_remaining_value_impl<ParseMode::json5>(event);
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
	if (auto skipped = skip_remaining_value(**ev); !skipped) {
		return std::unexpected(std::move(skipped).error());
	}
	return JsonByteRange{start, pos_};
}

} // namespace conflux::json
