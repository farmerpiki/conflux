module;
#include <ctime>
export module conflux.net.router_match;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.utils;

export struct Segment {
	std::string value;
	bool is_param; // true -> {name} single-segment capture
	bool is_wildcard; // true -> {*name} greedy tail capture (must be last segment)
};

[[nodiscard]] std::string route_param_name(
	std::string_view value) {
	if (auto colon = value.find(':'); colon != std::string_view::npos) {
		value = value.substr(0, colon);
	}
	return std::string{value};
}

export std::vector<Segment> parse_pattern(
	std::string_view pattern) {
	std::vector<Segment> segs;
	segs.reserve(static_cast<std::size_t>(std::ranges::count(pattern, '/')) + 1);
	std::size_t pos = 0;
	while (true) {
		auto next = pattern.find('/', pos);
		auto part = (next == std::string_view::npos) ? pattern.substr(pos) : pattern.substr(pos, next - pos);

		if (part.size() >= 3 && part.front() == '{' && part.back() == '}' && part[1] == '*') {
			segs.push_back({route_param_name(part.substr(2, part.size() - 3)), false, true});
		} else if (part.size() >= 2 && part.front() == '{' && part.back() == '}') {
			segs.push_back({route_param_name(part.substr(1, part.size() - 2)), true, false});
		} else {
			segs.push_back({std::string{part}, false, false});
		}

		if (next == std::string_view::npos) {
			break;
		}
		pos = next + 1;
	}
	return segs;
}

namespace {

void add_path_param(
	HttpFieldsView &params,
	std::string_view name,
	std::string_view raw_value) {
	if (raw_value.find('%') == std::string_view::npos) {
		params.emplace_back(name, raw_value);
		return;
	}
	params.emplace_back_owned_value(name, url_decode_path(raw_value));
}

} // namespace

export bool match_segments(
	std::vector<Segment> const &pattern,
	std::string_view path,
	HttpFieldsView &out_params) {
	// Wildcard tail: last segment {*name} matches everything remaining.
	if (!pattern.empty() && pattern.back().is_wildcard) {
		// Match all non-wildcard leading segments first.
		auto prefix_count = pattern.size() - 1;
		std::size_t pos = 0;
		HttpFieldsView tmp;
		for (std::size_t i = 0; i < prefix_count; ++i) {
			if (pos >= path.size()) {
				return false;
			}
			auto next = path.find('/', pos);
			auto part = (next == std::string_view::npos) ? path.substr(pos) : path.substr(pos, next - pos);
			if (next == std::string_view::npos && i + 1 < prefix_count) {
				return false;
			}
			if (pattern[i].is_param) {
				add_path_param(tmp, pattern[i].value, part);
			} else if (pattern[i].value != part) {
				return false;
			}
			pos = (next == std::string_view::npos) ? path.size() : next + 1;
		}
		// Capture the remainder (may be empty for trailing slash).
		add_path_param(tmp, pattern.back().value, path.substr(pos));
		out_params = std::move(tmp);
		return true;
	}

	std::size_t pos = 0;
	std::size_t i = 0;
	HttpFieldsView tmp;
	while (true) {
		if (i >= pattern.size()) {
			return false;
		}
		auto next = path.find('/', pos);
		auto part = (next == std::string_view::npos) ? path.substr(pos) : path.substr(pos, next - pos);
		if (pattern[i].is_param) {
			add_path_param(tmp, pattern[i].value, part);
		} else if (pattern[i].value != part) {
			return false;
		}
		++i;
		if (next == std::string_view::npos) {
			break;
		}
		pos = next + 1;
	}

	if (i != pattern.size()) {
		return false;
	}
	out_params = std::move(tmp);
	return true;
}

export std::string segments_to_pattern(
	std::vector<Segment> const &segs) {
	std::string out;
	bool first = true;
	for (auto const &seg: segs) {
		if (first && seg.value.empty() && !seg.is_param && !seg.is_wildcard) {
			first = false;
			continue;
		}
		first = false;
		out += '/';
		if (seg.is_wildcard) {
			out += "{*";
			out += seg.value;
			out += '}';
		} else if (seg.is_param) {
			out += '{';
			out += seg.value;
			out += '}';
		} else {
			out += seg.value;
		}
	}
	if (out.empty()) {
		out = "/";
	}
	return out;
}
