module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

namespace {

[[nodiscard]] std::expected<std::string_view, JsonError> decode_key_token_view(
	JsonStringToken const &token,
	JsonDecodeScratch &scratch) {
	if (auto borrowed = token.unescaped_borrow()) {
		return *borrowed;
	}
	std::size_t const needed = token.max_decoded_size();
	if (needed <= scratch.key_inline.size()) {
		return token.decode_into(std::span<char>{scratch.key_inline.data(), scratch.key_inline.size()});
	}
	scratch.key_overflow.resize(needed);
	return token.decode_into(std::span<char>{scratch.key_overflow.data(), scratch.key_overflow.size()});
}

[[nodiscard]] bool raw_json_name_fast_path_safe(
	std::string_view name) noexcept {
	for (char ch: name) {
		auto const c = static_cast<unsigned char>(ch);
		if (c < 0x20U || c == '\"' || c == '\\' || c >= 0x80U) {
			return false;
		}
	}
	return true;
}

} // namespace

template<ParseMode Mode>
std::expected<std::optional<JsonStringToken>, JsonError> JsonReader::next_object_key_token_impl() {
	if (has_error_) [[unlikely]] {
		return std::unexpected(last_error_);
	}
	if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
		return std::unexpected(std::move(ok).error());
	}
	if (stack_.empty() || stack_.back().kind != StateFrame::Kind::object || stack_.back().awaiting_value) [[unlikely]] {
		return std::unexpected(mk_err(JsonIssueCode::wrong_kind, "reader is not positioned at an object key"));
	}

	auto &top = stack_.back();
	if (pos_ < input_.size() && input_[pos_] == '}') {
		adv();
		stack_.pop_back();
		return std::optional<JsonStringToken>{};
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
				return std::optional<JsonStringToken>{};
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
	return std::optional<JsonStringToken>{key_token_};
}

std::expected<std::optional<JsonStringToken>, JsonError> JsonReader::next_object_key_token() {
	if (opts_.mode == ParseMode::strict) {
		return next_object_key_token_impl<ParseMode::strict>();
	}
	return next_object_key_token_impl<ParseMode::json5>();
}

template<ParseMode Mode>
std::expected<JsonReader::ObjectKeyMatch, JsonError> JsonReader::next_object_key_match_impl(
	std::string_view expected_key,
	JsonDecodeScratch &scratch,
	bool expected_key_raw_json_safe) {
	if (has_error_) [[unlikely]] {
		return std::unexpected(last_error_);
	}
	StateFrame *top_ptr = nullptr;
	if (!stack_.empty()) {
		auto &candidate = stack_.back();
		if (candidate.kind == StateFrame::Kind::object && !candidate.awaiting_value) {
			top_ptr = &candidate;
		}
	}
	if constexpr (Mode == ParseMode::strict) {
		if ((expected_key_raw_json_safe || raw_json_name_fast_path_safe(expected_key)) && top_ptr != nullptr) {
			auto &top = *top_ptr;
			std::size_t p = pos_;
			if (!top.first) {
				if (p < input_.size() && input_[p] == ',') {
					++p;
				} else {
					p = std::string_view::npos;
				}
			}
			if (p != std::string_view::npos
				&& p < input_.size()
				&& input_[p] == '"'
				&& expected_key.size() + 3U <= input_.size() - p) {
				std::size_t const body = p + 1U;
				std::size_t const close = body + expected_key.size();
				if (input_[close] == '"'
					&& input_[close + 1U] == ':'
					&& std::memcmp(input_.data() + body, expected_key.data(), expected_key.size()) == 0) {
					if (opts_.max_string_size.exceeds(expected_key.size(), kDefaultMaxString)) [[unlikely]] {
						auto e = mk_err(JsonIssueCode::string_too_large, "string exceeds max_string_size");
						set_error(e);
						return std::unexpected(last_error_);
					}
					std::size_t const consumed = close + 2U - pos_;
					pos_ += consumed;
					col_ += consumed;
					top.first = false;
					top.awaiting_value = true;
					return ObjectKeyMatch{.has_key = true, .matched = true, .key = expected_key};
				}
				auto const actual_scan = scan_short_string_body(input_, body);
				std::size_t const actual_close = body + actual_scan.count;
				if (actual_scan.closed && actual_close + 1U < input_.size() && input_[actual_close + 1U] == ':') {
					std::string_view const actual = input_.substr(body, actual_scan.count);
					if (opts_.max_string_size.exceeds(actual.size(), kDefaultMaxString)) [[unlikely]] {
						auto e = mk_err(JsonIssueCode::string_too_large, "string exceeds max_string_size");
						set_error(e);
						return std::unexpected(last_error_);
					}
					std::size_t const consumed = actual_close + 2U - pos_;
					pos_ += consumed;
					col_ += consumed;
					top.first = false;
					top.awaiting_value = true;
					return ObjectKeyMatch{.has_key = true, .matched = false, .key = actual};
				}
			}
		}
	}
	auto key = next_object_key_view_impl<Mode>(scratch);
	if (!key) [[unlikely]] {
		return std::unexpected(std::move(key).error());
	}
	if (!*key) {
		return ObjectKeyMatch{};
	}
	std::string_view const actual = **key;
	return ObjectKeyMatch{.has_key = true, .matched = actual == expected_key, .key = actual};
}

