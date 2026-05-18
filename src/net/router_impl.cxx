module conflux.net.router;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http.realtime;
import conflux.net.http.static_files;
import conflux.net.http.static_core;
import conflux.net.router_dispatch;
import conflux.net.router_match;
import conflux.net.router_static;
import conflux.work;
import conflux.net.config;
import conflux.socket_io;


struct TransparentSvHash {
	using is_transparent = void;

	[[nodiscard]] SZ operator ()(SV value) const noexcept {
		return hash<SV>{}(value);
	}

	[[nodiscard]] SZ operator ()(S const &value) const noexcept {
		return hash<SV>{}(value);
	}
};

struct TransparentSvEqual {
	using is_transparent = void;

	[[nodiscard]] bool operator ()(SV lhs, SV rhs) const noexcept {
		return lhs == rhs;
	}

	[[nodiscard]] bool operator ()(S const &lhs, SV rhs) const noexcept {
		return SV{lhs} == rhs;
	}

	[[nodiscard]] bool operator ()(SV lhs, S const &rhs) const noexcept {
		return lhs == SV{rhs};
	}

	[[nodiscard]] bool operator ()(S const &lhs, S const &rhs) const noexcept {
		return lhs == rhs;
	}
};

struct RouteLookupIndex {
	V<SZ> generic{};
	std::unordered_map<S, V<SZ>, TransparentSvHash, TransparentSvEqual> by_first_literal{};
};

struct MethodRouteLookupIndex {
	S method{};
	RouteLookupIndex routes{};
};

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
		V<MethodRouteLookupIndex> route_indexes{};
		RouteLookupIndex sse_index{};
		V<MethodRouteLookupIndex> context_route_indexes{};
		V<Middleware> middlewares{};
		Handler not_found_handler{};
		ErrorHandler error_handler{};
		SP<WorkPool> work_pool{make_shared<WorkPool>()};
		StaticCacheStore static_cache{};
		StaticFileCacheConfig static_file_cache{};
	};


namespace {

[[nodiscard]] V<SZ> const &empty_route_indices() noexcept {
	static V<SZ> const empty{};
	return empty;
}

[[nodiscard]] Opt<SV> first_literal_key(
	V<Segment> const &pattern) noexcept {
	SZ index = 0;
	if (pattern.size() > 1 && !pattern[0].is_param && !pattern[0].is_wildcard && pattern[0].value.empty()) {
		index = 1;
	}
	if (index >= pattern.size()) {
		return nullopt;
	}
	auto const &segment = pattern[index];
	if (segment.is_param || segment.is_wildcard) {
		return nullopt;
	}
	return SV{segment.value};
}

[[nodiscard]] Opt<SV> first_path_key(
	SV path) noexcept {
	if (path.empty()) {
		return nullopt;
	}
	SZ pos = (path.front() == '/') ? SZ{1} : SZ{0};
	auto const next = path.find('/', pos);
	return (next == SV::npos) ? path.substr(pos) : path.substr(pos, next - pos);
}

void index_route_pattern(
	RouteLookupIndex &index,
	V<Segment> const &pattern,
	SZ route_index) {
	if (auto key = first_literal_key(pattern)) {
		index.by_first_literal[S{*key}].push_back(route_index);
		return;
	}
	index.generic.push_back(route_index);
}

[[nodiscard]] MethodRouteLookupIndex &find_or_add_method_index(
	V<MethodRouteLookupIndex> &indexes,
	SV method) {
	for (auto &index: indexes) {
		if (index.method == method) {
			return index;
		}
	}
	MethodRouteLookupIndex added;
	added.method = S{method};
	indexes.push_back(move(added));
	return indexes.back();
}

[[nodiscard]] MethodRouteLookupIndex const *find_method_index(
	V<MethodRouteLookupIndex> const &indexes,
	SV method) noexcept {
	for (auto const &index: indexes) {
		if (index.method == method) {
			return &index;
		}
	}
	return nullptr;
}

struct RouteLookupSelection {
	V<SZ> const *literal{&empty_route_indices()};
	V<SZ> const *generic{&empty_route_indices()};
};

[[nodiscard]] RouteLookupSelection select_routes_for_path(
	RouteLookupIndex const &index,
	SV path) noexcept {
	RouteLookupSelection selected;
	selected.generic = &index.generic;
	if (auto key = first_path_key(path)) {
		if (auto found = index.by_first_literal.find(*key); found != index.by_first_literal.end()) {
			selected.literal = &found->second;
		}
	}
	return selected;
}

template<typename RouteT>
struct IndexedRouteRange {
	V<RouteT> const *routes{};
	V<SZ> const *literal_indices{&empty_route_indices()};
	V<SZ> const *generic_indices{&empty_route_indices()};

