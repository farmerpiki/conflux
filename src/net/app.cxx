module;

export module conflux.net.app;

export import conflux.net.app.response;
export import conflux.net.app.types;
import std;
import conflux.net.app.defer;
import conflux.net.app.extractor_helpers;
import conflux.net.app.json_helpers;
import conflux.net.app.metadata_helpers;
import conflux.net.app.openapi;
import conflux.net.app.route_helpers;
import conflux.net.app.traits;
import conflux.types;
import conflux.net.config;
import conflux.net.http.types;
import conflux.net.path;
import conflux.net.router;
import conflux.net.http_server;
import conflux.net.observability;
import conflux.net.request_id;
import conflux.net.tracing;
import conflux.uring;
import conflux.crypto;
import conflux.utils;
#if CONFLUX_HAS_JSON
import conflux.json;
import conflux.net.http.native_json;
#endif
import conflux.work;
export namespace conflux::http {

class App;
Router &router(App &app) noexcept;
Router const &router(App const &app) noexcept;
std::vector<RouteInfo> route_infos(App const &app);

namespace detail {

template<class T>
struct IsTaskResult : std::false_type {};

template<class T>
struct IsTaskResult<conflux::work::root::Task<T>> : std::true_type {};

template<class T>
inline constexpr bool IsTaskResultV = IsTaskResult<std::remove_cvref_t<T>>::value;

} // namespace detail

class App {
	using StateMap = std::unordered_map<std::type_index, std::shared_ptr<void>>;
	using ScopedMiddlewareList = std::vector<Router::Middleware>;
	using ScopedContextMiddlewareList = std::vector<Router::ContextMiddleware>;

	struct AppRouteRateLimit {
		std::string name;
		AppRateLimitOptions options{};
		bool enabled{};

		struct Bucket {
			unsigned tokens{};
			std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};
		};

		std::mutex mutex;
		std::unordered_map<std::string, Bucket> buckets;
	};

	struct AppRouteMetadata {
		std::string method;
		std::string path;
		std::string name;
		std::string handler_kind;
		std::string source_file;
		std::uint_least32_t source_line{};
		std::vector<std::string> extractors;
		std::vector<std::string> path_extractors;
		std::vector<std::pair<std::string, std::string>> path_extractor_types;
		std::vector<std::pair<std::size_t, std::string>> path_index_extractor_types;
		std::vector<std::string> path_params;
		std::map<std::string, std::string> path_param_types;
		std::vector<std::type_index> required_states;
		std::vector<std::string> consumes;
		std::vector<std::string> produces;
		std::string request_body_schema;
		std::string response_schema;
		int success_status{kHttpOk};
		bool problem_response{};
		std::shared_ptr<std::size_t> max_body_size = std::make_shared<std::size_t>(0);
		std::shared_ptr<AppRouteRateLimit> rate_limit = std::make_shared<AppRouteRateLimit>();
		std::shared_ptr<std::chrono::milliseconds> timeout = std::make_shared<std::chrono::milliseconds>();
		std::size_t middleware_count{};
		std::shared_ptr<std::string> auth_policy = std::make_shared<std::string>();
		std::string openapi_summary;
		bool uses_body{};
		bool allow_get_body{};
	};

	struct StaticMountMetadata {
		std::string url_prefix;
		std::string root_dir;
		std::string source_file;
		std::uint_least32_t source_line{};
	};

	[[nodiscard]] std::shared_ptr<ScopedMiddlewareList const> current_group_middlewares() const {
		if (group_middlewares_ == nullptr || group_middlewares_->empty()) {
			return {};
		}
		return std::make_shared<ScopedMiddlewareList>(*group_middlewares_);
	}

	[[nodiscard]] std::shared_ptr<ScopedContextMiddlewareList const> current_group_context_middlewares() const {
		if (group_context_middlewares_ == nullptr || group_context_middlewares_->empty()) {
			return {};
		}
		return std::make_shared<ScopedContextMiddlewareList>(*group_context_middlewares_);
	}

	[[nodiscard]] static Response run_scoped_middlewares(
		std::shared_ptr<ScopedMiddlewareList const> const &middlewares,
		RequestView const &req,
		Router::Handler inner) {
		if (!middlewares || middlewares->empty()) {
			return inner(req);
		}
		for (auto it = middlewares->rbegin(); it != middlewares->rend(); ++it) {
			auto mw = *it;
			auto next = std::move(inner);
			inner = [mw = std::move(mw), next = std::move(next)](RequestView const &r) mutable { return mw(r, next); };
		}
		return inner(req);
	}

	[[nodiscard]] static conflux::work::root::Task<Response> run_scoped_context_middlewares(
		std::shared_ptr<ScopedContextMiddlewareList const> middlewares,
		RequestView const &req,
		RequestContext const &ctx,
		Router::ContextHandler inner) {
		if (!middlewares || middlewares->empty()) {
			co_return co_await inner(req, ctx);
		}
		struct Step {
			std::shared_ptr<ScopedContextMiddlewareList const> middlewares;
			Router::ContextHandler inner;
			std::size_t index{};
			Router::ContextHandler next;

			void bind(
				std::shared_ptr<Step> self) {
				next = [self = std::move(self)](
						   RequestView const &r,
						   RequestContext const &c) -> conflux::work::root::Task<Response> { return self->call(r, c); };
			}

			conflux::work::root::Task<Response> call(
				RequestView const &r,
				RequestContext const &c) {
				if (index == middlewares->size()) {
					return inner(r, c);
				}
				auto const &middleware = (*middlewares)[index++];
				return middleware(r, c, next);
			}
		};
		auto step = std::make_shared<Step>(std::move(middlewares), std::move(inner));
		step->bind(step);
		co_return co_await step->call(req, ctx);
	}

	[[nodiscard]] static std::optional<Response> route_auth_failure(
		std::string_view policy,
		RequestView const &req) {
		if (policy.empty()) {
			return std::nullopt;
		}
		auto token = credentials_for_auth_scheme(req.header("authorization"), "Bearer");
		if (!token || token->empty()) {
			return Response::unauthorized("Bearer");
		}
		return std::nullopt;
	}

	[[nodiscard]] static std::optional<BasicAuth> parse_basic_auth(
		RequestView const &req) {
		auto credentials = credentials_for_auth_scheme(req.header("authorization"), "Basic");
		if (!credentials) {
			return std::nullopt;
		}
		auto decoded = base64_decode(*credentials);
		auto colon = decoded.find(':');
		if (colon == std::string::npos) {
			return std::nullopt;
		}
		return BasicAuth{.username = decoded.substr(0, colon), .password = decoded.substr(colon + 1)};
	}

	[[nodiscard]] static std::optional<Response> route_rate_limit_failure(
		AppRouteRateLimit &policy,
		RequestView const &req) {
		if (!policy.enabled) {
			return std::nullopt;
		}
		auto const capacity = policy.options.requests + policy.options.burst;
		if (capacity == 0) {
			auto response = Response::text("Too Many Requests", kHttpTooManyRequests);
			response.headers["Retry-After"] = std::format("{}", policy.options.window.count());
			return response;
		}

		auto const now = std::chrono::steady_clock::now();
		auto const key = req.remote_addr.empty() ? std::string{"unknown"} : std::string{req.remote_addr};
		auto retry_after = static_cast<unsigned>(policy.options.window.count());

		{
			std::scoped_lock const lock{policy.mutex};
			auto const max_clients = std::max<std::size_t>(policy.options.max_clients, 1);
			if (policy.buckets.size() >= max_clients && !policy.buckets.contains(key)) {
				policy.buckets.erase(policy.buckets.begin());
			}
			auto [it, inserted] =
				policy.buckets.try_emplace(key, AppRouteRateLimit::Bucket{.tokens = capacity, .window_start = now});
			auto &bucket = it->second;
			if (inserted) {
				bucket.tokens = capacity;
			}

			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - bucket.window_start);
			if (elapsed >= policy.options.window) {
				bucket.tokens = capacity;
				bucket.window_start = now;
				elapsed = std::chrono::seconds{0};
			}

			if (bucket.tokens > 0) {
				--bucket.tokens;
				return std::nullopt;
			}
			auto remaining = policy.options.window - elapsed;
			retry_after = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::seconds>(remaining).count());
		}

		auto response = Response::text("Too Many Requests", kHttpTooManyRequests);
		response.headers["Retry-After"] = std::format("{}", retry_after);
		return response;
	}

	[[nodiscard]] static Response apply_route_timeout(
		Response response,
		std::chrono::milliseconds timeout) {
		if (timeout.count() > 0 && response.is_deferred()) {
			if (auto const &deferred = response.deferred_response_ptr()) {
				deferred->set_deadline(std::chrono::steady_clock::now() + timeout);
			}
		}
		return response;
	}

