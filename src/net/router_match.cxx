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

export struct ParsedRoutePattern {
	std::optional<std::string> error;
	std::string shape;
	std::vector<Segment> segments;
	std::vector<std::string> params;
	std::vector<std::pair<std::string, std::string>> param_types;
};

[[nodiscard]] bool is_known_route_type_tag(
	std::string_view tag) noexcept {
	return tag.empty() || tag == "string" || tag == "u64" || tag == "i64" || tag == "u32" || tag == "i32";
}

export [[nodiscard]] ParsedRoutePattern parse_route_pattern(
	std::string_view pattern) {
	ParsedRoutePattern info;
	info.shape.reserve(pattern.size());
	info.segments.reserve(static_cast<std::size_t>(std::ranges::count(pattern, '/')) + 1);
	if (pattern.empty() || pattern.front() != '/') {
		info.error = "invalid route pattern: path must start with /";
		return info;
	}
	info.segments.push_back({std::string{}, false, false});
	for (std::size_t pos = 0;;) {
		auto next = pattern.find('/', pos + 1);
		auto segment =
			pattern.substr(pos + 1, next == std::string_view::npos ? pattern.size() - pos - 1 : next - pos - 1);
		auto const open = segment.find('{');
		auto const close = segment.find('}');
		info.shape += '/';
		if ((open == std::string_view::npos) != (close == std::string_view::npos) || open > close) {
			info.error = "invalid route pattern: unmatched path parameter braces";
			return info;
		}
		if (open != std::string_view::npos) {
			if (open != 0 || close + 1 != segment.size()) {
				info.error = "invalid route pattern: path parameter must occupy the full segment";
				return info;
			}
			auto name = segment.substr(1, segment.size() - 2);
			bool const wildcard = name.starts_with('*');
			if (wildcard) {
				name.remove_prefix(1);
				if (next != std::string_view::npos) {
					info.error = "invalid route pattern: wildcard parameter must be the final segment";
					return info;
				}
			}
			if (name.empty()) {
				info.error = "invalid route pattern: path parameter name is empty";
				return info;
			}
			std::string_view type;
			if (auto colon = name.find(':'); colon != std::string_view::npos) {
				type = name.substr(colon + 1);
				name = name.substr(0, colon);
				if (type.empty()) {
					info.error = "invalid route pattern: path parameter type is empty";
					return info;
				}
				if (!is_known_route_type_tag(type)) {
					info.error = std::format("invalid route pattern: unknown path parameter type '{}'", type);
					return info;
				}
			}
			info.shape += wildcard ? "{*}" : "{}";
			info.segments.push_back({std::string{name}, !wildcard, wildcard});
			info.params.emplace_back(name);
			info.param_types.emplace_back(std::string{name}, std::string{type});
		} else {
			info.shape += segment;
			info.segments.push_back({std::string{segment}, false, false});
		}
		if (next == std::string_view::npos) {
			break;
		}
		pos = next;
	}
	if (info.shape.empty()) {
		info.shape = "/";
	}
	return info;
}

export std::vector<Segment> parse_pattern(
	std::string_view pattern) {
	return parse_route_pattern(pattern).segments;
}

namespace {

void add_path_param(
	HttpFieldsView &params,
	std::string_view name,
	std::string_view raw_value) {
	if (!url_needs_path_decode(raw_value)) {
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
