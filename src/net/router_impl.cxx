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

namespace conflux::http {

struct RouteLookupIndex {
	std::vector<std::size_t> generic{};
	conflux::support::TransparentStringMap<std::vector<std::size_t>> by_first_literal{};
	conflux::support::TransparentStringMap<std::size_t> exact{};
};

struct MethodRouteLookupIndex {
	std::string method{};
	RouteLookupIndex routes{};
};

struct Router::Impl {
	struct Route {
		std::string method{};
		std::vector<conflux::http::detail::Segment> pattern{};
		std::string path_pattern{};
		std::string exact_path{};
		bool has_exact_path{};
		Handler handler{};
	};
	struct SseRoute {
		std::vector<conflux::http::detail::Segment> pattern{};
		std::string path_pattern{};
		std::string exact_path{};
		bool has_exact_path{};
		SseHandler handler{};
	};
	struct ContextRoute {
		std::string method{};
		std::vector<conflux::http::detail::Segment> pattern{};
		std::string path_pattern{};
		std::string exact_path{};
		bool has_exact_path{};
		std::shared_ptr<std::chrono::milliseconds> timeout{};
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
	std::shared_ptr<WorkPool> work_pool{};
	conflux::http::detail::StaticCacheStore static_cache{};
	conflux::http::StaticFileCacheConfig static_file_cache{};