std::expected<JsonReader::ObjectKeyMatch, JsonError> JsonReader::next_object_key_match(
	std::string_view expected_key,
	JsonDecodeScratch &scratch,
	bool expected_key_raw_json_safe) {
	if (opts_.mode == ParseMode::strict) {
		return next_object_key_match_impl<ParseMode::strict>(expected_key, scratch, expected_key_raw_json_safe);
	}
	return next_object_key_match_impl<ParseMode::json5>(expected_key, scratch, expected_key_raw_json_safe);
}

template<ParseMode Mode>
std::expected<std::optional<std::string_view>, JsonError> JsonReader::next_object_key_view_impl(
	JsonDecodeScratch &scratch) {
	if (has_error_) [[unlikely]] {
		return std::unexpected(last_error_);
	}
	if (auto ok = skip_ws_checked_fast_impl<Mode>(); !ok) [[unlikely]] {
		return std::unexpected(std::move(ok).error());
	}
	if (stack_.empty() || stack_.back().kind != StateFrame::Kind::object || stack_.back().awaiting_value) [[unlikely]] {
		return std::unexpected(mk_err(JsonIssueCode::wrong_kind, "reader is not positioned at an object key"));
	}

	auto &top = stack_.back();
	if (pos_ < input_.size() && input_[pos_] == '}') {
		adv();
		stack_.pop_back();
		return std::optional<std::string_view>{};
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
				return std::optional<std::string_view>{};
			}
		}
	}
	top.first = false;
	if (pos_ >= input_.size()) [[unlikely]] {
		auto e = mk_err(JsonIssueCode::unexpected_eof, "EOF in object");
		set_error(e);
		return std::unexpected(last_error_);
	}

	auto finish_key = [&](std::string_view key_name) -> std::expected<std::optional<std::string_view>, JsonError> {
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
		return std::optional<std::string_view>{key_name};
	};

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
			if (pos_ < input_.size() && input_[pos_] == ':') {
				adv();
				top.awaiting_value = true;
				return std::optional<std::string_view>{body};
			}
			return finish_key(body);
		}
		pos_ = body_start;
		col_ = body_col;
		if (auto str_res = parse_str_into_token(opts_.max_string_size, key_token_); !str_res) [[unlikely]] {
			set_error(str_res.error());
			return std::unexpected(last_error_);
		}
		auto key_view = decode_key_token_view(key_token_, scratch);
		if (!key_view) [[unlikely]] {
			return std::unexpected(std::move(key_view).error());
		}
		return finish_key(*key_view);
	}
	if constexpr (Mode == ParseMode::json5) {
		if (input_[pos_] == '\'') {
			adv();
			if (auto str_res = parse_str_sq_into_token(opts_.max_string_size, key_token_); !str_res) [[unlikely]] {
				set_error(str_res.error());
				return std::unexpected(last_error_);
			}
			auto key_view = decode_key_token_view(key_token_, scratch);
			if (!key_view) [[unlikely]] {
				return std::unexpected(std::move(key_view).error());
			}
			return finish_key(*key_view);
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
			return finish_key(input_.substr(key_start, pos_ - key_start));
		}
	}
	auto e = mk_err(JsonIssueCode::syntax_error, "std::expected string key");
	set_error(e);
	return std::unexpected(last_error_);
}

std::expected<std::optional<std::string_view>, JsonError> JsonReader::next_object_key_view(
	JsonDecodeScratch &scratch) {
	if (opts_.mode == ParseMode::strict) {
		return next_object_key_view_impl<ParseMode::strict>(scratch);
	}
	return next_object_key_view_impl<ParseMode::json5>(scratch);
}

#if defined(__GNUC__) || defined(__clang__)
	#define CONFLUX_JSON_READER_SECTION(name) __attribute__((section(name)))
#else
	#define CONFLUX_JSON_READER_SECTION(name)
#endif

#define CONFLUX_JSON_READER_OBJECT_KEYS_INSTANTIATE_MODE(mode_name, mode_value)                          \
	template std::expected<std::optional<JsonStringToken>, JsonError> JsonReader::next_object_key_token_impl< \
		mode_value>();                                                                                   \
	template CONFLUX_JSON_READER_SECTION(".text.conflux.json.reader." mode_name ".key-match")           \
	std::expected<JsonReader::ObjectKeyMatch, JsonError> JsonReader::next_object_key_match_impl<         \
		mode_value>(                                                                                     \
		std::string_view expected_key,                                                                   \
		JsonDecodeScratch &scratch,                                                                      \
		bool expected_key_raw_json_safe);                                                                \
	template CONFLUX_JSON_READER_SECTION(".text.conflux.json.reader." mode_name ".key-view")            \
	std::expected<std::optional<std::string_view>, JsonError> JsonReader::next_object_key_view_impl<     \
		mode_value>(JsonDecodeScratch &scratch)

CONFLUX_JSON_READER_OBJECT_KEYS_INSTANTIATE_MODE("strict", ParseMode::strict);
CONFLUX_JSON_READER_OBJECT_KEYS_INSTANTIATE_MODE("json5", ParseMode::json5);

#undef CONFLUX_JSON_READER_OBJECT_KEYS_INSTANTIATE_MODE
#undef CONFLUX_JSON_READER_SECTION

} // namespace conflux::json
