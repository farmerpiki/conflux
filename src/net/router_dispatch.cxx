module;
#include <ctime>
export module conflux.net.router_dispatch;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http.realtime;
import conflux.net.http.response;
import conflux.net.router_match;
import conflux.work;

#ifndef CONFLUX_ROUTER_LAZY_ROUTE_METADATA
	#define CONFLUX_ROUTER_LAZY_ROUTE_METADATA 1
#endif

export template<typename Pool, typename Handler>
void router_launch_sse_handler(
	Pool const &pool,
	Handler handler,
	Request matched,
	std::shared_ptr<SseChannel> const &channel) {
	if (!pool->enqueue([h = std::move(handler), matched = std::move(matched), channel]() mutable {
			RequestView const matched_view{matched};
			h(matched_view, channel);
			channel->close();
		})) {
		channel->close();
	}
}

export Response router_defer_http_task(
	conflux::work::root::Task<Response> task) {
	auto deferred = std::make_shared<DeferredResponse>();
	auto jh = std::make_shared<conflux::work::root::TaskJoinHandle<Response>>(
		conflux::work::root::into_join_handle(std::move(task)));
	deferred->attach_cancel(jh->control());
	jh->control().set_on_ready_or_run([deferred, jh]() noexcept {
		try {
			auto outcome = conflux::work::root::blocking_join(std::move(*jh));
			if (outcome.is_success()) {
				deferred->complete(std::move(outcome).success().value);
			} else {
				deferred->complete(Response::internal_error());
			}
		} catch (std::exception const &ex) { deferred->complete(Response::internal_error(ex.what())); } catch (...) {
			deferred->complete(Response::internal_error());
		}
	});
	return Response::deferred(std::move(deferred));
}

export Response router_run_async_http_task(
	conflux::work::root::Task<Response> task) {
	return router_defer_http_task(std::move(task));
}

