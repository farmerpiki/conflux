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

struct RouteLookupIndex {
	std::vector<std::size_t> generic{};
	conflux::support::TransparentStringMap<std::vector<std::size_t>> by_first_literal{};
};

struct MethodRouteLookupIndex {
	std::string method{};
	RouteLookupIndex routes{};
};

struct Router::Impl {
	struct Route {
		std::string method{};
		std::vector<Segment> pattern{};
		std::string path_pattern{};
		std::string exact_path{};
		bool has_exact_path{};
		Handler handler{};
	};
	struct SseRoute {
		std::vector<Segment> pattern{};
		std::string path_pattern{};
		std::string exact_path{};
		bool has_exact_path{};
		SseHandler handler{};
	};
	struct ContextRoute {
		std::string method{};
		std::vector<Segment> pattern{};
		std::string path_pattern{};
		std::string exact_path{};
		bool has_exact_path{};
		ContextHandler handler{};
	};
	std::vector<Route> routes{};
	std::vector<SseRoute> sse_routes{};
	std::vector<ContextRoute> context_routes{};
	std::vector<MethodRouteLookupIndex> route_indexes{};
	RouteLookupIndex sse_index{};
	std::vector<MethodRouteLookupIndex> context_route_indexes{};
	std::vector<Middleware> middlewares{};
	std::vector<ContextMiddleware> context_middlewares{};
	Handler not_found_handler{};
	ErrorHandler error_handler{};
	std::shared_ptr<WorkPool> work_pool{std::make_shared<WorkPool>()};
	StaticCacheStore static_cache{};
	StaticFileCacheConfig static_file_cache{};
};