	struct Iterator {
		V<RouteT> const *routes{};
		V<SZ> const *literal_indices{};
		V<SZ> const *generic_indices{};
		SZ literal_pos{};
		SZ generic_pos{};

		[[nodiscard]] bool done() const noexcept {
			return literal_pos >= literal_indices->size() && generic_pos >= generic_indices->size();
		}

		[[nodiscard]] SZ current_index() const noexcept {
			if (literal_pos >= literal_indices->size()) {
				return (*generic_indices)[generic_pos];
			}
			if (generic_pos >= generic_indices->size()) {
				return (*literal_indices)[literal_pos];
			}
			return min((*literal_indices)[literal_pos], (*generic_indices)[generic_pos]);
		}

		[[nodiscard]] RouteT const &operator *() const noexcept {
			return (*routes)[current_index()];
		}

		Iterator &operator ++() noexcept {
			auto const current = current_index();
			if (literal_pos < literal_indices->size() && (*literal_indices)[literal_pos] == current) {
				++literal_pos;
			}
			if (generic_pos < generic_indices->size() && (*generic_indices)[generic_pos] == current) {
				++generic_pos;
			}
			return *this;
		}

		[[nodiscard]] bool operator !=(std::default_sentinel_t) const noexcept {
			return !done();
		}
	};

	[[nodiscard]] bool empty() const noexcept {
		return literal_indices->empty() && generic_indices->empty();
	}

	[[nodiscard]] Iterator begin() const noexcept {
		return Iterator{
			.routes = routes,
			.literal_indices = literal_indices,
			.generic_indices = generic_indices,
		};
	}

	[[nodiscard]] std::default_sentinel_t end() const noexcept {
		return {};
	}
};

template<typename RouteT>
[[nodiscard]] IndexedRouteRange<RouteT> indexed_route_range(
	V<RouteT> const &routes,
	RouteLookupSelection selected) noexcept {
	return IndexedRouteRange<RouteT>{
		.routes = &routes,
		.literal_indices = selected.literal,
		.generic_indices = selected.generic,
	};
}

[[nodiscard]] RouteLookupSelection select_method_routes(
	V<MethodRouteLookupIndex> const &indexes,
	SV method,
	SV path) noexcept {
	auto const *index = find_method_index(indexes, method);
	if (!index) {
		return {};
	}
	return select_routes_for_path(index->routes, path);
}

template<typename ImplT>
[[nodiscard]] HttpResponse dispatch_router_sync(
	ImplT const &impl,
	HttpRequestView const &req,
	SV path_sv,
	bool is_head) {
	auto const route_method = is_head ? SV{"GET"} : req.method;
	auto routes = indexed_route_range(impl.routes, select_method_routes(impl.route_indexes, route_method, path_sv));
	auto sse_routes = indexed_route_range(
		impl.sse_routes,
		is_head ? RouteLookupSelection{} : select_routes_for_path(impl.sse_index, path_sv));
	return dispatch_sync_routes(
		req,
		path_sv,
		is_head,
		routes,
		sse_routes,
		impl.not_found_handler,
		impl.error_handler,
		impl.work_pool);
}

template<typename ImplT>
[[nodiscard]] Opt<HttpResponse> dispatch_router_async(
	ImplT const &impl,
	HttpRequest const &req,
	RequestContext const &ctx,
	SV path_sv,
	bool is_head) {
	auto const route_method = is_head ? SV{"GET"} : req.method;
	auto routes = indexed_route_range(
		impl.context_routes,
		select_method_routes(impl.context_route_indexes, route_method, path_sv));
	return dispatch_context_routes(req, ctx, path_sv, is_head, routes);
}

} // namespace

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
	auto pattern = parse_pattern(path);
	auto const route_index = impl_->routes.size();
	index_route_pattern(find_or_add_method_index(impl_->route_indexes, method).routes, pattern, route_index);
	impl_->routes.push_back({S{method}, move(pattern), move(handler)});
}

