module conflux.json;

import std;
import std.compat;
import conflux.types;

namespace conflux::json {

std::expected<std::int64_t, JsonError> JsonNumberView::to_i64() const {
	if ((flags_ & kLexIntForm) == 0) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::invalid_number,
				.message = "to_i64 requires integer-form number"});
	}
	if ((flags_ & kValKindInt) != 0) {
		return std::bit_cast<std::int64_t>(raw_payload_);
	}
	// kValKindUint, kValKindF64, or no kValKind* (overflow): integer-form
	// lexeme outside std::int64_t range → number_out_of_range (Correction K).
	return std::unexpected(
		JsonError{
			.stage = JsonStage::lookup,
			.code = JsonIssueCode::number_out_of_range,
			.message = std::format("value out of std::int64_t range: {}", lexeme_)});
}

std::expected<std::uint64_t, JsonError> JsonNumberView::to_u64() const {
	if ((flags_ & kLexIntForm) == 0) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::invalid_number,
				.message = "to_u64 requires integer-form number"});
	}
	if (!lexeme_.empty() && lexeme_[0] == '-') {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::sign_mismatch,
				.message = std::format("negative integer passed to to_u64: {}", lexeme_)});
	}
	if ((flags_ & kValKindUint) != 0) {
		return raw_payload_;
	}
	if ((flags_ & kValKindInt) != 0) {
		auto const v = std::bit_cast<std::int64_t>(raw_payload_);
		if (v >= 0) {
			return static_cast<std::uint64_t>(v);
		}
	}
	return std::unexpected(
		JsonError{
			.stage = JsonStage::lookup,
			.code = JsonIssueCode::number_out_of_range,
			.message = std::format("value out of std::uint64_t range: {}", lexeme_)});
}

std::expected<double, JsonError> JsonNumberView::to_f64() const {
	if ((flags_ & kValKindF64) != 0) {
		return std::bit_cast<double>(raw_payload_);
	}
	if ((flags_ & kValKindInt) != 0) {
		return static_cast<double>(std::bit_cast<std::int64_t>(raw_payload_));
	}
	if ((flags_ & kValKindUint) != 0) {
		return static_cast<double>(raw_payload_);
	}
	if ((flags_ & kValKindDeferred) != 0) {
		auto res = detail::classify_range_error_slow(lexeme_.data(), lexeme_.data() + lexeme_.size());
		if (!res) {
			auto err = std::move(res).error();
			err.stage = JsonStage::lookup;
			return std::unexpected(std::move(err));
		}
		if (res->kind == detail::ClassifiedDouble::Kind::underflow_finite) {
			return res->value;
		}
		return std::unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::number_out_of_range,
				.message = std::format("f64 conversion overflows: {}", lexeme_)});
	}
	// No kValKind* set → f64-overflow (Correction K).
	return std::unexpected(
		JsonError{
			.stage = JsonStage::lookup,
			.code = JsonIssueCode::number_out_of_range,
			.message = std::format("f64 conversion overflows: {}", lexeme_)});
}

bool validate_number_lexeme(
	std::string_view lex) noexcept {
	if (lex.empty()) {
		return false;
	}
	std::size_t i = 0;
	if (lex[i] == '-') {
		++i;
		if (i >= lex.size()) {
			return false;
		}
	}
	if (lex[i] == '0') {
		++i;
	} else if (lex[i] >= '1' && lex[i] <= '9') {
		while (i < lex.size() && lex[i] >= '0' && lex[i] <= '9') {
			++i;
		}
	} else {
		return false;
	}
	if (i < lex.size() && lex[i] == '.') {
		++i;
		if (i >= lex.size() || lex[i] < '0' || lex[i] > '9') {
			return false;
		}
		while (i < lex.size() && lex[i] >= '0' && lex[i] <= '9') {
			++i;
		}
	}
	if (i < lex.size() && (lex[i] == 'e' || lex[i] == 'E')) {
		++i;
		if (i < lex.size() && (lex[i] == '+' || lex[i] == '-')) {
			++i;
		}
		if (i >= lex.size() || lex[i] < '0' || lex[i] > '9') {
			return false;
		}
		while (i < lex.size() && lex[i] >= '0' && lex[i] <= '9') {
			++i;
		}
	}
	return i == lex.size();
}

} // namespace conflux::json
