module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

namespace {

[[nodiscard]] std::expected<std::string_view, JsonError> decode_string_token_view(
	JsonStringToken const &token,
	JsonDecodeScratch &scratch) {
	if (auto borrowed = token.unescaped_borrow()) {
		return *borrowed;
	}
	std::size_t const needed = token.max_decoded_size();
	if (needed <= scratch.string_inline.size()) {
		return token.decode_into(std::span<char>{scratch.string_inline.data(), scratch.string_inline.size()});
	}
	scratch.string_overflow.resize(needed);
	return token.decode_into(std::span<char>{scratch.string_overflow.data(), scratch.string_overflow.size()});
}

} // namespace

template<ParseMode Mode>
std::expected<bool, JsonError> JsonReader::try_next_object_null_value_impl() {
	Checkpoint checkpoint_before_value = checkpoint();
	auto prepared = prepare_object_value_impl<Mode>();
	if (!prepared) [[unlikely]] {
		return std::unexpected(std::move(prepared).error());
	}
	if (pos_ < input_.size() && input_[pos_] == 'n') {
		if (input_.substr(pos_, 4) != "null") [[unlikely]] {
			auto e = mk_err(JsonIssueCode::syntax_error, "invalid token");
			set_error(e);
			return std::unexpected(last_error_);
		}
		adv(4);
		return true;
	}
	restore(std::move(checkpoint_before_value));
	return false;
}

std::expected<bool, JsonError> JsonReader::try_next_object_null_value() {
	if (opts_.mode == ParseMode::strict) {
		return try_next_object_null_value_impl<ParseMode::strict>();
	}
	return try_next_object_null_value_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::next_object_array_value_impl() {
	auto prepared = prepare_object_value_impl<Mode>();
	if (!prepared) [[unlikely]] {
		return std::unexpected(std::move(prepared).error());
	}
	if (opts_.max_depth.exceeds(stack_.size(), kDefaultMaxDepth)) [[unlikely]] {
		auto e = mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded");
		set_error(e);
		return std::unexpected(last_error_);
	}
	if (pos_ >= input_.size()) [[unlikely]] {
		auto e = mk_err(JsonIssueCode::unexpected_eof, "EOF in object array value");
		set_error(e);
		return std::unexpected(last_error_);
	}
	if (input_[pos_] != '[') [[unlikely]] {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::wrong_kind,
				.source = JsonSourceLocation{.offset = pos_, .line = line_, .column = col_},
				.message = "expected array"
        });
	}
	adv();
	if (auto pushed = push_frame(StateFrame{.kind = StateFrame::Kind::array, .first = true, .awaiting_value = false});
		!pushed) [[unlikely]] {
		set_error(pushed.error());
		return std::unexpected(last_error_);
	}
	return {};
}

std::expected<void, JsonError> JsonReader::next_object_array_value() {
	if (opts_.mode == ParseMode::strict) {
		return next_object_array_value_impl<ParseMode::strict>();
	}
	return next_object_array_value_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::next_object_object_value_impl() {
	auto prepared = prepare_object_value_impl<Mode>();
	if (!prepared) [[unlikely]] {
		return std::unexpected(std::move(prepared).error());
	}
	if (opts_.max_depth.exceeds(stack_.size(), kDefaultMaxDepth)) [[unlikely]] {
		auto e = mk_err(JsonIssueCode::nesting_too_deep, "nesting depth limit exceeded");
		set_error(e);
		return std::unexpected(last_error_);
	}
	if (pos_ >= input_.size()) [[unlikely]] {
		auto e = mk_err(JsonIssueCode::unexpected_eof, "EOF in object object value");
		set_error(e);
		return std::unexpected(last_error_);
	}
	if (input_[pos_] != '{') [[unlikely]] {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::wrong_kind,
				.source = JsonSourceLocation{.offset = pos_, .line = line_, .column = col_},
				.message = "expected object"
        });
	}
	adv();
	if (auto pushed = push_frame(StateFrame{.kind = StateFrame::Kind::object, .first = true, .awaiting_value = false});
		!pushed) [[unlikely]] {
		set_error(pushed.error());
		return std::unexpected(last_error_);
	}
	return {};
}

