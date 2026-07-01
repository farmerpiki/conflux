module;
#include <memory>
#include <mutex>

export module conflux.net.app;

import conflux.net.app.response;
import conflux.net.app.types;
import std;
import conflux.net.app.extractor_helpers;
import conflux.net.app.json_helpers;
import conflux.net.app.metadata_helpers;
import conflux.net.app.openapi;
import conflux.net.app.policies;
import conflux.net.app.route_helpers;
import conflux.net.app.traits;
import conflux.types;
import conflux.net.config;
import conflux.net.auth;
import conflux.net.http.types;
import conflux.net.http.parse_helpers;
import conflux.net.http.server_types;
import conflux.net.http_server;
import conflux.net.path;
import conflux.net.router;
import conflux.net.observability;
import conflux.net.request_id;
import conflux.net.tracing;
#if !defined(CONFLUX_INTERFACE_HEADER)
import conflux.uring;
#endif
import conflux.crypto;
#if CONFLUX_HAS_JSON
import conflux.json;
import conflux.net.http.native_json;
#endif
import conflux.work.root;
export namespace conflux::http {

class App;
conflux::http::Router &router(App &app) noexcept;
conflux::http::Router const &router(App const &app) noexcept;
std::vector<conflux::http::RouteInfo> route_infos(App const &app);

namespace detail {

template<class T, class... Args>
[[nodiscard]] std::shared_ptr<T> shared_new(
	Args &&...args) {
	return std::shared_ptr<T>{new T(std::forward<Args>(args)...)};
}

template<class T>
[[nodiscard]] std::string duplicate_state_message() {
	std::string out{"duplicate app state: "};
	out += typeid(T).name();
	return out;
}

template<class T>
struct IsTaskResult : std::false_type {};

template<class T>
struct IsTaskResult<conflux::work::root::Task<T>> : std::true_type {};

template<class T>
inline constexpr bool IsTaskResultV = IsTaskResult<std::remove_cvref_t<T>>::value;

template<class T>
[[nodiscard]] decltype(auto) move_if_move_only(
	T &value) noexcept {
	if constexpr (std::copy_constructible<std::remove_cvref_t<T>>) {
		return (value);
	} else {
		return std::move(value);
	}
}

template<class Tuple>
struct TupleAllCopyConstructible;

template<class... Args>
struct TupleAllCopyConstructible<std::tuple<Args...>>
	: std::bool_constant<(std::copy_constructible<std::remove_cvref_t<Args>> && ...)> {};

template<class Tuple>
inline constexpr bool TupleAllCopyConstructibleV = TupleAllCopyConstructible<std::remove_cvref_t<Tuple>>::value;

struct AppRouteVerbAccessors {
	template<typename F>
	auto get(
		this auto &self,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_method_route<HttpMethod::get>(path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	auto get(
		this auto &self,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_fixed_method_route<HttpMethod::get, Path>(std::forward<F>(handler), loc);
	}
	template<typename F>
	auto post(
		this auto &self,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_method_route<HttpMethod::post>(path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	auto post(
		this auto &self,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_fixed_method_route<HttpMethod::post, Path>(std::forward<F>(handler), loc);
	}
	template<typename F>
	auto put(
		this auto &self,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_method_route<HttpMethod::put>(path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	auto put(
		this auto &self,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_fixed_method_route<HttpMethod::put, Path>(std::forward<F>(handler), loc);
	}
	template<typename F>
	auto patch(
		this auto &self,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_method_route<HttpMethod::patch>(path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	auto patch(
		this auto &self,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_fixed_method_route<HttpMethod::patch, Path>(std::forward<F>(handler), loc);
	}
	template<typename F>
	auto del(
		this auto &self,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_method_route<HttpMethod::delete_>(path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	auto del(
		this auto &self,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_fixed_method_route<HttpMethod::delete_, Path>(std::forward<F>(handler), loc);
	}
	template<typename F>
	auto options(
		this auto &self,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_method_route<HttpMethod::options>(path, std::forward<F>(handler), loc);
	}
	template<FixedString Path, typename F>
	auto options(
		this auto &self,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return self.template add_fixed_method_route<HttpMethod::options, Path>(std::forward<F>(handler), loc);
	}
};

} // namespace detail

class App : public detail::AppRouteVerbAccessors {
	using StateMap = std::unordered_map<std::type_index, std::shared_ptr<void>>;
	using ScopedMiddlewareList = std::vector<conflux::http::Router::Middleware>;
	using ScopedContextMiddlewareList = std::vector<conflux::http::Router::ContextMiddleware>;

	struct AppRouteMetadata {
		std::string method{};
		std::string path{};
		std::string name{};
		std::string handler_kind{};
		std::string source_file{};
		std::uint_least32_t source_line{};
		std::vector<std::string> extractors{};
		std::vector<std::string> path_extractors{};
		std::vector<std::pair<std::string, std::string>> path_extractor_types{};
		std::vector<std::pair<std::size_t, std::string>> path_index_extractor_types{};
		std::vector<std::string> path_params{};
		std::vector<std::pair<std::string, std::string>> path_param_types{};
		std::vector<std::type_index> required_states{};
		std::vector<std::string> consumes{};
		std::vector<std::string> produces{};
		std::string request_body_schema{};
		std::string response_schema{};
		int success_status{kHttpOk};
		bool problem_response{};
		std::shared_ptr<std::size_t> max_body_size = detail::shared_new<std::size_t>(std::size_t{0});
		std::shared_ptr<detail::AppRouteRateLimit> rate_limit = detail::shared_new<detail::AppRouteRateLimit>();
		std::shared_ptr<std::chrono::milliseconds> timeout = detail::shared_new<std::chrono::milliseconds>();
		std::size_t middleware_count{};
		std::shared_ptr<std::string> bearer_token_policy = detail::shared_new<std::string>();
		std::string openapi_auth_scheme{};
		std::string openapi_summary{};
		std::string openapi_description{};
		std::vector<std::string> openapi_tags{};
		bool uses_body{};
		bool allow_get_body{};
		BodyMode body_mode{BodyMode::none};
	};

	struct CapturedRoutePolicy {
		std::shared_ptr<std::string> bearer_token_policy;
		std::shared_ptr<detail::AppRouteRateLimit> rate_limit;
		std::shared_ptr<std::chrono::milliseconds> timeout;
		std::shared_ptr<ScopedMiddlewareList const> middlewares;
		std::shared_ptr<ScopedContextMiddlewareList const> context_middlewares;
		std::shared_ptr<std::size_t> max_body_size;
		std::size_t app_max_body_size{};
#if CONFLUX_HAS_JSON
		std::shared_ptr<AppJsonOptions> json_options;
#endif
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
		return detail::shared_new<ScopedMiddlewareList>(*group_middlewares_);
	}

	[[nodiscard]] std::shared_ptr<ScopedContextMiddlewareList const> current_group_context_middlewares() const {
		if (group_context_middlewares_ == nullptr || group_context_middlewares_->empty()) {
			return {};
		}
		return detail::shared_new<ScopedContextMiddlewareList>(*group_context_middlewares_);
	}

	[[nodiscard]] CapturedRoutePolicy capture_route_policy() const {
		auto const &route = route_metadata_.back();
		return CapturedRoutePolicy{
			.bearer_token_policy = route.bearer_token_policy,
			.rate_limit = route.rate_limit,
			.timeout = route.timeout,
			.middlewares = current_group_middlewares(),
			.context_middlewares = current_group_context_middlewares(),
			.max_body_size = route.max_body_size,
			.app_max_body_size = cfg_.max_body_size
#if CONFLUX_HAS_JSON
			,
			.json_options = json_options_
#endif
		};
	}

	[[nodiscard]] static Response run_scoped_middlewares(
		std::shared_ptr<ScopedMiddlewareList const> const &middlewares,
		conflux::http::RequestView const &req,
		conflux::http::Router::Handler inner) {
		if (!middlewares || middlewares->empty()) {
			return inner(req);
		}
		for (auto it = middlewares->rbegin(); it != middlewares->rend(); ++it) {
			auto mw = *it;
			auto next = std::move(inner);
			inner = [mw = std::move(mw), next = std::move(next)](conflux::http::RequestView const &r) mutable {
				return mw(r, next);
			};
		}
		return inner(req);
	}

	[[nodiscard]] static conflux::work::root::Task<Response> run_scoped_context_middlewares(
		std::shared_ptr<ScopedContextMiddlewareList const> middlewares,
		conflux::http::RequestView req,
		RequestContext const &ctx,
		conflux::http::Router::ContextHandler inner);
	[[nodiscard]] static conflux::work::root::Task<Response> run_scoped_sync_route_as_context(
		std::shared_ptr<ScopedContextMiddlewareList const> context_middlewares,
		std::shared_ptr<ScopedMiddlewareList const> middlewares,
		conflux::http::RequestView req,
		RequestContext const &ctx,
		conflux::http::Router::Handler inner);
	[[nodiscard]] static conflux::work::root::Task<Response> run_owned_scoped_context_route(
		conflux::http::Router::ContextHandler inner,
		conflux::http::OwnedRequest req,
		RequestContext ctx);
	[[nodiscard]] static conflux::work::root::Task<Response> run_scoped_context_route(
		std::shared_ptr<ScopedMiddlewareList const> middlewares,
		std::shared_ptr<ScopedContextMiddlewareList const> context_middlewares,
		conflux::http::RequestView req,
		RequestContext const &ctx,
		conflux::http::Router::ContextHandler inner);

	[[nodiscard]] static std::optional<Response> route_prelude_failure(
		CapturedRoutePolicy const &policy,
		conflux::http::RequestView const &req,
		RequestContext const *ctx = nullptr) {
		auto const limit = route_body_limit(policy);
		if (ctx != nullptr && ctx->upload_body) {
			ctx->upload_body->set_body_limit(limit);
		}
		auto fail = [ctx](Response response) -> std::optional<Response> {
			if (ctx != nullptr && ctx->upload_body) {
				ctx->upload_body->mark_prelude_rejected();
			}
			return std::optional<Response>{std::move(response)};
		};
		if (auto denied = detail::route_auth_failure(*policy.bearer_token_policy, req)) {
			return fail(*std::move(denied));
		}
		if (auto limited = detail::route_rate_limit_failure(*policy.rate_limit, req)) {
			return fail(*std::move(limited));
		}
		if (limit != 0 && ctx != nullptr && ctx->upload_body) {
			if (auto content_length = req.header("content-length"); !content_length.empty()) {
				auto parsed = parse_content_length_limited(content_length, limit);
				if (!parsed) {
					if (parsed.error() == ContentLengthParseError::malformed) {
						return fail(Response::bad_request("Content-Length is not a valid decimal length"));
					}
#if CONFLUX_HAS_JSON
					return fail(detail::json_body_too_large_problem());
#else
					return fail(Response::content_too_large());
#endif
				}
			}
		}
		if (limit != 0 && req.body.size() > limit) {
#if CONFLUX_HAS_JSON
			return fail(detail::json_body_too_large_problem());
#else
			return fail(Response::content_too_large());
#endif
		}
		return std::nullopt;
	}

	[[nodiscard]] static std::size_t route_body_limit(
		CapturedRoutePolicy const &policy) {
		return effective_body_limit(
			*policy.max_body_size,
			policy.app_max_body_size,
#if CONFLUX_HAS_JSON
			policy.json_options->max_body_size);
#else
			0);
#endif
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
			policy.buckets.emplace(std::max<std::size_t>(policy.options.max_clients, 1));
			return *this;
		}

		RouteRef &require_bearer_token(
			std::string_view value) {
			*metadata().bearer_token_policy = std::string{value};
			return *this;
		}

		RouteRef &openapi_summary(
			std::string_view value) {
			metadata().openapi_summary = std::string{value};
			return *this;
		}

		RouteRef &openapi_description(
			std::string_view value) {
			metadata().openapi_description = std::string{value};
			return *this;
		}

		RouteRef &openapi_tags(
			std::initializer_list<std::string_view> tags) {
			metadata().openapi_tags.clear();
			for (auto t: tags) {
				metadata().openapi_tags.emplace_back(t);
			}
			return *this;
		}

	private:
		[[nodiscard]] AppRouteMetadata &metadata() const { return app_->route_metadata_.at(index_); }

		App *app_;
		std::size_t index_;
	};

	[[nodiscard]] static App default_server() { return App{conflux::http::Config::public_server()}; }
	explicit App(
		conflux::http::Config cfg = conflux::http::Config::public_server())
		: cfg_(std::move(cfg))
		, router_(cfg_)
		, states_(detail::shared_new<StateMap>())
#if CONFLUX_HAS_JSON
		, json_options_(detail::shared_new<AppJsonOptions>())
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
			if (detail::validate_path_pattern(path)) {
				return *this;
			}
			auto policy = capture_route_policy();
#if CONFLUX_HAS_JSON
			auto json_options = json_options_;
#endif
			auto make_inner = [policy,
#if CONFLUX_HAS_JSON
							   json_options,
#endif
							   fn = std::decay_t<F>(std::forward<F>(handler))]() mutable {
				return conflux::http::Router::Handler{[policy,
#if CONFLUX_HAS_JSON
													   json_options,
#endif
													   &fn](conflux::http::RequestView const &inner_req) mutable {
					if (auto denied = detail::route_auth_failure(*policy.bearer_token_policy, inner_req)) {
						return *std::move(denied);
					}
					if (auto limited = detail::route_rate_limit_failure(*policy.rate_limit, inner_req)) {
						return *std::move(limited);
					}
					return apply_route_timeout(
#if CONFLUX_HAS_JSON
						into_app_response(fn(), *json_options),
#else
						into_app_response(fn()),
#endif
						*policy.timeout);
				}};
			};
			if (policy.context_middlewares && !policy.context_middlewares->empty()) {
				router_.add_context_with_timeout(
					method,
					path,
					policy.timeout,
					[policy, make_inner = std::move(make_inner)](
						conflux::http::RequestView const &req,
						RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
						return run_scoped_sync_route_as_context(
							policy.context_middlewares,
							policy.middlewares,
							req,
							ctx,
							make_inner());
					});
			} else {
				router_.add(
					method,
					path,
					[policy, make_inner = std::move(make_inner)](conflux::http::RequestView const &req) mutable {
						return run_scoped_middlewares(policy.middlewares, req, make_inner());
					});
			}
		} else if constexpr (requires(Fn &fn, conflux::http::RequestView const &req) {
								 { into_response(fn(req)) } -> std::same_as<Response>;
							 }) {
			record_route_metadata<std::tuple<conflux::http::RequestView>>(method, path, "app", loc);
			record_return_metadata<std::invoke_result_t<Fn &, conflux::http::RequestView const &>>();
			auto policy = capture_route_policy();
#if CONFLUX_HAS_JSON
			auto json_options = json_options_;
#endif
			auto make_inner = [policy,
#if CONFLUX_HAS_JSON
							   json_options,
#endif
							   fn = Fn(std::forward<F>(handler))]() mutable {
				return conflux::http::Router::Handler{[policy,
#if CONFLUX_HAS_JSON
													   json_options,
#endif
													   &fn](conflux::http::RequestView const &inner_req) mutable {
					if (auto denied = detail::route_auth_failure(*policy.bearer_token_policy, inner_req)) {
						return *std::move(denied);
					}
					if (auto limited = detail::route_rate_limit_failure(*policy.rate_limit, inner_req)) {
						return *std::move(limited);
					}
					return apply_route_timeout(
#if CONFLUX_HAS_JSON
						into_app_response(fn(inner_req), *json_options),
#else
						into_app_response(fn(inner_req)),
#endif
						*policy.timeout);
				}};
			};
			if (policy.context_middlewares && !policy.context_middlewares->empty()) {
				router_.add_context_with_timeout(
					method,
					path,
					policy.timeout,
					[policy, make_inner = std::move(make_inner)](
						conflux::http::RequestView const &req,
						RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
						return run_scoped_sync_route_as_context(
							policy.context_middlewares,
							policy.middlewares,
							req,
							ctx,
							make_inner());
					});
			} else {
				router_.add(
					method,
					path,
					[policy, make_inner = std::move(make_inner)](conflux::http::RequestView const &req) mutable {
						return run_scoped_middlewares(policy.middlewares, req, make_inner());
					});
			}
		} else if constexpr (requires(Fn &fn, conflux::http::OwnedRequest const &req) {
								 { into_response(fn(req)) } -> std::same_as<Response>;
							 }) {
			record_route_metadata<std::tuple<conflux::http::OwnedRequest>>(method, path, "app", loc);
			record_return_metadata<std::invoke_result_t<Fn &, conflux::http::OwnedRequest const &>>();
			auto policy = capture_route_policy();
#if CONFLUX_HAS_JSON
			auto json_options = json_options_;
#endif
			auto make_inner = [policy,
#if CONFLUX_HAS_JSON
							   json_options,
#endif
							   fn = Fn(std::forward<F>(handler))]() mutable {
				return conflux::http::Router::Handler{[policy,
#if CONFLUX_HAS_JSON
													   json_options,
#endif
													   &fn](conflux::http::RequestView const &inner_req) mutable {
					if (auto denied = detail::route_auth_failure(*policy.bearer_token_policy, inner_req)) {
						return *std::move(denied);
					}
					if (auto limited = detail::route_rate_limit_failure(*policy.rate_limit, inner_req)) {
						return *std::move(limited);
					}
					auto owned = inner_req.to_owned();
					return apply_route_timeout(
#if CONFLUX_HAS_JSON
						into_app_response(fn(owned), *json_options),
#else
						into_app_response(fn(owned)),
#endif
						*policy.timeout);
				}};
			};
			if (policy.context_middlewares && !policy.context_middlewares->empty()) {
				router_.add_context_with_timeout(
					method,
					path,
					policy.timeout,
					[policy, make_inner = std::move(make_inner)](
						conflux::http::RequestView const &req,
						RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
						return run_scoped_sync_route_as_context(
							policy.context_middlewares,
							policy.middlewares,
							req,
							ctx,
							make_inner());
					});
			} else {
				router_.add(
					method,
					path,
					[policy, make_inner = std::move(make_inner)](conflux::http::RequestView const &req) mutable {
						return run_scoped_middlewares(policy.middlewares, req, make_inner());
					});
			}
		} else if constexpr (requires(Fn &fn, conflux::http::RequestView const &req) {
								 { fn(req) } -> std::same_as<conflux::work::root::Task<Response>>;
							 }) {
			record_route_metadata<std::tuple<conflux::http::RequestView>>(method, path, "app", loc);
			record_return_metadata<std::invoke_result_t<Fn &, conflux::http::RequestView const &>>();
			auto policy = capture_route_policy();
			router_.add_context_with_timeout(
				method,
				path,
				policy.timeout,
				[policy, fn = Fn(std::forward<F>(handler))](
					conflux::http::RequestView const &req,
					RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
					conflux::http::Router::ContextHandler inner =
						[policy, &fn](
							conflux::http::RequestView const &inner_req,
							RequestContext const &inner_ctx) mutable -> conflux::work::root::Task<Response> {
						if (auto failed = route_prelude_failure(policy, inner_req, &inner_ctx)) {
							co_return *std::move(failed);
						}
						co_return co_await fn(inner_req);
					};
					co_return co_await run_scoped_context_route(
						policy.middlewares,
						policy.context_middlewares,
						req,
						ctx,
						std::move(inner));
				});
		} else if constexpr (requires(Fn &fn, conflux::http::OwnedRequest const &req) {
								 { fn(req) } -> std::same_as<conflux::work::root::Task<Response>>;
							 }) {
			record_route_metadata<std::tuple<conflux::http::OwnedRequest>>(method, path, "app", loc);
			record_return_metadata<std::invoke_result_t<Fn &, conflux::http::OwnedRequest const &>>();
			auto policy = capture_route_policy();
			router_.add_context_with_timeout(
				method,
				path,
				policy.timeout,
				[policy, fn = Fn(std::forward<F>(handler))](
					conflux::http::RequestView const &req,
					RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
					conflux::http::Router::ContextHandler inner =
						[policy, &fn](
							conflux::http::RequestView const &inner_req,
							RequestContext const &inner_ctx) mutable -> conflux::work::root::Task<Response> {
						if (auto failed = route_prelude_failure(policy, inner_req, &inner_ctx)) {
							co_return *std::move(failed);
						}
						auto owned = inner_req.to_owned();
						co_return co_await fn(owned);
					};
					co_return co_await run_scoped_context_route(
						policy.middlewares,
						policy.context_middlewares,
						req,
						ctx,
						std::move(inner));
				});
		} else if constexpr (ContextHandlerFunction<Fn>) {
			add_context_route(method, path, std::forward<F>(handler), loc);
		} else {
			router_.add(method, path, std::forward<F>(handler));
		}
		return *this;
	}
	template<typename F>
	App &add(
		HttpMethod method,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add(http_method_name(method), path, std::forward<F>(handler), loc);
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
	[[nodiscard]] RouteRef route(
		HttpMethod method,
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
			record_route_metadata<std::tuple<conflux::http::RequestView>>(method, path, "raw", loc);
		}
		return RouteRef{*this, route_metadata_.size() - 1};
	}
	template<typename F>
	[[nodiscard]] RouteRef add_route_ref(
		HttpMethod method,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		auto const before = route_metadata_.size();
		add(method, path, std::forward<F>(handler), loc);
		if (route_metadata_.size() == before) {
			record_route_metadata<std::tuple<conflux::http::RequestView>>(http_method_name(method), path, "raw", loc);
		}
		return RouteRef{*this, route_metadata_.size() - 1};
	}
	template<HttpMethod Method, typename F>
	RouteRef add_method_route(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_route_ref(Method, path, std::forward<F>(handler), loc);
	}
	template<HttpMethod Method, FixedString Path, typename F>
	RouteRef add_fixed_method_route(
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_fixed_route<Path>(http_method_name(Method), std::forward<F>(handler), loc);
	}
#if CONFLUX_HAS_JSON
	template<HttpMethod Method, class Body, typename F>
	RouteRef add_method_json_body(
		std::string_view path,
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return add_json_body<Body>(
			http_method_name(Method),
			path,
			std::forward<F>(handler),
			std::move(decode_opts),
			loc);
	}
	template<HttpMethod Method, FixedString Path, class Body, typename F>
	RouteRef add_fixed_method_json_body(
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return add_method_json_body<Method, Body>(Path.view(), std::forward<F>(handler), std::move(decode_opts), loc);
	}
#endif
	template<HttpMethod Method, typename F>
		requires ContextHandlerFunction<F>
	App &add_method_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_context(Method, path, std::forward<F>(handler), loc);
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
		return add_method_json_body<HttpMethod::post, Body>(
			path,
			std::forward<F>(handler),
			std::move(decode_opts),
			loc);
	}
	template<FixedString Path, class Body, typename F>
	RouteRef post_body(
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return add_fixed_method_json_body<HttpMethod::post, Path, Body>(
			std::forward<F>(handler),
			std::move(decode_opts),
			loc);
	}
#endif
#if CONFLUX_HAS_JSON
	template<class Body, typename F>
	RouteRef put_body(
		std::string_view path,
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return add_method_json_body<HttpMethod::put, Body>(path, std::forward<F>(handler), std::move(decode_opts), loc);
	}
	template<FixedString Path, class Body, typename F>
	RouteRef put_body(
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return add_fixed_method_json_body<HttpMethod::put, Path, Body>(
			std::forward<F>(handler),
			std::move(decode_opts),
			loc);
	}
#endif
#if CONFLUX_HAS_JSON
	template<class Body, typename F>
	RouteRef patch_body(
		std::string_view path,
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return add_method_json_body<HttpMethod::patch, Body>(
			path,
			std::forward<F>(handler),
			std::move(decode_opts),
			loc);
	}
	template<FixedString Path, class Body, typename F>
	RouteRef patch_body(
		F &&handler,
		std::optional<conflux::json::boundary::DecodeOptions> decode_opts = std::nullopt,
		std::source_location loc = std::source_location::current()) {
		return add_fixed_method_json_body<HttpMethod::patch, Path, Body>(
			std::forward<F>(handler),
			std::move(decode_opts),
			loc);
	}
#endif
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
	App &add_context(
		HttpMethod method,
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_context(http_method_name(method), path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &get_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_method_context<HttpMethod::get>(path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &post_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_method_context<HttpMethod::post>(path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &put_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_method_context<HttpMethod::put>(path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &patch_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_method_context<HttpMethod::patch>(path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &del_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_method_context<HttpMethod::delete_>(path, std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &options_context(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		return add_method_context<HttpMethod::options>(path, std::forward<F>(handler), loc);
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
			router_.use(conflux::http::request_id_middleware());
			++middleware_count_;
		}
		if (middleware.options.trace_context) {
			router_.use(conflux::http::tracing_middleware());
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
		record_route_metadata<std::tuple<conflux::http::RequestView>>(
			http_method_name(HttpMethod::get),
			path,
			"sse",
			loc);
		record_return_metadata<Response>();
		router_.sse(path, std::forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &ws(
		std::string_view path,
		F &&handler,
		std::source_location loc = std::source_location::current()) {
		record_route_metadata<std::tuple<conflux::http::RequestView>>(
			http_method_name(HttpMethod::get),
			path,
			"ws",
			loc);
		record_return_metadata<Response>();
		router_.ws(path, std::forward<F>(handler));
		return *this;
	}
	App &serve_static(
		std::string_view url_prefix,
		std::string root_dir,
		conflux::http::StaticOptions const &sopts = {},
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
			state_issues_.push_back(detail::duplicate_state_message<T>());
		}
		(*states_)[key] = std::shared_ptr<void>{std::addressof(value), [](void *) {}};
		return *this;
	}
	template<class T>
	App &state(
		std::shared_ptr<T> value) {
		auto const key = std::type_index{typeid(T)};
		if (states_->contains(key)) {
			state_issues_.push_back(detail::duplicate_state_message<T>());
		}
		auto shared_value = std::move(value);
		(*states_)[key] = shared_value;
		using SharedState = std::shared_ptr<T>;
		auto const shared_key = std::type_index{typeid(SharedState)};
		if (states_->contains(shared_key)) {
			state_issues_.push_back(detail::duplicate_state_message<SharedState>());
		}
		(*states_)[shared_key] = detail::shared_new<SharedState>(std::move(shared_value));
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
		return state_shared(detail::shared_new<T>(std::move(value)));
	}
	template<class T>
	[[nodiscard]] State<T> state() const {
		auto const it = states_->find(std::type_index{typeid(T)});
		if (it == states_->end()) {
			return {};
		}
		return State<T>{.value = static_cast<T *>(it->second.get())};
	}

	class Group : public detail::AppRouteVerbAccessors {
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
			return add_scoped_route(
				[&] { return app_.add_route_ref(method, full_path(path), std::forward<F>(handler), loc); });
		}
		template<typename F>
		[[nodiscard]] RouteRef add(
			HttpMethod method,
			std::string_view path,
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return add_scoped_route(
				[&] { return app_.add_route_ref(method, full_path(path), std::forward<F>(handler), loc); });
		}
		template<typename F>
		[[nodiscard]] RouteRef add_scoped_route(
			F &&fn) {
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
			auto route = std::forward<F>(fn)();
			apply_policies(route);
			return route;
		}
		template<HttpMethod Method, typename F>
		[[nodiscard]] RouteRef add_method_route(
			std::string_view path,
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return add(Method, path, std::forward<F>(handler), loc);
		}
		template<HttpMethod Method, FixedString Path, typename F>
		[[nodiscard]] RouteRef add_fixed_method_route(
			F &&handler,
			std::source_location loc = std::source_location::current()) {
			return add_method_route<Method>(Path.view(), std::forward<F>(handler), loc);
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
		Group &require_bearer_token(
			std::string_view value) {
			bearer_token_policy_ = std::string{value};
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
			, bearer_token_policy_(parent.bearer_token_policy_) {}

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
			if (bearer_token_policy_) {
				route.require_bearer_token(*bearer_token_policy_);
			}
		}

		App &app_;
		std::string prefix_;
		ScopedMiddlewareList middlewares_;
		ScopedContextMiddlewareList context_middlewares_;
		std::optional<std::size_t> max_body_size_;
		std::optional<std::chrono::milliseconds> timeout_;
		std::optional<GroupRateLimit> rate_limit_;
		std::optional<std::string> bearer_token_policy_;
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
	[[nodiscard]] conflux::http::Config &config() { return cfg_; }
	[[nodiscard]] conflux::http::Config const &config() const { return cfg_; }
	friend conflux::http::Router &router(App &app) noexcept;
	friend conflux::http::Router const &router(App const &app) noexcept;
	friend std::vector<conflux::http::RouteInfo> route_infos(App const &app);
	[[nodiscard]] std::vector<AppRouteInfo> routes() const;
	[[nodiscard]] std::vector<AppStaticMountInfo> static_mounts() const;
	[[nodiscard]] std::string route_table() const;
	[[nodiscard]] std::string openapi_spec(
		std::string_view title = "API",
		std::string_view version = "1.0.0",
		detail::OpenApiAppInfo app_info = {}) const;
	[[nodiscard]] RouteRef openapi(
		std::string_view path = "/openapi.json",
		std::string_view title = "API",
		std::string_view version = "1.0.0",
		detail::OpenApiAppInfo app_info = {},
		std::source_location loc = std::source_location::current());
	[[nodiscard]] ValidationReport validate() const;
	void validate_app_state(ValidationReport &report) const;
	void validate_runtime_config(ValidationReport &report) const;
	void validate_route_patterns_and_uniqueness(ValidationReport &report) const;
	void validate_route_extractors_and_body_policy(ValidationReport &report) const;
	void validate_static_mounts(ValidationReport &report) const;
	[[nodiscard]] bool route_has_body_limit(AppRouteMetadata const &route) const noexcept;
	void validate_openapi_completeness(AppRouteMetadata const &route, ValidationReport &report) const;
	void validate_tls_config(ValidationReport &report) const;

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
							  { codec::json::response_or_internal_error(value, opts) }->std::same_as<Response>;
						  }) {
				return codec::json::response_or_internal_error(
					result.value,
					codec::json::ResponseOptions{.dump = json_options.dump});
			} else {
				static_assert(
					kDependentFalse<Body>,
					"http::Json<T> responses require T to be serializable; add conflux::json::JsonCodec<T>, "
					"conflux::json::JsonMembers<T>, or reflection JSON support for T");
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
		record_route_metadata<std::tuple<conflux::http::RequestView>>(method, path, "context", loc);
		record_return_metadata<conflux::work::root::Task<Response>>();
		auto policy = capture_route_policy();
		router_.add_context_with_timeout(
			method,
			path,
			policy.timeout,
			[policy, fn = Fn(std::forward<F>(handler))](
				conflux::http::RequestView const &req,
				RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
				conflux::http::Router::ContextHandler inner =
					[policy, &fn](
						conflux::http::RequestView const &inner_req,
						RequestContext const &inner_ctx) -> conflux::work::root::Task<Response> {
					if (auto failed = route_prelude_failure(policy, inner_req, &inner_ctx)) {
						co_return *std::move(failed);
					}
					co_return co_await fn(inner_req, inner_ctx);
				};
				co_return co_await run_scoped_context_route(
					policy.middlewares,
					policy.context_middlewares,
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
		auto const contains_extractor = [&meta](std::string_view name) {
			for (std::size_t i = 0; i < meta.extractors.size(); ++i) {
				if (std::string_view{meta.extractors[i]} == name) {
					return true;
				}
			}
			return false;
		};
		if (contains_extractor("RequiredBearerToken")) {
			meta.openapi_auth_scheme = "bearer";
		}
		if (contains_extractor("RequiredBearerToken") && meta.bearer_token_policy->empty()) {
			*meta.bearer_token_policy = "bearer";
		}
		if (contains_extractor("RequiredBasicAuth")) {
			meta.openapi_auth_scheme = "basic";
		}
		meta.uses_body = detail::has_body_extractor<Args>() || handler_kind == "json_body";
		if (contains_extractor("UploadBody")) {
			meta.body_mode = BodyMode::streaming_raw;
			meta.consumes = {"application/octet-stream"};
		} else if (contains_extractor("Multipart")) {
			meta.body_mode = BodyMode::buffered_multipart;
		} else if (meta.uses_body || handler_kind == "json_body") {
			meta.body_mode = BodyMode::buffered_raw;
		}
		if constexpr (detail::has_body_extractor<Args>()) {
			if (contains_extractor("UploadBody")) {
				meta.consumes = {"application/octet-stream"};
			} else if (contains_extractor("JsonDocument")) {
				meta.consumes = {"application/json", "application/problem+json"};
			} else if (contains_extractor("JsonPatch")) {
				meta.consumes = {"application/json-patch+json"};
				meta.request_body_schema = R"({"type":"array","items":{"type":"object","required":["op","path"]}})";
			} else if (contains_extractor("MergePatch")) {
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
		if constexpr (std::same_as<Clean, Created> || detail::CreatedBodyArg<Clean>) {
			meta.success_status = kHttpCreated;
		}
		if constexpr (detail::CreatedBodyArg<Clean>) {
			meta.produces = {"application/json"};
#if CONFLUX_HAS_JSON
			using JsonValue = typename detail::CreatedBodyType<Clean>::type;
			meta.response_schema = detail::schema_json_or_object<JsonValue>();
#endif
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

#if CONFLUX_HAS_JSON
	template<class ContentTypePredicate>
	static void validate_json_body_request(
		conflux::http::RequestView const &req,
		ContentTypePredicate content_type_ok,
		std::size_t max_body_size) {
		if (!content_type_ok(req.header("content-type"))) {
			throw ExtractorFailure{detail::unsupported_json_content_type_problem()};
		}
		if (max_body_size != 0 && req.body.size() > max_body_size) {
			throw ExtractorFailure{detail::json_body_too_large_problem()};
		}
	}

	[[nodiscard]] static conflux::json::boundary::DecodeOptions typed_json_decode_options(
		AppJsonOptions const &json_options) noexcept {
		auto opts = json_options.decode;
		if (json_options.direct_typed_decode) {
			opts.copy_input = false;
		}
		return opts;
	}
#endif

	[[nodiscard]] static std::size_t effective_body_limit(
		std::size_t route_max_body_size,
		std::size_t app_max_body_size,
		std::size_t json_max_body_size) noexcept {
		if (route_max_body_size != 0) {
			return route_max_body_size;
		}
		if (json_max_body_size != 0) {
			return json_max_body_size;
		}
		return app_max_body_size;
	}

	template<class Clean>
	[[nodiscard]] static auto make_request_arg(
		conflux::http::RequestView const &req) {
		if constexpr (detail::RequestViewArg<Clean>) {
			return req;
		} else {
			return req.to_owned();
		}
	}

	template<class Clean>
	[[nodiscard]] static auto make_state_arg(
		StateMap const &states) {
		using StateValue = typename detail::StateType<Clean>::type;
		auto const it = states.find(std::type_index{typeid(StateValue)});
		if (it == states.end()) {
			throw ExtractorFailure{Response::internal_error("missing app state")};
		}
		return State<StateValue>{.value = static_cast<StateValue *>(it->second.get())};
	}

	template<class Clean>
	[[nodiscard]] static auto make_path_arg(
		conflux::http::RequestView const &req) {
		if constexpr (detail::PathArg<Clean>) {
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
		} else {
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
		}
	}

	template<class Clean>
	[[nodiscard]] static auto make_context_arg(
		conflux::http::RequestView const &req) {
		if constexpr (detail::MultipartArg<Clean>) {
			return Multipart{.form = req.form, .files = req.files};
		} else if constexpr (detail::RequestIdArg<Clean>) {
			return RequestId{.value = req.header("x-request-id")};
		} else if constexpr (detail::ConnectionInfoArg<Clean>) {
			return ConnectionInfo{.remote_addr = req.remote_addr, .is_tls = req.is_tls};
		} else {
			return TraceContext{.traceparent = req.header("traceparent")};
		}
	}

	template<class Clean>
	[[nodiscard]] static auto make_field_arg(
		conflux::http::RequestView const &req) {
		if constexpr (detail::QueryArg<Clean>) {
			using QueryValue = typename detail::QueryType<Clean>::type;
			if constexpr (std::same_as<QueryValue, std::string_view>) {
				if constexpr (detail::QueryType<Clean>::required) {
					return Clean{
						.value = detail::extract_or_throw(
							req.template query_as<QueryValue>(detail::QueryType<Clean>::name.view()),
							"Query")};
				} else {
					return Clean{.value = req.query_value(detail::QueryType<Clean>::name.view())};
				}
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
				if constexpr (detail::HeaderType<Clean>::required) {
					return Clean{
						.value = detail::extract_or_throw(
							req.template header_as<HeaderValue>(detail::HeaderType<Clean>::name.view()),
							"Header")};
				} else {
					return Clean{.value = req.header(detail::HeaderType<Clean>::name.view())};
				}
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
				if constexpr (detail::CookieType<Clean>::required) {
					return Clean{
						.value = detail::extract_or_throw(
							req.template cookie_as<CookieValue>(detail::CookieType<Clean>::name.view()),
							"Cookie")};
				} else {
					return Clean{.value = req.cookie(detail::CookieType<Clean>::name.view())};
				}
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
				if constexpr (detail::FormType<Clean>::required) {
					return Clean{
						.value = detail::extract_or_throw(
							req.template form_as<FormValue>(detail::FormType<Clean>::name.view()),
							"Form")};
				} else {
					return Clean{.value = req.form_value(detail::FormType<Clean>::name.view())};
				}
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
		} else {
			using FormValue = typename detail::FormParamsType<Clean>::type;
			return Clean{.value = detail::extract_form_params<FormValue>(req)};
#endif
		}
	}

	template<class Clean>
	[[nodiscard]] static auto make_body_arg(
		conflux::http::RequestView const &req
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		std::size_t max_body_size
#endif
	) {
		if constexpr (detail::BodyTextArg<Clean>) {
			return BodyText{.value = req.body};
		} else if constexpr (detail::BodyBytesArg<Clean>) {
			return BodyBytes{.value = std::as_bytes(std::span{req.body.data(), req.body.size()})};
		} else if constexpr (detail::OwnedBodyBytesArg<Clean>) {
			return OwnedBodyBytes{.value = std::string{req.body}};
#if CONFLUX_HAS_JSON
		} else if constexpr (detail::JsonDocumentArg<Clean>) {
			validate_json_body_request(req, detail::content_type_is_json_request, max_body_size);
			auto parsed = codec::json::DefaultJsonProvider::parse_json_document(req.body, json_options.decode);
			if (!parsed) {
				throw ExtractorFailure{detail::json_decode_problem(parsed.error())};
			}
			return JsonDocument{.value = std::move(*parsed)};
		} else if constexpr (detail::JsonPatchArg<Clean>) {
			validate_json_body_request(req, detail::content_type_is_json_patch, max_body_size);
			auto parsed = codec::json::DefaultJsonProvider::parse_json_document(req.body, json_options.decode);
			if (!parsed) {
				throw ExtractorFailure{detail::json_decode_problem(parsed.error())};
			}
			if (auto ok = conflux::json::validate_patch(parsed->root()); !ok) {
				throw ExtractorFailure{detail::json_patch_problem(ok.error())};
			}
			return JsonPatch{.value = std::move(*parsed)};
		} else if constexpr (detail::MergePatchArg<Clean>) {
			validate_json_body_request(req, detail::content_type_is_merge_patch, max_body_size);
			auto parsed = codec::json::DefaultJsonProvider::parse_json_document(req.body, json_options.decode);
			if (!parsed) {
				throw ExtractorFailure{detail::json_decode_problem(parsed.error())};
			}
			return MergePatch{.value = std::move(*parsed)};
		} else {
			using BodyValue = typename detail::JsonType<Clean>::type;
			validate_json_body_request(req, detail::content_type_is_json_request, max_body_size);
			auto decode_opts = typed_json_decode_options(json_options);
			auto decoded = conflux::json::boundary::decode_with<codec::json::DefaultJsonProvider, BodyValue>(
				req.body,
				decode_opts);
			if (!decoded) {
				throw ExtractorFailure{detail::json_decode_problem(decoded.error())};
			}
			return Json<BodyValue>{std::move(*decoded)};
#endif
		}
	}

	template<class Clean>
	[[nodiscard]] static auto make_auth_arg(
		conflux::http::RequestView const &req) {
		if constexpr (detail::BearerArg<Clean>) {
			auto token = credentials_for_auth_scheme(req.header("authorization"), "Bearer");
			return Clean{.token = token.value_or(std::string_view{})};
		} else if constexpr (detail::RequiredBearerArg<Clean>) {
			auto token = credentials_for_auth_scheme(req.header("authorization"), "Bearer");
			if (!token || token->empty()) {
				throw ExtractorFailure{Response::unauthorized("Bearer")};
			}
			return Clean{.token = *token};
		} else if constexpr (detail::OptionalBearerArg<Clean>) {
			auto token = credentials_for_auth_scheme(req.header("authorization"), "Bearer");
			return Clean{.token = token && !token->empty() ? std::optional<std::string_view>{*token} : std::nullopt};
		} else if constexpr (detail::BasicAuthArg<Clean>) {
			return detail::parse_basic_auth(req).value_or(BasicAuth{});
		} else if constexpr (detail::RequiredBasicAuthArg<Clean>) {
			auto credentials = detail::parse_basic_auth(req);
			if (!credentials) {
				throw ExtractorFailure{Response::unauthorized("Basic")};
			}
			return RequiredBasicAuth{.credentials = std::move(*credentials)};
		} else {
			return OptionalBasicAuth{.credentials = detail::parse_basic_auth(req)};
		}
	}

	template<class Arg>
	[[nodiscard]] static auto make_handler_arg(
		StateMap const &states,
		conflux::http::RequestView const &req,
		RequestContext const *ctx
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		std::size_t max_body_size
#endif
	) {
		using Clean = std::remove_cvref_t<Arg>;
		if constexpr (detail::RequestViewArg<Clean> || detail::RequestArg<Clean>) {
			return make_request_arg<Clean>(req);
		} else if constexpr (detail::StateArg<Clean>) {
			return make_state_arg<Clean>(states);
		} else if constexpr (detail::PathArg<Clean> || detail::PathAtArg<Clean>) {
			return make_path_arg<Clean>(req);
		} else if constexpr (
			detail::QueryArg<Clean>
			|| detail::HeaderArg<Clean>
			|| detail::CookieArg<Clean>
			|| detail::FormArg<Clean>
#if CONFLUX_HAS_JSON
			|| detail::QueryParamsArg<Clean>
			|| detail::FormParamsArg<Clean>
#endif
		) {
			return make_field_arg<Clean>(req);
		} else if constexpr (
			detail::BodyTextArg<Clean>
			|| detail::BodyBytesArg<Clean>
			|| detail::OwnedBodyBytesArg<Clean>
#if CONFLUX_HAS_JSON
			|| detail::JsonDocumentArg<Clean>
			|| detail::JsonPatchArg<Clean>
			|| detail::MergePatchArg<Clean>
			|| detail::JsonArg<Clean>
#endif
		) {
			return make_body_arg<Clean>(
				req
#if CONFLUX_HAS_JSON
				,
				json_options,
				max_body_size
#endif
			);
		} else if constexpr (detail::UploadBodyArg<Clean>) {
			if (ctx == nullptr || !ctx->upload_body) {
				throw ExtractorFailure{Response::bad_request("upload body is not available")};
			}
			return UploadBody{ctx->upload_body};
		} else if constexpr (
			detail::MultipartArg<Clean>
			|| detail::RequestIdArg<Clean>
			|| detail::ConnectionInfoArg<Clean>
			|| detail::TraceContextArg<Clean>) {
			return make_context_arg<Clean>(req);
		} else if constexpr (
			detail::BearerArg<Clean>
			|| detail::RequiredBearerArg<Clean>
			|| detail::OptionalBearerArg<Clean>
			|| detail::BasicAuthArg<Clean>
			|| detail::RequiredBasicAuthArg<Clean>
			|| detail::OptionalBasicAuthArg<Clean>) {
			return make_auth_arg<Clean>(req);
		} else {
			static_assert(
				kDependentFalse<Arg>,
				"HTTP app handler argument must be http::RequestView, http::OwnedRequest, http::Path<...>, "
				"http::PathAt<...>, http::Query<...>, http::Header<...>, http::Cookie<...>, http::Form<...>, "
				"http::QueryParams<...>, http::FormParams<...>, http::BodyText, http::Json<T>, http::JsonDocument, "
				"http::JsonPatch, http::MergePatch, http::BodyBytes, http::OwnedBodyBytes, http::Multipart, "
				"http::RequestId, http::ConnectionInfo, http::TraceContext, http::BearerToken, "
				"http::RequiredBearerToken, http::OptionalBearerToken, http::BasicAuth, http::RequiredBasicAuth, "
				"http::OptionalBasicAuth, "
				"or http::State<T>");
		}
	}

	template<class Arg, std::size_t Index>
	[[nodiscard]] static auto make_inline_path_arg(
		conflux::http::RequestView const &req) {
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
		conflux::http::RequestView const &req,
		RequestContext const *ctx
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
				req,
				ctx
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
		conflux::http::RequestView const &req,
		std::index_sequence<Is...>
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		[[maybe_unused]] std::size_t max_body_size
#endif
	) {
		try {
			return into_app_response(
				fn(make_fixed_route_arg<Args, Is>(
					states,
					req,
					nullptr
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
	[[nodiscard]] static conflux::work::root::Task<Response> run_app_task_response(
		Fn *handler,
		ExtractedArgs extracted_args
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions json_options
#endif
	) {
		return conflux::work::root::make_cancellable_task([handler,
														   extracted_args = std::move(extracted_args)
#if CONFLUX_HAS_JSON
															   ,
														   json_options
#endif
		](conflux::work::root::Cancellation) mutable {
			return [](Fn *inner_handler,
					  ExtractedArgs inner_extracted_args
#if CONFLUX_HAS_JSON
					  ,
					  AppJsonOptions inner_json_options
#endif
					  ) -> conflux::work::root::Task<Response> {
				try {
					auto result = [&]() {
						if constexpr (detail::TupleAllCopyConstructibleV<ExtractedArgs>) {
							return std::apply(
								[inner_handler](auto &...args) { return (*inner_handler)(args...); },
								inner_extracted_args);
						} else {
							return std::apply(
								[inner_handler](auto &...args) {
									return (*inner_handler)(detail::move_if_move_only(args)...);
								},
								inner_extracted_args);
						}
					}();
#if CONFLUX_HAS_JSON
					co_return into_app_response(co_await std::move(result), inner_json_options);
#else
					co_return into_app_response(co_await std::move(result));
#endif
				} catch (ExtractorFailure &failure) { co_return std::move(failure).response(); }
			}(handler,
						std::move(extracted_args)
#if CONFLUX_HAS_JSON
							,
						json_options
#endif
						);
		});
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
#if CONFLUX_HAS_JSON
		return run_app_task_response(&handler, std::move(extracted_args), json_options);
#else
		return run_app_task_response(&handler, std::move(extracted_args));
#endif
	}

	[[nodiscard]] static conflux::work::root::Task<Response> extraction_failure_response(Response response);

	template<class Args, class Fn, std::size_t... Is>
	[[nodiscard]] static conflux::work::root::Task<Response> invoke_fixed_route_async(
		StateMap const &states,
		Fn &fn,
		conflux::http::RequestView req,
		RequestContext const &ctx,
		std::index_sequence<Is...>
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		std::size_t max_body_size
#endif
	) {
		try {
#if CONFLUX_HAS_JSON
			if constexpr (sizeof...(Is) == 0) {
				auto _ = max_body_size;
			}
#endif
			auto extracted_args = std::make_tuple(
				make_fixed_route_arg<Args, Is>(
					states,
					req,
					std::addressof(ctx)
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
		} catch (ExtractorFailure &failure) { return extraction_failure_response(std::move(failure).response()); }
	}

	template<FixedString Path, class Args, std::size_t I>
	static void record_inline_path_extractor(
		AppRouteMetadata &meta) {
		using Arg = std::tuple_element_t<I, Args>;
		if constexpr (detail::InlinePathArg<Arg>) {
			constexpr auto path_index = detail::inline_path_arg_index<Args, I>();
			auto const params = detail::collect_path_params(Path.view());
			auto label = std::string{"Path<"};
			if (path_index < params.size()) {
				label += params[path_index];
			}
			label += '>';
			meta.extractors[I] = std::move(label);
			meta.path_index_extractor_types.emplace_back(path_index, std::string{detail::route_type_tag<Arg>()});
		}
	}

	template<FixedString Path, class Args, std::size_t... Is>
	static void record_inline_path_extractors(
		AppRouteMetadata &meta,
		std::index_sequence<Is...>) {
		(record_inline_path_extractor<Path, Args, Is>(meta), ...);
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
		auto policy = capture_route_policy();
		using Indices = std::make_index_sequence<std::tuple_size_v<Args>>;
		using Result = typename ExtractedInvokeResult<Fn, Args, Indices>::type;
		if constexpr (detail::IsTaskResultV<Result>) {
			auto add_context = [&](auto handler_fn) {
				if constexpr (detail::has_upload_body_arg<Args>()) {
					router_.add_upload_context_with_timeout(method, Path.view(), policy.timeout, std::move(handler_fn));
				} else {
					router_.add_context_with_timeout(method, Path.view(), policy.timeout, std::move(handler_fn));
				}
			};
			add_context(
				[states = states_, policy, fn = Fn(std::forward<F>(handler))](
					conflux::http::RequestView const &req,
					RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
					conflux::http::Router::ContextHandler inner =
						[states, policy, &fn](
							conflux::http::RequestView const &inner_req,
							RequestContext const &inner_ctx) mutable -> conflux::work::root::Task<Response> {
						if (auto failed = route_prelude_failure(policy, inner_req, &inner_ctx)) {
							co_return *std::move(failed);
						}
						auto const body_limit = route_body_limit(policy);
						co_return co_await invoke_fixed_route_async<Args>(
							*states,
							fn,
							inner_req,
							inner_ctx,
							std::make_index_sequence<std::tuple_size_v<Args>>{}
#if CONFLUX_HAS_JSON
							,
							*policy.json_options,
							body_limit
#endif
						);
					};
					co_return co_await run_scoped_context_route(
						policy.middlewares,
						policy.context_middlewares,
						req,
						ctx,
						std::move(inner));
				});
		} else {
			static_assert(
				!detail::has_upload_body_arg<Args>(),
				"http::UploadBody requires an async handler returning conflux::work::Task<http::Response>");
			auto make_inner = [states = states_, policy, fn = Fn(std::forward<F>(handler))]() mutable {
				return conflux::http::Router::Handler{
					[states, policy, &fn](conflux::http::RequestView const &inner_req) mutable {
						if (auto failed = route_prelude_failure(policy, inner_req)) {
							return *std::move(failed);
						}
						auto const body_limit = route_body_limit(policy);
						return apply_route_timeout(
							invoke_fixed_route<Args>(
								*states,
								fn,
								inner_req,
								std::make_index_sequence<std::tuple_size_v<Args>>{}
#if CONFLUX_HAS_JSON
								,
								*policy.json_options,
								body_limit
#endif
								),
							*policy.timeout);
					}};
			};
			if (policy.context_middlewares && !policy.context_middlewares->empty()) {
				router_.add_context_with_timeout(
					method,
					Path.view(),
					policy.timeout,
					[policy, make_inner = std::move(make_inner)](
						conflux::http::RequestView const &req,
						RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
						return run_scoped_sync_route_as_context(
							policy.context_middlewares,
							policy.middlewares,
							req,
							ctx,
							make_inner());
					});
			} else {
				router_.add(
					method,
					Path.view(),
					[policy, make_inner = std::move(make_inner)](conflux::http::RequestView const &req) mutable {
						return run_scoped_middlewares(policy.middlewares, req, make_inner());
					});
			}
		}
		return RouteRef{*this, route_metadata_.size() - 1};
	}

	template<class Args, class Fn, std::size_t... Is>
	[[nodiscard]] static Response invoke_extracted(
		StateMap const &states,
		Fn &fn,
		conflux::http::RequestView const &req,
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
					req,
					nullptr
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
		conflux::http::RequestView req,
		RequestContext const &ctx,
		std::index_sequence<Is...>
#if CONFLUX_HAS_JSON
		,
		AppJsonOptions const &json_options,
		std::size_t max_body_size
#endif
	) {
		try {
			auto extracted_args = std::make_tuple(
				make_handler_arg<std::tuple_element_t<Is, Args>>(
					states,
					req,
					std::addressof(ctx)
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
		} catch (ExtractorFailure &failure) { return extraction_failure_response(std::move(failure).response()); }
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
		auto policy = capture_route_policy();
		using Indices = std::make_index_sequence<std::tuple_size_v<Args>>;
		using Result = typename ExtractedInvokeResult<Fn, Args, Indices>::type;
		if constexpr (detail::IsTaskResultV<Result>) {
			auto add_context = [&](auto handler_fn) {
				if constexpr (detail::has_upload_body_arg<Args>()) {
					router_.add_upload_context_with_timeout(method, path, policy.timeout, std::move(handler_fn));
				} else {
					router_.add_context_with_timeout(method, path, policy.timeout, std::move(handler_fn));
				}
			};
			add_context(
				[states = states_, policy, fn = Fn(std::forward<F>(handler))](
					conflux::http::RequestView const &req,
					RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
					conflux::http::Router::ContextHandler inner =
						[states, policy, &fn](
							conflux::http::RequestView const &inner_req,
							RequestContext const &inner_ctx) mutable -> conflux::work::root::Task<Response> {
						if (auto failed = route_prelude_failure(policy, inner_req, &inner_ctx)) {
							co_return *std::move(failed);
						}
						auto const body_limit = route_body_limit(policy);
						co_return co_await invoke_extracted_async<Args>(
							*states,
							fn,
							inner_req,
							inner_ctx,
							std::make_index_sequence<std::tuple_size_v<Args>>{}
#if CONFLUX_HAS_JSON
							,
							*policy.json_options,
							body_limit
#endif
						);
					};
					co_return co_await run_scoped_context_route(
						policy.middlewares,
						policy.context_middlewares,
						req,
						ctx,
						std::move(inner));
				});
		} else {
			static_assert(
				!detail::has_upload_body_arg<Args>(),
				"http::UploadBody requires an async handler returning conflux::work::Task<http::Response>");
			auto make_inner = [states = states_, policy, fn = Fn(std::forward<F>(handler))]() mutable {
				return conflux::http::Router::Handler{
					[states, policy, &fn](conflux::http::RequestView const &inner_req) mutable {
						if (auto failed = route_prelude_failure(policy, inner_req)) {
							return *std::move(failed);
						}
						auto const body_limit = route_body_limit(policy);
						return apply_route_timeout(
							invoke_extracted<Args>(
								*states,
								fn,
								inner_req,
								std::make_index_sequence<std::tuple_size_v<Args>>{}
#if CONFLUX_HAS_JSON
								,
								*policy.json_options,
								body_limit
#endif
								),
							*policy.timeout);
					}};
			};
			if (policy.context_middlewares && !policy.context_middlewares->empty()) {
				router_.add_context_with_timeout(
					method,
					path,
					policy.timeout,
					[policy, make_inner = std::move(make_inner)](
						conflux::http::RequestView const &req,
						RequestContext const &ctx) mutable -> conflux::work::root::Task<Response> {
						return run_scoped_sync_route_as_context(
							policy.context_middlewares,
							policy.middlewares,
							req,
							ctx,
							make_inner());
					});
			} else {
				router_.add(
					method,
					path,
					[policy, make_inner = std::move(make_inner)](conflux::http::RequestView const &req) mutable {
						return run_scoped_middlewares(policy.middlewares, req, make_inner());
					});
			}
		}
		return *this;
	}

#if CONFLUX_HAS_JSON
	template<class Arg, class Body>
	[[nodiscard]] static auto make_json_handler_arg(
		StateMap const &states,
		conflux::http::RequestView const &req,
		Json<Body> const &body) {
		using Clean = std::remove_cvref_t<Arg>;
		if constexpr (detail::JsonArg<Clean>) {
			return body;
		} else if constexpr (detail::RawJsonBodyArg<Clean, Body>) {
			return body.value;
		} else {
			return make_handler_arg<Arg>(states, req, nullptr, AppJsonOptions{}, 0);
		}
	}

	template<class Args, class Body, class Fn, std::size_t... Is>
	[[nodiscard]] static Response invoke_json_extracted(
		StateMap const &states,
		Fn &fn,
		conflux::http::RequestView const &req,
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
		auto app_max_body_size = cfg_.max_body_size;
		auto json_options = json_options_;
		auto bearer_token_policy = route_metadata_.back().bearer_token_policy;
		auto rate_limit = route_metadata_.back().rate_limit;
		auto timeout = route_metadata_.back().timeout;
		router_.add(
			method,
			path,
			[states = states_,
			 bearer_token_policy,
			 rate_limit,
			 timeout,
			 fn = Fn(std::forward<F>(handler)),
			 decode_opts = decode_opts,
			 max_body_size,
			 app_max_body_size,
			 json_options](conflux::http::RequestView const &req) mutable -> Response {
				if (auto denied = detail::route_auth_failure(*bearer_token_policy, req)) {
					return *std::move(denied);
				}
				if (auto limited = detail::route_rate_limit_failure(*rate_limit, req)) {
					return *std::move(limited);
				}
				auto content_type = req.header("content-type");
				if (!detail::content_type_is_json_request(content_type)) {
					return detail::unsupported_json_content_type_problem();
				}
				auto const limit = effective_body_limit(*max_body_size, app_max_body_size, json_options->max_body_size);
				if (limit != 0 && req.body.size() > limit) {
					return detail::json_body_too_large_problem();
				}
				auto effective_decode_opts = decode_opts ? *decode_opts : typed_json_decode_options(*json_options);
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
	[[nodiscard]] std::expected<std::unique_ptr<HttpServer>, std::string> try_server(AppRunOptions opts = {}) &&;
	[[nodiscard]] std::expected<std::unique_ptr<HttpServer>, std::string> prepare_server(AppRunOptions opts = {}) &&;
	[[nodiscard]] std::expected<RunStatus, std::string> try_run(AppRunOptions opts = {}) &&;
	[[nodiscard]] RunStatus run(AppRunOptions opts = {}) && noexcept;

private:
	conflux::http::Config cfg_;
	conflux::http::Router router_;
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

[[nodiscard]] conflux::http::Router &router(
	App &app) noexcept {
	return app.router_;
}

[[nodiscard]] conflux::http::Router const &router(
	App const &app) noexcept {
	return app.router_;
}

[[nodiscard]] std::vector<conflux::http::RouteInfo> route_infos(
	App const &app) {
	return router(app).route_infos();
}

[[nodiscard]] App app(
	conflux::http::Config cfg = conflux::http::Config::public_server()) {
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