export template<typename RouteRange, typename SseRange, typename NotFoundHandler, typename ErrorHandler, typename Pool>
[[nodiscard]] Response dispatch_immediate_routes(
	RequestView const &req,
	std::string_view path_sv,
	bool is_head,
	RouteRange const &routes,
	SseRange const &sse_routes,
	NotFoundHandler const &not_found_handler,
	ErrorHandler const &error_handler,
	Pool const &work_pool) {
	try {
		HttpFieldsView matched_params;
		bool const observe_route = req.params.get("__conflux_observe_route").has_value();

		// Regular routes first. Candidate selection has already filtered by method.
		for (auto const &route: routes) {
			matched_params.clear();
			bool const matched = route.has_exact_path ? (route.exact_path == path_sv) :
														match_segments(route.pattern, path_sv, matched_params);
			if (matched) {
				// HEAD matched to a GET route: present as GET so handlers are HEAD-transparent.
				std::string_view const effective_method =
					(is_head && route.method == "GET") ? std::string_view{"GET"} : req.method;
				if (route.has_exact_path
					&& !observe_route
					&& matched_params.empty()
					&& effective_method == req.method) {
					try {
						auto resp = route.handler(req);
						if (is_head) {
							resp.head_only = true;
						}
						return resp;
					} catch (std::exception const &ex) {
						return error_handler ? error_handler(req, ex) : Response::internal_error(ex.what());
					} catch (...) {
						return error_handler ? error_handler(req, std::runtime_error{"unknown std::exception"}) :
											   Response::internal_error();
					}
				}
				auto all_params = req.params;
				for (auto const &[k, v]: matched_params) {
					if (!all_params.get(k)) {
						all_params.emplace_back(k, v);
					}
				}
#if CONFLUX_ROUTER_LAZY_ROUTE_METADATA
				if (observe_route && !all_params.get("__conflux_route_pattern")) {
#else
				if (!all_params.get("__conflux_route_pattern")) {
#endif
					all_params.emplace_back_owned_value("__conflux_route_pattern", route.path_pattern);
				}
				RequestView const matched_view{
					effective_method,
					req.path,
					req.version,
					req.remote_addr,
					req.is_tls,
					std::move(all_params),
					req.headers,
					req.query,
					req.form,
					req.cookies,
					req.files,
					req.body};
				try {
					auto resp = route.handler(matched_view);
					if (is_head) {
						resp.head_only = true;
					}
					if (observe_route) {
						resp.headers.set("__conflux-route-pattern", route.path_pattern);
					}
					return resp;
				} catch (std::exception const &ex) {
					return error_handler ? error_handler(matched_view, ex) : Response::internal_error(ex.what());
				} catch (...) {
					return error_handler ? error_handler(matched_view, std::runtime_error{"unknown std::exception"}) :
										   Response::internal_error();
				}
			}
		}

		// SSE routes (GET only).
		if (req.method == "GET") {
			for (auto const &route: sse_routes) {
				matched_params.clear();
				bool const matched = route.has_exact_path ? (route.exact_path == path_sv) :
															match_segments(route.pattern, path_sv, matched_params);
				if (matched) {
					auto channel = std::make_shared<SseChannel>();
					Request matched = req.to_owned();
					for (auto &[k, v]: matched_params) {
						matched.params.emplace_back(std::string{k}, std::string{v});
					}
					matched.params.emplace_back("__conflux_route_pattern", route.path_pattern);
					router_launch_sse_handler(work_pool, route.handler, std::move(matched), channel);
					auto resp = Response::sse(std::move(channel));
					if (observe_route) {
						resp.headers.set("__conflux-route-pattern", route.path_pattern);
					}
					return resp;
				}
			}
		}

		if (not_found_handler) {
			return not_found_handler(req);
		}
		return Response::not_found(path_sv);
	} catch (...) { return Response::internal_error(); }
}

export template<typename RouteRange, typename SseRange, typename NotFoundHandler, typename ErrorHandler, typename Pool>
[[nodiscard]] Response dispatch_sync_routes(
	RequestView const &req,
	std::string_view path_sv,
	bool is_head,
	RouteRange const &routes,
	SseRange const &sse_routes,
	NotFoundHandler const &not_found_handler,
	ErrorHandler const &error_handler,
	Pool const &work_pool) {
	return dispatch_immediate_routes(
		req,
		path_sv,
		is_head,
		routes,
		sse_routes,
		not_found_handler,
		error_handler,
		work_pool);
}

export template<typename ContextRouteRange, typename Ctx>
[[nodiscard]] std::optional<conflux::work::root::Task<Response>> dispatch_context_route_tasks(
	RequestView const &req,
	Ctx const &ctx,
	std::string_view path_sv,
	ContextRouteRange const &context_routes) {
	if (context_routes.empty()) {
		return std::nullopt;
	}
	HttpFieldsView matched_params;
	bool const observe_route = req.params.get("__conflux_observe_route").has_value();
	for (auto const &route: context_routes) {
		matched_params.clear();
		bool const matched = route.has_exact_path ? (route.exact_path == path_sv) :
													match_segments(route.pattern, path_sv, matched_params);
		if (matched) {
			auto all_params = req.params;
			for (auto const &[k, v]: matched_params) {
				if (!all_params.get(k)) {
					all_params.emplace_back(k, v);
				}
			}
			std::string pattern;
			if (observe_route) {
				pattern = route.path_pattern;
				all_params.emplace_back_owned_value("__conflux_route_pattern", pattern);
			}
			RequestView const matched_view{
				req.method,
				req.path,
				req.version,
				req.remote_addr,
				req.is_tls,
				std::move(all_params),
				req.headers,
				req.query,
				req.form,
				req.cookies,
				req.files,
				req.body};
			return [](auto handler, RequestView req, Ctx const &ctx, std::string route_pattern, bool should_annotate)
					   -> conflux::work::root::Task<Response> {
				auto resp = co_await handler(req, ctx);
				if (should_annotate) {
					resp.headers.set("__conflux-route-pattern", std::move(route_pattern));
				}
				co_return resp;
			}(route.handler, std::move(matched_view), ctx, std::move(pattern), observe_route);
		}
	}
	return std::nullopt;
}

export template<typename ContextRouteRange, typename Ctx>
[[nodiscard]] std::optional<Response> dispatch_context_routes(
	RequestView const &req,
	Ctx const &ctx,
	std::string_view path_sv,
	ContextRouteRange const &context_routes) {
	auto task = dispatch_context_route_tasks(req, ctx, path_sv, context_routes);
	if (!task) {
		return std::nullopt;
	}
	return router_defer_http_task(std::move(*task));
}