public:
	class RouteRef {
	public:
		RouteRef(
			App &app,
			std::size_t index)
			: app_(std::addressof(app))
			, index_(index) {}

		RouteRef &name(
			std::string_view value) {
			metadata().name = std::string{value};
			return *this;
		}

		RouteRef &max_body_size(
			std::size_t value) {
			*metadata().max_body_size = value;
			return *this;
		}

		RouteRef &allow_get_body() {
			metadata().allow_get_body = true;
			return *this;
		}

		RouteRef &timeout(
			std::chrono::milliseconds value) {
			*metadata().timeout = value;
			return *this;
		}

		RouteRef &rate_limit(
			std::string_view value) {
			return rate_limit(value, AppRateLimitOptions{});
		}

		RouteRef &rate_limit(
			std::string_view value,
			AppRateLimitOptions options) {
			auto &policy = *metadata().rate_limit;
			policy.name = std::string{value};
			policy.options = options;
			policy.enabled = !policy.name.empty();
			return *this;
		}

		RouteRef &auth_policy(
			std::string_view value) {
			*metadata().auth_policy = std::string{value};
			return *this;
		}

		RouteRef &openapi_summary(
			std::string_view value) {
			metadata().openapi_summary = std::string{value};
			return *this;
		}

	private:
		[[nodiscard]] AppRouteMetadata &metadata() const { return app_->route_metadata_.at(index_); }

		App *app_;
		std::size_t index_;
	};

	[[nodiscard]] static App default_server() { return App{Config::public_server()}; }
	explicit App(
		Config cfg = Config::public_server())
		: cfg_(std::move(cfg))
		, router_(cfg_)
		, states_(std::make_shared<StateMap>())
#if CONFLUX_HAS_JSON
		, json_options_(std::make_shared<AppJsonOptions>())