namespace {

[[nodiscard]] std::vector<std::size_t> const &empty_route_indices() noexcept {
	static std::vector<std::size_t> const empty{};
	return empty;
}

[[nodiscard]] std::optional<std::string_view> first_literal_key(
	std::vector<Segment> const &pattern) noexcept {
	std::size_t index = 0;
	if (pattern.size() > 1 && !pattern[0].is_param && !pattern[0].is_wildcard && pattern[0].value.empty()) {
		index = 1;
	}
	if (index >= pattern.size()) {
		return std::nullopt;
	}
	auto const &segment = pattern[index];
	if (segment.is_param || segment.is_wildcard) {
		return std::nullopt;
	}
	return std::string_view{segment.value};
}

[[nodiscard]] std::optional<std::string_view> first_path_key(
	std::string_view path) noexcept {
	if (path.empty()) {
		return std::nullopt;
	}
	std::size_t pos = (path.front() == '/') ? std::size_t{1} : std::size_t{0};
	auto const next = path.find('/', pos);
	return (next == std::string_view::npos) ? path.substr(pos) : path.substr(pos, next - pos);
}

[[nodiscard]] bool is_exact_literal_pattern(
	std::vector<Segment> const &pattern) noexcept {
	return std::ranges::none_of(pattern, [](Segment const &segment) {
		return segment.is_param || segment.is_wildcard;
	});
}

void index_route_pattern(
	RouteLookupIndex &index,
	std::vector<Segment> const &pattern,
	std::size_t route_index) {
	if (auto key = first_literal_key(pattern)) {
		index.by_first_literal[std::string{*key}].push_back(route_index);
		return;
	}
	index.generic.push_back(route_index);
}

[[nodiscard]] MethodRouteLookupIndex &find_or_add_method_index(
	std::vector<MethodRouteLookupIndex> &indexes,
	std::string_view method) {
	for (auto &index: indexes) {
		if (index.method == method) {
			return index;
		}
	}
	MethodRouteLookupIndex added;
	added.method = std::string{method};
	indexes.push_back(std::move(added));
	return indexes.back();
}

[[nodiscard]] MethodRouteLookupIndex const *find_method_index(
	std::vector<MethodRouteLookupIndex> const &indexes,
	std::string_view method) noexcept {
	for (auto const &index: indexes) {
		if (index.method == method) {
			return &index;
		}
	}
	return nullptr;
}

struct RouteLookupSelection {
	std::vector<std::size_t> const *literal{&empty_route_indices()};
	std::vector<std::size_t> const *generic{&empty_route_indices()};
};

[[nodiscard]] RouteLookupSelection select_routes_for_path(
	RouteLookupIndex const &index,
	std::string_view path) noexcept {
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
	std::vector<RouteT> const *routes{};
	std::vector<std::size_t> const *literal_indices{&empty_route_indices()};
	std::vector<std::size_t> const *generic_indices{&empty_route_indices()};

	struct Iterator {
		std::vector<RouteT> const *routes{};
		std::vector<std::size_t> const *literal_indices{};
		std::vector<std::size_t> const *generic_indices{};
		std::size_t literal_pos{};
		std::size_t generic_pos{};

		[[nodiscard]] bool done() const noexcept {
			return literal_pos >= literal_indices->size() && generic_pos >= generic_indices->size();
		}

		[[nodiscard]] std::size_t current_index() const noexcept {
			if (literal_pos >= literal_indices->size()) {
				return (*generic_indices)[generic_pos];
			}
			if (generic_pos >= generic_indices->size()) {
				return (*literal_indices)[literal_pos];
			}
			return std::min((*literal_indices)[literal_pos], (*generic_indices)[generic_pos]);
		}

		[[nodiscard]] RouteT const &operator *() const noexcept { return (*routes)[current_index()]; }

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

		[[nodiscard]] bool operator !=(
			std::default_sentinel_t) const noexcept {
			return !done();
		}
	};

	[[nodiscard]] bool empty() const noexcept { return literal_indices->empty() && generic_indices->empty(); }

	[[nodiscard]] Iterator begin() const noexcept {
		return Iterator{
			.routes = routes,
			.literal_indices = literal_indices,
			.generic_indices = generic_indices,
		};
	}

	[[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }
};

template<typename RouteT>
[[nodiscard]] IndexedRouteRange<RouteT> indexed_route_range(
	std::vector<RouteT> const &routes,
	RouteLookupSelection selected) noexcept {
	return IndexedRouteRange<RouteT>{
		.routes = &routes,
		.literal_indices = selected.literal,
		.generic_indices = selected.generic,
	};
}

[[nodiscard]] RouteLookupSelection select_method_routes(
	std::vector<MethodRouteLookupIndex> const &indexes,
	std::string_view method,
	std::string_view path) noexcept {
	auto const *index = find_method_index(indexes, method);
	if (!index) {
		return {};
	}
	return select_routes_for_path(index->routes, path);
}

template<typename RouteT>
void append_route_info(
	std::vector<RouteInfo> &result,
	std::string_view method,
	RouteT const &route) {
	RouteInfo info;
	info.method = std::string{method};
	info.path_pattern = route.path_pattern;
	for (auto const &seg: route.pattern) {
		if (seg.is_param || seg.is_wildcard) {
			info.path_params.push_back(seg.value);
		}
	}
	result.push_back(std::move(info));
}

template<typename RouteT>
void append_route_infos(
	std::vector<RouteInfo> &result,
	std::vector<RouteT> const &routes) {
	for (auto const &route: routes) {
		append_route_info(result, route.method, route);
	}
}

template<typename ImplT>
[[nodiscard]] Response dispatch_router_sync(
	ImplT const &impl,
	RequestView const &req,
	std::string_view path_sv,
	bool is_head) {
	auto const route_method = is_head ? std::string_view{"GET"} : req.method;
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
[[nodiscard]] std::optional<Response> dispatch_router_async(
	ImplT const &impl,
	RequestView const &req,
	RequestContext const &ctx,
	std::string_view path_sv,
	bool is_head) {
	auto const route_method = is_head ? std::string_view{"GET"} : req.method;
	auto routes = indexed_route_range(
		impl.context_routes,
		select_method_routes(impl.context_route_indexes, route_method, path_sv));
	return dispatch_context_routes(req, ctx, path_sv, routes);
}

template<typename ImplT>
[[nodiscard]] conflux::work::root::Task<Response> dispatch_router_context_task(
	ImplT const &impl,
	RequestView const &req,
	RequestContext const &ctx,
	std::string_view path_sv,
	bool is_head) {
	if (!impl.context_middlewares.empty()) {
		Router::ContextHandler inner = [&impl, path_sv, is_head](
										   RequestView const &r,
										   RequestContext const &c) -> conflux::work::root::Task<Response> {
			auto const route_method = is_head ? std::string_view{"GET"} : r.method;
			auto routes = indexed_route_range(
				impl.context_routes,
				select_method_routes(impl.context_route_indexes, route_method, path_sv));
			if (auto task = dispatch_context_route_tasks(r, c, path_sv, routes)) {
				auto resp = co_await std::move(*task);
				if (is_head) {
					resp.head_only = true;
				}
				co_return resp;
			}
			co_return dispatch_router_sync(impl, r, path_sv, is_head);
		};
		struct Step {
			ImplT const *impl_;
			Router::ContextHandler inner_;
			std::size_t idx_{0};
			Router::ContextHandler next_;

			Step(
				ImplT const *impl,
				Router::ContextHandler const *inner)
				: impl_(impl)
				, inner_(*inner) {}

			void bind_next(
				std::shared_ptr<Step> self) {
				next_ = [self = std::move(self)](RequestView const &r, RequestContext const &c)
					-> conflux::work::root::Task<Response> { co_return co_await self->call(r, c); };
			}

			conflux::work::root::Task<Response> call(
				RequestView const &r,
				RequestContext const &c) {
				if (idx_ == impl_->context_middlewares.size()) {
					co_return co_await inner_(r, c);
				}
				auto const &mw = impl_->context_middlewares[idx_++];
				co_return co_await mw(r, c, next_);
			}
		};
		auto step = std::make_shared<Step>(&impl, &inner);
		step->bind_next(step);
		co_return co_await step->call(req, ctx);
	}
	if (auto resp = dispatch_router_async(impl, req, ctx, path_sv, is_head)) {
		co_return std::move(*resp);
	}
	co_return dispatch_router_sync(impl, req, path_sv, is_head);
}

} // namespace

[[nodiscard]] std::string_view trim_ascii_ws(
	std::string_view s) noexcept {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
		s.remove_prefix(1);
	}
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
		s.remove_suffix(1);
	}
	return s;
}

