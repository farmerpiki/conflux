module conflux.net.app;
import std;
import conflux.net.app.openapi;
import conflux.net.app.route_helpers;
import conflux.net.config;
import conflux.net.http_server;
import conflux.net.observability;
import conflux.net.router_dispatch;
import conflux.types;
import conflux.utils;

namespace conflux::http {

std::vector<AppRouteInfo> App::routes() const {
	std::vector<AppRouteInfo> out;
	out.reserve(route_metadata_.size());
	std::ranges::transform(route_metadata_, std::back_inserter(out), [](auto const &route) {
		return AppRouteInfo{
			.method = route.method,
			.path = route.path,
			.name = route.name,
			.handler_kind = route.handler_kind,
			.source_file = route.source_file,
			.source_line = route.source_line,
			.extractors = route.extractors,
			.path_params = route.path_params,
			.path_param_types = route.path_param_types,
			.required_state_count = route.required_states.size(),
			.consumes = route.consumes,
			.produces = route.produces,
			.request_body_schema = route.request_body_schema,
			.response_schema = route.response_schema,
			.success_status = route.success_status,
			.problem_response = route.problem_response,
			.max_body_size = *route.max_body_size,
			.timeout = *route.timeout,
			.middleware_count = route.middleware_count,
			.rate_limit = route.rate_limit->name,
			.bearer_token_policy = *route.bearer_token_policy,
			.openapi_summary = route.openapi_summary,
			.allow_get_body = route.allow_get_body,
			.body_mode = route.body_mode};
	});
	return out;
}

std::vector<AppStaticMountInfo> App::static_mounts() const {
	std::vector<AppStaticMountInfo> out;
	out.reserve(static_mounts_.size());
	std::ranges::transform(static_mounts_, std::back_inserter(out), [](auto const &mount) {
		return AppStaticMountInfo{
			.url_prefix = mount.url_prefix,
			.root_dir = mount.root_dir,
			.source_file = mount.source_file,
			.source_line = mount.source_line};
	});
	return out;
}

std::string App::route_table() const {
	std::string out;
	for (auto const &route: route_metadata_) {
		if (!out.empty()) {
			out += '\n';
		}
		out += std::format("{} {} [{}]", route.method, route.path, route.handler_kind);
		if (!route.name.empty()) {
			out += std::format(" name={}", route.name);
		}
		if (route.middleware_count != 0) {
			out += std::format(" middleware={}", route.middleware_count);
		}
		if (*route.max_body_size != 0) {
			out += std::format(" max_body={}", *route.max_body_size);
		}
		if (route.timeout->count() != 0) {
			out += std::format(" timeout={}ms", route.timeout->count());
		}
		if (!route.rate_limit->name.empty()) {
			out += std::format(" rate_limit={}", route.rate_limit->name);
		}
		if (!route.bearer_token_policy->empty()) {
			out += std::format(" bearer_token={}", *route.bearer_token_policy);
		}
		if (!route.extractors.empty()) {
			out += " ";
			for (std::size_t i = 0; i < route.extractors.size(); ++i) {
				if (i != 0) {
					out += ",";
				}
				out += route.extractors[i];
			}
		}
		if (route.body_mode != BodyMode::none) {
			out += std::format(" body_mode={}", body_mode_name(route.body_mode));
		}
	}
	for (auto const &mount: static_mounts_) {
		if (!out.empty()) {
			out += '\n';
		}
		out += std::format("STATIC {} root={}", mount.url_prefix, mount.root_dir);
	}
	return out;
}

std::string App::openapi_spec(
	std::string_view title,
	std::string_view version,
	detail::OpenApiAppInfo app_info) const {
	std::vector<detail::AppOpenApiRoute> routes;
	routes.reserve(route_metadata_.size());
	std::ranges::transform(route_metadata_, std::back_inserter(routes), [](auto const &route) {
		return detail::AppOpenApiRoute{
			.method = route.method,
			.path = route.path,
			.name = route.name,
			.openapi_summary = route.openapi_summary,
			.openapi_description = route.openapi_description,
			.openapi_tags = route.openapi_tags,
			.bearer_token_policy = *route.bearer_token_policy,
			.auth_scheme = route.openapi_auth_scheme,
			.timeout = *route.timeout,
			.rate_limit = route.rate_limit->name,
			.max_body_size = *route.max_body_size,
			.middleware_count = route.middleware_count,
			.path_params = route.path_params,
			.path_param_types = std::addressof(route.path_param_types),
			.consumes = route.consumes,
			.request_body_schema = route.request_body_schema,
			.success_status = route.success_status,
			.produces = route.produces,
			.response_schema = route.response_schema,
			.problem_response = route.problem_response};
	});
	return detail::render_openapi_spec(routes, title, version, app_info);
}

App::RouteRef App::openapi(
	std::string_view path,
	std::string_view title,
	std::string_view version,
	detail::OpenApiAppInfo app_info,
	std::source_location loc) {
	auto spec = openapi_spec(title, version, app_info);
	return get(
		path,
		[spec = std::move(spec)](conflux::http::RequestView const &) -> Response { return Response::json(spec); },
		loc);
}

ValidationReport App::validate() const {
	ValidationReport report;
	validate_app_state(report);
	validate_runtime_config(report);
	validate_route_patterns_and_uniqueness(report);
	validate_route_extractors_and_body_policy(report);
	validate_static_mounts(report);
	return report;
}

void App::validate_app_state(
	ValidationReport &report) const {
	for (auto const &issue: state_issues_) {
		report.issues.push_back(
			ValidationIssue{.code = "app.validation", .message = issue, .method = "APP", .path = "state"});
	}
}

void App::validate_runtime_config(
	ValidationReport &report) const {
	report.config_issues = conflux::http::validate_config(cfg_);
	if (auto caps = conflux::runtime::detect_capabilities()) {
		report.capability_issues = conflux::http::validate_config_capabilities(cfg_, *caps);
		report.capability_issues_block_startup =
			cfg_.feature_fallback == conflux::runtime::FeatureFallback::fail_fast && !report.capability_issues.empty();
	} else {
		report.capability_issues.push_back(caps.error());
		report.capability_issues_block_startup = cfg_.feature_fallback == conflux::runtime::FeatureFallback::fail_fast;
	}
	validate_tls_config(report);
}

void App::validate_route_patterns_and_uniqueness(
	ValidationReport &report) const {
	std::map<std::pair<std::string, std::string>, AppRouteMetadata const *> seen;
	std::map<std::pair<std::string, std::string>, AppRouteMetadata const *> seen_shapes;
	for (auto const &route: route_metadata_) {
		if (auto pattern_issue = detail::validate_path_pattern(route.path)) {
			report.issues.push_back(
				ValidationIssue{
					.code = "http.route.invalid_pattern",
					.message = *pattern_issue,
					.method = route.method,
					.path = route.path,
					.source_file = route.source_file,
					.source_line = route.source_line});
		}
		auto key = std::pair{route.method, route.path};
		auto [it, inserted] = seen.emplace(key, std::addressof(route));
		if (!inserted) {
			report.issues.push_back(
				ValidationIssue{
					.code = "http.route.duplicate",
					.message = "duplicate route",
					.method = route.method,
					.path = route.path,
					.source_file = route.source_file,
					.source_line = route.source_line,
					.related_source_file = it->second->source_file,
					.related_source_line = it->second->source_line});
		}
		auto shape_key = std::pair{route.method, detail::route_shape(route.path)};
		auto [shape_it, shape_inserted] = seen_shapes.emplace(shape_key, std::addressof(route));
		if (!shape_inserted && shape_it->second->path != route.path) {
			report.issues.push_back(
				ValidationIssue{
					.code = "http.route.ambiguous",
					.message = std::format("ambiguous route; also matches {}", shape_it->second->path),
					.method = route.method,
					.path = route.path,
					.source_file = route.source_file,
					.source_line = route.source_line,
					.related_source_file = shape_it->second->source_file,
					.related_source_line = shape_it->second->source_line});
		}
	}
}

void App::validate_route_extractors_and_body_policy(
	ValidationReport &report) const {
	for (auto const &route: route_metadata_) {
		for (auto const &state_type: route.required_states) {
			if (!states_->contains(state_type)) {
				report.issues.push_back(
					ValidationIssue{
						.code = "app.state.missing",
						.message = "missing app state",
						.method = route.method,
						.path = route.path,
						.source_file = route.source_file,
						.source_line = route.source_line});
			}
		}
		for (auto const &path_extractor: route.path_extractors) {
			if (!std::ranges::contains(route.path_params, path_extractor)) {
				report.issues.push_back(
					ValidationIssue{
						.message = std::format(
							"missing path parameter for Path<{}>.{}",
							path_extractor,
							detail::available_path_params_message(route.path_params, route.path_param_types)),
						.method = route.method,
						.path = route.path,
						.source_file = route.source_file,
						.source_line = route.source_line});
			}
		}
		for (auto const &[name, expected_type]: route.path_extractor_types) {
			if (expected_type.empty()) {
				continue;
			}
			auto const it =
				std::ranges::find(route.path_param_types, name, &std::pair<std::string, std::string>::first);
			if (it != route.path_param_types.end() && !it->second.empty() && it->second != expected_type) {
				report.issues.push_back(
					ValidationIssue{
						.message = std::format(
							"path parameter type mismatch for Path<{}>: route has {}, handler expects {}",
							name,
							it->second,
							expected_type),
						.method = route.method,
						.path = route.path,
						.source_file = route.source_file,
						.source_line = route.source_line});
			}
		}
		for (auto const &[index, expected_type]: route.path_index_extractor_types) {
			if (index >= route.path_params.size()) {
				report.issues.push_back(
					ValidationIssue{
						.message = std::format(
							"missing path parameter for Path<{}>.{}",
							index,
							detail::available_path_params_message(route.path_params, route.path_param_types)),
						.method = route.method,
						.path = route.path,
						.source_file = route.source_file,
						.source_line = route.source_line});
				continue;
			}
			if (expected_type.empty()) {
				continue;
			}
			auto const type_it = std::ranges::find(
				route.path_param_types,
				route.path_params[index],
				&std::pair<std::string, std::string>::first);
			if (type_it != route.path_param_types.end()
				&& !type_it->second.empty()
				&& type_it->second != expected_type) {
				report.issues.push_back(
					ValidationIssue{
						.message = std::format(
							"path parameter type mismatch for Path<{}>: route has {}, handler expects {}",
							index,
							type_it->second,
							expected_type),
						.method = route.method,
						.path = route.path,
						.source_file = route.source_file,
						.source_line = route.source_line});
			}
		}
		if (route.method == "GET" && route.uses_body && !route.allow_get_body) {
			report.issues.push_back(
				ValidationIssue{
					.message = "body extractor used on GET route",
					.method = route.method,
					.path = route.path,
					.source_file = route.source_file,
					.source_line = route.source_line});
		}
		auto const has_upload_body = std::ranges::contains(route.extractors, "UploadBody");
		if (has_upload_body && route.handler_kind != "app") {
			report.issues.push_back(
				ValidationIssue{
					.message = "UploadBody extractor requires an app async handler",
					.method = route.method,
					.path = route.path,
					.source_file = route.source_file,
					.source_line = route.source_line});
		}
		if (route.uses_body && !route_has_body_limit(route)) {
			report.issues.push_back(
				ValidationIssue{
					.message = "body extractor used without body limit",
					.method = route.method,
					.path = route.path,
					.source_file = route.source_file,
					.source_line = route.source_line});
		}
		if (openapi_strict_) {
			validate_openapi_completeness(route, report);
		}
	}
}

void App::validate_static_mounts(
	ValidationReport &report) const {
	for (auto const &mount: static_mounts_) {
		std::error_code ec;
		auto const status = std::filesystem::status(mount.root_dir, ec);
		if (ec || !std::filesystem::exists(status)) {
			report.issues.push_back(
				ValidationIssue{
					.message = std::format("static root does not exist: {}", mount.root_dir),
					.method = "STATIC",
					.path = mount.url_prefix,
					.source_file = mount.source_file,
					.source_line = mount.source_line});
			continue;
		}
		if (!std::filesystem::is_directory(status)) {
			report.issues.push_back(
				ValidationIssue{
					.message = std::format("static root is not a directory: {}", mount.root_dir),
					.method = "STATIC",
					.path = mount.url_prefix,
					.source_file = mount.source_file,
					.source_line = mount.source_line});
		}
	}
}

bool App::route_has_body_limit(
	AppRouteMetadata const &route) const noexcept {
#if CONFLUX_HAS_JSON
	return effective_body_limit(
			   *route.max_body_size,
			   cfg_.max_body_size,
			   json_options_ ? json_options_->max_body_size : 0)
		!= 0;
#else
	return effective_body_limit(*route.max_body_size, cfg_.max_body_size, 0) != 0;
#endif
}

void App::validate_openapi_completeness(
	AppRouteMetadata const &route,
	ValidationReport &report) const {
	auto add_issue = [&](std::string message) {
		report.issues.push_back(
			ValidationIssue{
				.message = std::move(message),
				.method = route.method,
				.path = route.path,
				.source_file = route.source_file,
				.source_line = route.source_line});
	};
	if (route.name.empty()) {
		add_issue("OpenAPI strict mode: route operationId is missing");
	}
	if (route.openapi_summary.empty()) {
		add_issue("OpenAPI strict mode: route summary is missing");
	}
	if (route.produces.empty() && route.method != "HEAD") {
		add_issue("OpenAPI strict mode: route response content metadata is missing");
	}
	if (route.uses_body && route.consumes.empty()) {
		add_issue("OpenAPI strict mode: route request body content metadata is missing");
	}
}

void App::validate_tls_config(
	ValidationReport &report) const {
	auto const primary_file_cert = !cfg_.cert_file.empty();
	auto const primary_file_key = !cfg_.key_file.empty();
	auto const primary_pem_cert = !cfg_.cert_pem.empty();
	auto const primary_pem_key = !cfg_.key_pem.empty();
	auto const primary_file_tls = primary_file_cert && primary_file_key;
	auto const primary_pem_tls = primary_pem_cert && primary_pem_key;
	auto const primary_tls = primary_file_tls || primary_pem_tls;
	if (primary_file_cert != primary_file_key) {
		report.issues.push_back(
			ValidationIssue{
				.message = "TLS config invalid: cert_file and key_file must be set together",
				.method = "APP",
				.path = "config"});
	}
	if (primary_pem_cert != primary_pem_key) {
		report.issues.push_back(
			ValidationIssue{
				.message = "TLS config invalid: cert_pem and key_pem must be set together",
				.method = "APP",
				.path = "config"});
	}
	if ((primary_file_cert || primary_file_key) && (primary_pem_cert || primary_pem_key)) {
		report.issues.push_back(
			ValidationIssue{
				.message = "TLS config invalid: choose either cert_file/key_file or cert_pem/key_pem",
				.method = "APP",
				.path = "config"});
	}
	if (cfg_.http3.enabled && !primary_tls) {
		report.issues.push_back(
			ValidationIssue{
				.message = "TLS config invalid: HTTP/3 requires TLS credentials",
				.method = "APP",
				.path = "config"});
	}
	if (cfg_.http_redirect_to_https && !primary_tls) {
		report.issues.push_back(
			ValidationIssue{
				.message = "TLS config invalid: HTTPS redirect requires TLS credentials",
				.method = "APP",
				.path = "config"});
	}
	for (auto const &host: cfg_.virtual_hosts) {
		auto const host_file_cert = !host.cert_file.empty();
		auto const host_file_key = !host.key_file.empty();
		auto const host_pem_cert = !host.cert_pem.empty();
		auto const host_pem_key = !host.key_pem.empty();
		auto const host_has_tls = (host_file_cert && host_file_key) || (host_pem_cert && host_pem_key);
		if (host.hostname.empty()) {
			report.issues.push_back(
				ValidationIssue{
					.message = "TLS config invalid: virtual host hostname is empty",
					.method = "APP",
					.path = "config"});
		}
		if (host_file_cert != host_file_key) {
			report.issues.push_back(
				ValidationIssue{
					.message = std::format(
						"TLS config invalid: virtual host '{}' cert_file and key_file must be set together",
						host.hostname),
					.method = "APP",
					.path = "config"});
		}
		if (host_pem_cert != host_pem_key) {
			report.issues.push_back(
				ValidationIssue{
					.message = std::format(
						"TLS config invalid: virtual host '{}' cert_pem and key_pem must be set together",
						host.hostname),
					.method = "APP",
					.path = "config"});
		}
		if ((host_file_cert || host_file_key) && (host_pem_cert || host_pem_key)) {
			report.issues.push_back(
				ValidationIssue{
					.message = std::format(
						"TLS config invalid: virtual host '{}' chooses both file and PEM credentials",
						host.hostname),
					.method = "APP",
					.path = "config"});
		}
		if (host_has_tls && !primary_tls) {
			report.issues.push_back(
				ValidationIssue{
					.message = std::format(
						"TLS config invalid: virtual host '{}' requires primary TLS credentials",
						host.hostname),
					.method = "APP",
					.path = "config"});
		}
	}
}

std::expected<std::unique_ptr<HttpServer>, std::string> App::try_server(
	AppRunOptions opts) && {
	auto report = validate();
	if (!report) {
		return std::unexpected{report.summary()};
	}
	cfg_.port = opts.port;
	auto server = HttpServer::try_create(cfg_, std::move(router_));
	if (server && observability_) {
		(*server)->set_observability_hooks(observability_server_hooks(*observability_));
	}
	return server;
}

std::expected<std::unique_ptr<HttpServer>, std::string> App::prepare_server(
	AppRunOptions opts) && {
	return std::move(*this).try_server(opts);
}

std::expected<RunStatus, std::string> App::try_run(
	AppRunOptions opts) && {
	auto srv = std::move(*this).try_server(opts);
	if (!srv) {
		return std::unexpected{std::move(srv.error())};
	}
	return (*srv)->run();
}

RunStatus App::run(
	AppRunOptions opts) && noexcept {
	try {
		auto report = validate();
		if (!report) {
			auto summary = report.summary();
			eprintln("http app validation failed:");
			eprintln(summary);
			return RunStatus::fatal_internal_exception;
		}
		cfg_.port = opts.port;
		HttpServer srv{cfg_, std::move(router_)};
		if (observability_) {
			srv.set_observability_hooks(observability_server_hooks(*observability_));
		}
		return srv.run();
	} catch (std::exception const &ex) {
		eprintln("http app run failed:");
		eprintln(ex.what());
		return RunStatus::fatal_internal_exception;
	} catch (...) {
		eprintln("http app run failed: unknown exception");
		return RunStatus::fatal_internal_exception;
	}
}
conflux::work::root::Task<Response> App::run_scoped_context_middlewares(
	std::shared_ptr<ScopedContextMiddlewareList const> middlewares,
	conflux::http::RequestView req,
	RequestContext const &ctx,
	conflux::http::Router::ContextHandler inner) {
	if (!middlewares || middlewares->empty()) {
		co_return co_await inner(req, ctx);
	}
	struct Step {
		std::shared_ptr<ScopedContextMiddlewareList const> middlewares;
		conflux::http::Router::ContextHandler inner;
		std::size_t index{};

		conflux::work::root::Task<Response> call(
			std::shared_ptr<Step> self,
			conflux::http::RequestView const &r,
			RequestContext const &c) {
			if (index == middlewares->size()) {
				return inner(r, c);
			}
			auto const &middleware = (*middlewares)[index++];
			conflux::http::Router::ContextHandler next =
				[self = std::move(self)](
					conflux::http::RequestView const &next_req,
					RequestContext const &next_ctx) mutable -> conflux::work::root::Task<Response> {
				return self->call(self, next_req, next_ctx);
			};
			return middleware(r, c, next);
		}
	};
	auto step = detail::shared_new<Step>(std::move(middlewares), std::move(inner));
	co_return co_await step->call(step, req, ctx);
}

conflux::work::root::Task<Response> App::run_scoped_sync_route_as_context(
	std::shared_ptr<ScopedContextMiddlewareList const> context_middlewares,
	std::shared_ptr<ScopedMiddlewareList const> middlewares,
	conflux::http::RequestView req,
	RequestContext const &ctx,
	conflux::http::Router::Handler inner) {
	conflux::http::Router::ContextHandler context_inner =
		[middlewares = std::move(middlewares), inner = std::move(inner)](
			conflux::http::RequestView req,
			RequestContext const &) mutable -> conflux::work::root::Task<Response> {
		co_return run_scoped_middlewares(middlewares, req, std::move(inner));
	};
	co_return co_await run_scoped_context_middlewares(
		std::move(context_middlewares),
		std::move(req),
		ctx,
		std::move(context_inner));
}

conflux::work::root::Task<Response> App::run_owned_scoped_context_route(
	conflux::http::Router::ContextHandler inner,
	conflux::http::OwnedRequest req,
	RequestContext ctx) {
	conflux::http::RequestView const view{req};
	co_return co_await inner(view, ctx);
}

conflux::work::root::Task<Response> App::run_scoped_context_route(
	std::shared_ptr<ScopedMiddlewareList const> middlewares,
	std::shared_ptr<ScopedContextMiddlewareList const> context_middlewares,
	conflux::http::RequestView req,
	RequestContext const &ctx,
	conflux::http::Router::ContextHandler inner) {
	auto context_inner = [context_middlewares = std::move(context_middlewares), inner = std::move(inner)](
							 conflux::http::RequestView const &inner_req,
							 RequestContext const &inner_ctx) mutable -> conflux::work::root::Task<Response> {
		co_return co_await run_scoped_context_middlewares(context_middlewares, inner_req, inner_ctx, inner);
	};
	if (!middlewares || middlewares->empty()) {
		co_return co_await context_inner(req, ctx);
	}
	conflux::http::Router::Handler sync_inner = [ctx, context_inner = std::move(context_inner)](
													conflux::http::RequestView const &inner_req) mutable -> Response {
		return conflux::http::detail::router_defer_http_task(
			run_owned_scoped_context_route(context_inner, inner_req.to_owned(), ctx));
	};
	co_return run_scoped_middlewares(middlewares, req, std::move(sync_inner));
}

conflux::work::root::Task<Response> App::extraction_failure_response(
	Response response) {
	co_return response;
}

} // namespace conflux::http