	[[nodiscard]] std::shared_ptr<WorkPool> ensure_work_pool() {
		if (work_pool == nullptr) {
			work_pool = std::make_shared<WorkPool>();
		}
		return work_pool;
	}
};

namespace {

[[nodiscard]] std::vector<std::size_t> const &empty_route_indices() noexcept {
	static std::vector<std::size_t> const empty{};
	return empty;
}

template<typename Fn>
[[nodiscard]] auto make_one_shot_next(
	Fn &&fn) {
	auto used = std::make_shared<std::atomic_bool>(false);
	return [used, fn = std::forward<Fn>(fn)]<typename... Args>(Args &&...args) mutable -> decltype(auto) {
		if (used->exchange(true, std::memory_order_acq_rel)) {
			throw std::logic_error{"middleware next() called more than once"};
		}
		return fn(std::forward<Args>(args)...);
	};
}

[[nodiscard]] std::optional<std::string_view> first_literal_key(
	std::vector<conflux::http::detail::Segment> const &pattern) noexcept {
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
	std::vector<conflux::http::detail::Segment> const &pattern) noexcept {
	return std::ranges::none_of(pattern, [](conflux::http::detail::Segment const &segment) {
		return segment.is_param || segment.is_wildcard;
	});
}

void index_route_pattern(
	RouteLookupIndex &index,
	std::vector<conflux::http::detail::Segment> const &pattern,
	std::string_view exact_path,
	std::size_t route_index) {
	if (!exact_path.empty()) {
		index.exact.try_emplace(std::string{exact_path}, route_index);
	}
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
	std::optional<std::size_t> exact{};
	std::vector<std::size_t> const *literal{&empty_route_indices()};
	std::vector<std::size_t> const *generic{&empty_route_indices()};
};

[[nodiscard]] RouteLookupSelection select_routes_for_path(
	RouteLookupIndex const &index,
	std::string_view path) noexcept {
	RouteLookupSelection selected;
	selected.generic = &index.generic;
	if (auto exact = index.exact.find(path); exact != index.exact.end()) {
		selected.exact = exact->second;
	}
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
	std::optional<std::size_t> exact_index{};
	std::vector<std::size_t> const *literal_indices{&empty_route_indices()};
	std::vector<std::size_t> const *generic_indices{&empty_route_indices()};

	struct Iterator {
		using value_type = RouteT;
		using difference_type = std::ptrdiff_t;
		using iterator_concept = std::input_iterator_tag;
		using iterator_category = std::input_iterator_tag;

		std::vector<RouteT> const *routes{};
		std::optional<std::size_t> exact_index{};
		std::vector<std::size_t> const *literal_indices{};
		std::vector<std::size_t> const *generic_indices{};
		bool exact_done{};
		std::size_t literal_pos{};
		std::size_t generic_pos{};

		[[nodiscard]] bool done() const noexcept {
			return (!exact_index || exact_done)
				&& literal_pos >= literal_indices->size()
				&& generic_pos >= generic_indices->size();
		}

		[[nodiscard]] std::size_t current_index() const noexcept {
			auto current = exact_index && !exact_done ? *exact_index : std::numeric_limits<std::size_t>::max();
			if (literal_pos < literal_indices->size()) {
				current = std::min(current, (*literal_indices)[literal_pos]);
			}
			if (generic_pos < generic_indices->size()) {
				current = std::min(current, (*generic_indices)[generic_pos]);
			}
			return current;
		}

		[[nodiscard]] RouteT const &operator *() const noexcept { return (*routes)[current_index()]; }

		Iterator &operator ++() noexcept {
			auto const current = current_index();
			if (exact_index && !exact_done && *exact_index == current) {
				exact_done = true;
			}
			if (literal_pos < literal_indices->size() && (*literal_indices)[literal_pos] == current) {
				++literal_pos;
			}
			if (generic_pos < generic_indices->size() && (*generic_indices)[generic_pos] == current) {
				++generic_pos;
			}
			return *this;
		}

		void operator ++(
			int) noexcept {
			++(*this);
		}

		[[nodiscard]] bool operator ==(
			std::default_sentinel_t) const noexcept {
			return done();
		}

		[[nodiscard]] bool operator !=(
			std::default_sentinel_t) const noexcept {
			return !done();
		}
	};

	[[nodiscard]] bool empty() const noexcept {
		return !exact_index && literal_indices->empty() && generic_indices->empty();
	}

	[[nodiscard]] Iterator begin() const noexcept {
		return Iterator{
			.routes = routes,
			.exact_index = exact_index,
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
		.exact_index = selected.exact,
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
	std::vector<conflux::http::RouteInfo> &result,
	std::string_view method,
	RouteT const &route) {
	conflux::http::RouteInfo info;
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
	std::vector<conflux::http::RouteInfo> &result,
	std::vector<RouteT> const &routes) {
	for (auto const &route: routes) {
		append_route_info(result, route.method, route);
	}
}

template<typename ImplT>
[[nodiscard]] Response dispatch_router_sync(
	ImplT const &impl,
	conflux::http::RequestView const &req,
	std::string_view path_sv,
	bool is_head) {
	auto const route_method = is_head ? std::string_view{"GET"} : req.method;
	auto routes = indexed_route_range(impl.routes, select_method_routes(impl.route_indexes, route_method, path_sv));
	auto sse_routes = indexed_route_range(
		impl.sse_routes,
		is_head ? RouteLookupSelection{} : select_routes_for_path(impl.sse_index, path_sv));
	return conflux::http::detail::dispatch_immediate_routes(
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
	conflux::http::RequestView const &req,
	conflux::http::RequestContext const &ctx,
	std::string_view path_sv,
	bool is_head) {
	auto const route_method = is_head ? std::string_view{"GET"} : req.method;
	auto routes = indexed_route_range(
		impl.context_routes,
		select_method_routes(impl.context_route_indexes, route_method, path_sv));
	return conflux::http::detail::dispatch_context_routes(req, ctx, path_sv, routes);
}

template<typename ImplT>
[[nodiscard]] conflux::work::root::Task<Response> dispatch_router_context_task(
	ImplT const &impl,
	conflux::http::RequestView const &req,
	conflux::http::RequestContext const &ctx,
	std::string_view path_sv,
	bool is_head) {
	if (!impl.context_middlewares.empty()) {
		Router::ContextHandler inner =
			[&impl, path_sv, is_head](
				conflux::http::RequestView const &r,
				conflux::http::RequestContext const &c) -> conflux::work::root::Task<Response> {
			auto const route_method = is_head ? std::string_view{"GET"} : r.method;
			auto routes = indexed_route_range(
				impl.context_routes,
				select_method_routes(impl.context_route_indexes, route_method, path_sv));
			if (auto deferred_task = conflux::http::detail::dispatch_context_route_tasks(r, c, path_sv, routes)) {
				auto resp = co_await std::move(deferred_task->task);
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

			Step(
				ImplT const *impl,
				Router::ContextHandler const *inner)
				: impl_(impl)
				, inner_(*inner) {}

			conflux::work::root::Task<Response> call(
				std::shared_ptr<Step> self,
				conflux::http::RequestView const &r,
				conflux::http::RequestContext const &c) {
				if (idx_ == impl_->context_middlewares.size()) {
					co_return co_await inner_(r, c);
				}
				auto const &mw = impl_->context_middlewares[idx_++];
				Router::ContextHandler next_handler =
					[self = std::move(self)](
						conflux::http::RequestView const &next_req,
						conflux::http::RequestContext const &next_ctx) mutable -> conflux::work::root::Task<Response> {
					co_return co_await self->call(self, next_req, next_ctx);
				};
				auto next = make_one_shot_next(std::move(next_handler));
				co_return co_await mw(r, c, next);
			}
		};
		auto step = std::make_shared<Step>(&impl, &inner);
		co_return co_await step->call(step, req, ctx);
	}
	if (auto resp = dispatch_router_async(impl, req, ctx, path_sv, is_head)) {
		co_return std::move(*resp);
	}
	co_return dispatch_router_sync(impl, req, path_sv, is_head);
}

} // namespace

Router::ContextHandler Router::Group::wrap_context(
	ContextHandler h) const {
	for (int i = static_cast<int>(context_middlewares_.size()) - 1; i >= 0; --i) {
		auto mw = context_middlewares_[static_cast<std::size_t>(i)];
		h = [mw = std::move(mw), n = std::move(h)](conflux::http::RequestView const &r, conflux::http::RequestContext const &c)
			-> conflux::work::root::Task<Response> { co_return co_await mw(r, c, n); };
	}
	return h;
}

Router::Router()
	: impl_(std::make_unique<Impl>()) {}

Router::Router(
	conflux::http::Config const &cfg)
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
	auto pattern = conflux::http::detail::parse_pattern(path);
	auto const route_index = impl_->routes.size();
	auto const has_exact_path = is_exact_literal_pattern(pattern);
	auto path_pattern = conflux::http::detail::segments_to_pattern(pattern);
	index_route_pattern(
		find_or_add_method_index(impl_->route_indexes, method).routes,
		pattern,
		has_exact_path ? path : std::string_view{},
		route_index);
	impl_->routes.push_back({
		.method = std::string{method},
		.pattern = std::move(pattern),
		.path_pattern = std::move(path_pattern),
		.exact_path = has_exact_path ? std::string{path} : std::string{},
		.has_exact_path = has_exact_path,
		.handler = std::move(handler),
	});
}

void Router::add_prepared(
	conflux::http::HttpMethod method,
	std::string_view path,
	Handler handler) {
	add_prepared(conflux::http::http_method_name(method), path, std::move(handler));
}

void Router::add_context_prepared(
	std::string_view method,
	std::string_view path,
	ContextHandler handler) {
	add_context_prepared(method, path, nullptr, std::move(handler));
}

void Router::add_context_prepared(
	std::string_view method,
	std::string_view path,
	std::shared_ptr<std::chrono::milliseconds> timeout,
	ContextHandler handler) {
	auto pattern = conflux::http::detail::parse_pattern(path);
	auto const route_index = impl_->context_routes.size();
	auto const has_exact_path = is_exact_literal_pattern(pattern);
	auto path_pattern = conflux::http::detail::segments_to_pattern(pattern);
	index_route_pattern(
		find_or_add_method_index(impl_->context_route_indexes, method).routes,
		pattern,
		has_exact_path ? path : std::string_view{},
		route_index);
	impl_->context_routes.push_back({
		.method = std::string{method},
		.pattern = std::move(pattern),
		.path_pattern = std::move(path_pattern),
		.exact_path = has_exact_path ? std::string{path} : std::string{},
		.has_exact_path = has_exact_path,
		.timeout = std::move(timeout),
		.handler = std::move(handler),
	});
}

void Router::add_context_prepared(
	conflux::http::HttpMethod method,
	std::string_view path,
	ContextHandler handler) {
	add_context_prepared(conflux::http::http_method_name(method), path, std::move(handler));
}

void Router::add_context_prepared(
	conflux::http::HttpMethod method,
	std::string_view path,
	std::shared_ptr<std::chrono::milliseconds> timeout,
	ContextHandler handler) {
	add_context_prepared(conflux::http::http_method_name(method), path, std::move(timeout), std::move(handler));
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
	(void)impl_->ensure_work_pool();
	auto pattern = conflux::http::detail::parse_pattern(path);
	auto const route_index = impl_->sse_routes.size();
	auto const has_exact_path = is_exact_literal_pattern(pattern);
	auto path_pattern = conflux::http::detail::segments_to_pattern(pattern);
	index_route_pattern(impl_->sse_index, pattern, has_exact_path ? path : std::string_view{}, route_index);
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
	conflux::http::StaticFileCacheConfig cfg) {
	impl_->static_file_cache = cfg;
	return *this;
}

Router &Router::ws_prepared(
	std::string_view path,
	WsHandler handler) {
	(void)impl_->ensure_work_pool();
	add_prepared(
		conflux::http::HttpMethod::get,
		path,
		Handler{[h = std::move(handler)](conflux::http::RequestView const &req) mutable -> Response {
			if (!conflux::http::detail::is_valid_handshake(req)) {
				return Response::bad_request();
			}
			auto key = conflux::http::trim_http_whitespace(req.headers["sec-websocket-key"]);
			auto up = std::make_shared<conflux::http::WsUpgrade>();
			up->accept_key = conflux::http::detail::ws_accept_key(key);
			up->handler = h;
			Response r{.status = 101, .status_text = "Switching Protocols"};
			r.set_ws_upgrade(std::move(up));
			return r;
		}});
	return *this;
}

[[nodiscard]] std::vector<conflux::http::RouteInfo> Router::route_infos() const {
	std::vector<conflux::http::RouteInfo> result;
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
	return conflux::http::detail::router_defer_http_task(std::move(task));
}

[[nodiscard]] Response Router::run_async_http_task(
	conflux::work::root::Task<Response> task) {
	return defer_http_task(std::move(task));
}

void Router::launch_sse_handler(
	std::shared_ptr<WorkPool> const &pool,
	SseHandler handler,
	conflux::http::OwnedRequest matched,
	std::shared_ptr<conflux::http::SseChannel> const &channel) {
	conflux::http::detail::router_launch_sse_handler(pool, std::move(handler), std::move(matched), channel);
}

[[nodiscard]] Response Router::run_middlewares(
	conflux::http::RequestView const &req,
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
			, next_([this](conflux::http::RequestView const &r) -> Response { return call(r); }) {}

		Response call(
			conflux::http::RequestView const &r) {
			if (idx_ == impl_->middlewares.size()) {
				return (*inner_)(r);
			}
			auto const &mw = impl_->middlewares[idx_++];
			auto next = make_one_shot_next(next_);
			return mw(r, next);
		}
	};
	Step s{impl_.get(), &inner};
	return s.call(req);
}

[[nodiscard]] std::optional<Response> Router::run_context_middlewares(
	conflux::http::RequestView const &req,
	conflux::http::RequestContext const &ctx,
	ContextHandler const &inner) const {
	if (impl_->context_middlewares.empty()) {
		return std::nullopt;
	}
	struct Step {
		Router::Impl const *impl_;
		ContextHandler inner_;
		std::size_t idx_{0};

		Step(
			Router::Impl const *impl,
			ContextHandler const *inner)
			: impl_(impl)
			, inner_(*inner) {}

		conflux::work::root::Task<Response> call(
			std::shared_ptr<Step> self,
			conflux::http::RequestView const &r,
			conflux::http::RequestContext const &c) {
			if (idx_ == impl_->context_middlewares.size()) {
				return inner_(r, c);
			}
			auto const &mw = impl_->context_middlewares[idx_++];
			ContextHandler next_handler =
				[self = std::move(self)](
					conflux::http::RequestView const &next_req,
					conflux::http::RequestContext const &next_ctx) mutable -> conflux::work::root::Task<Response> {
				return self->call(self, next_req, next_ctx);
			};
			auto next = make_one_shot_next(std::move(next_handler));
			return mw(r, c, next);
		}
	};
	auto step = std::make_shared<Step>(impl_.get(), &inner);
	auto run_chain = [](std::shared_ptr<Step> chain, conflux::http::RequestView request, conflux::http::RequestContext request_ctx)
		-> conflux::work::root::Task<Response> { co_return co_await chain->call(chain, request, request_ctx); };
	return defer_http_task(run_chain(std::move(step), conflux::http::RequestView{req}, ctx));
}

Router &Router::serve_static(
	std::string_view url_prefix,
	std::string root_dir,
	conflux::http::StaticOptions const &sopts) {
	auto routes = make_static_route_registration(
		url_prefix,
		std::move(root_dir),
		sopts,
		impl_->static_file_cache,
		impl_->static_cache);
	add_prepared(conflux::http::HttpMethod::get, routes.pattern, std::move(routes.get));
	if (routes.put) {
		add_prepared(conflux::http::HttpMethod::put, routes.pattern, std::move(*routes.put));
	}
	if (routes.del) {
		add_prepared(conflux::http::HttpMethod::delete_, routes.pattern, std::move(*routes.del));
	}
	return *this;
}

[[nodiscard]] Response Router::dispatch(
	conflux::http::OwnedRequest const &req) const {
	conflux::http::RequestView const req_view{req};
	return dispatch(req_view);
}

[[nodiscard]] Response Router::dispatch(
	conflux::http::RequestView const &req) const {
	// HEAD is dispatched as GET; response body is suppressed before sending.
	bool const is_head = (req.method == "HEAD");

	// Strip query std::string before matching.
	auto const path_sv = conflux::http::path_without_query(std::string_view{req.path});

	if (impl_->middlewares.empty()) {
		return dispatch_router_sync(*impl_, req, path_sv, is_head);
	}

	// Inner handler: performs route matching + 404. Middleware wraps this whole thing.
	Handler inner = [this, path_sv, is_head](conflux::http::RequestView const &r) -> Response {
		return dispatch_router_sync(*impl_, r, path_sv, is_head);
	};

	return run_middlewares(req, inner);
}

[[nodiscard]] std::optional<Response> Router::dispatch_context(
	conflux::http::RequestView const &req,
	conflux::http::RequestContext const &ctx) const {
	bool const is_head = (req.method == "HEAD");
	std::string_view const path_sv = conflux::http::path_without_query(req.path);
	if (!impl_->context_middlewares.empty()) {
		ContextHandler inner = [this, path_sv, is_head](
								   conflux::http::RequestView const &r,
								   conflux::http::RequestContext const &c) -> conflux::work::root::Task<Response> {
			if (auto deferred_task = conflux::http::detail::dispatch_context_route_tasks(r, c, path_sv, impl_->context_routes)) {
				auto resp = co_await std::move(deferred_task->task);
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

} // namespace conflux::http
