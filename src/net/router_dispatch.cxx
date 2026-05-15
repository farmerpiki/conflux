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

export template<typename Pool, typename Handler>
void router_launch_sse_handler(
	Pool const &pool,
	Handler handler,
	HttpRequest matched,
	SP<SseChannel> const &channel) {
	if (!pool->enqueue([h = move(handler), matched = move(matched), channel]() mutable {
			HttpRequestView const matched_view{matched};
			h(matched_view, channel);
			channel->close();
		})) {
		channel->close();
	}
}

export HttpResponse router_defer_http_task(
	conflux::work::root::Task<HttpResponse> task) {
	auto deferred = make_shared<DeferredResponse>();
	auto jh = make_shared<conflux::work::root::TaskJoinHandle<HttpResponse>>(
		conflux::work::root::into_join_handle(move(task)));
	deferred->attach_cancel(jh->control());
	jh->control().set_on_ready_or_run([deferred, jh]() noexcept {
		try {
			auto outcome = conflux::work::root::join(move(*jh));
			if (outcome.is_success()) {
				deferred->complete(move(outcome).success().value);
			} else {
				deferred->complete(HttpResponse::internal_error());
			}
		} catch (exception const &ex) { deferred->complete(HttpResponse::internal_error(ex.what())); } catch (...) {
			deferred->complete(HttpResponse::internal_error());
		}
	});
	return HttpResponse::deferred(move(deferred));
}


export HttpResponse router_run_async_http_task(
	conflux::work::root::Task<HttpResponse> task) {
	return router_defer_http_task(move(task));
}

export template<typename RouteRange, typename SseRange, typename NotFoundHandler, typename ErrorHandler, typename Pool>
[[nodiscard]] HttpResponse dispatch_immediate_routes(
	HttpRequestView const &req,
	SV path_sv,
	bool is_head,
	RouteRange const &routes,
	SseRange const &sse_routes,
	NotFoundHandler const &not_found_handler,
	ErrorHandler const &error_handler,
	Pool const &work_pool) {
	try {
		HttpFieldsView matched_params;

		// Regular routes first.
		for (auto const &route: routes) {
			if (route.method != req.method && !(is_head && route.method == "GET")) {
				continue;
			}
			matched_params.clear();
			if (match_segments(route.pattern, path_sv, matched_params)) {
				auto all_params = req.params;
				for (auto const &[k, v]: matched_params) {
					if (!all_params.get(k)) {
						all_params.emplace_back(k, v);
					}
				}
				// HEAD matched to a GET route: present as GET so handlers are HEAD-transparent.
				SV const effective_method = (is_head && route.method == "GET") ? SV{"GET"} : req.method;
				HttpRequestView const matched_view{
					effective_method,
					req.path,
					req.version,
					req.remote_addr,
					req.is_tls,
					move(all_params),
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
					return resp;
				} catch (exception const &ex) {
					return error_handler ? error_handler(matched_view, ex) :
										  HttpResponse::internal_error(ex.what());
				} catch (...) {
					return error_handler ? error_handler(matched_view, RE{"unknown exception"}) :
										  HttpResponse::internal_error();
				}
			}
		}

		// SSE routes (GET only).
		if (req.method == "GET") {
			for (auto const &route: sse_routes) {
				matched_params.clear();
				if (match_segments(route.pattern, path_sv, matched_params)) {
					auto channel = make_shared<SseChannel>();
					HttpRequest matched = req.to_owned();
					for (auto &[k, v]: matched_params) {
						matched.params.emplace_back(S{k}, S{v});
					}
					router_launch_sse_handler(work_pool, route.handler, move(matched), channel);
					return HttpResponse::sse(move(channel));
				}
			}
		}

		if (not_found_handler) {
			return not_found_handler(req);
		}
		return HttpResponse::not_found(path_sv);
	} catch (...) { return HttpResponse::internal_error(); }
}


export template<typename RouteRange, typename SseRange, typename NotFoundHandler, typename ErrorHandler, typename Pool>
[[nodiscard]] HttpResponse dispatch_sync_routes(
	HttpRequestView const &req,
	SV path_sv,
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
[[nodiscard]] Opt<HttpResponse> dispatch_context_routes(
	HttpRequest const &req,
	Ctx const &ctx,
	SV path_sv,
	bool is_head,
	ContextRouteRange const &context_routes) {
	if (context_routes.empty()) {
		return nullopt;
	}
	HttpFieldsView matched_params;
	for (auto const &route: context_routes) {
		if (route.method != req.method && !(is_head && route.method == "GET")) {
			continue;
		}
		matched_params.clear();
		if (match_segments(route.pattern, path_sv, matched_params)) {
			HttpRequest call_req = req;
			for (auto const &[k, v]: matched_params) {
				if (!call_req.params.get(k)) {
					call_req.params.emplace_back(S{k}, S{v});
				}
			}
			return router_defer_http_task(route.handler(call_req, ctx));
		}
	}
	return nullopt;
}

export template<typename ContextRouteRange, typename Ctx>
[[nodiscard]] Opt<HttpResponse> dispatch_async_routes(
	HttpRequest const &req,
	Ctx const &ctx,
	SV path_sv,
	bool is_head,
	ContextRouteRange const &context_routes) {
	return dispatch_context_routes(req, ctx, path_sv, is_head, context_routes);
}
