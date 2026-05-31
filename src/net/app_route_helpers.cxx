export module conflux.net.app.route_helpers;

import std;
import conflux.net.app.types;
import conflux.net.http.types;
import conflux.net.router_match;

export namespace conflux::http::detail {

struct RoutePatternInfo {
	std::optional<std::string> error;
	std::string shape;
	std::vector<std::string> params;
	std::vector<std::pair<std::string, std::string>> param_types;
};

[[nodiscard]] RoutePatternInfo route_pattern_info(
	std::string_view path) {
	auto parsed = conflux::http::detail::parse_route_pattern(path);
	return {
		.error = std::move(parsed.error),
		.shape = std::move(parsed.shape),
		.params = std::move(parsed.params),
		.param_types = std::move(parsed.param_types),
	};
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
