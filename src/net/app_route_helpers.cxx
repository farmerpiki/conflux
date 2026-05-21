export module conflux.net.app.route_helpers;

import std;
import conflux.net.app.types;
import conflux.net.http.types;

export namespace conflux::http::detail {

[[nodiscard]] std::optional<std::string> validate_path_pattern(
	std::string_view path) {
	if (path.empty() || path.front() != '/') {
		return "invalid route pattern: path must start with /";
	}
	for (std::size_t pos = 0, segment_index = 0;; ++segment_index) {
		auto next = path.find('/', pos + 1);
		auto segment = path.substr(pos + 1, next == std::string_view::npos ? path.size() - pos - 1 : next - pos - 1);
		auto const open = segment.find('{');
		auto const close = segment.find('}');
		if ((open == std::string_view::npos) != (close == std::string_view::npos) || open > close) {
			return "invalid route pattern: unmatched path parameter braces";
		}
		if (open != std::string_view::npos) {
			if (open != 0 || close + 1 != segment.size()) {
				return "invalid route pattern: path parameter must occupy the full segment";
			}
			auto name = segment.substr(1, segment.size() - 2);
			bool const wildcard = name.starts_with('*');
			if (wildcard) {
				name.remove_prefix(1);
				if (next != std::string_view::npos) {
					return "invalid route pattern: wildcard parameter must be the final segment";
				}
			}
			if (name.empty()) {
				return "invalid route pattern: path parameter name is empty";
			}
		}
		(void)segment_index;
		if (next == std::string_view::npos) {
			break;
		}
		pos = next;
	}
	return std::nullopt;
}

[[nodiscard]] std::string route_shape(
	std::string_view path) {
	std::string out;
	out.reserve(path.size());
	for (std::size_t pos = 0;;) {
		auto next = path.find('/', pos + 1);
		auto segment = path.substr(pos + 1, next == std::string_view::npos ? path.size() - pos - 1 : next - pos - 1);
		out += '/';
		if (segment.size() >= 2 && segment.front() == '{' && segment.back() == '}') {
			out += segment.starts_with("{*") ? "{*}" : "{}";
		} else {
			out += segment;
		}
		if (next == std::string_view::npos) {
			break;
		}
		pos = next;
	}
	return out.empty() ? "/" : out;
}

[[nodiscard]] std::vector<std::string> collect_path_params(
	std::string_view path) {
	std::vector<std::string> out;
	for (std::size_t pos = 0; pos < path.size();) {
		auto open = path.find('{', pos);
		if (open == std::string_view::npos) {
			break;
		}
		auto close = path.find('}', open + 1);
		if (close == std::string_view::npos) {
			break;
		}
		auto name = path.substr(open + 1, close - open - 1);
		if (name.starts_with('*')) {
			name.remove_prefix(1);
		}
		if (auto colon = name.find(':'); colon != std::string_view::npos) {
			name = name.substr(0, colon);
		}
		if (!name.empty()) {
			out.emplace_back(name);
		}
		pos = close + 1;
	}
	return out;
}

[[nodiscard]] std::map<std::string, std::string> collect_path_param_types(
	std::string_view path) {
	std::map<std::string, std::string> out;
	for (std::size_t pos = 0; pos < path.size();) {
		auto open = path.find('{', pos);
		if (open == std::string_view::npos) {
			break;
		}
		auto close = path.find('}', open + 1);
		if (close == std::string_view::npos) {
			break;
		}
		auto name = path.substr(open + 1, close - open - 1);
		if (name.starts_with('*')) {
			name.remove_prefix(1);
		}
		std::string_view type;
		if (auto colon = name.find(':'); colon != std::string_view::npos) {
			type = name.substr(colon + 1);
			name = name.substr(0, colon);
		}
		if (!name.empty()) {
			out.emplace(std::string{name}, std::string{type});
		}
		pos = close + 1;
	}
	return out;
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
	std::map<std::string, std::string> const &path_param_types) {
	if (path_params.empty()) {
		return " Available path parameters: none.";
	}
	std::string out = " Available path parameters:";
	for (auto const &name: path_params) {
		out += ' ';
		out += name;
		auto const type_it = path_param_types.find(name);
		if (type_it != path_param_types.end() && !type_it->second.empty()) {
			out += ':';
			out += type_it->second;
		}
	}
	out += '.';
	return out;
}

[[nodiscard]] std::string_view trim_ascii(
	std::string_view value) noexcept {
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
		value.remove_prefix(1);
	}
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
		value.remove_suffix(1);
	}
	return value;
}

[[nodiscard]] std::optional<std::string_view> credentials_for_scheme(
	std::string_view auth,
	std::string_view scheme) noexcept {
	if (auth.size() <= scheme.size() || auth[scheme.size()] != ' ') {
		return std::nullopt;
	}
	if (!ascii_iequals(auth.substr(0, scheme.size()), scheme)) {
		return std::nullopt;
	}
	return trim_ascii(auth.substr(scheme.size() + 1));
}

} // namespace conflux::http::detail
