module;
#include <fcntl.h>
#include <unistd.h>
module conflux.net.router;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http.realtime;
import conflux.net.http.static_files;
import conflux.net.http.static_core;
import conflux.net.http.static_async;
import conflux.net.router_dispatch;
import conflux.net.router_match;
import conflux.net.router_static;
import conflux.work;
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
		StaticCacheStore static_cache{};
		StaticFileCacheConfig static_file_cache{};
	};

[[nodiscard]] SV trim_ascii_ws(
	SV s) noexcept {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
		s.remove_prefix(1);
	}
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
		s.remove_suffix(1);
	}
	return s;
}



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
		auto key = trim_ascii_ws(req.headers["sec-websocket-key"]);
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
	return router_run_async_http_task(move(task));
}

void Router::launch_sse_handler(
	SP<WorkPool> const &pool,
	SseHandler handler,
	HttpRequest matched,
	SP<SseChannel> const &channel) {
	router_launch_sse_handler(pool, move(handler), move(matched), channel);
}

[[nodiscard]] Router::Handler Router::wrap_middlewares(
	Handler h) const {
		return [this, h = move(h)](HttpRequestView const &req) -> HttpResponse {
			struct Step {
				Router::Impl const *impl_;
				Handler const *h_;
				SZ idx_{0};
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
	std::string_view url_prefix,
	std::string root_dir,
	StaticOptions const &sopts) {
	auto add_get = [this](SV pattern, auto handler) { get(pattern, move(handler)); };
	auto add_put = [this](SV pattern, auto handler) { put(pattern, move(handler)); };
	auto add_del = [this](SV pattern, auto handler) { del(pattern, move(handler)); };
	serve_static_routes(
		add_get,
		add_put,
		add_del,
		move(url_prefix),
		move(root_dir),
		sopts,
		impl_->static_file_cache,
		impl_->static_cache);
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
			return dispatch_sync_routes(
				r,
				path_sv,
				is_head,
				impl_->routes,
				impl_->sse_routes,
				impl_->not_found_handler,
				impl_->error_handler,
				impl_->work_pool);
		};

		return wrap_middlewares(move(inner))(req);
	}

[[nodiscard]] Opt<HttpResponse> Router::dispatch_async(
	HttpRequest const &req,
	RequestContext const &ctx) const {
		bool const is_head = (req.method == "HEAD");
		SV path_sv{req.path};
		if (auto q = path_sv.find('?'); q != SV::npos) {
			path_sv = path_sv.substr(0, q);
		}
		return dispatch_async_routes(req, ctx, path_sv, is_head, impl_->context_routes);
	}