#endif
	{
	}
	template<typename F>
	App &add(
		std::string_view method,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		using Fn = std::decay_t<F>;
		if constexpr (requires {
						  typename detail::CallableArgs<Fn>::type;
					  } && detail::has_state_arg<typename detail::CallableArgs<Fn>::type>()) {
			add_extracted(method, path, std::forward<F>(handler), loc);
		} else if constexpr (NullaryRawStringHandler<Fn>) {
			static_assert(
				kDependentFalse<Fn>,
				"HTTP app handlers must not return raw strings; use http::text(...), http::html(...), or "
				"http::Json{...}");
		} else if constexpr (requires(Fn &fn) {
								 { into_response(fn()) } -> std::same_as<Response>;
							 }) {
			record_route_metadata<std::tuple<>>(method, path, "app", loc);
			record_return_metadata<std::invoke_result_t<Fn &>>();
			auto auth_policy = route_metadata_.back().auth_policy;
			auto rate_limit = route_metadata_.back().rate_limit;
			auto timeout = route_metadata_.back().timeout;
			auto scoped_middlewares = current_group_middlewares();
#if CONFLUX_HAS_JSON
			auto json_options = json_options_;
#endif
			router_.add(
				method,
				path,
				[auth_policy,
				 rate_limit,
				 timeout,
				 scoped_middlewares,
#if CONFLUX_HAS_JSON
				 json_options,
#endif
				 fn = std::decay_t<F>(std::forward<F>(handler))](RequestView const &req) mutable {
					Router::Handler inner = [auth_policy,
											 rate_limit,
											 timeout,
#if CONFLUX_HAS_JSON
											 json_options,
#endif
											 &fn](RequestView const &inner_req) mutable {
						if (auto denied = route_auth_failure(*auth_policy, inner_req)) {
							return *std::move(denied);
						}
						if (auto limited = route_rate_limit_failure(*rate_limit, inner_req)) {
							return *std::move(limited);
						}
						return apply_route_timeout(
#if CONFLUX_HAS_JSON
							into_app_response(fn(), *json_options),
#else
							into_app_response(fn()),
#endif
							*timeout);
					};
					return run_scoped_middlewares(scoped_middlewares, req, std::move(inner));
				});
		} else if constexpr (requires(Fn &fn, RequestView const &req) {
								 { into_response(fn(req)) } -> std::same_as<Response>;
							 }) {
			record_route_metadata<std::tuple<RequestView>>(method, path, "app", loc);
			record_return_metadata<std::invoke_result_t<Fn &, RequestView const &>>();
			auto auth_policy = route_metadata_.back().auth_policy;
			auto rate_limit = route_metadata_.back().rate_limit;
			auto timeout = route_metadata_.back().timeout;
			auto scoped_middlewares = current_group_middlewares();
#if CONFLUX_HAS_JSON
			auto json_options = json_options_;
#endif
			router_.add(
				method,
				path,
				[auth_policy,
				 rate_limit,
				 timeout,
				 scoped_middlewares,
#if CONFLUX_HAS_JSON
				 json_options,
#endif
				 fn = Fn(std::forward<F>(handler))](RequestView const &req) mutable {
					Router::Handler inner = [auth_policy,
											 rate_limit,
											 timeout,
#if CONFLUX_HAS_JSON
											 json_options,
#endif
											 &fn](RequestView const &inner_req) mutable {
						if (auto denied = route_auth_failure(*auth_policy, inner_req)) {
							return *std::move(denied);
						}
						if (auto limited = route_rate_limit_failure(*rate_limit, inner_req)) {
							return *std::move(limited);
						}
						return apply_route_timeout(
#if CONFLUX_HAS_JSON
							into_app_response(fn(inner_req), *json_options),
#else
							into_app_response(fn(inner_req)),
#endif
							*timeout);
					};
					return run_scoped_middlewares(scoped_middlewares, req, std::move(inner));
				});
		} else if constexpr (requires(Fn &fn, Request const &req) {
								 { into_response(fn(req)) } -> std::same_as<Response>;
							 }) {
			record_route_metadata<std::tuple<Request>>(method, path, "app", loc);
			record_return_metadata<std::invoke_result_t<Fn &, Request const &>>();
			auto auth_policy = route_metadata_.back().auth_policy;
			auto rate_limit = route_metadata_.back().rate_limit;
			auto timeout = route_metadata_.back().timeout;
			auto scoped_middlewares = current_group_middlewares();
#if CONFLUX_HAS_JSON
			auto json_options = json_options_;
#endif
			router_.add(
				method,
				path,
				[auth_policy,
				 rate_limit,
				 timeout,
				 scoped_middlewares,
#if CONFLUX_HAS_JSON
				 json_options,
#endif
				 fn = Fn(std::forward<F>(handler))](RequestView const &req) mutable {
					Router::Handler inner = [auth_policy,
											 rate_limit,
											 timeout,
#if CONFLUX_HAS_JSON
											 json_options,
#endif
											 &fn](RequestView const &inner_req) mutable {
						if (auto denied = route_auth_failure(*auth_policy, inner_req)) {
							return *std::move(denied);
						}
						if (auto limited = route_rate_limit_failure(*rate_limit, inner_req)) {
							return *std::move(limited);
						}
						auto owned = inner_req.to_owned();
						return apply_route_timeout(
#if CONFLUX_HAS_JSON
							into_app_response(fn(owned), *json_options),
#else
							into_app_response(fn(owned)),
#endif
							*timeout);
					};
					return run_scoped_middlewares(scoped_middlewares, req, std::move(inner));
				});
		} else if constexpr (ContextHandlerFunction<Fn>) {
			add_context_route(method, path, std::forward<F>(handler), loc);
		} else {
			router_.add(method, path, std::forward<F>(handler));
		}
		return *this;
	}
	template<typename F>
	[[nodiscard]] RouteRef route(
		std::string_view method,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_route_ref(method, path, std::forward<F>(handler), loc);
	}
	template<typename F>
	[[nodiscard]] RouteRef add_route_ref(
		std::string_view method,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		auto const before = route_metadata_.size();
		add(method, path, std::forward<F>(handler), loc);
		if (route_metadata_.size() == before) {
			record_route_metadata<std::tuple<RequestView>>(method, path, "raw", loc);
		}
		return RouteRef{*this, route_metadata_.size() - 1};
	}
	template<typename F>
	RouteRef get(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_route_ref("GET", path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	RouteRef get(
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_fixed_route<Path>("GET", std::forward<F>(handler), loc);
	}
	template<typename F>
	RouteRef post(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_route_ref("POST", path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	RouteRef post(
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_fixed_route<Path>("POST", std::forward<F>(handler), loc);
	}
#if CONFLUX_HAS_JSON
	App &json_options(
		AppJsonOptions opts) {
		*json_options_ = opts;
		return *this;
	}

	template<class Body, typename F>
	RouteRef post_body(
		std::string_view path,
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return add_json_body<Body>("POST", path, std::forward<F>(handler), std::move(decode_opts), loc);
	}
	template<FixedString Path, class Body, typename F>
	RouteRef post_body(
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return post_body<Body>(Path.view(), std::forward<F>(handler), std::move(decode_opts), loc);
	}
#endif
	template<typename F>
	RouteRef put(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_route_ref("PUT", path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	RouteRef put(
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_fixed_route<Path>("PUT", std::forward<F>(handler), loc);
	}
#if CONFLUX_HAS_JSON
	template<class Body, typename F>
	RouteRef put_body(
		std::string_view path,
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return add_json_body<Body>("PUT", path, std::forward<F>(handler), std::move(decode_opts), loc);
	}
	template<FixedString Path, class Body, typename F>
	RouteRef put_body(
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return put_body<Body>(Path.view(), std::forward<F>(handler), std::move(decode_opts), loc);
	}
#endif
	template<typename F>
	RouteRef patch(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_route_ref("PATCH", path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	RouteRef patch(
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_fixed_route<Path>("PATCH", std::forward<F>(handler), loc);
	}
#if CONFLUX_HAS_JSON
	template<class Body, typename F>
	RouteRef patch_body(
		std::string_view path,
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return add_json_body<Body>("PATCH", path, std::forward<F>(handler), std::move(decode_opts), loc);
	}
	template<FixedString Path, class Body, typename F>
	RouteRef patch_body(
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return patch_body<Body>(Path.view(), std::forward<F>(handler), std::move(decode_opts), loc);
	}
#endif
	template<typename F>
	RouteRef del(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_route_ref("DELETE", path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	RouteRef del(
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_fixed_route<Path>("DELETE", std::forward<F>(handler), loc);
	}
	template<typename F>
	RouteRef options(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_route_ref("OPTIONS", path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	RouteRef options(
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_fixed_route<Path>("OPTIONS", std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &add_context(
		std::string_view method,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		add_context_route(method, path, std::forward<F>(handler), loc);
		return *this;
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &get_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_context("GET", path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &post_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_context("POST", path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &put_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_context("PUT", path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &patch_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_context("PATCH", path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &del_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_context("DELETE", path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &options_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_context("OPTIONS", path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires(!std::same_as<std::remove_cvref_t<F>, ObservabilityMiddleware>)
	App &use(
		F &&middleware) {
		router_.use(std::forward<F>(middleware));
		++middleware_count_;
		return *this;
	}
	App &use(
		ObservabilityMiddleware const &middleware) {
		if (middleware.options.request_id) {
			router_.use(::request_id_middleware());
			++middleware_count_;
		}
		if (middleware.options.trace_context) {
			router_.use(::tracing_middleware());
			++middleware_count_;
		}
		router_.use(middleware);
		++middleware_count_;
#if CONFLUX_HAS_METRICS
		if (middleware.options.register_metrics_route) {
			get(middleware.options.metrics_path, observability_metrics_handler(middleware));
		}
#endif
		observability_ = middleware;
		return *this;
	}
	template<typename F>
	App &sse(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		record_route_metadata<std::tuple<RequestView>>("GET", path, "sse", loc);
		record_return_metadata<Response>();
		router_.sse(path, std::forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &ws(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		record_route_metadata<std::tuple<RequestView>>("GET", path, "ws", loc);
		record_return_metadata<Response>();
		router_.ws(path, std::forward<F>(handler));
		return *this;
	}
	App &serve_static(
		std::string_view url_prefix,
		std::string root_dir,
		StaticOptions const &sopts = {},
		std::source_location loc = std::source_location::current()) {
		static_mounts_.push_back(
			StaticMountMetadata{
				.url_prefix = std::string{url_prefix},
				.root_dir = root_dir,
				.source_file = loc.file_name(),
				.source_line = loc.line()});
		router_.serve_static(url_prefix, std::move(root_dir), sopts);
		return *this;
	}
	App &openapi_strict(
		bool enabled = true) noexcept {
		openapi_strict_ = enabled;
		return *this;
	}
	template<typename F>
	App &on_not_found(
		F &&handler) {
		router_.on_not_found(std::forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &on_error(
		F &&handler) {
		router_.on_error(std::forward<F>(handler));
		return *this;
	}
	template<class T>
	App &state(
		T &value) {
		auto const key = std::type_index{typeid(T)};
		if (states_->contains(key)) {
			state_issues_.push_back(std::format("duplicate app state: {}", typeid(T).name()));
		}
		(*states_)[key] = std::shared_ptr<void>{std::addressof(value), [](void *) {}};
		return *this;
	}
	template<class T>
	App &state(
		std::shared_ptr<T> value) {
		auto const key = std::type_index{typeid(T)};
		if (states_->contains(key)) {
			state_issues_.push_back(std::format("duplicate app state: {}", typeid(T).name()));
		}
		auto shared_value = std::move(value);
		(*states_)[key] = shared_value;
		using SharedState = std::shared_ptr<T>;
		auto const shared_key = std::type_index{typeid(SharedState)};
		if (states_->contains(shared_key)) {
			state_issues_.push_back(std::format("duplicate app state: {}", typeid(SharedState).name()));
		}
		(*states_)[shared_key] = std::make_shared<SharedState>(std::move(shared_value));
		return *this;
	}
	template<class T>
	App &state_ref(
		T &value) {
		return state(value);
	}
	template<class T>
	App &state_shared(
		std::shared_ptr<T> value) {
		return state(std::move(value));
	}
	template<class T>
	App &state_owned(
		T value) {
		return state_shared(std::make_shared<T>(std::move(value)));
	}
	template<class T>
	[[nodiscard]] State<T> state() const {
		auto const it = states_->find(std::type_index{typeid(T)});
		if (it == states_->end()) {
			return {};
		}
		return State<T>{.value = static_cast<T *>(it->second.get())};
	}

	class Group {
	public:
		template<typename F>
		Group &use(
			F &&middleware) {
			if constexpr (ContextMiddlewareFunction<F>) {
				context_middlewares_.emplace_back(std::forward<F>(middleware));
			} else {
				middlewares_.emplace_back(std::forward<F>(middleware));
			}
			return *this;
		}
		template<typename F>
		[[nodiscard]] RouteRef add(
			std::string_view method,
			std::string_view path,
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			auto *previous = app_.group_middlewares_;
			auto *previous_context = app_.group_context_middlewares_;
			app_.group_middlewares_ = &middlewares_;
			app_.group_context_middlewares_ = &context_middlewares_;
			struct Restore {
				App &app;
				ScopedMiddlewareList *previous;
				ScopedContextMiddlewareList *previous_context;
				~Restore() {
					app.group_middlewares_ = previous;
					app.group_context_middlewares_ = previous_context;
				}
			} restore{app_, previous, previous_context};
			auto route = app_.add_route_ref(method, full_path(path), std::forward<F>(handler), loc);
			apply_policies(route);
			return route;
		}
		Group &max_body_size(
			std::size_t value) {
			max_body_size_ = value;
			return *this;
		}
		Group &timeout(
			std::chrono::milliseconds value) {
			timeout_ = value;
			return *this;
		}
		Group &rate_limit(
			std::string_view value) {
			return rate_limit(value, AppRateLimitOptions{});
		}
		Group &rate_limit(
			std::string_view value,
			AppRateLimitOptions options) {
			rate_limit_ = GroupRateLimit{.name = std::string{value}, .options = options};
			return *this;
		}
		Group &auth_policy(
			std::string_view value) {
			auth_policy_ = std::string{value};
			return *this;
		}
		template<typename F>
		Group &group(
			std::string_view prefix,
			F &&fn) {
			Group child{*this, full_path(prefix)};
			std::invoke(std::forward<F>(fn), child);
			return *this;
		}
		template<typename F>
		[[nodiscard]] RouteRef get(
			std::string_view path,
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return add("GET", path, std::forward<F>(handler), loc);
		}
		template<FixedString Path, typename F>
		[[nodiscard]] RouteRef get(
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return get(Path.view(), std::forward<F>(handler), loc);
		}
		template<typename F>
		[[nodiscard]] RouteRef post(
			std::string_view path,
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return add("POST", path, std::forward<F>(handler), loc);
		}
		template<FixedString Path, typename F>
		[[nodiscard]] RouteRef post(
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return post(Path.view(), std::forward<F>(handler), loc);
		}
		template<typename F>
		[[nodiscard]] RouteRef put(
			std::string_view path,
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return add("PUT", path, std::forward<F>(handler), loc);
		}
		template<FixedString Path, typename F>
		[[nodiscard]] RouteRef put(
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return put(Path.view(), std::forward<F>(handler), loc);
		}
		template<typename F>
		[[nodiscard]] RouteRef patch(
			std::string_view path,
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return add("PATCH", path, std::forward<F>(handler), loc);
		}
		template<FixedString Path, typename F>
		[[nodiscard]] RouteRef patch(
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return patch(Path.view(), std::forward<F>(handler), loc);
		}
		template<typename F>
		[[nodiscard]] RouteRef del(
			std::string_view path,
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return add("DELETE", path, std::forward<F>(handler), loc);
		}
		template<FixedString Path, typename F>
		[[nodiscard]] RouteRef del(
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return del(Path.view(), std::forward<F>(handler), loc);
		}
		template<typename F>
		[[nodiscard]] RouteRef options(
			std::string_view path,
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return add("OPTIONS", path, std::forward<F>(handler), loc);
		}
		template<FixedString Path, typename F>
		[[nodiscard]] RouteRef options(
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return options(Path.view(), std::forward<F>(handler), loc);
		}

	private:
		friend class App;
		struct GroupRateLimit {
			std::string name;
			AppRateLimitOptions options{};
		};
		Group(
			App &app,
			std::string_view prefix)
			: app_(app)
			, prefix_(prefix) {}
		Group(
			Group const &parent,
			std::string prefix)
			: app_(parent.app_)
			, prefix_(std::move(prefix))
			, middlewares_(parent.middlewares_)
			, context_middlewares_(parent.context_middlewares_)
			, max_body_size_(parent.max_body_size_)
			, timeout_(parent.timeout_)
			, rate_limit_(parent.rate_limit_)
			, auth_policy_(parent.auth_policy_) {}

		[[nodiscard]] std::string full_path(
			std::string_view path) const {
			return detail::join_route_path(prefix_, path);
		}
		void apply_policies(
			RouteRef &route) const {
			if (max_body_size_) {
				route.max_body_size(*max_body_size_);
			}
			if (timeout_) {
				route.timeout(*timeout_);
			}
			if (rate_limit_) {
				route.rate_limit(rate_limit_->name, rate_limit_->options);
			}
			if (auth_policy_) {
				route.auth_policy(*auth_policy_);
			}
		}

		App &app_;
		std::string prefix_;
		ScopedMiddlewareList middlewares_;
		ScopedContextMiddlewareList context_middlewares_;
		std::optional<std::size_t> max_body_size_;
		std::optional<std::chrono::milliseconds> timeout_;
		std::optional<GroupRateLimit> rate_limit_;
		std::optional<std::string> auth_policy_;
	};

	template<typename F>
	App &group(
		std::string_view prefix,
		F &&fn) {
		if constexpr (std::invocable<F &, Group &>) {
			Group group{*this, prefix};
			std::invoke(std::forward<F>(fn), group);
		} else {
			router_.group(prefix, std::forward<F>(fn));
		}
		return *this;
	}
	[[nodiscard]] Config &config() { return cfg_; }
	[[nodiscard]] Config const &config() const { return cfg_; }
	friend Router &router(App &app) noexcept;
	friend Router const &router(App const &app) noexcept;
	friend std::vector<RouteInfo> route_infos(App const &app);
	[[nodiscard]] std::vector<AppRouteInfo> routes() const {
		std::vector<AppRouteInfo> out;
		out.reserve(route_metadata_.size());
		for (auto const &route: route_metadata_) {
			out.push_back(
				AppRouteInfo{
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
					.auth_policy = *route.auth_policy,
					.openapi_summary = route.openapi_summary,
					.allow_get_body = route.allow_get_body});
		}
		return out;
	}
	[[nodiscard]] std::vector<AppStaticMountInfo> static_mounts() const {
		std::vector<AppStaticMountInfo> out;
		out.reserve(static_mounts_.size());
		for (auto const &mount: static_mounts_) {
			out.push_back(
				AppStaticMountInfo{
					.url_prefix = mount.url_prefix,
					.root_dir = mount.root_dir,
					.source_file = mount.source_file,
					.source_line = mount.source_line});
		}
		return out;
	}
	[[nodiscard]] std::string route_table() const {
		std::string out;
		for (auto const &route: routes()) {
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
			if (route.max_body_size != 0) {
				out += std::format(" max_body={}", route.max_body_size);
			}
			if (route.timeout.count() != 0) {
				out += std::format(" timeout={}ms", route.timeout.count());
			}
			if (!route.rate_limit.empty()) {
				out += std::format(" rate_limit={}", route.rate_limit);
			}
			if (!route.auth_policy.empty()) {
				out += std::format(" auth={}", route.auth_policy);
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
		}
		for (auto const &mount: static_mounts()) {
			if (!out.empty()) {
				out += '\n';
			}
			out += std::format("STATIC {} root={}", mount.url_prefix, mount.root_dir);
		}
		return out;
	}
	[[nodiscard]] std::string openapi_spec(
		std::string_view title = "API",
		std::string_view version = "1.0.0") const {
		std::vector<detail::AppOpenApiRoute> routes;
		routes.reserve(route_metadata_.size());
		for (auto const &route: route_metadata_) {
			routes.push_back(
				detail::AppOpenApiRoute{
					.method = route.method,
					.path = route.path,
					.name = route.name,
					.openapi_summary = route.openapi_summary,
					.auth_policy = *route.auth_policy,
					.timeout = *route.timeout,
					.rate_limit = route.rate_limit->name,
					.max_body_size = *route.max_body_size,
					.middleware_count = route.middleware_count,
					.path_params = route.path_params,
					.path_param_types = route.path_param_types,
					.consumes = route.consumes,
					.request_body_schema = route.request_body_schema,
					.success_status = route.success_status,
					.produces = route.produces,
					.response_schema = route.response_schema,
					.problem_response = route.problem_response});
		}
		return detail::render_openapi_spec(routes, title, version);
	}
	[[nodiscard]] RouteRef openapi(
		std::string_view path = "/openapi.json",
		std::string_view title = "API",
		std::string_view version = "1.0.0",
		std::source_location loc = std::source_location::current()) {
		auto spec = openapi_spec(title, version);
		return get(
			path,
			[spec = std::move(spec)](RequestView const &) -> Response { return Response::json(spec); },
			loc);
	}
	[[nodiscard]] ValidationReport validate() const {
		ValidationReport report;
		for (auto const &issue: state_issues_) {
			report.issues.push_back(
				ValidationIssue{.code = "app.validation", .message = issue, .method = "APP", .path = "state"});
		}
		report.config_issues = validate_config(cfg_);
		if (auto caps = conflux::runtime::detect_capabilities()) {
			report.capability_issues = validate_config_capabilities(cfg_, *caps);
			report.capability_issues_block_startup =
				cfg_.feature_fallback == conflux::runtime::FeatureFallback::fail_fast
				&& !report.capability_issues.empty();
		} else {
			report.capability_issues.push_back(caps.error());
			report.capability_issues_block_startup =
				cfg_.feature_fallback == conflux::runtime::FeatureFallback::fail_fast;
		}
		validate_tls_config(report);
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
				auto const it = route.path_param_types.find(name);
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
				auto const type_it = route.path_param_types.find(route.path_params[index]);
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
		return report;
	}

	[[nodiscard]] bool route_has_body_limit(
		AppRouteMetadata const &route) const noexcept {
		if (*route.max_body_size != 0 || cfg_.max_body_size != 0) {
			return true;
		}
#if CONFLUX_HAS_JSON
		return json_options_ && json_options_->max_body_size != 0;
#else
		return false;
#endif
	}

	void validate_openapi_completeness(
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

	void validate_tls_config(
		ValidationReport &report) const {
		auto const primary_cert = !cfg_.cert_file.empty();
		auto const primary_key = !cfg_.key_file.empty();
		auto const primary_tls = primary_cert && primary_key;
		if (primary_cert != primary_key) {
			report.issues.push_back(
				ValidationIssue{
					.message = "TLS config invalid: cert_file and key_file must be set together",
					.method = "APP",
					.path = "config"});
		}
		if (cfg_.http3.enabled && !primary_tls) {
			report.issues.push_back(
				ValidationIssue{
					.message = "TLS config invalid: HTTP/3 requires cert_file and key_file",
					.method = "APP",
					.path = "config"});
		}
		if (cfg_.http_redirect_to_https && !primary_tls) {
			report.issues.push_back(
				ValidationIssue{
					.message = "TLS config invalid: HTTPS redirect requires cert_file and key_file",
					.method = "APP",
					.path = "config"});
		}
		for (auto const &host: cfg_.virtual_hosts) {
			auto const host_cert = !host.cert_file.empty();
			auto const host_key = !host.key_file.empty();
			if (host.hostname.empty()) {
				report.issues.push_back(
					ValidationIssue{
						.message = "TLS config invalid: virtual host hostname is empty",
						.method = "APP",
						.path = "config"});
			}
			if (host_cert != host_key) {
				report.issues.push_back(
					ValidationIssue{
						.message = std::format(
							"TLS config invalid: virtual host '{}' cert_file and key_file must be set together",
							host.hostname),
						.method = "APP",
						.path = "config"});
			}
			if ((host_cert || host_key) && !primary_tls) {
				report.issues.push_back(
					ValidationIssue{
						.message = std::format(
							"TLS config invalid: virtual host '{}' requires primary cert_file and key_file",
							host.hostname),
						.method = "APP",
						.path = "config"});
			}
		}
	}

#if CONFLUX_HAS_JSON
	template<class T>
	[[nodiscard]] static Response into_app_response(
		T &&result,
		AppJsonOptions const &json_options) {
		using Clean = std::remove_cvref_t<T>;
		if constexpr (ExpectedHttpProblem<Clean>) {
			if (result) {
				return into_app_response(*std::forward<T>(result), json_options);
			}
			return into_response(std::forward<T>(result).error());
		} else if constexpr (detail::JsonArg<Clean>) {
			using Body = typename detail::JsonType<Clean>::type;
			if constexpr (requires(Body const &value, codec::json::ResponseOptions const &opts) {
							  { codec::json::response_or_internal_error(value, opts) } -> std::same_as<Response>;
						  }) {
				return codec::json::response_or_internal_error(
					result.value,
					codec::json::ResponseOptions{.dump = json_options.dump});
			} else {
				static_assert(
					kDependentFalse<Body>,
					"http::Json<T> responses require T to be serializable; add JsonCodec<T>, JsonMembers<T>, or "
					"reflection JSON support for T");
			}
		} else {
			return into_response(std::forward<T>(result));
		}
	}
#else
	template<class T>
	[[nodiscard]] static Response into_app_response(
		T &&result) {
		return into_response(std::forward<T>(result));
	}
#endif

#if CONFLUX_HAS_JSON
	template<class Args, std::size_t... Is>
	static void apply_json_body_metadata(
		AppRouteMetadata &meta,
		std::index_sequence<Is...>) {
		(
			[&] {
				using Arg = std::tuple_element_t<Is, Args>;
				using Clean = std::remove_cvref_t<Arg>;
				if constexpr (detail::JsonArg<Clean>) {
					using JsonValue = typename detail::JsonType<Clean>::type;
					meta.consumes = {"application/json", "application/problem+json"};
					meta.request_body_schema = detail::schema_json_or_object<JsonValue>();
				} else if constexpr (detail::JsonPatchArg<Clean>) {
					meta.consumes = {"application/json-patch+json"};
					meta.request_body_schema = R"({"type":"array","items":{"type":"object","required":["op","path"]}})";
				} else if constexpr (detail::MergePatchArg<Clean>) {
					meta.consumes = {"application/merge-patch+json"};
					meta.request_body_schema = "{}";
				}
			}(),
			...);
	}
#endif

	template<typename F>
	void add_context_route(
		std::string_view method,
		std::string_view path,
		F &&handler,
		std::source_location loc) {
		using Fn = std::decay_t<F>;
		record_route_metadata<std::tuple<RequestView>>(method, path, "context", loc);
		record_return_metadata<conflux::work::root::Task<Response>>();
		auto auth_policy = route_metadata_.back().auth_policy;
		auto rate_limit = route_metadata_.back().rate_limit;
		auto scoped_context_middlewares = current_group_context_middlewares();
		router_.add_context(
			method,
			path,
			[auth_policy, rate_limit, scoped_context_middlewares, fn = Fn(std::forward<F>(handler))](
				RequestView const &req,
				RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
				Router::ContextHandler inner =
					[auth_policy, rate_limit, &fn](
						RequestView const &inner_req,
						RequestContext const &inner_ctx) -> conflux::work::root::Task<Response> {
					if (auto denied = route_auth_failure(*auth_policy, inner_req)) {
						co_return *std::move(denied);
					}
					if (auto limited = route_rate_limit_failure(*rate_limit, inner_req)) {
						co_return *std::move(limited);
					}
					co_return co_await fn(inner_req, inner_ctx);
				};
				co_return co_await run_scoped_context_middlewares(
					scoped_context_middlewares,
					req,
					ctx,
					std::move(inner));
			});
	}

	template<class Args>
	void record_route_metadata(
		std::string_view method,
		std::string_view path,
		std::string_view handler_kind,
		std::source_location loc) {
		AppRouteMetadata meta{
			.method = std::string{method},
			.path = std::string{path},
			.handler_kind = std::string{handler_kind},
			.source_file = loc.file_name(),
			.source_line = loc.line(),
			.middleware_count = middleware_count_
							  + (group_middlewares_ == nullptr ? 0U : group_middlewares_->size())
							  + (group_context_middlewares_ == nullptr ? 0U : group_context_middlewares_->size())};
		detail::append_extractors<Args>(meta.extractors, std::make_index_sequence<std::tuple_size_v<Args>>{});
		detail::append_path_extractors<Args>(meta.path_extractors, std::make_index_sequence<std::tuple_size_v<Args>>{});
		detail::append_path_extractor_types<Args>(
			meta.path_extractor_types,
			meta.path_index_extractor_types,
			std::make_index_sequence<std::tuple_size_v<Args>>{});
		auto pattern = detail::route_pattern_info(path);
		meta.path_params = std::move(pattern.params);
		meta.path_param_types = std::move(pattern.param_types);
		detail::append_required_states<Args>(meta.required_states, std::make_index_sequence<std::tuple_size_v<Args>>{});
		if (std::ranges::contains(meta.extractors, std::string_view{"RequiredBearer"}) && meta.auth_policy->empty()) {
			*meta.auth_policy = "bearer";
		}
		meta.uses_body = detail::has_body_extractor<Args>() || handler_kind == "json_body";
		if constexpr (detail::has_body_extractor<Args>()) {
			if (std::ranges::contains(meta.extractors, std::string_view{"JsonDocument"})) {
				meta.consumes = {"application/json", "application/problem+json"};
			} else if (std::ranges::contains(meta.extractors, std::string_view{"JsonPatch"})) {
				meta.consumes = {"application/json-patch+json"};
				meta.request_body_schema = R"({"type":"array","items":{"type":"object","required":["op","path"]}})";
			} else if (std::ranges::contains(meta.extractors, std::string_view{"MergePatch"})) {
				meta.consumes = {"application/merge-patch+json"};
				meta.request_body_schema = "{}";
			}
#if CONFLUX_HAS_JSON
			apply_json_body_metadata<Args>(meta, std::make_index_sequence<std::tuple_size_v<Args>>{});
#endif
		}
		route_metadata_.push_back(std::move(meta));
	}

	template<class Return>
	static void apply_return_metadata(
		AppRouteMetadata &meta) {
		using ReturnClean = std::remove_cvref_t<Return>;
		if constexpr (detail::IsTaskResultV<ReturnClean>) {
			apply_return_metadata<typename ReturnClean::value_type>(meta);
			return;
		}
		using Clean = typename detail::ResponseMetadataType<Return>::type;
		if constexpr (std::same_as<Clean, Created>) {
			meta.success_status = kHttpCreated;
		}
		if constexpr (detail::JsonArg<Clean>) {
			meta.produces = {"application/json"};
#if CONFLUX_HAS_JSON
			using JsonValue = typename detail::JsonType<Clean>::type;
			meta.response_schema = detail::schema_json_or_object<JsonValue>();
#endif
		}
		if constexpr (detail::ReturnsProblemResponse<std::remove_cvref_t<Return>>::value) {
			meta.problem_response = true;
		}
	}

	template<class Return>
	void record_return_metadata() {
		apply_return_metadata<Return>(route_metadata_.back());
	}

	template<class Arg>
	[[nodiscard]] static auto make_handler_arg(
		StateMap const &states,
		RequestView const &req
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		std::size_t max_body_size
#endif
	) {
		using Clean = std::remove_cvref_t<Arg>;
		if constexpr (detail::RequestViewArg<Clean>) {
			return req;
		} else if constexpr (detail::RequestArg<Clean>) {
			return req.to_owned();
		} else if constexpr (detail::StateArg<Clean>) {
			using StateValue = typename detail::StateType<Clean>::type;
			auto const it = states.find(std::type_index{typeid(StateValue)});
			if (it == states.end()) {
				throw ExtractorFailure{Response::internal_error("missing app state")};
			}
			return State<StateValue>{.value = static_cast<StateValue *>(it->second.get())};
		} else if constexpr (detail::PathArg<Clean>) {
			using PathValue = typename detail::PathType<Clean>::type;
			if constexpr (std::same_as<PathValue, std::string_view>) {
				return Clean{.value = req.param(detail::PathType<Clean>::name.view())};
			} else if constexpr (detail::OptionalFieldValue<PathValue>) {
				using FieldValue = typename detail::OptionalFieldType<PathValue>::type;
				return Clean{
					.value = detail::extract_or_throw(
						req.template optional_param_as<FieldValue>(detail::PathType<Clean>::name.view()),
						"Path")};
			} else {
				return Clean{
					.value = detail::extract_or_throw(
						req.template param_as<PathValue>(detail::PathType<Clean>::name.view()),
						"Path")};
			}
		} else if constexpr (detail::PathAtArg<Clean>) {
			using PathValue = typename detail::PathAtType<Clean>::type;
			if constexpr (std::same_as<PathValue, std::string_view>) {
				auto param = detail::path_param_at(req, detail::PathAtType<Clean>::index);
				return Clean{.value = param ? param->second : std::string_view{}};
			} else {
				return Clean{
					.value = detail::extract_or_throw(
						detail::path_param_as_at<PathValue>(req, detail::PathAtType<Clean>::index),
						"PathAt")};
			}
		} else if constexpr (detail::QueryArg<Clean>) {
			using QueryValue = typename detail::QueryType<Clean>::type;
			if constexpr (std::same_as<QueryValue, std::string_view>) {
				return Clean{.value = req.query_value(detail::QueryType<Clean>::name.view())};
			} else if constexpr (detail::OptionalFieldValue<QueryValue>) {
				using FieldValue = typename detail::OptionalFieldType<QueryValue>::type;
				return Clean{
					.value = detail::extract_or_throw(
						req.template optional_query_as<FieldValue>(detail::QueryType<Clean>::name.view()),
						"Query")};
			} else {
				return Clean{
					.value = detail::extract_or_throw(
						req.template query_as<QueryValue>(detail::QueryType<Clean>::name.view()),
						"Query")};
			}
		} else if constexpr (detail::HeaderArg<Clean>) {
			using HeaderValue = typename detail::HeaderType<Clean>::type;
			if constexpr (std::same_as<HeaderValue, std::string_view>) {
				return Clean{.value = req.header(detail::HeaderType<Clean>::name.view())};
			} else if constexpr (detail::OptionalFieldValue<HeaderValue>) {
				using FieldValue = typename detail::OptionalFieldType<HeaderValue>::type;
				return Clean{
					.value = detail::extract_or_throw(
						req.template optional_header_as<FieldValue>(detail::HeaderType<Clean>::name.view()),
						"Header")};
			} else {
				return Clean{
					.value = detail::extract_or_throw(
						req.template header_as<HeaderValue>(detail::HeaderType<Clean>::name.view()),
						"Header")};
			}
		} else if constexpr (detail::CookieArg<Clean>) {
			using CookieValue = typename detail::CookieType<Clean>::type;
			if constexpr (std::same_as<CookieValue, std::string_view>) {
				return Clean{.value = req.cookie(detail::CookieType<Clean>::name.view())};
			} else if constexpr (detail::OptionalFieldValue<CookieValue>) {
				using FieldValue = typename detail::OptionalFieldType<CookieValue>::type;
				return Clean{
					.value = detail::extract_or_throw(
						req.template optional_cookie_as<FieldValue>(detail::CookieType<Clean>::name.view()),
						"Cookie")};
			} else {
				return Clean{
					.value = detail::extract_or_throw(
						req.template cookie_as<CookieValue>(detail::CookieType<Clean>::name.view()),
						"Cookie")};
			}
		} else if constexpr (detail::FormArg<Clean>) {
			using FormValue = typename detail::FormType<Clean>::type;
			if constexpr (std::same_as<FormValue, std::string_view>) {
				return Clean{.value = req.form_value(detail::FormType<Clean>::name.view())};
			} else if constexpr (detail::OptionalFieldValue<FormValue>) {
				using FieldValue = typename detail::OptionalFieldType<FormValue>::type;
				return Clean{
					.value = detail::extract_or_throw(
						req.template optional_form_as<FieldValue>(detail::FormType<Clean>::name.view()),
						"Form")};
			} else {
				return Clean{
					.value = detail::extract_or_throw(
						req.template form_as<FormValue>(detail::FormType<Clean>::name.view()),
						"Form")};
			}
#if CONFLUX_HAS_JSON
		} else if constexpr (detail::QueryParamsArg<Clean>) {
			using QueryValue = typename detail::QueryParamsType<Clean>::type;
			return Clean{.value = detail::extract_query_params<QueryValue>(req)};
		} else if constexpr (detail::FormParamsArg<Clean>) {
			using FormValue = typename detail::FormParamsType<Clean>::type;
			return Clean{.value = detail::extract_form_params<FormValue>(req)};
#endif
		} else if constexpr (detail::BodyTextArg<Clean>) {
			return BodyText{.value = req.body};
		} else if constexpr (detail::BodyBytesArg<Clean>) {
			return BodyBytes{.value = std::as_bytes(std::span{req.body.data(), req.body.size()})};
		} else if constexpr (detail::OwnedBodyBytesArg<Clean>) {
			return OwnedBodyBytes{.value = std::string{req.body}};
#if CONFLUX_HAS_JSON
		} else if constexpr (detail::JsonDocumentArg<Clean>) {
			auto content_type = req.header("content-type");
			if (!detail::content_type_is_json_request(content_type)) {
				throw ExtractorFailure{detail::unsupported_json_content_type_problem()};
			}
			auto const limit = max_body_size != 0 ? max_body_size : json_options.max_body_size;
			if (limit != 0 && req.body.size() > limit) {
				throw ExtractorFailure{detail::json_body_too_large_problem()};
			}
			auto parsed = codec::json::DefaultJsonProvider::parse_json_document(req.body, json_options.decode);
			if (!parsed) {
				throw ExtractorFailure{detail::json_decode_problem(parsed.error())};
			}
			return JsonDocument{.value = std::move(*parsed)};
		} else if constexpr (detail::JsonPatchArg<Clean>) {
			auto content_type = req.header("content-type");
			if (!detail::content_type_is_json_patch(content_type)) {
				throw ExtractorFailure{detail::unsupported_json_content_type_problem()};
			}
			auto const limit = max_body_size != 0 ? max_body_size : json_options.max_body_size;
			if (limit != 0 && req.body.size() > limit) {
				throw ExtractorFailure{detail::json_body_too_large_problem()};
			}
			auto parsed = codec::json::DefaultJsonProvider::parse_json_document(req.body, json_options.decode);
			if (!parsed) {
				throw ExtractorFailure{detail::json_decode_problem(parsed.error())};
			}
			if (auto ok = conflux::json::validate_patch(parsed->root()); !ok) {
				throw ExtractorFailure{detail::json_patch_problem(ok.error())};
			}
			return JsonPatch{.value = std::move(*parsed)};
		} else if constexpr (detail::MergePatchArg<Clean>) {
			auto content_type = req.header("content-type");
			if (!detail::content_type_is_merge_patch(content_type)) {
				throw ExtractorFailure{detail::unsupported_json_content_type_problem()};
			}
			auto const limit = max_body_size != 0 ? max_body_size : json_options.max_body_size;
			if (limit != 0 && req.body.size() > limit) {
				throw ExtractorFailure{detail::json_body_too_large_problem()};
			}
			auto parsed = codec::json::DefaultJsonProvider::parse_json_document(req.body, json_options.decode);
			if (!parsed) {
				throw ExtractorFailure{detail::json_decode_problem(parsed.error())};
			}
			return MergePatch{.value = std::move(*parsed)};
		} else if constexpr (detail::JsonArg<Clean>) {
			using BodyValue = typename detail::JsonType<Clean>::type;
			auto content_type = req.header("content-type");
			if (!detail::content_type_is_json_request(content_type)) {
				throw ExtractorFailure{detail::unsupported_json_content_type_problem()};
			}
			auto const limit = max_body_size != 0 ? max_body_size : json_options.max_body_size;
			if (limit != 0 && req.body.size() > limit) {
				throw ExtractorFailure{detail::json_body_too_large_problem()};
			}
			auto decoded = conflux::json::boundary::decode_with<codec::json::DefaultJsonProvider, BodyValue>(
				req.body,
				json_options.decode);
			if (!decoded) {
				throw ExtractorFailure{detail::json_decode_problem(decoded.error())};
			}
			return Json<BodyValue>{std::move(*decoded)};
#endif
		} else if constexpr (detail::MultipartArg<Clean>) {
			return Multipart{.form = req.form, .files = req.files};
		} else if constexpr (detail::RequestIdArg<Clean>) {
			return RequestId{.value = req.header("x-request-id")};
		} else if constexpr (detail::ConnectionInfoArg<Clean>) {
			return ConnectionInfo{.remote_addr = req.remote_addr, .is_tls = req.is_tls};
		} else if constexpr (detail::TraceContextArg<Clean>) {
			return TraceContext{.traceparent = req.header("traceparent")};
		} else if constexpr (detail::BearerArg<Clean>) {
			auto token = credentials_for_auth_scheme(req.header("authorization"), "Bearer");
			return Bearer{.token = token.value_or(std::string_view{})};
		} else if constexpr (detail::RequiredBearerArg<Clean>) {
			auto token = credentials_for_auth_scheme(req.header("authorization"), "Bearer");
			if (!token || token->empty()) {
				throw ExtractorFailure{Response::unauthorized("Bearer")};
			}
			return RequiredBearer{.token = *token};
		} else if constexpr (detail::OptionalBearerArg<Clean>) {
			auto token = credentials_for_auth_scheme(req.header("authorization"), "Bearer");
			return OptionalBearer{
				.token = token && !token->empty() ? std::optional<std::string_view>{*token} : std::nullopt};
		} else if constexpr (detail::BasicAuthArg<Clean>) {
			return parse_basic_auth(req).value_or(BasicAuth{});
		} else if constexpr (detail::RequiredBasicAuthArg<Clean>) {
			auto credentials = parse_basic_auth(req);
			if (!credentials) {
				throw ExtractorFailure{Response::unauthorized("Basic")};
			}
			return RequiredBasicAuth{.credentials = std::move(*credentials)};
		} else if constexpr (detail::OptionalBasicAuthArg<Clean>) {
			return OptionalBasicAuth{.credentials = parse_basic_auth(req)};
		} else {
			static_assert(
				kDependentFalse<Arg>,
				"HTTP app handler argument must be http::RequestView, http::Request, http::Path<...>, "
				"http::PathAt<...>, http::Query<...>, http::Header<...>, http::Cookie<...>, http::Form<...>, "
				"http::QueryParams<...>, http::FormParams<...>, http::BodyText, http::Json<T>, http::JsonDocument, "
				"http::JsonPatch, http::MergePatch, http::BodyBytes, http::OwnedBodyBytes, http::Multipart, "
				"http::RequestId, http::ConnectionInfo, http::TraceContext, http::Bearer, http::RequiredBearer, "
				"http::OptionalBearer, http::BasicAuth, http::RequiredBasicAuth, http::OptionalBasicAuth, "
				"or http::State<T>");
		}
	}

	template<class Arg, std::size_t Index>
	[[nodiscard]] static auto make_inline_path_arg(
		RequestView const &req) {
		using Clean = std::remove_cvref_t<Arg>;
		if constexpr (std::same_as<Clean, std::string_view>) {
			auto param = detail::path_param_at(req, Index);
			return param ? param->second : std::string_view{};
		} else if constexpr (std::same_as<Clean, std::string>) {
			auto param = detail::path_param_at(req, Index);
			return param ? std::string{param->second} : std::string{};
		} else {
			return detail::extract_or_throw(detail::path_param_as_at<Clean>(req, Index), "Path");
		}
	}

	template<class Args, std::size_t Index>
	[[nodiscard]] static auto make_fixed_route_arg(
		StateMap const &states,
		RequestView const &req
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		std::size_t max_body_size
#endif
	) {
		using Arg = std::tuple_element_t<Index, Args>;
		using Clean = std::remove_cvref_t<Arg>;
		if constexpr (detail::InlinePathArg<Clean>) {
			return make_inline_path_arg<Clean, detail::inline_path_arg_index<Args, Index>()>(req);
		} else {
			return make_handler_arg<Arg>(
				states,
				req
#if CONFLUX_HAS_JSON
				,
				json_options,
				max_body_size
#endif
			);
		}
	}

	template<class Args, class Fn, std::size_t... Is>
	[[nodiscard]] static Response invoke_fixed_route(
		StateMap const &states,
		Fn &fn,
		RequestView const &req,
		std::index_sequence<Is...>
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		std::size_t max_body_size
#endif
	) {
		try {
			return into_app_response(
				fn(make_fixed_route_arg<Args, Is>(
					states,
					req
#if CONFLUX_HAS_JSON
					,
					json_options,
					max_body_size
#endif
					)...)
#if CONFLUX_HAS_JSON
					,
				json_options
#endif
			);
		} catch (ExtractorFailure &failure) { return std::move(failure).response(); }
	}

	template<class Fn, class ExtractedArgs>
	[[nodiscard]] static conflux::work::root::Task<Response> await_app_task_response(
		Fn &handler,
		ExtractedArgs extracted_args
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions json_options
#endif
	) {
		try {
			auto result = std::apply([&handler](auto &...args) { return handler(args...); }, extracted_args);
#if CONFLUX_HAS_JSON
			co_return into_app_response(co_await std::move(result), json_options);
#else
			co_return into_app_response(co_await std::move(result));
#endif
		} catch (ExtractorFailure &failure) { co_return std::move(failure).response(); }
	}

	[[nodiscard]] static conflux::work::root::Task<Response> extraction_failure_response(
		Response response) {
		co_return response;
	}

	template<class Args, class Fn, std::size_t... Is>
	[[nodiscard]] static conflux::work::root::Task<Response> invoke_fixed_route_async(
		StateMap const &states,
		Fn &fn,
		RequestView req,
		std::index_sequence<Is...>
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		std::size_t max_body_size
#endif
	) {
		return conflux::work::root::spawn(
			[&states,
			 &fn,
			 req = std::move(req)
#if CONFLUX_HAS_JSON
				 ,
			 &json_options,
			 max_body_size
#endif
		]() mutable -> conflux::work::root::Task<Response> {
				try {
					auto extracted_args = std::make_tuple(
						make_fixed_route_arg<Args, Is>(
							states,
							req
#if CONFLUX_HAS_JSON
							,
							json_options,
							max_body_size
#endif
							)...);
#if CONFLUX_HAS_JSON
					return await_app_task_response(fn, std::move(extracted_args), json_options);
#else
					return await_app_task_response(fn, std::move(extracted_args));
#endif
				} catch (ExtractorFailure &failure) {
					return extraction_failure_response(std::move(failure).response());
				}
			});
	}

	template<FixedString Path, class Args, std::size_t... Is>
	static void record_inline_path_extractors(
		AppRouteMetadata &meta,
		std::index_sequence<Is...>) {
		(
			[&] {
				using Arg = std::tuple_element_t<Is, Args>;
				if constexpr (detail::InlinePathArg<Arg>) {
					constexpr auto path_index = detail::inline_path_arg_index<Args, Is>();
					meta.extractors[Is] = std::format("Path<{}>", detail::fixed_path_param_name<Path, path_index>());
					meta.path_index_extractor_types.emplace_back(
						path_index,
						std::string{detail::route_type_tag<Arg>()});
				}
			}(),
			...);
	}

	template<class Fn, class Args, class Indices>
	struct ExtractedInvokeResult;

	template<class Fn, class Args, std::size_t... Is>
	struct ExtractedInvokeResult<Fn, Args, std::index_sequence<Is...>> {
		using type = std::invoke_result_t<Fn &, std::tuple_element_t<Is, Args>...>;
	};

	template<FixedString Path, typename F>
	[[nodiscard]] RouteRef add_fixed_route(
		std::string_view method,
		F &&handler,
		std::source_location loc) {
		using Fn = std::decay_t<F>;
		using Args = typename detail::CallableArgs<Fn>::type;
		record_route_metadata<Args>(method, Path.view(), "app", loc);
		auto &meta = route_metadata_.back();
		record_inline_path_extractors<Path, Args>(meta, std::make_index_sequence<std::tuple_size_v<Args>>{});
		record_extracted_return_metadata<Fn, Args>(std::make_index_sequence<std::tuple_size_v<Args>>{});
#if CONFLUX_HAS_JSON
		auto max_body_size = route_metadata_.back().max_body_size;
		auto json_options = json_options_;
#endif
		auto auth_policy = route_metadata_.back().auth_policy;
		auto rate_limit = route_metadata_.back().rate_limit;
		auto timeout = route_metadata_.back().timeout;
		auto scoped_middlewares = current_group_middlewares();
		auto scoped_context_middlewares = current_group_context_middlewares();
		using Indices = std::make_index_sequence<std::tuple_size_v<Args>>;
		using Result = typename ExtractedInvokeResult<Fn, Args, Indices>::type;
		if constexpr (detail::IsTaskResultV<Result>) {
			router_.add_context(
				method,
				Path.view(),
				[states = states_,
				 auth_policy,
				 rate_limit,
				 scoped_context_middlewares,
				 fn = Fn(std::forward<F>(handler))
#if CONFLUX_HAS_JSON
					 ,
				 max_body_size,
				 json_options
#endif
			](RequestView const &req, RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
					Router::ContextHandler inner =
						[states,
						 auth_policy,
						 rate_limit,
						 &fn
#if CONFLUX_HAS_JSON
						 ,
						 max_body_size,
						 json_options
#endif
					](RequestView const &inner_req,
						RequestContext const &) mutable -> conflux::work::root::Task<Response> {
						if (auto denied = route_auth_failure(*auth_policy, inner_req)) {
							co_return *std::move(denied);
						}
						if (auto limited = route_rate_limit_failure(*rate_limit, inner_req)) {
							co_return *std::move(limited);
						}
						co_return co_await invoke_fixed_route_async<Args>(
							*states,
							fn,
							inner_req,
							std::make_index_sequence<std::tuple_size_v<Args>>{}
#if CONFLUX_HAS_JSON
							,
							*json_options,
							*max_body_size
#endif
						);
					};
					co_return co_await run_scoped_context_middlewares(
						scoped_context_middlewares,
						req,
						ctx,
						std::move(inner));
				});
		} else {
			router_.add(
				method,
				Path.view(),
				[states = states_,
				 auth_policy,
				 rate_limit,
				 timeout,
				 scoped_middlewares,
				 fn = Fn(std::forward<F>(handler))
#if CONFLUX_HAS_JSON
					 ,
				 max_body_size,
				 json_options
#endif
			](RequestView const &req) mutable {
					Router::Handler inner = [states,
											 auth_policy,
											 rate_limit,
											 timeout,
											 &fn
#if CONFLUX_HAS_JSON
											 ,
											 max_body_size,
											 json_options
#endif
					](RequestView const &inner_req) mutable {
						if (auto denied = route_auth_failure(*auth_policy, inner_req)) {
							return *std::move(denied);
						}
						if (auto limited = route_rate_limit_failure(*rate_limit, inner_req)) {
							return *std::move(limited);
						}
						return apply_route_timeout(
							invoke_fixed_route<Args>(
								*states,
								fn,
								inner_req,
								std::make_index_sequence<std::tuple_size_v<Args>>{}
#if CONFLUX_HAS_JSON
								,
								*json_options,
								*max_body_size
#endif
								),
							*timeout);
					};
					return run_scoped_middlewares(scoped_middlewares, req, std::move(inner));
				});
		}
		return RouteRef{*this, route_metadata_.size() - 1};
	}

	template<class Args, class Fn, std::size_t... Is>
	[[nodiscard]] static Response invoke_extracted(
		StateMap const &states,
		Fn &fn,
		RequestView const &req,
		std::index_sequence<Is...>
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		std::size_t max_body_size
#endif
	) {
		try {
			return into_app_response(
				fn(make_handler_arg<std::tuple_element_t<Is, Args>>(
					states,
					req
#if CONFLUX_HAS_JSON
					,
					json_options,
					max_body_size
#endif
					)...)
#if CONFLUX_HAS_JSON
					,
				json_options
#endif
			);
		} catch (ExtractorFailure &failure) { return std::move(failure).response(); }
	}

	template<class Args, class Fn, std::size_t... Is>
	[[nodiscard]] static conflux::work::root::Task<Response> invoke_extracted_async(
		StateMap const &states,
		Fn &fn,
		RequestView req,
		std::index_sequence<Is...>
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		std::size_t max_body_size
#endif
	) {
		return conflux::work::root::spawn(
			[&states,
			 &fn,
			 req = std::move(req)
#if CONFLUX_HAS_JSON
				 ,
			 &json_options,
			 max_body_size
#endif
		]() mutable -> conflux::work::root::Task<Response> {
				try {
					auto extracted_args = std::make_tuple(
						make_handler_arg<std::tuple_element_t<Is, Args>>(
							states,
							req
#if CONFLUX_HAS_JSON
							,
							json_options,
							max_body_size
#endif
							)...);
#if CONFLUX_HAS_JSON
					return await_app_task_response(fn, std::move(extracted_args), json_options);
#else
					return await_app_task_response(fn, std::move(extracted_args));
#endif
				} catch (ExtractorFailure &failure) {
					return extraction_failure_response(std::move(failure).response());
				}
			});
	}

	template<class Fn, class Args, std::size_t... Is>
	void record_extracted_return_metadata(
		std::index_sequence<Is...>) {
		using Result = std::invoke_result_t<Fn &, std::tuple_element_t<Is, Args>...>;
		if constexpr (RawStringResponse<Result>) {
			static_assert(
				kDependentFalse<Fn>,
				"HTTP app handlers must not return raw strings; use http::text(...), http::html(...), or "
				"http::Json{...}");
		} else {
			record_return_metadata<Result>();
		}
	}

	template<typename F>
	App &add_extracted(
		std::string_view method,
		std::string_view path,
		F &&handler,
		std::source_location loc) {
		using Fn = std::decay_t<F>;
		using Args = typename detail::CallableArgs<Fn>::type;
		record_route_metadata<Args>(method, path, "app", loc);
		record_extracted_return_metadata<Fn, Args>(std::make_index_sequence<std::tuple_size_v<Args>>{});
#if CONFLUX_HAS_JSON
		auto max_body_size = route_metadata_.back().max_body_size;
		auto json_options = json_options_;
#endif
		auto auth_policy = route_metadata_.back().auth_policy;
		auto rate_limit = route_metadata_.back().rate_limit;
		auto timeout = route_metadata_.back().timeout;
		auto scoped_middlewares = current_group_middlewares();
		auto scoped_context_middlewares = current_group_context_middlewares();
		using Indices = std::make_index_sequence<std::tuple_size_v<Args>>;
		using Result = typename ExtractedInvokeResult<Fn, Args, Indices>::type;
		if constexpr (detail::IsTaskResultV<Result>) {
			router_.add_context(
				method,
				path,
				[states = states_,
				 auth_policy,
				 rate_limit,
				 scoped_context_middlewares,
				 fn = Fn(std::forward<F>(handler))
#if CONFLUX_HAS_JSON
					 ,
				 max_body_size,
				 json_options
#endif
			](RequestView const &req, RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
					Router::ContextHandler inner =
						[states,
						 auth_policy,
						 rate_limit,
						 &fn
#if CONFLUX_HAS_JSON
						 ,
						 max_body_size,
						 json_options
#endif
					](RequestView const &inner_req,
						RequestContext const &) mutable -> conflux::work::root::Task<Response> {
						if (auto denied = route_auth_failure(*auth_policy, inner_req)) {
							co_return *std::move(denied);
						}
						if (auto limited = route_rate_limit_failure(*rate_limit, inner_req)) {
							co_return *std::move(limited);
						}
						co_return co_await invoke_extracted_async<Args>(
							*states,
							fn,
							inner_req,
							std::make_index_sequence<std::tuple_size_v<Args>>{}
#if CONFLUX_HAS_JSON
							,
							*json_options,
							*max_body_size
#endif
						);
					};
					co_return co_await run_scoped_context_middlewares(
						scoped_context_middlewares,
						req,
						ctx,
						std::move(inner));
				});
		} else {
			router_.add(
				method,
				path,
				[states = states_,
				 auth_policy,
				 rate_limit,
				 timeout,
				 scoped_middlewares,
				 fn = Fn(std::forward<F>(handler))
#if CONFLUX_HAS_JSON
					 ,
				 max_body_size,
				 json_options
#endif
			](RequestView const &req) mutable {
					Router::Handler inner = [states,
											 auth_policy,
											 rate_limit,
											 timeout,
											 &fn
#if CONFLUX_HAS_JSON
											 ,
											 max_body_size,
											 json_options
#endif
					](RequestView const &inner_req) mutable {
						if (auto denied = route_auth_failure(*auth_policy, inner_req)) {
							return *std::move(denied);
						}
						if (auto limited = route_rate_limit_failure(*rate_limit, inner_req)) {
							return *std::move(limited);
						}
						return apply_route_timeout(
							invoke_extracted<Args>(
								*states,
								fn,
								inner_req,
								std::make_index_sequence<std::tuple_size_v<Args>>{}
#if CONFLUX_HAS_JSON
								,
								*json_options,
								*max_body_size
#endif
								),
							*timeout);
					};
					return run_scoped_middlewares(scoped_middlewares, req, std::move(inner));
				});
		}
		return *this;
	}

#if CONFLUX_HAS_JSON
	template<class Arg, class Body>
	[[nodiscard]] static auto make_json_handler_arg(
		StateMap const &states,
		RequestView const &req,
		Json<Body> const &body) {
		using Clean = std::remove_cvref_t<Arg>;
		if constexpr (detail::JsonArg<Clean>) {
			return body;
		} else if constexpr (detail::RawJsonBodyArg<Clean, Body>) {
			return body.value;
		} else {
			return make_handler_arg<Arg>(states, req, AppJsonOptions{}, 0);
		}
	}

	template<class Args, class Body, class Fn, std::size_t... Is>
	[[nodiscard]] static Response invoke_json_extracted(
		StateMap const &states,
		Fn &fn,
		RequestView const &req,
		Json<Body> const &body,
		std::index_sequence<Is...>,
		AppJsonOptions const &json_options) {
		try {
			return into_app_response(
				fn(make_json_handler_arg<std::tuple_element_t<Is, Args>>(states, req, body)...),
				json_options);
		} catch (ExtractorFailure &failure) { return std::move(failure).response(); }
	}

	template<class Body, typename F>
	RouteRef add_json_body(
		std::string_view method,
		std::string_view path,
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts,
		std::source_location loc) {
		using BodyValue = std::remove_cvref_t<Body>;
		using Fn = std::decay_t<F>;
		using Args = typename detail::CallableArgs<Fn>::type;
		record_route_metadata<Args>(method, path, "json_body", loc);
		route_metadata_.back().consumes = {"application/json", "application/problem+json"};
		route_metadata_.back().produces = {"application/json"};
		route_metadata_.back().request_body_schema = detail::schema_json_or_object<BodyValue>();
		auto max_body_size = route_metadata_.back().max_body_size;
		auto json_options = json_options_;
		auto auth_policy = route_metadata_.back().auth_policy;
		auto rate_limit = route_metadata_.back().rate_limit;
		auto timeout = route_metadata_.back().timeout;
		router_.add(
			method,
			path,
			[states = states_,
			 auth_policy,
			 rate_limit,
			 timeout,
			 fn = Fn(std::forward<F>(handler)),
			 decode_opts = decode_opts,
			 max_body_size,
			 json_options](RequestView const &req) mutable -> Response {
				if (auto denied = route_auth_failure(*auth_policy, req)) {
					return *std::move(denied);
				}
				if (auto limited = route_rate_limit_failure(*rate_limit, req)) {
					return *std::move(limited);
				}
				auto content_type = req.header("content-type");
				if (!detail::content_type_is_json_request(content_type)) {
					return detail::unsupported_json_content_type_problem();
				}
				auto const limit = *max_body_size != 0 ? *max_body_size : json_options->max_body_size;
				if (limit != 0 && req.body.size() > limit) {
					return detail::json_body_too_large_problem();
				}
				auto const &effective_decode_opts = decode_opts ? *decode_opts : json_options->decode;
				auto decoded = conflux::json::boundary::decode_with<codec::json::DefaultJsonProvider, BodyValue>(
					req.body,
					effective_decode_opts);
				if (!decoded) {
					return detail::json_decode_problem(decoded.error());
				}
				auto body = Json<BodyValue>{std::move(*decoded)};
				return apply_route_timeout(
					invoke_json_extracted<Args>(
						*states,
						fn,
						req,
						body,
						std::make_index_sequence<std::tuple_size_v<Args>>{},
						*json_options),
					*timeout);
			});
		return RouteRef{*this, route_metadata_.size() - 1};
	}
#endif
	[[nodiscard]] std::expected<std::unique_ptr<HttpServer>, std::string> try_server(
		AppRunOptions opts = {}) && {
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
	[[nodiscard]] std::expected<std::unique_ptr<HttpServer>, std::string> listen(
		AppRunOptions opts = {}) && {
		return std::move(*this).try_server(opts);
	}
	[[nodiscard]] std::expected<RunStatus, std::string> try_run(
		AppRunOptions opts = {}) && {
		auto srv = std::move(*this).try_server(opts);
		if (!srv) {
			return std::unexpected{std::move(srv.error())};
		}
		return (*srv)->run();
	}
	[[nodiscard]] RunStatus run(
		AppRunOptions opts = {}) && noexcept {
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

private:
	Config cfg_;
	Router router_;
	std::shared_ptr<StateMap> states_;
	std::vector<std::string> state_issues_;
	std::vector<AppRouteMetadata> route_metadata_;
	std::vector<StaticMountMetadata> static_mounts_;
	std::size_t middleware_count_{};
	ScopedMiddlewareList *group_middlewares_{};
	ScopedContextMiddlewareList *group_context_middlewares_{};
	bool openapi_strict_{};
	std::optional<ObservabilityMiddleware> observability_{};
#if CONFLUX_HAS_JSON
	std::shared_ptr<AppJsonOptions> json_options_;
#endif
};

[[nodiscard]] Router &router(
	App &app) noexcept {
	return app.router_;
}

[[nodiscard]] Router const &router(
	App const &app) noexcept {
	return app.router_;
}

[[nodiscard]] std::vector<RouteInfo> route_infos(
	App const &app) {
	return router(app).route_infos();
}

[[nodiscard]] App app(
	Config cfg = Config::public_server()) {
	return App{std::move(cfg)};
}

[[nodiscard]] RunStatus run(
	App app,
	AppRunOptions opts = {}) noexcept {
	return std::move(app).run(opts);
}

[[nodiscard]] constexpr int exit_code(
	RunStatus status) noexcept {
	return static_cast<int>(status);
}

[[nodiscard]] int run_main(
	App app,
	AppRunOptions opts = {}) noexcept {
	return exit_code(run(std::move(app), opts));
}

[[nodiscard]] int run_main(
	std::expected<RunStatus, std::string> result) noexcept {
	if (!result) {
		eprintln("http app run failed:");
		eprintln(result.error());
		return exit_code(RunStatus::fatal_internal_exception);
	}
	return exit_code(*result);
}

} // namespace conflux::http
