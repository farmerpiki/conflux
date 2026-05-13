module;
module conflux.net.router;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http.realtime;
import conflux.net.http.static_core;
import conflux.net.http.static_async;
import conflux.work;
import conflux.utils;
import conflux.net.config;
import conflux.socket_io;


struct Router::Impl {
		struct Route {
			S method{};
			V<Segment> pattern{};
			Handler handler{};
		};
		struct SseRoute {
			V<Segment> pattern{};
			SseHandler handler{};
		};
		struct ContextRoute {
			S method{};
			V<Segment> pattern{};
			ContextHandler handler{};
		};
		V<Route> routes{};
		V<SseRoute> sse_routes{};
		V<ContextRoute> context_routes{};
		V<Middleware> middlewares{};
		Handler not_found_handler{};
		ErrorHandler error_handler{};
		SP<WorkPool> work_pool{make_shared<WorkPool>()};
		StaticFileCacheConfig static_file_cache{};
	};



Router::Router()
	: impl_(make_unique<Impl>()) {}

Router::Router(Config const &cfg)
	: impl_(make_unique<Impl>()) {
	impl_->static_file_cache = cfg.static_file_cache;
}

Router::~Router() {}

Router::Router(Router &&o) noexcept
	: impl_(move(o.impl_)) {}

Router &Router::operator =(Router &&o) noexcept {
	impl_ = move(o.impl_);
	return *this;
}

void Router::add_prepared(
	SV method,
	SV path,
	Handler handler) {
	impl_->routes.push_back({S{method}, parse_pattern(path), move(handler)});
}

void Router::add_context_prepared(
	SV method,
	SV path,
	ContextHandler handler) {
	impl_->context_routes.push_back({S{method}, parse_pattern(path), move(handler)});
}

void Router::use_prepared(Middleware mw) {
	impl_->middlewares.push_back(move(mw));
}

void Router::set_not_found_handler(Handler handler) {
	impl_->not_found_handler = move(handler);
}

void Router::set_error_handler(ErrorHandler handler) {
	impl_->error_handler = move(handler);
}

void Router::sse_prepared(
	SV path,
	SseHandler handler) {
	impl_->sse_routes.push_back({parse_pattern(path), move(handler)});
}

[[nodiscard]] bool Router::has_context_routes() const noexcept {
	return !impl_->context_routes.empty();
}

Router &Router::set_work_pool(SP<WorkPool> pool) {
	impl_->work_pool = move(pool);
	return *this;
}

[[nodiscard]] SP<WorkPool> Router::work_pool() const {
	return impl_->work_pool;
}

Router &Router::set_static_file_cache(StaticFileCacheConfig cfg) {
	impl_->static_file_cache = cfg;
	return *this;
}

Router &Router::ws_prepared(
	SV path,
	WsHandler handler) {
	add_prepared("GET", path, Handler{[h = move(handler)](HttpRequestView const &req) mutable -> HttpResponse {
		if (!ws_detail::is_valid_handshake(req)) {
			return HttpResponse::bad_request();
		}
		auto key = trim(req.headers["sec-websocket-key"]);
		auto up = make_shared<WsUpgrade>();
		up->accept_key = ws_detail::ws_accept_key(key);
		up->handler = h;
		HttpResponse r{.status = 101, .status_text = "Switching Protocols"};
		r.set_ws_upgrade(move(up));
		return r;
	}});
	return *this;
}


[[nodiscard]] V<RouteInfo> Router::route_infos() const {
		V<RouteInfo> result;
		result.reserve(impl_->routes.size());
		for (auto const &route: impl_->routes) {
			RouteInfo info;
			info.method = route.method;
			info.path_pattern = segments_to_pattern(route.pattern);
			for (auto const &seg: route.pattern) {
				if (seg.is_param || seg.is_wildcard) {
					info.path_params.push_back(seg.value);
				}
			}
			result.push_back(move(info));
		}
		return result;
	}

