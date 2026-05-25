export module conflux.net.app.route_helpers;

import std;
import conflux.net.app.types;
import conflux.net.http.types;

export namespace conflux::http::detail {

struct RoutePatternInfo {
	std::optional<std::string> error;
	std::string shape;
	std::vector<std::string> params;
	std::vector<std::pair<std::string, std::string>> param_types;
};

[[nodiscard]] bool is_known_route_type_tag(
	std::string_view tag) noexcept {
	return tag.empty() || tag == "string" || tag == "u64" || tag == "i64" || tag == "u32" || tag == "i32";
}

[[nodiscard]] RoutePatternInfo route_pattern_info(
	std::string_view path) {
	RoutePatternInfo info;
	info.shape.reserve(path.size());
	if (path.empty() || path.front() != '/') {
		info.error = "invalid route pattern: path must start with /";
		return info;
	}
	for (std::size_t pos = 0, segment_index = 0;; ++segment_index) {
		auto next = path.find('/', pos + 1);
		auto segment = path.substr(pos + 1, next == std::string_view::npos ? path.size() - pos - 1 : next - pos - 1);
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
			info.params.emplace_back(name);
			info.param_types.emplace_back(std::string{name}, std::string{type});
		} else {
			info.shape += segment;
		}
		(void)segment_index;
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

[[nodiscard]] std::optional<std::string> validate_path_pattern(
	std::string_view path) {
	return route_pattern_info(path).error;
}

[[nodiscard]] std::string route_shape(
	std::string_view path) {
	return route_pattern_info(path).shape;
}

[[nodiscard]] std::vector<std::string> collect_path_params(
	std::string_view path) {
	return route_pattern_info(path).params;
}

[[nodiscard]] std::vector<std::pair<std::string, std::string>> collect_path_param_types(
	std::string_view path) {
	return route_pattern_info(path).param_types;
}

template<FixedString Path, std::size_t Index>
[[nodiscard]] consteval std::string_view fixed_path_param_name() {
	auto const path = Path.view();
	std::size_t count = 0;
	for (std::size_t pos = 0; pos < path.size();) {
		auto open = path.find('{', pos);
		if (open == std::string_view::npos) {
			break;
		}
		auto close = path.find('}', open + 1);
		if (close == std::string_view::npos) {
			break;
		}
		if (count == Index) {
			auto name = path.substr(open + 1, close - open - 1);
			if (name.starts_with('*')) {
				name.remove_prefix(1);
			}
			if (auto colon = name.find(':'); colon != std::string_view::npos) {
				name = name.substr(0, colon);
			}
			return name;
		}
		++count;
		pos = close + 1;
	}
	return {};
}

[[nodiscard]] std::string available_path_params_message(
	std::vector<std::string> const &path_params,
	std::vector<std::pair<std::string, std::string>> const &path_param_types) {
	if (path_params.empty()) {
		return " Available path parameters: none.";
	}
	std::string out = " Available path parameters:";
	for (auto const &name: path_params) {
		out += ' ';
		out += name;
		auto const type_it = std::ranges::find(path_param_types, name, &std::pair<std::string, std::string>::first);
		if (type_it != path_param_types.end() && !type_it->second.empty()) {
			out += ':';
			out += type_it->second;
		}
	}
	out += '.';
	return out;
}

} // namespace conflux::http::detail
