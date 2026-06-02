export module conflux.net.http.json_string;

import std;
import conflux.utils;

export namespace conflux::http::detail {

using conflux::utils::append_json_string_fallback;
using conflux::utils::json_string_fallback;

void append_json_string(
	std::string &out,
	std::string_view value) {
	append_json_string_fallback(out, value);
}

[[nodiscard]] std::string json_string(
	std::string_view value) {
	return json_string_fallback(value);
}

struct ProblemBodyBuilder {
	std::string body{"{"};
	bool first{true};

	ProblemBodyBuilder &member(
		std::string_view name,
		std::string_view value) {
		append_name(name);
		append_json_string(body, value);
		return *this;
	}

	ProblemBodyBuilder &raw_member(
		std::string_view name,
		std::string_view value) {
		append_name(name);
		body.append(value);
		return *this;
	}

	[[nodiscard]] std::string finish() {
		body.push_back('}');
		return std::move(body);
	}

private:
	void append_name(
		std::string_view name) {
		if (!first) {
			body.push_back(',');
		}
		first = false;
		append_json_string(body, name);
		body.push_back(':');
	}
};

} // namespace conflux::http::detail
