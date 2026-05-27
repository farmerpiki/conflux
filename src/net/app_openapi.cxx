export module conflux.net.app.openapi;

import std;
import conflux.net.http.types;
import conflux.net.http.response;
import conflux.net.http.json_string;
import conflux.utils;
#if CONFLUX_HAS_JSON
import conflux.json;
#endif

export namespace conflux::http::detail {

struct AppOpenApiRoute {
	std::string_view method;
	std::string_view path;
	std::string_view name;
	std::string_view openapi_summary;
	std::string_view bearer_token_policy;
	std::string_view auth_scheme;
	std::chrono::milliseconds timeout{};
	std::string_view rate_limit;
	std::size_t max_body_size{};
	std::size_t middleware_count{};
	std::span<std::string const> path_params;
	std::vector<std::pair<std::string, std::string>> const *path_param_types{};
	std::span<std::string const> consumes;
	std::string_view request_body_schema;
	int success_status{kHttpOk};
	std::span<std::string const> produces;
	std::string_view response_schema;
	bool problem_response{};
};

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

[[nodiscard]] std::string_view openapi_auth_scheme_name(
	std::string_view scheme) noexcept {
	if (scheme == "basic") {
		return "basicAuth";
	}
	return "bearerAuth";
}

[[nodiscard]] std::string render_openapi_spec(
	std::span<AppOpenApiRoute const> routes,
	std::string_view title,
	std::string_view version) {
	std::string out;
	out += R"({"openapi":"3.0.0","info":{"title":)";
	out += json_string(title);
	out += R"(,"version":)";
	out += json_string(version);
	out += R"(})";
	bool has_bearer_auth = false;
	bool has_basic_auth = false;
	for (auto const &route: routes) {
		if (!route.auth_scheme.empty() || !route.bearer_token_policy.empty()) {
			auto const scheme = route.auth_scheme.empty() ? std::string_view{"bearer"} : route.auth_scheme;
			if (scheme == "basic") {
				has_basic_auth = true;
			} else {
				has_bearer_auth = true;
			}
		}
	}
	if (has_bearer_auth || has_basic_auth) {
		out += R"(,"components":{"securitySchemes":{)";
		bool first_scheme = true;
		if (has_bearer_auth) {
			out += R"("bearerAuth":{"type":"http","scheme":"bearer"})";
			first_scheme = false;
		}
		if (has_basic_auth) {
			if (!first_scheme) {
				out += ',';
			}
			out += R"("basicAuth":{"type":"http","scheme":"basic"})";
		}
		out += R"(}})";
	}
	out += R"(,"paths":{)";
	std::vector<std::string_view> path_order;
	std::map<std::string_view, std::vector<AppOpenApiRoute const *>> routes_by_path;
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
		out += json_string(path);
		out += ":{";
		for (std::size_t route_index = 0; route_index < path_routes.size(); ++route_index) {
			auto const &route = *path_routes[route_index];
			if (route_index != 0) {
				out += ',';
			}
			out += json_string(openapi_method_key(route.method));
			out += ":{";
			if (!route.name.empty()) {
				out += R"("operationId":)";
				out += json_string(route.name);
				out += ',';
			}
			if (!route.openapi_summary.empty()) {
				out += R"("summary":)";
				out += json_string(route.openapi_summary);
				out += ',';
			}
			if (!route.auth_scheme.empty() || !route.bearer_token_policy.empty()) {
				auto const scheme = route.auth_scheme.empty() ? std::string_view{"bearer"} : route.auth_scheme;
				out += R"("security":[{)";
				out += json_string(openapi_auth_scheme_name(scheme));
				out += R"(:[]}])";
				if (!route.bearer_token_policy.empty()) {
					out += R"(,"x-bearer-token-policy":)";
					out += json_string(route.bearer_token_policy);
				}
				out += ',';
			}
			if (route.timeout.count() != 0) {
				out += R"("x-timeout-ms":)";
				out += std::to_string(route.timeout.count());
				out += ',';
			}
			if (!route.rate_limit.empty()) {
				out += R"("x-rate-limit":)";
				out += json_string(route.rate_limit);
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
				out += json_string(route.path_params[i]);
				out += R"(,"in":"path","required":true,"schema":)";
				if (auto const *types = route.path_param_types; types != nullptr) {
					auto const type =
						std::ranges::find(*types, route.path_params[i], &std::pair<std::string, std::string>::first);
					if (type != types->end()) {
						out += openapi_schema_for_path_type(type->second);
					} else {
						out += openapi_schema_for_path_type({});
					}
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
					out += json_string(route.consumes[i]);
					out += R"(:{"schema":)";
					out += route.request_body_schema.empty() ? R"({"type":"object"})" : route.request_body_schema;
					out += "}";
				}
				out += "}}";
			}
			out += R"(,"responses":{)";
			out += json_string(std::to_string(route.success_status));
			out += R"(:{"description":)";
			out += json_string(route.success_status == kHttpCreated ? "Created" : "OK");
			if (!route.produces.empty()) {
				out += R"(,"content":{)";
				for (std::size_t i = 0; i < route.produces.size(); ++i) {
					if (i != 0) {
						out += ',';
					}
					out += json_string(route.produces[i]);
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
			if (!route.auth_scheme.empty() || !route.bearer_token_policy.empty()) {
				out += R"(,"401":{"description":"Unauthorized"})";
			}
			if (!route.rate_limit.empty()) {
				out += R"(,"429":{"description":"Too Many Requests"})";
			}
			if (route.timeout.count() != 0) {
				out += R"(,"504":{"description":"Gateway Timeout"})";
			}
			out += "}";
			out += "}";
		}
		out += "}";
	}
	out += "}}";
	return out;
}

} // namespace conflux::http::detail
