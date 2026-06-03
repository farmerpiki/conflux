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

} // namespace conflux::json