Router::ContextHandler Router::Group::wrap_context(
	ContextHandler h) const {
	for (int i = static_cast<int>(context_middlewares_.size()) - 1; i >= 0; --i) {
		auto mw = context_middlewares_[static_cast<std::size_t>(i)];
		h = [mw = std::move(mw),
			 n = std::move(h)](RequestView const &r, RequestContext const &c) -> conflux::work::root::Task<Response> {
			co_return co_await mw(r, c, n);
		};
	}
	return h;
}

Router::Router()
	: impl_(std::make_unique<Impl>()) {}

Router::Router(
	Config const &cfg)
	: impl_(std::make_unique<Impl>()) {
	impl_->static_file_cache = cfg.static_file_cache;
}

Router::~Router() {}

Router::Router(
	Router &&o) noexcept
	: impl_(std::move(o.impl_)) {}

Router &Router::operator =(
	Router &&o) noexcept {
	impl_ = std::move(o.impl_);
	return *this;
}

void Router::add_prepared(
	std::string_view method,
	std::string_view path,
	Handler handler) {
	auto pattern = parse_pattern(path);
	auto const route_index = impl_->routes.size();
	auto const has_exact_path = is_exact_literal_pattern(pattern);
	auto path_pattern = segments_to_pattern(pattern);
	index_route_pattern(find_or_add_method_index(impl_->route_indexes, method).routes, pattern, route_index);
	impl_->routes.push_back({
		.method = std::string{method},
		.pattern = std::move(pattern),
		.path_pattern = std::move(path_pattern),
		.exact_path = has_exact_path ? std::string{path} : std::string{},
		.has_exact_path = has_exact_path,
		.handler = std::move(handler),
	});
}

