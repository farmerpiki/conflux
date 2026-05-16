module conflux.json;

import std;
import std.compat;
import conflux.types;

void JsonStreamReader::refresh_reader_input() noexcept {
	reader_.replace_input(SV{buf_.data(), buf_.size()});
}

void JsonStreamReader::set_error(
	JsonError e) noexcept {
	has_error_ = true;
	last_error_ = move(e);
}

SZ JsonStreamReader::configured_cap() const noexcept {
	constexpr SZ kU32Ceiling = (SZ{1} << 32) - 1;
	SZ const hard_cap = kU32Ceiling - 1;
	if (opts_.max_input_size.is_unlimited()) {
		return hard_cap;
	}
	return min(opts_.max_input_size.explicit_value().value_or(kDefaultMaxInput), hard_cap);
}

JsonError JsonStreamReader::make_stream_error(
	JsonIssueCode code,
	S message) const {
	return JsonError{
		.stage = JsonStage::parse,
		.code = code,
		.source = JsonSourceLocation{.offset = reader_.pos_, .line = reader_.line_, .column = reader_.col_},
		.message = move(message),
	};
}

bool JsonStreamReader::tail_is_prefix_of_literal(
	SZ start) const noexcept {
	SV const tail{buf_.data() + start, buf_.size() - start};
	return (tail.size() < SV{"true"}.size() && SV{"true"}.starts_with(tail))
		|| (tail.size() < SV{"false"}.size() && SV{"false"}.starts_with(tail))
		|| (tail.size() < SV{"null"}.size() && SV{"null"}.starts_with(tail));
}

bool JsonStreamReader::trailing_utf8_prefix_needs_more() const noexcept {
	if (reader_.pos_ >= buf_.size()) {
		return false;
	}
	auto const c = static_cast<unsigned char>(buf_[reader_.pos_]);
	SZ const seq = utf8_seq_len(c);
	return seq != 0 && reader_.pos_ + seq > buf_.size();
}

bool JsonStreamReader::recoverable_need_more(
	SZ checkpoint_pos,
	JsonError const &error) const noexcept {
	if (closed_) {
		return false;
	}
	if (error.code == JsonIssueCode::unexpected_eof) {
		return true;
	}
	if (checkpoint_pos > buf_.size()) {
		return false;
	}
	if (error.code == JsonIssueCode::syntax_error) {
		if (checkpoint_pos < buf_.size() && tail_is_prefix_of_literal(checkpoint_pos)) {
			return true;
		}
		if (reader_.pos_ >= buf_.size()) {
			return true;
		}
	}
	if (error.code == JsonIssueCode::invalid_unicode_escape) {
		return reader_.pos_ + 4 > buf_.size();
	}
	if (error.code == JsonIssueCode::invalid_utf8) {
		return trailing_utf8_prefix_needs_more();
	}
	return false;
}

bool JsonStreamReader::event_needs_more_before_emit(
	Event event) const noexcept {
	// GCC 16's module lookup can be fragile here; keep the check on the stable
	// underlying event ordinal instead of relying on the scoped enumerator lookup.
	return !closed_ && static_cast<unsigned>(event) == 6U && reader_.pos_ == buf_.size();
}

JsonStreamReader::JsonStreamReader(
	JsonParseOptions const &opts)
	: opts_{opts}
	, reader_{SV{buf_.data(), buf_.size()}, opts} {}

expected<void, JsonError> JsonStreamReader::feed(
	SV chunk) {
	return feed(span<byte const>{reinterpret_cast<byte const *>(chunk.data()), chunk.size()});
}

expected<void, JsonError> JsonStreamReader::feed(
	span<byte const> chunk) {
	if (has_error_) {
		return unexpected(last_error_);
	}
	if (closed_) {
		return unexpected(make_stream_error(JsonIssueCode::invalid_value, "cannot feed after close"));
	}
	SZ const cap = configured_cap();
	if (buf_.size() > cap || chunk.size() > cap - buf_.size()) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::input_too_large,
				.message = "stream buffer exceeds max_input_size"});
	}
	buf_.append(reinterpret_cast<char const *>(chunk.data()), chunk.size());
	refresh_reader_input();
	return {};
}

expected<void, JsonError> JsonStreamReader::close() {
	if (has_error_) {
		return unexpected(last_error_);
	}
	closed_ = true;
	refresh_reader_input();
	return {};
}