[[nodiscard]] HttpResponse Router::run_async_http_task(
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

void Router::launch_sse_handler(
	SP<WorkPool> const &pool,
	SseHandler handler,
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

[[nodiscard]] Router::Handler Router::wrap_middlewares(
	Handler h) const {
		return [this, h = move(h)](HttpRequestView const &req) -> HttpResponse {
			struct Step {
				Router::Impl const *impl_;
				Handler const *h_;
				size_t idx_{0};
				HttpResponse call(
					HttpRequestView const &r) {
					if (idx_ == impl_->middlewares.size()) {
						return (*h_)(r);
					}
					auto const &mw = impl_->middlewares[idx_++];
					return mw(r, [this](HttpRequestView const &rr) -> HttpResponse { return call(rr); });
				}
			};
			Step s{impl_.get(), &h};
			return s.call(req);
		};
	}

Router &Router::serve_static(
	SV url_prefix,
	S root_dir,
	StaticOptions const &sopts) {
	// Strip trailing slash from root_dir.
	while (!root_dir.empty() && root_dir.back() == '/') {
		root_dir.pop_back();
	}

	auto pattern = S{url_prefix} + "/{*file}";
	auto effective_sopts = sopts;
	if (!effective_sopts.file_cache.enabled) {
		effective_sopts.file_cache = impl_->static_file_cache;
	}
	auto static_cache = make_shared<StaticCacheStore>();
	auto root_dir_fd = open_static_root_dir(root_dir);
	auto rd = move(root_dir);

	// NOLINTNEXTLINE(bugprone-exception-escape): delegated static component handles failures as HTTP responses.
	get(pattern,
		[rd, root_dir_fd, sopts = effective_sopts, static_cache](
			HttpRequestView const &req) -> HttpResponse {
			return handle_static_get_request(rd, *root_dir_fd, sopts, req, static_cache);
		});

	if (effective_sopts.allow_put) {
		// NOLINTNEXTLINE(bugprone-exception-escape): delegated static component handles failures as HTTP responses.
		put(pattern,
			[rd, root_dir_fd, sopts = effective_sopts, static_cache](
				HttpRequestView const &req) -> HttpResponse {
				return handle_static_put(rd, *root_dir_fd, sopts, req, static_cache);
			});
	}

	if (effective_sopts.allow_delete) {
		// NOLINTNEXTLINE(bugprone-exception-escape): delegated static component handles failures as HTTP responses.
		del(pattern,
			[rd, root_dir_fd, sopts = effective_sopts, static_cache](
				HttpRequestView const &req) -> HttpResponse {
				return handle_static_delete(rd, *root_dir_fd, sopts, req, static_cache);
			});
	}

	return *this;
}

[[nodiscard]] HttpResponse Router::dispatch(
	HttpRequest const &req) const {
		HttpRequestView const req_view{req};
		return dispatch(req_view);
	}

[[nodiscard]] HttpResponse Router::dispatch(
	HttpRequestView const &req) const {
		// HEAD is dispatched as GET; response body is suppressed before sending.
		bool const is_head = (req.method == "HEAD");

		// Strip query S before matching.
		auto path_sv = SV{req.path};
		if (auto q = path_sv.find('?'); q != SV::npos) {
			path_sv = path_sv.substr(0, q);
		}

		// Inner handler: performs route matching + 404. Middleware wraps this whole thing.
		Handler inner = [this, path_sv, is_head](HttpRequestView const &r) -> HttpResponse {
			try {
				HttpFieldsView matched_params;

				// Regular routes first.
				for (auto const &route: impl_->routes) {
					if (route.method != r.method && !(is_head && route.method == "GET")) {
						continue;
					}
					matched_params.clear();
					if (match_segments(route.pattern, path_sv, matched_params)) {
						auto all_params = r.params;
						for (auto const &[k, v]: matched_params) {
							if (!all_params.get(k)) {
								all_params.emplace_back(k, v);
							}
						}
						// HEAD matched to a GET route: present as GET so handlers are HEAD-transparent.
						SV const effective_method = (is_head && route.method == "GET") ? SV{"GET"} : r.method;
						HttpRequestView const matched_view{
							effective_method,
							r.path,
							r.version,
							r.remote_addr,
							r.is_tls,
							move(all_params),
							r.headers,
							r.query,
							r.form,
							r.cookies,
							r.files,
							r.body};
						try {
							auto resp = route.handler(matched_view);
							if (is_head) {
								resp.head_only = true;
							}
							return resp;
						} catch (exception const &ex) {
							return impl_->error_handler ? impl_->error_handler(matched_view, ex) :
														  HttpResponse::internal_error(ex.what());
						} catch (...) {
							return impl_->error_handler ? impl_->error_handler(matched_view, RE{"unknown exception"}) :
														  HttpResponse::internal_error();
						}
					}
				}

				// SSE routes (GET only).
				if (r.method == "GET") {
					for (auto const &route: impl_->sse_routes) {
						matched_params.clear();
						if (match_segments(route.pattern, path_sv, matched_params)) {
							auto channel = make_shared<SseChannel>();
							HttpRequest matched = r.to_owned();
							for (auto &[k, v]: matched_params) {
								matched.params.emplace_back(S{k}, S{v});
							}
							launch_sse_handler(impl_->work_pool, route.handler, move(matched), channel);
							return HttpResponse::sse(move(channel));
						}
					}
				}

				if (impl_->not_found_handler) {
					return impl_->not_found_handler(r);
				}
				return HttpResponse::not_found(path_sv);
			} catch (...) { return HttpResponse::internal_error(); }
		};

		return wrap_middlewares(move(inner))(req);
	}

[[nodiscard]] Opt<HttpResponse> Router::dispatch_async(
	HttpRequest const &req,
	RequestContext const &ctx) const {
		if (impl_->context_routes.empty()) {
			return nullopt;
		}
		bool const is_head = (req.method == "HEAD");
		SV path_sv{req.path};
		if (auto q = path_sv.find('?'); q != SV::npos) {
			path_sv = path_sv.substr(0, q);
		}
		HttpFieldsView matched_params;
		for (auto const &route: impl_->context_routes) {
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
				return run_async_http_task(route.handler(call_req, ctx));
			}
		}
		return nullopt;
	}
