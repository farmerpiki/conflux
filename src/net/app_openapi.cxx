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
	std::string_view openapi_description;
	std::span<std::string const> openapi_tags;
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

struct OpenApiAppInfo {
	std::string_view description;
	std::string_view contact_name;
	std::string_view contact_url;
	std::string_view server_url;
};

[[nodiscard]] std::string openapi_method_key(
	std::string_view method) {
	return ascii_lower(method);
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

} // namespace conflux::http::detail

namespace {

using conflux::http::detail::AppOpenApiRoute;
using conflux::http::detail::json_string;
using conflux::http::detail::openapi_auth_scheme_name;
using conflux::http::detail::openapi_method_key;
using conflux::http::detail::openapi_schema_for_path_type;
using conflux::http::kHttpCreated;

struct OpenApiAuthUsage {
	bool bearer{};
	bool basic{};
};

struct OpenApiRouteGroups {
	std::vector<std::string_view> path_order;
	std::map<std::string_view, std::vector<AppOpenApiRoute const *>> routes_by_path;
};

[[nodiscard]] OpenApiAuthUsage collect_openapi_auth_usage(
	std::span<AppOpenApiRoute const> routes) {
	auto usage = OpenApiAuthUsage{};
	for (auto const &route: routes) {
		if (!route.auth_scheme.empty() || !route.bearer_token_policy.empty()) {
			auto const scheme = route.auth_scheme.empty() ? std::string_view{"bearer"} : route.auth_scheme;
			if (scheme == "basic") {
				usage.basic = true;
			} else {
				usage.bearer = true;
			}
		}
	}
	return usage;
}

[[nodiscard]] OpenApiRouteGroups collect_openapi_route_groups(
	std::span<AppOpenApiRoute const> routes) {
	auto groups = OpenApiRouteGroups{};
	for (auto const &route: routes) {
		auto [it, inserted] = groups.routes_by_path.try_emplace(route.path);
		if (inserted) {
			groups.path_order.push_back(route.path);
		}
		it->second.push_back(std::addressof(route));
	}
	return groups;
}

void append_openapi_info(
	std::string &out,
	std::string_view title,
	std::string_view version,
	conflux::http::detail::OpenApiAppInfo const &app_info) {
	out += R"({"openapi":"3.0.0","info":{"title":)";
	out += json_string(title);
	out += R"(,"version":)";
	out += json_string(version);
	if (!app_info.description.empty()) {
		out += R"(,"description":)";
		out += json_string(app_info.description);
	}
	if (!app_info.contact_name.empty() || !app_info.contact_url.empty()) {
		out += R"(,"contact":{)";
		auto first = true;
		if (!app_info.contact_name.empty()) {
			out += R"("name":)";
			out += json_string(app_info.contact_name);
			first = false;
		}
		if (!app_info.contact_url.empty()) {
			if (!first) {
				out += ',';
			}
			out += R"("url":)";
			out += json_string(app_info.contact_url);
		}
		out += '}';
	}
	out += R"(})";
	if (!app_info.server_url.empty()) {
		out += R"(,"servers":[{"url":)";
		out += json_string(app_info.server_url);
		out += R"(}])";
	}
}

void append_openapi_security_components(
	std::string &out,
	OpenApiAuthUsage usage) {
	if (!usage.bearer && !usage.basic) {
		return;
	}
	out += R"(,"components":{"securitySchemes":{)";
	auto first_scheme = true;
	if (usage.bearer) {
		out += R"("bearerAuth":{"type":"http","scheme":"bearer"})";
		first_scheme = false;
	}
	if (usage.basic) {
		if (!first_scheme) {
			out += ',';
		}
		out += R"("basicAuth":{"type":"http","scheme":"basic"})";
	}
	out += R"(}})";
}

void append_openapi_operation_metadata(
	std::string &out,
	AppOpenApiRoute const &route) {
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
	if (!route.openapi_description.empty()) {
		out += R"("description":)";
		out += json_string(route.openapi_description);
		out += ',';
	}
	if (!route.openapi_tags.empty()) {
		out += R"("tags":[)";
		for (std::size_t i = 0; i < route.openapi_tags.size(); ++i) {
			if (i != 0) {
				out += ',';
			}
			out += json_string(route.openapi_tags[i]);
		}
		out += R"(],)";
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
}

void append_openapi_parameters(
	std::string &out,
	AppOpenApiRoute const &route) {
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
}

void append_openapi_request_body(
	std::string &out,
	AppOpenApiRoute const &route) {
	if (route.consumes.empty()) {
		return;
	}
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

void append_openapi_responses(
	std::string &out,
	AppOpenApiRoute const &route) {
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
}

void append_openapi_operation(
	std::string &out,
	AppOpenApiRoute const &route) {
	out += json_string(openapi_method_key(route.method));
	out += ":{";
	append_openapi_operation_metadata(out, route);
	append_openapi_parameters(out, route);
	append_openapi_request_body(out, route);
	append_openapi_responses(out, route);
	out += "}";
}

void append_openapi_paths(
	std::string &out,
	OpenApiRouteGroups const &groups) {
	out += R"(,"paths":{)";
	for (std::size_t path_index = 0; path_index < groups.path_order.size(); ++path_index) {
		auto const &path = groups.path_order[path_index];
		auto const &path_routes = groups.routes_by_path.at(path);
		if (path_index != 0) {
			out += ',';
		}
		out += json_string(path);
		out += ":{";
		for (std::size_t route_index = 0; route_index < path_routes.size(); ++route_index) {
			if (route_index != 0) {
				out += ',';
			}
			append_openapi_operation(out, *path_routes[route_index]);
		}
		out += "}";
	}
	out += "}";
}

} // namespace

export namespace conflux::http::detail {

[[nodiscard]] std::string render_openapi_spec(
	std::span<AppOpenApiRoute const> routes,
	std::string_view title,
	std::string_view version,
	OpenApiAppInfo app_info = {}) {
	std::string out;
	append_openapi_info(out, title, version, app_info);
	append_openapi_security_components(out, collect_openapi_auth_usage(routes));
	append_openapi_paths(out, collect_openapi_route_groups(routes));
	out += "}";
	return out;
}

} // namespace conflux::http::detail