std::expected<void, JsonError> JsonReader::next_object_object_value() {
	if (opts_.mode == ParseMode::strict) {
		return next_object_object_value_impl<ParseMode::strict>();
	}
	return next_object_object_value_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::skip_next_object_value_impl() {
	auto prepared = prepare_object_value_impl<Mode>();
	if (!prepared) [[unlikely]] {
		return std::unexpected(std::move(prepared).error());
	}
	if (auto skipped = skip_value_raw_impl<Mode>(0); !skipped) [[unlikely]] {
		set_error(skipped.error());
		return std::unexpected(last_error_);
	}
	return {};
}

std::expected<void, JsonError> JsonReader::skip_next_object_value() {
	if (opts_.mode == ParseMode::strict) {
		return skip_next_object_value_impl<ParseMode::strict>();
	}
	return skip_next_object_value_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<JsonStringToken, JsonError> JsonReader::next_object_string_value_token_impl() {
	auto prepared = prepare_object_value_impl<Mode>();
	if (!prepared) [[unlikely]] {
		return std::unexpected(std::move(prepared).error());
	}
	if (pos_ >= input_.size()) [[unlikely]] {
		auto e = mk_err(JsonIssueCode::unexpected_eof, "EOF in object string value");
		set_error(e);
		return std::unexpected(last_error_);
	}
	if (input_[pos_] == '"') {
		adv();
		if (auto str_res = parse_str_into_token(opts_.max_string_size, str_token_); !str_res) [[unlikely]] {
			set_error(str_res.error());
			return std::unexpected(last_error_);
		}
		return str_token_;
	}
	if constexpr (Mode == ParseMode::json5) {
		if (input_[pos_] == '\'') {
			adv();
			if (auto str_res = parse_str_sq_into_token(opts_.max_string_size, str_token_); !str_res) [[unlikely]] {
				set_error(str_res.error());
				return std::unexpected(last_error_);
			}
			return str_token_;
		}
	}
	return std::unexpected(
		JsonError{
			.stage = JsonStage::decode,
			.code = JsonIssueCode::wrong_kind,
			.source = JsonSourceLocation{.offset = pos_, .line = line_, .column = col_},
			.message = "expected string"
    });
}

std::expected<JsonStringToken, JsonError> JsonReader::next_object_string_value_token() {
	if (opts_.mode == ParseMode::strict) {
		return next_object_string_value_token_impl<ParseMode::strict>();
	}
	return next_object_string_value_token_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<std::string_view, JsonError> JsonReader::next_object_string_value_view_impl(
	JsonDecodeScratch &scratch) {
	auto prepared = prepare_object_value_impl<Mode>();
	if (!prepared) [[unlikely]] {
		return std::unexpected(std::move(prepared).error());
	}
	if (pos_ >= input_.size()) [[unlikely]] {
		auto e = mk_err(JsonIssueCode::unexpected_eof, "EOF in object string value");
		set_error(e);
		return std::unexpected(last_error_);
	}
	if (input_[pos_] == '"') {
		adv();
		std::size_t const body_start = pos_;
		std::size_t const body_col = col_;
		auto const short_scan = scan_short_string_body(input_, pos_);
		std::size_t skip = short_scan.count;
		if (!short_scan.closed && !short_scan.special) {
			skip += detail::simd::scan_str_until_special(input_.data() + pos_ + skip, input_.size() - pos_ - skip);
		}
		pos_ += skip;
		col_ += skip;
		if (short_scan.closed || (pos_ < input_.size() && input_[pos_] == '"')) {
			std::string_view const body = input_.substr(body_start, skip);
			if (opts_.max_string_size.exceeds(body.size(), kDefaultMaxString)) [[unlikely]] {
				auto e = mk_err(JsonIssueCode::string_too_large, "string exceeds max_string_size");
				set_error(e);
				return std::unexpected(last_error_);
			}
			adv();
			return body;
		}
		pos_ = body_start;
		col_ = body_col;
		if (auto str_res = parse_str_into_token(opts_.max_string_size, str_token_); !str_res) [[unlikely]] {
			set_error(str_res.error());
			return std::unexpected(last_error_);
		}
		return decode_string_token_view(str_token_, scratch);
	}
	if constexpr (Mode == ParseMode::json5) {
		if (input_[pos_] == '\'') {
			adv();
			if (auto str_res = parse_str_sq_into_token(opts_.max_string_size, str_token_); !str_res) [[unlikely]] {
				set_error(str_res.error());
				return std::unexpected(last_error_);
			}
			return decode_string_token_view(str_token_, scratch);
		}
	}
	return std::unexpected(
		JsonError{
			.stage = JsonStage::decode,
			.code = JsonIssueCode::wrong_kind,
			.source = JsonSourceLocation{.offset = pos_, .line = line_, .column = col_},
			.message = "expected string"
    });
}

std::expected<std::string_view, JsonError> JsonReader::next_object_string_value_view(
	JsonDecodeScratch &scratch) {
	if (opts_.mode == ParseMode::strict) {
		return next_object_string_value_view_impl<ParseMode::strict>(scratch);
	}
	return next_object_string_value_view_impl<ParseMode::json5>(scratch);
}

template<ParseMode Mode>
std::expected<void, JsonError> JsonReader::next_object_bool_value_impl(
	bool &out) {
	auto prepared = prepare_object_value_impl<Mode>();
	if (!prepared) [[unlikely]] {
		return std::unexpected(std::move(prepared).error());
	}
	if (pos_ + 4U <= input_.size() && input_.substr(pos_, 4) == "true") {
		adv(4);
		out = true;
		return {};
	}
	if (pos_ + 5U <= input_.size() && input_.substr(pos_, 5) == "false") {
		adv(5);
		out = false;
		return {};
	}
	return std::unexpected(
		JsonError{
			.stage = JsonStage::decode,
			.code = JsonIssueCode::wrong_kind,
			.source = JsonSourceLocation{.offset = pos_, .line = line_, .column = col_},
			.message = "expected bool"
    });
}

std::expected<void, JsonError> JsonReader::next_object_bool_value(
	bool &out) {
	if (opts_.mode == ParseMode::strict) {
		return next_object_bool_value_impl<ParseMode::strict>(out);
	}
	return next_object_bool_value_impl<ParseMode::json5>(out);
}

#define CONFLUX_JSON_READER_OBJECT_VALUE_INSTANTIATE_MODE(mode_value) \
	template std::expected<bool, JsonError> JsonReader::try_next_object_null_value_impl<mode_value>(); \
	template std::expected<void, JsonError> JsonReader::next_object_array_value_impl<mode_value>(); \
	template std::expected<void, JsonError> JsonReader::next_object_object_value_impl<mode_value>(); \
	template std::expected<void, JsonError> JsonReader::skip_next_object_value_impl<mode_value>(); \
	template std::expected<JsonStringToken, JsonError> JsonReader::next_object_string_value_token_impl<mode_value>(); \
	template std::expected<std::string_view, JsonError> JsonReader::next_object_string_value_view_impl<mode_value>(JsonDecodeScratch &scratch); \
	template std::expected<void, JsonError> JsonReader::next_object_bool_value_impl<mode_value>(bool &out)

CONFLUX_JSON_READER_OBJECT_VALUE_INSTANTIATE_MODE(ParseMode::strict);
CONFLUX_JSON_READER_OBJECT_VALUE_INSTANTIATE_MODE(ParseMode::json5);

#undef CONFLUX_JSON_READER_OBJECT_VALUE_INSTANTIATE_MODE

} // namespace conflux::json
