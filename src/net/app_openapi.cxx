export module conflux.net.app:openapi;

import std;
import conflux.net.http.types;
import conflux.net.http.response;

export namespace conflux::http::detail {

struct AppOpenApiRoute {
	std::string method;
	std::string path;
	std::string name;
	std::string openapi_summary;
	std::string auth_policy;
	std::chrono::milliseconds timeout{};
	std::string rate_limit;
	std::size_t max_body_size{};
	std::size_t middleware_count{};
	std::vector<std::string> path_params;
	std::map<std::string, std::string> path_param_types;
	std::vector<std::string> consumes;
	std::string request_body_schema;
	int success_status{kHttpOk};
	std::vector<std::string> produces;
	std::string response_schema;
	bool problem_response{};
};

[[nodiscard]] std::string openapi_json_str(
	std::string_view value) {
	std::string out = "\"";
	for (auto const ch: value) {
		auto const c = static_cast<unsigned char>(ch);
		if (c == '"') {
			out += "\\\"";
		} else if (c == '\\') {
			out += "\\\\";
		} else if (c == '\n') {
			out += "\\n";
		} else if (c == '\r') {
			out += "\\r";
		} else if (c == '\t') {
			out += "\\t";
		} else if (c < 0x20) {
			out += std::format("\\u{:04x}", c);
		} else {
			out += static_cast<char>(c);
		}
	}
	out += '"';
	return out;
}

[[nodiscard]] std::string openapi_method_key(
	std::string_view method) {
	std::string out;
	out.reserve(method.size());
	for (char const ch: method) {
		out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return out;
}

[[nodiscard]] std::string openapi_schema_for_path_type(
	std::string_view type) {
	if (type == "u64") {
		return std::string{R"({"type":"integer","format":"uint64","minimum":0})"};
	}
	if (type == "i64") {
		return std::string{R"({"type":"integer","format":"int64"})"};
	}
	if (type == "u32") {
		return std::string{R"({"type":"integer","format":"uint32","minimum":0})"};
	}
	if (type == "i32") {
		return std::string{R"({"type":"integer","format":"int32"})"};
	}
	return std::string{R"({"type":"string"})"};
}

[[nodiscard]] std::string render_openapi_spec(
	std::span<AppOpenApiRoute const> routes,
	std::string_view title,
	std::string_view version) {
	std::string out;
	out += R"({"openapi":"3.0.0","info":{"title":)";
	out += openapi_json_str(title);
	out += R"(,"version":)";
	out += openapi_json_str(version);
	out += R"(})";
	bool has_auth_policy = false;
	for (auto const &route: routes) {
		if (!route.auth_policy.empty()) {
			has_auth_policy = true;
			break;
		}
	}
	if (has_auth_policy) {
		out += R"(,"components":{"securitySchemes":{"bearerAuth":{"type":"http","scheme":"bearer"}}})";
	}
	out += R"(,"paths":{)";
	std::vector<std::string> path_order;
	std::map<std::string, std::vector<AppOpenApiRoute const *>> routes_by_path;
	for (auto const &route: routes) {
		auto [it, inserted] = routes_by_path.try_emplace(route.path);
		if (inserted) {
			path_order.push_back(route.path);
		}
		it->second.push_back(std::addressof(route));
	}
	for (std::size_t path_index = 0; path_index < path_order.size(); ++path_index) {
		auto const &path = path_order[path_index];
		auto const &path_routes = routes_by_path.at(path);
		if (path_index != 0) {
			out += ',';
		}
		out += openapi_json_str(path);
		out += ":{";
		for (std::size_t route_index = 0; route_index < path_routes.size(); ++route_index) {
			auto const &route = *path_routes[route_index];
			if (route_index != 0) {
				out += ',';
			}
			out += openapi_json_str(openapi_method_key(route.method));
			out += ":{";
			if (!route.name.empty()) {
				out += R"("operationId":)";
				out += openapi_json_str(route.name);
				out += ',';
			}
			if (!route.openapi_summary.empty()) {
				out += R"("summary":)";
				out += openapi_json_str(route.openapi_summary);
				out += ',';
			}
			if (!route.auth_policy.empty()) {
				out += R"("security":[{"bearerAuth":[]}],"x-auth-policy":)";
				out += openapi_json_str(route.auth_policy);
				out += ',';
			}
			if (route.timeout.count() != 0) {
				out += R"("x-timeout-ms":)";
				out += std::to_string(route.timeout.count());
				out += ',';
			}
			if (!route.rate_limit.empty()) {
				out += R"("x-rate-limit":)";
				out += openapi_json_str(route.rate_limit);
				out += ',';
			}
			if (route.max_body_size != 0) {
				out += R"("x-max-body-size":)";
				out += std::to_string(route.max_body_size);
				out += ',';
			}
			if (route.middleware_count != 0) {
				out += R"("x-middleware-count":)";
				out += std::to_string(route.middleware_count);
				out += ',';
			}
			out += R"("parameters":[)";
			for (std::size_t i = 0; i < route.path_params.size(); ++i) {
				if (i != 0) {
					out += ',';
				}
				out += R"({"name":)";
				out += openapi_json_str(route.path_params[i]);
				out += R"(,"in":"path","required":true,"schema":)";
				if (auto type = route.path_param_types.find(route.path_params[i]);
					type != route.path_param_types.end()) {
					out += openapi_schema_for_path_type(type->second);
				} else {
					out += openapi_schema_for_path_type({});
				}
				out += "}";
			}
			out += ']';
			if (!route.consumes.empty()) {
				out += R"(,"requestBody":{"content":{)";
				for (std::size_t i = 0; i < route.consumes.size(); ++i) {
					if (i != 0) {
						out += ',';
					}
					out += openapi_json_str(route.consumes[i]);
					out += R"(:{"schema":)";
					out += route.request_body_schema.empty() ? R"({"type":"object"})" : route.request_body_schema;
					out += "}";
				}
				out += "}}";
			}
			out += R"(,"responses":{)";
			out += openapi_json_str(std::to_string(route.success_status));
			out += R"(:{"description":)";
			out += openapi_json_str(route.success_status == kHttpCreated ? "Created" : "OK");
			if (!route.produces.empty()) {
				out += R"(,"content":{)";
				for (std::size_t i = 0; i < route.produces.size(); ++i) {
					if (i != 0) {
						out += ',';
					}
					out += openapi_json_str(route.produces[i]);
					out += R"(:{"schema":)";
					out += route.response_schema.empty() ? R"({"type":"object"})" : route.response_schema;
					out += "}";
				}
				out += "}";
			}
			out += "}";
			if (route.problem_response) {
				out +=
					R"(,"400":{"description":"Problem","content":{"application/problem+json":{"schema":{"type":"object"}}}})";
			}
			if (!route.auth_policy.empty()) {
				out += R"(,"401":{"description":"Unauthorized"})";
			}
			if (!route.rate_limit.empty()) {
				out += R"(,"429":{"description":"Too Many Requests"})";
			}
			if (route.timeout.count() != 0) {
				out += R"(,"504":{"description":"Gateway Timeout"})";
			}
			out += "}";
		}
		out += "}";
	}
	out += "}}";
	return out;
}

} // namespace conflux::http::detail
