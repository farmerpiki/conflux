module;
#include <ctime>
export module conflux.net.router_dispatch;

import std;
import conflux.types;
import conflux.small_function;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http.realtime;
import conflux.net.http.response;
import conflux.net.router_match;
import conflux.work;

#ifndef CONFLUX_ROUTER_LAZY_ROUTE_METADATA
	#define CONFLUX_ROUTER_LAZY_ROUTE_METADATA 1
#endif

namespace conflux::http::detail {

export struct DeferredTaskOptions {
	std::chrono::milliseconds timeout = conflux::http::DeferredResponse::kDefaultTimeout;
};

export struct DeferredRouteTask {
	conflux::work::root::Task<conflux::http::Response> task;
	DeferredTaskOptions options{};
};

export template<typename Pool, typename Handler>
void router_launch_sse_handler(
	Pool const &pool,
	Handler handler,
	conflux::http::OwnedRequest matched,
	std::shared_ptr<conflux::http::SseChannel> const &channel) {
	if (!pool->enqueue([h = std::move(handler), matched = std::move(matched), channel]() mutable {
			conflux::http::RequestView const matched_view{matched};
			h(matched_view, channel);
			channel->close();
		})) {
		channel->close();
	}
}

export conflux::http::Response router_defer_http_task(
	conflux::work::root::Task<conflux::http::Response> task,
	DeferredTaskOptions options = {}) {
	namespace wroot = conflux::work::root;
	auto deferred = std::make_shared<conflux::http::DeferredResponse>(options.timeout);
	auto jh = std::make_shared<conflux::work::root::TaskJoinHandle<conflux::http::Response>>(
		conflux::work::root::into_join_handle(std::move(task)));
	deferred->attach_cancel(jh->control());
	auto complete_ready = [deferred, jh]() noexcept {
		try {
			auto outcome = wroot::join_ready(std::move(*jh));
			if (outcome.is_success()) {
				deferred->complete(std::move(outcome).success().value);
			} else if (outcome.is_cancelled() && outcome.cancelled().reason == wroot::CancelReason::deadline) {
				deferred->complete(conflux::http::Response::gateway_timeout());
			} else {
				deferred->complete(conflux::http::Response::internal_error());
			}
		} catch (std::exception const &ex) {
			deferred->complete(conflux::http::Response::internal_error(ex.what()));
		} catch (...) { deferred->complete(conflux::http::Response::internal_error()); }
	};
	auto result = jh->control().try_set_on_ready(::conflux::detail::small_move_only_function<void()>{complete_ready});
	switch (result.status) {
	case wroot::ReadyRegistration::installed: break;
	case wroot::ReadyRegistration::already_ready:
		if (result.rejected_fn) {
			result.rejected_fn();
		}
		break;
	case wroot::ReadyRegistration::already_installed:
		(void)jh->control().request_cancel();
		deferred->complete(conflux::http::Response::internal_error("task already has a ready callback"));
		break;
	case wroot::ReadyRegistration::empty:
		deferred->complete(conflux::http::Response::internal_error("empty async task"));
		break;
	}
	return conflux::http::Response::deferred(std::move(deferred));
}

[[nodiscard]] bool route_matches(
	auto const &route,
	std::string_view path,
	conflux::http::HttpFieldsView &matched_params) {
	return route.has_exact_path ? (route.exact_path == path) :
								  conflux::http::detail::match_segments(route.pattern, path, matched_params);
}

[[nodiscard]] bool should_observe_route(
	conflux::http::RequestView const &req) {
	return req.params.get("__conflux_observe_route").has_value();
}

[[nodiscard]] std::string_view effective_regular_route_method(
	conflux::http::RequestView const &req,
	bool is_head,
	auto const &route) {
	return (is_head && route.method == "GET") ? std::string_view{"GET"} : req.method;
}

[[nodiscard]] bool can_use_exact_route_request(
	conflux::http::RequestView const &req,
	auto const &route,
	conflux::http::HttpFieldsView const &matched_params,
	std::string_view effective_method,
	bool observe_route) {
	return route.has_exact_path && !observe_route && matched_params.empty() && effective_method == req.method;
}

[[nodiscard]] conflux::http::HttpFieldsView route_params_with_matches(
	conflux::http::RequestView const &req,
	conflux::http::HttpFieldsView const &matched_params,
	std::string const &route_pattern,
	[[maybe_unused]] bool observe_route) {
	conflux::http::HttpFieldsView all_params;
	all_params.reserve(matched_params.size() + req.params.size() + 1);
	for (auto const &[k, v]: matched_params) {
		all_params.emplace_back(k, v);
	}
	for (auto const &[k, v]: req.params) {
		all_params.emplace_back(k, v);
	}
#if CONFLUX_ROUTER_LAZY_ROUTE_METADATA
	if (observe_route && !all_params.get("__conflux_route_pattern")) {
#else
	if (!all_params.get("__conflux_route_pattern")) {
#endif
		all_params.emplace_back_owned_value("__conflux_route_pattern", route_pattern);
	}
	return all_params;
}

[[nodiscard]] conflux::http::RequestView matched_regular_route_view(
	conflux::http::RequestView const &req,
	std::string_view effective_method,
	conflux::http::HttpFieldsView const &matched_params,
	std::string const &route_pattern,
	bool observe_route) {
	return conflux::http::RequestView{
		effective_method,
		req.path,
		req.version,
		req.remote_addr,
		req.is_tls,
		route_params_with_matches(req, matched_params, route_pattern, observe_route),
		req.headers,
		req.query,
		req.form,
		req.cookies,
		req.files,
		req.body};
}

[[nodiscard]] conflux::http::Response route_handler_error_response(
	conflux::http::RequestView const &req,
	auto const &error_handler,
	std::exception const &ex) {
	return error_handler ? error_handler(req, ex) : conflux::http::Response::internal_error(ex.what());
}

[[nodiscard]] conflux::http::Response route_handler_unknown_error_response(
	conflux::http::RequestView const &req,
	auto const &error_handler) {
	return error_handler ? error_handler(req, std::runtime_error{"unknown std::exception"}) :
						   conflux::http::Response::internal_error();
}

[[nodiscard]] conflux::http::Response invoke_regular_route_handler(
	auto const &route,
	conflux::http::RequestView const &req,
	bool is_head,
	bool observe_route,
	auto const &error_handler) {
	try {
		auto resp = route.handler(req);
		if (is_head) {
			resp.head_only = true;
		}
		if (observe_route) {
			resp.headers.set("__conflux-route-pattern", route.path_pattern);
		}
		return resp;
	} catch (std::exception const &ex) { return route_handler_error_response(req, error_handler, ex); } catch (...) {
		return route_handler_unknown_error_response(req, error_handler);
	}
}

[[nodiscard]] std::optional<conflux::http::Response> dispatch_regular_route(
	conflux::http::RequestView const &req,
	std::string_view path,
	bool is_head,
	auto const &route,
	conflux::http::HttpFieldsView &matched_params,
	bool observe_route,
	auto const &error_handler) {
	matched_params.clear();
	if (!route_matches(route, path, matched_params)) {
		return std::nullopt;
	}
	auto const effective_method = effective_regular_route_method(req, is_head, route);
	if (can_use_exact_route_request(req, route, matched_params, effective_method, observe_route)) {
		return invoke_regular_route_handler(route, req, is_head, false, error_handler);
	}
	auto const matched_view =
		matched_regular_route_view(req, effective_method, matched_params, route.path_pattern, observe_route);
	return invoke_regular_route_handler(route, matched_view, is_head, observe_route, error_handler);
}

[[nodiscard]] conflux::http::OwnedRequest matched_sse_route_request(
	conflux::http::RequestView const &req,
	conflux::http::HttpFieldsView const &matched_params,
	std::string const &route_pattern) {
	auto matched = req.to_owned();
	conflux::http::HttpFields params;
	params.reserve(matched_params.size() + matched.params.size() + 1);
	for (auto const &[k, v]: matched_params) {
		params.emplace_back(std::string{k}, std::string{v});
	}
	for (auto const &[k, v]: matched.params) {
		params.emplace_back(std::string{k}, std::string{v});
	}
	params.emplace_back("__conflux_route_pattern", route_pattern);
	matched.params = std::move(params);
	return matched;
}

[[nodiscard]] conflux::http::Response launch_sse_route_response(
	conflux::http::RequestView const &req,
	auto const &route,
	conflux::http::HttpFieldsView const &matched_params,
	auto const &work_pool,
	bool observe_route) {
	auto channel = std::make_shared<conflux::http::SseChannel>();
	conflux::http::detail::router_launch_sse_handler(
		work_pool,
		route.handler,
		matched_sse_route_request(req, matched_params, route.path_pattern),
		channel);
	auto resp = conflux::http::Response::sse(std::move(channel));
	if (observe_route) {
		resp.headers.set("__conflux-route-pattern", route.path_pattern);
	}
	return resp;
}

[[nodiscard]] std::optional<conflux::http::Response> dispatch_sse_route(
	conflux::http::RequestView const &req,
	std::string_view path,
	auto const &route,
	conflux::http::HttpFieldsView &matched_params,
	auto const &work_pool,
	bool observe_route) {
	matched_params.clear();
	if (!route_matches(route, path, matched_params)) {
		return std::nullopt;
	}
	return launch_sse_route_response(req, route, matched_params, work_pool, observe_route);
}

[[nodiscard]] conflux::http::Response dispatch_not_found(
	conflux::http::RequestView const &req,
	std::string_view path,
	auto const &not_found_handler) {
	if (not_found_handler) {
		return not_found_handler(req);
	}
	return conflux::http::Response::not_found(path);
}

export template<typename RouteRange, typename SseRange, typename NotFoundHandler, typename ErrorHandler, typename Pool>
[[nodiscard]] conflux::http::Response dispatch_immediate_routes(
	conflux::http::RequestView const &req,
	std::string_view path_sv,
	bool is_head,
	RouteRange const &routes,
	SseRange const &sse_routes,
	NotFoundHandler const &not_found_handler,
	ErrorHandler const &error_handler,
	Pool const &work_pool) {
	try {
		conflux::http::HttpFieldsView matched_params;
		bool const observe_route = should_observe_route(req);

		for (auto const &route: routes) {
			if (auto resp = dispatch_regular_route(
					req,
					path_sv,
					is_head,
					route,
					matched_params,
					observe_route,
					error_handler)) {
				return std::move(*resp);
			}
		}

		if (req.method == "GET") {
			for (auto const &route: sse_routes) {
				if (auto resp = dispatch_sse_route(req, path_sv, route, matched_params, work_pool, observe_route)) {
					return std::move(*resp);
				}
			}
		}

		return dispatch_not_found(req, path_sv, not_found_handler);
	} catch (...) { return conflux::http::Response::internal_error(); }
}

template<typename Handler, typename Ctx>
[[nodiscard]] conflux::work::root::Task<conflux::http::Response> run_context_route_task(
	Handler handler,
	conflux::http::OwnedRequest req,
	Ctx ctx,
	std::string route_pattern,
	bool should_annotate,
	bool is_head,
	conflux::work::root::Cancellation cancel) {
	conflux::http::RequestView const view{req};
	auto resp = co_await cancel.await(handler(view, ctx));
	if (is_head) {
		resp.head_only = true;
	}
	if (should_annotate) {
		resp.headers.set("__conflux-route-pattern", std::move(route_pattern));
	}
	co_return resp;
}

export template<typename ContextRouteRange, typename Ctx>
[[nodiscard]] std::optional<DeferredRouteTask> dispatch_context_route_tasks(
	conflux::http::RequestView const &req,
	Ctx const &ctx,
	std::string_view path_sv,
	ContextRouteRange const &context_routes,
	bool is_head) {
	if (context_routes.empty()) {
		return std::nullopt;
	}
	conflux::http::HttpFieldsView matched_params;
	bool const observe_route = req.params.get("__conflux_observe_route").has_value();
	for (auto const &route: context_routes) {
		matched_params.clear();
		bool const matched = route.has_exact_path ?
								 (route.exact_path == path_sv) :
								 conflux::http::detail::match_segments(route.pattern, path_sv, matched_params);
		if (matched) {
			auto matched_req = req.to_owned();
			conflux::http::HttpFields params;
			params.reserve(matched_params.size() + matched_req.params.size() + 1);
			for (auto const &[k, v]: matched_params) {
				params.emplace_back(std::string{k}, std::string{v});
			}
			for (auto const &[k, v]: matched_req.params) {
				params.emplace_back(std::string{k}, std::string{v});
			}
			std::string pattern;
			if (observe_route) {
				pattern = route.path_pattern;
				params.emplace_back("__conflux_route_pattern", pattern);
			}
			matched_req.params = std::move(params);
			DeferredTaskOptions options{};
			if (route.timeout && *route.timeout > std::chrono::milliseconds{0}) {
				options.timeout = *route.timeout;
			}
			return DeferredRouteTask{
				.task = [](auto handler,
						   conflux::http::OwnedRequest req,
						   Ctx const &ctx,
						   std::string route_pattern,
						   bool should_annotate,
						   bool is_head) -> conflux::work::root::Task<conflux::http::Response> {
					return conflux::work::root::make_cancellable_task(
						[handler = std::move(handler),
						 req = std::move(req),
						 ctx,
						 route_pattern = std::move(route_pattern),
						 should_annotate,
						 is_head](conflux::work::root::Cancellation cancel) mutable
							-> conflux::work::root::Task<conflux::http::Response> {
							return run_context_route_task(
								std::move(handler),
								std::move(req),
								ctx,
								std::move(route_pattern),
								should_annotate,
								is_head,
								std::move(cancel));
						});
				}(route.handler, std::move(matched_req), ctx, std::move(pattern), observe_route, is_head),
				.options = options,
			};
		}
	}
	return std::nullopt;
}

export template<typename ContextRouteRange, typename Ctx>
[[nodiscard]] std::optional<conflux::http::Response> dispatch_context_routes(
	conflux::http::RequestView const &req,
	Ctx const &ctx,
	std::string_view path_sv,
	ContextRouteRange const &context_routes,
	bool is_head = false) {
	auto deferred_task =
		conflux::http::detail::dispatch_context_route_tasks(req, ctx, path_sv, context_routes, is_head);
	if (!deferred_task) {
		return std::nullopt;
	}
	return router_defer_http_task(std::move(deferred_task->task), deferred_task->options);
}

} // namespace conflux::http::detail
