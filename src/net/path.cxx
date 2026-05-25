export module conflux.net.path;

import std;

export namespace conflux::http::detail {

[[nodiscard]] std::string join_route_path(
	std::string_view prefix,
	std::string_view path) {
	if (prefix.empty()) {
		return std::string{path};
	}
	if (path.empty()) {
		return std::string{prefix};
	}
	bool const prefix_slash = prefix.back() == '/';
	bool const path_slash = path.front() == '/';
	if (prefix_slash && path_slash) {
		std::string out;
		out.reserve(prefix.size() + path.size() - 1U);
		out.append(prefix.data(), prefix.size() - 1U);
		out.append(path.data(), path.size());
		return out;
	}
	if (!prefix_slash && !path_slash) {
		std::string out;
		out.reserve(prefix.size() + path.size() + 1U);
		out.append(prefix.data(), prefix.size());
		out.push_back('/');
		out.append(path.data(), path.size());
		return out;
	}
	std::string out;
	out.reserve(prefix.size() + path.size());
	out.append(prefix.data(), prefix.size());
	out.append(path.data(), path.size());
	return out;
}

} // namespace conflux::http::detail