void Router::add_context_prepared(
	SV method,
	SV path,
	ContextHandler handler) {
	auto pattern = parse_pattern(path);
	auto const route_index = impl_->context_routes.size();
	index_route_pattern(find_or_add_method_index(impl_->context_route_indexes, method).routes, pattern, route_index);
	impl_->context_routes.push_back({S{method}, move(pattern), move(handler)});
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
	auto pattern = parse_pattern(path);
	auto const route_index = impl_->sse_routes.size();
	index_route_pattern(impl_->sse_index, pattern, route_index);
	impl_->sse_routes.push_back({move(pattern), move(handler)});
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

[[nodiscard]] HttpResponse Router::defer_http_task(
	conflux::work::root::Task<HttpResponse> task) {
	return router_defer_http_task(move(task));
}

[[nodiscard]] HttpResponse Router::run_async_http_task(
	conflux::work::root::Task<HttpResponse> task) {
	return defer_http_task(move(task));
}

void Router::launch_sse_handler(
	SP<WorkPool> const &pool,
	SseHandler handler,
	HttpRequest matched,
	SP<SseChannel> const &channel) {
	router_launch_sse_handler(pool, move(handler), move(matched), channel);
}

[[nodiscard]] HttpResponse Router::run_middlewares(
	HttpRequestView const &req,
	Handler const &inner) const {
		struct Step {
			Router::Impl const *impl_;
			Handler const *inner_;
			SZ idx_{0};
			Handler next_;

			Step(
				Router::Impl const *impl,
				Handler const *inner)
				: impl_(impl)
				, inner_(inner)
				, next_([this](HttpRequestView const &r) -> HttpResponse { return call(r); }) {}

			HttpResponse call(
				HttpRequestView const &r) {
				if (idx_ == impl_->middlewares.size()) {
					return (*inner_)(r);
				}
				auto const &mw = impl_->middlewares[idx_++];
				return mw(r, next_);
			}
		};
		Step s{impl_.get(), &inner};
		return s.call(req);
	}

Router &Router::serve_static(
	std::string_view url_prefix,
	std::string root_dir,
	StaticOptions const &sopts) {
	auto routes = make_static_route_registration(
		url_prefix,
		move(root_dir),
		sopts,
		impl_->static_file_cache,
		impl_->static_cache);
	add_prepared("GET", routes.pattern, move(routes.get));
	if (routes.put) {
		add_prepared("PUT", routes.pattern, move(*routes.put));
	}
	if (routes.del) {
		add_prepared("DELETE", routes.pattern, move(*routes.del));
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

		if (impl_->middlewares.empty()) {
			return dispatch_router_sync(*impl_, req, path_sv, is_head);
		}

		// Inner handler: performs route matching + 404. Middleware wraps this whole thing.
		Handler inner = [this, path_sv, is_head](HttpRequestView const &r) -> HttpResponse {
			return dispatch_router_sync(*impl_, r, path_sv, is_head);
		};

		return run_middlewares(req, inner);
	}

[[nodiscard]] Opt<HttpResponse> Router::dispatch_context(
	HttpRequest const &req,
	RequestContext const &ctx) const {
		bool const is_head = (req.method == "HEAD");
		SV path_sv{req.path};
		if (auto q = path_sv.find('?'); q != SV::npos) {
			path_sv = path_sv.substr(0, q);
		}
		return dispatch_router_async(*impl_, req, ctx, path_sv, is_head);
	}