expected<Opt<JsonStreamReader::Event>, JsonError> JsonStreamReader::next() {
	if (has_error_) {
		return unexpected(last_error_);
	}
	refresh_reader_input();
	auto checkpoint = reader_.checkpoint();
	auto ev = reader_.next();
	if (!ev) {
		auto error = move(ev).error();
		if (recoverable_need_more(checkpoint.pos, error)) {
			reader_.restore(move(checkpoint));
			return Opt<Event>{};
		}
		set_error(move(error));
		return unexpected(last_error_);
	}
	if (*ev && event_needs_more_before_emit(**ev)) {
		reader_.restore(move(checkpoint));
		return Opt<Event>{};
	}
	return ev;
}

JsonStringToken JsonStreamReader::key_token() const noexcept {
	return reader_.key_token();
}

JsonStringToken JsonStreamReader::string_token() const noexcept {
	return reader_.string_token();
}

JsonNumberView JsonStreamReader::number_val() const noexcept {
	return reader_.number_val();
}

bool JsonStreamReader::bool_val() const noexcept {
	return reader_.bool_val();
}

SV JsonStreamReader::input() const noexcept {
	return SV{buf_.data(), buf_.size()};
}

SZ JsonStreamReader::depth() const noexcept {
	return reader_.depth();
}

bool JsonStreamReader::has_error() const noexcept {
	return has_error_ || reader_.has_error();
}

bool JsonStreamReader::closed() const noexcept {
	return closed_;
}

SZ JsonStreamReader::pos() const noexcept {
	return reader_.pos();
}

SZ JsonStreamReader::value_start_pos() const noexcept {
	return reader_.value_start_pos();
}

SZ JsonStreamReader::buffered_bytes() const noexcept {
	return buf_.size();
}

void JsonStreamReader::reset() noexcept {
	buf_.clear();
	closed_ = false;
	has_error_ = false;
	last_error_ = {};
	reader_.reset();
	refresh_reader_input();
}

NdjsonRange::NdjsonRange(
	SV input,
	JsonParseOptions const &opts) noexcept
	: input_{input}
	, opts_{opts} {}

void NdjsonRange::Iterator::advance_one() noexcept {
	cache_.reset();
	while (!remaining_.empty()) {
		auto pos = remaining_.find('\n');
		SV line;
		if (pos == SV::npos) {
			line = remaining_;
			remaining_ = {};
		} else {
			line = remaining_.substr(0, pos);
			remaining_.remove_prefix(pos + 1);
		}
		// strip trailing CR
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		if (line.empty()) {
			continue;
		}
		cache_ = conflux::json::parse_borrowed_unsafe(line, opts_);
		return;
	}
}

NdjsonRange::Iterator::Iterator(
	SV remaining,
	JsonParseOptions const &opts) noexcept
	: remaining_{remaining}
	, opts_{opts} {
	advance_one();
}

NdjsonRange::Iterator::reference NdjsonRange::Iterator::operator *() const noexcept {
	return *cache_;
}

NdjsonRange::Iterator::pointer NdjsonRange::Iterator::operator ->() const noexcept {
	return &*cache_;
}

NdjsonRange::Iterator &NdjsonRange::Iterator::operator ++() noexcept {
	advance_one();
	return *this;
}

void NdjsonRange::Iterator::operator ++(
	int) noexcept {
	++*this;
}

bool NdjsonRange::Iterator::operator ==(
	std::default_sentinel_t) const noexcept {
	return !cache_.has_value();
}

NdjsonRange::Iterator NdjsonRange::begin() const noexcept {
	return {input_, opts_};
}

std::default_sentinel_t NdjsonRange::end() const noexcept {
	return {};
}

JsonAccumulator::JsonAccumulator(
	JsonParseOptions const &opts) noexcept
	: opts_{opts} {}

expected<void, JsonError> JsonAccumulator::feed(
	SV chunk) {
	return feed(span<byte const>{reinterpret_cast<byte const *>(chunk.data()), chunk.size()});
}

expected<void, JsonError> JsonAccumulator::feed(
	span<byte const> chunk) {
	constexpr SZ kU32Ceiling = (SZ{1} << 32) - 1;
	SZ const hard_cap = kU32Ceiling - 1;
	SZ const configured_cap = opts_.max_input_size.is_unlimited() ?
								  hard_cap :
								  min(opts_.max_input_size.explicit_value().value_or(kDefaultMaxInput), hard_cap);
	if (buf_.size() > configured_cap || chunk.size() > configured_cap - buf_.size()) {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::input_too_large,
				.message = "accumulated size exceeds max_input_size"});
	}
	buf_.append(reinterpret_cast<char const *>(chunk.data()), chunk.size());
	return {};
}

expected<Document, JsonError> JsonAccumulator::finish() {
	return conflux::json::parse_copy(move(buf_), opts_);
}

void JsonAccumulator::reset() noexcept {
	buf_.clear();
}

SZ JsonAccumulator::buffered_bytes() const noexcept {
	return buf_.size();
}