void Router::add_context_prepared(
	std::string_view method,
	std::string_view path,
	ContextHandler handler) {
	auto pattern = parse_pattern(path);
	auto const route_index = impl_->context_routes.size();
	auto const has_exact_path = is_exact_literal_pattern(pattern);
	auto path_pattern = segments_to_pattern(pattern);
	index_route_pattern(find_or_add_method_index(impl_->context_route_indexes, method).routes, pattern, route_index);
	impl_->context_routes.push_back({
		.method = std::string{method},
		.pattern = std::move(pattern),
		.path_pattern = std::move(path_pattern),
		.exact_path = has_exact_path ? std::string{path} : std::string{},
		.has_exact_path = has_exact_path,
		.handler = std::move(handler),
	});
}

void Router::use_prepared(
	Middleware mw) {
	impl_->middlewares.push_back(std::move(mw));
}

void Router::use_context_prepared(
	ContextMiddleware mw) {
	impl_->context_middlewares.push_back(std::move(mw));
}

void Router::set_not_found_handler(
	Handler handler) {
	impl_->not_found_handler = std::move(handler);
}

void Router::set_error_handler(
	ErrorHandler handler) {
	impl_->error_handler = std::move(handler);
}

void Router::sse_prepared(
	std::string_view path,
	SseHandler handler) {
	auto pattern = parse_pattern(path);
	auto const route_index = impl_->sse_routes.size();
	auto const has_exact_path = is_exact_literal_pattern(pattern);
	auto path_pattern = segments_to_pattern(pattern);
	index_route_pattern(impl_->sse_index, pattern, route_index);
	impl_->sse_routes.push_back({
		.pattern = std::move(pattern),
		.path_pattern = std::move(path_pattern),
		.exact_path = has_exact_path ? std::string{path} : std::string{},
		.has_exact_path = has_exact_path,
		.handler = std::move(handler),
	});
}

[[nodiscard]] bool Router::has_context_routes() const noexcept {
	return !impl_->context_routes.empty() || !impl_->context_middlewares.empty();
}

Router &Router::set_work_pool(
	std::shared_ptr<WorkPool> pool) {
	impl_->work_pool = std::move(pool);
	return *this;
}

[[nodiscard]] std::shared_ptr<WorkPool> Router::work_pool() const {
	return impl_->work_pool;
}

Router &Router::set_static_file_cache(
	StaticFileCacheConfig cfg) {
	impl_->static_file_cache = cfg;
	return *this;
}

Router &Router::ws_prepared(
	std::string_view path,
	WsHandler handler) {
	add_prepared("GET", path, Handler{[h = std::move(handler)](RequestView const &req) mutable -> Response {
					 if (!ws_detail::is_valid_handshake(req)) {
						 return Response::bad_request();
					 }
					 auto key = trim_ascii_ws(req.headers["sec-websocket-key"]);
					 auto up = std::make_shared<WsUpgrade>();
					 up->accept_key = ws_detail::ws_accept_key(key);
					 up->handler = h;
					 Response r{.status = 101, .status_text = "Switching Protocols"};
					 r.set_ws_upgrade(std::move(up));
					 return r;
				 }});
	return *this;
}

[[nodiscard]] std::vector<RouteInfo> Router::route_infos() const {
	std::vector<RouteInfo> result;
	result.reserve(impl_->routes.size() + impl_->context_routes.size() + impl_->sse_routes.size());
	append_route_infos(result, impl_->routes);
	append_route_infos(result, impl_->context_routes);
	for (auto const &route: impl_->sse_routes) {
		append_route_info(result, "GET", route);
	}
	return result;
}

[[nodiscard]] Response Router::defer_http_task(
	conflux::work::root::Task<Response> task) {
	return router_defer_http_task(std::move(task));
}

[[nodiscard]] Response Router::run_async_http_task(
	conflux::work::root::Task<Response> task) {
	return defer_http_task(std::move(task));
}

void Router::launch_sse_handler(
	std::shared_ptr<WorkPool> const &pool,
	SseHandler handler,
	Request matched,
	std::shared_ptr<SseChannel> const &channel) {
	router_launch_sse_handler(pool, std::move(handler), std::move(matched), channel);
}

