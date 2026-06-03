module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

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

#define CONFLUX_JSON_READER_ARRAY_INSTANTIATE_MODE(mode_value)                           \
	template std::expected<void, JsonError> JsonReader::skip_array_body_raw_impl<mode_value>( \
		std::size_t raw_depth);                                                          \
	template std::expected<std::size_t, JsonError> JsonReader::count_array_elements_raw_impl<mode_value>(); \
	template std::expected<std::size_t, JsonError>                                      \
	JsonReader::count_remaining_array_elements_impl<mode_value>()

CONFLUX_JSON_READER_ARRAY_INSTANTIATE_MODE(ParseMode::strict);
CONFLUX_JSON_READER_ARRAY_INSTANTIATE_MODE(ParseMode::json5);

#undef CONFLUX_JSON_READER_ARRAY_INSTANTIATE_MODE

} // namespace conflux::json