[[nodiscard]] Response Router::run_middlewares(
	RequestView const &req,
	Handler const &inner) const {
	struct Step {
		Router::Impl const *impl_;
		Handler const *inner_;
		std::size_t idx_{0};
		Handler next_;

		Step(
			Router::Impl const *impl,
			Handler const *inner)
			: impl_(impl)
			, inner_(inner)
			, next_([this](RequestView const &r) -> Response { return call(r); }) {}

		Response call(
			RequestView const &r) {
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

[[nodiscard]] std::optional<Response> Router::run_context_middlewares(
	RequestView const &req,
	RequestContext const &ctx,
	ContextHandler const &inner) const {
	if (impl_->context_middlewares.empty()) {
		return std::nullopt;
	}
	struct Step {
		Router::Impl const *impl_;
		ContextHandler inner_;
		std::size_t idx_{0};
		ContextHandler next_;

		Step(
			Router::Impl const *impl,
			ContextHandler const *inner)
			: impl_(impl)
			, inner_(*inner) {}

		void bind_next(
			std::shared_ptr<Step> self) {
			next_ = [self = std::move(self)](
						RequestView const &r,
						RequestContext const &c) -> conflux::work::root::Task<Response> { return self->call(r, c); };
		}

		conflux::work::root::Task<Response> call(
			RequestView const &r,
			RequestContext const &c) {
			if (idx_ == impl_->context_middlewares.size()) {
				return inner_(r, c);
			}
			auto const &mw = impl_->context_middlewares[idx_++];
			return mw(r, c, next_);
		}
	};
	auto step = std::make_shared<Step>(impl_.get(), &inner);
	step->bind_next(step);
	return defer_http_task(step->call(req, ctx));
}

Router &Router::serve_static(
	std::string_view url_prefix,
	std::string root_dir,
	StaticOptions const &sopts) {
	auto routes = make_static_route_registration(
		url_prefix,
		std::move(root_dir),
		sopts,
		impl_->static_file_cache,
		impl_->static_cache);
	add_prepared("GET", routes.pattern, std::move(routes.get));
	if (routes.put) {
		add_prepared("PUT", routes.pattern, std::move(*routes.put));
	}
	if (routes.del) {
		add_prepared("DELETE", routes.pattern, std::move(*routes.del));
	}
	return *this;
}

[[nodiscard]] Response Router::dispatch(
	Request const &req) const {
	RequestView const req_view{req};
	return dispatch(req_view);
}

[[nodiscard]] Response Router::dispatch(
	RequestView const &req) const {
	// HEAD is dispatched as GET; response body is suppressed before sending.
	bool const is_head = (req.method == "HEAD");

	// Strip query std::string before matching.
	auto const path_sv = conflux::http::path_without_query(std::string_view{req.path});

	if (impl_->middlewares.empty()) {
		return dispatch_router_sync(*impl_, req, path_sv, is_head);
	}

	// Inner handler: performs route matching + 404. Middleware wraps this whole thing.
	Handler inner = [this, path_sv, is_head](RequestView const &r) -> Response {
		return dispatch_router_sync(*impl_, r, path_sv, is_head);
	};

	return run_middlewares(req, inner);
}

[[nodiscard]] std::optional<Response> Router::dispatch_context(
	RequestView const &req,
	RequestContext const &ctx) const {
	bool const is_head = (req.method == "HEAD");
	std::string_view const path_sv = conflux::http::path_without_query(req.path);
	if (!impl_->context_middlewares.empty()) {
		ContextHandler inner = [this, path_sv, is_head](
								   RequestView const &r,
								   RequestContext const &c) -> conflux::work::root::Task<Response> {
			if (auto task = dispatch_context_route_tasks(r, c, path_sv, impl_->context_routes)) {
				auto resp = co_await std::move(*task);
				if (is_head) {
					resp.head_only = true;
				}
				co_return resp;
			}
			co_return dispatch_router_sync(*impl_, r, path_sv, is_head);
		};
		return run_context_middlewares(req, ctx, inner);
	}
	return dispatch_router_async(*impl_, req, ctx, path_sv, is_head);
}
