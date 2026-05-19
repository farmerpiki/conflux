module;
#include <memory>
#include <typeindex>

export module conflux.net.app;

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http_server;
import conflux.crypto;
#if CONFLUX_HAS_JSON
import conflux.net.http.native_json;
#endif
import conflux.work;
export namespace conflux::http {

struct Problem {
	HttpResponse response;
	std::string code;
	std::string detail;
};

struct Created {
	HttpResponse response;

	[[nodiscard]] Created header(
		std::string_view name,
		std::string value) && {
		response.headers[name] = std::move(value);
		return std::move(*this);
	}

	[[nodiscard]] Created header(
		std::string_view name,
		std::string_view value) && {
		response.headers[name] = std::string{value};
		return std::move(*this);
	}

	[[nodiscard]] Created header(
		std::string_view name,
		char const *value) && {
		response.headers[name] = std::string{value};
		return std::move(*this);
	}
};

template<class T>
struct Json {
	using value_type = std::remove_cvref_t<T>;

	value_type value;

	constexpr Json(
		value_type v)
		: value(std::move(v)) {}

	[[nodiscard]] constexpr value_type const &operator *() const noexcept { return value; }
	[[nodiscard]] constexpr value_type const *operator ->() const noexcept { return std::addressof(value); }
};

template<class T>
Json(T) -> Json<std::remove_cvref_t<T>>;

template<std::size_t N>
struct FixedString {
	char value[N]{};

	consteval FixedString(
		char const (&text)[N]) {
		std::copy_n(text, N, value);
	}

	[[nodiscard]] constexpr std::string_view view() const noexcept { return {value, N - 1}; }
};

template<FixedString Name, class T = std::string_view>
struct Path {
	using value_type = T;

	T value{};

	[[nodiscard]] static constexpr std::string_view name() noexcept { return Name.view(); }
	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

template<std::size_t Index, class T = std::string_view>
struct PathAt {
	using value_type = T;
	static constexpr std::size_t index = Index;

	T value{};

	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

template<FixedString Name, class T = std::string_view>
struct Query {
	using value_type = T;

	T value{};

	[[nodiscard]] static constexpr std::string_view name() noexcept { return Name.view(); }
	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

template<FixedString Name, class T = std::string_view>
struct Header {
	using value_type = T;

	T value{};

	[[nodiscard]] static constexpr std::string_view name() noexcept { return Name.view(); }
	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

template<FixedString Name, class T = std::string_view>
struct Cookie {
	using value_type = T;

	T value{};

	[[nodiscard]] static constexpr std::string_view name() noexcept { return Name.view(); }
	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

template<FixedString Name, class T = std::string_view>
struct Form {
	using value_type = T;

	T value{};

	[[nodiscard]] static constexpr std::string_view name() noexcept { return Name.view(); }
	[[nodiscard]] constexpr T const &get() const noexcept { return value; }
	[[nodiscard]] constexpr T const &operator *() const noexcept { return value; }
};

struct BodyText {
	std::string_view value{};

	[[nodiscard]] constexpr std::string_view get() const noexcept { return value; }
	[[nodiscard]] constexpr std::string_view operator *() const noexcept { return value; }
};

struct BodyBytes {
	std::string_view value{};

	[[nodiscard]] constexpr std::string_view get() const noexcept { return value; }
	[[nodiscard]] constexpr std::string_view operator *() const noexcept { return value; }
};

struct OwnedBodyBytes {
	std::string value;

	[[nodiscard]] std::string const &get() const noexcept { return value; }
	[[nodiscard]] std::string const &operator *() const noexcept { return value; }
};

struct Multipart {
	HttpFieldsView form;
	std::span<UploadedFile const> files;

	[[nodiscard]] std::string_view form_value(
		std::string_view name) const {
		return form[name];
	}

	[[nodiscard]] UploadedFile const *file(
		std::string_view name) const noexcept {
		for (auto const &item: files) {
			if (item.name == name) {
				return std::addressof(item);
			}
		}
		return nullptr;
	}
};

struct RequestId {
	std::string_view value{};

	[[nodiscard]] constexpr std::string_view get() const noexcept { return value; }
	[[nodiscard]] constexpr std::string_view operator *() const noexcept { return value; }
};

struct ConnectionInfo {
	std::string_view remote_addr{};
	bool is_tls{};
};

struct TraceContext {
	std::string_view traceparent{};
};

struct Bearer {
	std::string_view token{};

	[[nodiscard]] constexpr std::string_view get() const noexcept { return token; }
	[[nodiscard]] constexpr std::string_view operator *() const noexcept { return token; }
};

struct BasicAuth {
	std::string username;
	std::string password;
};

template<class T>
struct State {
	T *value{};

	[[nodiscard]] constexpr T &get() const noexcept { return *value; }
	[[nodiscard]] constexpr T &operator *() const noexcept { return *value; }
	[[nodiscard]] constexpr T *operator ->() const noexcept { return value; }
};

struct ValidationIssue {
	std::string message;
	std::string method;
	std::string path;
	std::string source_file;
	std::uint_least32_t source_line{};
	std::string related_source_file;
	std::uint_least32_t related_source_line{};
};

struct ValidationReport {
	std::vector<ValidationIssue> issues;

	[[nodiscard]] bool ok() const noexcept { return issues.empty(); }
	explicit operator bool() const noexcept { return ok(); }
	[[nodiscard]] std::string summary() const {
		if (issues.empty()) {
			return {};
		}
		std::string out;
		for (auto const &issue: issues) {
			if (!out.empty()) {
				out += '\n';
			}
			out += std::format("{} {}: {}", issue.method, issue.path, issue.message);
		}
		return out;
	}
};

struct AppRouteInfo {
	std::string method;
	std::string path;
	std::string name;
	std::string handler_kind;
	std::string source_file;
	std::uint_least32_t source_line{};
	std::vector<std::string> extractors;
	std::vector<std::string> path_params;
	std::size_t required_state_count{};
	std::vector<std::string> consumes;
	std::vector<std::string> produces;
	std::size_t max_body_size{};
	std::chrono::milliseconds timeout{};
	std::size_t middleware_count{};
	std::string rate_limit;
	std::string auth_policy;
	std::string openapi_summary;
};

template<class T>
concept ExpectedHttpProblem = requires(T value) {
	typename T::value_type;
	typename T::error_type;
	requires std::same_as<typename T::error_type, Problem>;
	{ static_cast<bool>(value) } -> std::same_as<bool>;
	{ *value };
	{ value.error() } -> std::same_as<Problem &>;
};

[[nodiscard]] inline HttpResponse into_response(
	HttpResponse response) {
	return response;
}

[[nodiscard]] inline HttpResponse into_response(
	Problem problem) {
	return std::move(problem.response);
}

[[nodiscard]] inline HttpResponse into_response(
	Created created) {
	return std::move(created.response);
}

template<class T>
[[nodiscard]] HttpResponse into_response(
	Json<T> const &body) {
#if CONFLUX_HAS_JSON
	return json::response_or_internal_error(body.value);
#else
	(void)body;
	return HttpResponse::internal_error("JSON support is not enabled");
#endif
}

template<class T>
[[nodiscard]] HttpResponse into_response(
	Json<T> &&body) {
#if CONFLUX_HAS_JSON
	return json::response_or_internal_error(body.value);
#else
	(void)body;
	return HttpResponse::internal_error("JSON support is not enabled");
#endif
}

template<class T>
[[nodiscard]] HttpResponse into_response(
	T &&result)
	requires ExpectedHttpProblem<std::remove_cvref_t<T>>
{
	if (result) {
		return into_response(*std::forward<T>(result));
	}
	return into_response(std::forward<T>(result).error());
}

template<class T>
concept IntoResponse = requires(T &&value) {
	{ into_response(std::forward<T>(value)) } -> std::same_as<HttpResponse>;
};

template<class>
inline constexpr bool kDependentFalse = false;

class ExtractorFailure final : public std::exception {
public:
	explicit ExtractorFailure(
		HttpResponse response)
		: response_(std::move(response)) {}

	[[nodiscard]] char const *what() const noexcept override { return "HTTP extractor failure"; }
	[[nodiscard]] HttpResponse response() && { return std::move(response_); }

private:
	HttpResponse response_;
};

namespace detail {

template<class T>
struct FunctionArgs;

template<class R, class... Args>
struct FunctionArgs<R (*)(Args...)> {
	using type = std::tuple<Args...>;
};

template<class C, class R, class... Args>
struct FunctionArgs<R (C::*)(Args...)> {
	using type = std::tuple<Args...>;
};

template<class C, class R, class... Args>
struct FunctionArgs<R (C::*)(Args...) const> {
	using type = std::tuple<Args...>;
};

template<class F>
concept HasFunctionArgs = requires { typename FunctionArgs<decltype(&std::remove_reference_t<F>::operator ())>::type; };

template<class F>
struct CallableArgs {
	using type = FunctionArgs<decltype(&std::remove_reference_t<F>::operator ())>::type;
};

template<class R, class... Args>
struct CallableArgs<R (*)(Args...)> {
	using type = std::tuple<Args...>;
};

template<class T>
struct StateType {};

template<class T>
struct StateType<State<T>> {
	using type = T;
};

template<class T>
concept StateArg = requires { typename StateType<std::remove_cvref_t<T>>::type; };

template<class T>
struct JsonType {};

template<class T>
struct JsonType<Json<T>> {
	using type = T;
};

template<class T>
concept JsonArg = requires { typename JsonType<std::remove_cvref_t<T>>::type; };

template<class T>
struct PathType {};

template<FixedString Name, class T>
struct PathType<Path<Name, T>> {
	using type = T;
	static constexpr auto name = Name;
};

template<class T>
concept PathArg = requires { typename PathType<std::remove_cvref_t<T>>::type; };

template<class T>
struct PathAtType {};

template<std::size_t Index, class T>
struct PathAtType<PathAt<Index, T>> {
	using type = T;
	static constexpr std::size_t index = Index;
};

template<class T>
concept PathAtArg = requires { typename PathAtType<std::remove_cvref_t<T>>::type; };

template<class T>
struct QueryType {};

template<FixedString Name, class T>
struct QueryType<Query<Name, T>> {
	using type = T;
	static constexpr auto name = Name;
};

template<class T>
concept QueryArg = requires { typename QueryType<std::remove_cvref_t<T>>::type; };

template<class T>
struct HeaderType {};

template<FixedString Name, class T>
struct HeaderType<Header<Name, T>> {
	using type = T;
	static constexpr auto name = Name;
};

template<class T>
concept HeaderArg = requires { typename HeaderType<std::remove_cvref_t<T>>::type; };

template<class T>
struct CookieType {};

template<FixedString Name, class T>
struct CookieType<Cookie<Name, T>> {
	using type = T;
	static constexpr auto name = Name;
};

template<class T>
concept CookieArg = requires { typename CookieType<std::remove_cvref_t<T>>::type; };

template<class T>
struct FormType {};

template<FixedString Name, class T>
struct FormType<Form<Name, T>> {
	using type = T;
	static constexpr auto name = Name;
};

template<class T>
concept FormArg = requires { typename FormType<std::remove_cvref_t<T>>::type; };

template<class Arg>
concept RequestViewArg = std::same_as<std::remove_cvref_t<Arg>, RequestView>;

template<class Arg>
concept RequestArg = std::same_as<std::remove_cvref_t<Arg>, Request>;

template<class Arg>
concept BodyTextArg = std::same_as<std::remove_cvref_t<Arg>, BodyText>;

template<class Arg>
concept BodyBytesArg = std::same_as<std::remove_cvref_t<Arg>, BodyBytes>;

template<class Arg>
concept OwnedBodyBytesArg = std::same_as<std::remove_cvref_t<Arg>, OwnedBodyBytes>;

template<class Arg>
concept MultipartArg = std::same_as<std::remove_cvref_t<Arg>, Multipart>;

template<class Arg>
concept RequestIdArg = std::same_as<std::remove_cvref_t<Arg>, RequestId>;

template<class Arg>
concept ConnectionInfoArg = std::same_as<std::remove_cvref_t<Arg>, ConnectionInfo>;

template<class Arg>
concept TraceContextArg = std::same_as<std::remove_cvref_t<Arg>, TraceContext>;

template<class Arg>
concept BearerArg = std::same_as<std::remove_cvref_t<Arg>, Bearer>;

template<class Arg>
concept BasicAuthArg = std::same_as<std::remove_cvref_t<Arg>, BasicAuth>;

template<class Arg, class Body>
concept RawJsonBodyArg = std::same_as<std::remove_cvref_t<Arg>, std::remove_cvref_t<Body>>;

template<class T>
struct ExpectedValueType {};

template<class T, class E>
struct ExpectedValueType<std::expected<T, E>> {
	using type = T;
};

template<class T>
struct ResponseMetadataType {
	using type = std::remove_cvref_t<T>;
};

template<class T>
	requires requires { typename ExpectedValueType<std::remove_cvref_t<T>>::type; }
struct ResponseMetadataType<T> {
	using type = typename ExpectedValueType<std::remove_cvref_t<T>>::type;
};

template<class Args, std::size_t... Is>
consteval bool has_state_arg_impl(
	std::index_sequence<Is...>) {
	return (
		false
		|| ...
		|| (StateArg<std::tuple_element_t<Is, Args>>
			|| PathArg<std::tuple_element_t<Is, Args>>
			|| PathAtArg<std::tuple_element_t<Is, Args>>
			|| QueryArg<std::tuple_element_t<Is, Args>>
			|| HeaderArg<std::tuple_element_t<Is, Args>>
			|| CookieArg<std::tuple_element_t<Is, Args>>
			|| FormArg<std::tuple_element_t<Is, Args>>
			|| BodyTextArg<std::tuple_element_t<Is, Args>>
			|| BodyBytesArg<std::tuple_element_t<Is, Args>>
			|| OwnedBodyBytesArg<std::tuple_element_t<Is, Args>>
			|| MultipartArg<std::tuple_element_t<Is, Args>>
			|| RequestIdArg<std::tuple_element_t<Is, Args>>
			|| ConnectionInfoArg<std::tuple_element_t<Is, Args>>
			|| TraceContextArg<std::tuple_element_t<Is, Args>>
			|| BearerArg<std::tuple_element_t<Is, Args>>
			|| BasicAuthArg<std::tuple_element_t<Is, Args>>));
}

template<class Args>
consteval bool has_state_arg() {
	return has_state_arg_impl<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{});
}

} // namespace detail

template<typename Fn>
	requires(std::invocable<Fn &> && std::same_as<std::invoke_result_t<Fn &>, HttpResponse>)
[[nodiscard]] HttpResponse defer(
	std::shared_ptr<WorkPool> const &pool,
	Fn &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	if (!pool) {
		return HttpResponse::internal_error("defer: null pool");
	}
	auto deferred = std::make_shared<DeferredResponse>(timeout);
	bool const enqueued = pool->enqueue([deferred, work = std::decay_t<Fn>(std::forward<Fn>(fn))]() mutable {
		try {
			deferred->complete(work());
		} catch (std::exception const &ex) {
			deferred->complete(HttpResponse::internal_error(ex.what()));
		} catch (...) { deferred->complete(HttpResponse::internal_error()); }
	});
	if (!enqueued) {
		return HttpResponse::internal_error("offload queue full");
	}
	return HttpResponse::deferred(std::move(deferred));
}
template<typename Fn>
	requires(std::invocable<Fn &> && std::same_as<std::invoke_result_t<Fn &>, HttpResponse>)
[[nodiscard]] HttpResponse defer(
	WorkPool &pool,
	Fn &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	auto deferred = std::make_shared<DeferredResponse>(timeout);
	bool const enqueued = pool.enqueue([deferred, work = std::decay_t<Fn>(std::forward<Fn>(fn))]() mutable {
		try {
			deferred->complete(work());
		} catch (std::exception const &ex) {
			deferred->complete(HttpResponse::internal_error(ex.what()));
		} catch (...) { deferred->complete(HttpResponse::internal_error()); }
	});
	if (!enqueued) {
		return HttpResponse::internal_error("offload queue full");
	}
	return HttpResponse::deferred(std::move(deferred));
}
struct AppRunOptions {
	std::uint16_t port = kConfigDefaultPort;
};
#if CONFLUX_HAS_JSON
struct AppJsonOptions {
	conflux::json::boundary::DecodeOptions decode{};
	std::size_t max_body_size{};
};
#endif
class App {
	using StateMap = std::unordered_map<std::type_index, std::shared_ptr<void>>;

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
		std::shared_ptr<std::size_t> max_body_size = std::make_shared<std::size_t>(0);
		std::chrono::milliseconds timeout{};
		std::size_t middleware_count{};
		std::string rate_limit;
		std::string auth_policy;
		std::string openapi_summary;
		bool uses_body{};
	};

	struct StaticMountMetadata {
		std::string url_prefix;
		std::string root_dir;
		std::string source_file;
		std::uint_least32_t source_line{};
	};

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

		RouteRef &timeout(
			std::chrono::milliseconds value) {
			metadata().timeout = value;
			return *this;
		}

		RouteRef &rate_limit(
			std::string_view value) {
			metadata().rate_limit = std::string{value};
			return *this;
		}

		RouteRef &auth_policy(
			std::string_view value) {
			metadata().auth_policy = std::string{value};
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
		} else if constexpr (requires(Fn &fn) {
								 { into_response(fn()) } -> std::same_as<HttpResponse>;
							 }) {
			record_route_metadata<std::tuple<>>(method, path, "app", loc);
			record_return_metadata<std::invoke_result_t<Fn &>>();
			router_.add(method, path, [fn = std::decay_t<F>(std::forward<F>(handler))](RequestView const &) mutable {
				return into_response(fn());
			});
		} else if constexpr (requires(Fn &fn, RequestView const &req) {
								 { into_response(fn(req)) } -> std::same_as<HttpResponse>;
							 }) {
			record_route_metadata<std::tuple<RequestView>>(method, path, "app", loc);
			record_return_metadata<std::invoke_result_t<Fn &, RequestView const &>>();
			router_.add(method, path, [fn = Fn(std::forward<F>(handler))](RequestView const &req) mutable {
				return into_response(fn(req));
			});
		} else if constexpr (requires(Fn &fn, Request const &req) {
								 { into_response(fn(req)) } -> std::same_as<HttpResponse>;
							 }) {
			record_route_metadata<std::tuple<Request>>(method, path, "app", loc);
			record_return_metadata<std::invoke_result_t<Fn &, Request const &>>();
			router_.add(method, path, [fn = Fn(std::forward<F>(handler))](RequestView const &req) mutable {
				auto owned = req.to_owned();
				return into_response(fn(owned));
			});
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
		return get(Path.view(), std::forward<F>(handler), loc);
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
		return post(Path.view(), std::forward<F>(handler), loc);
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
		return put(Path.view(), std::forward<F>(handler), loc);
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
		return patch(Path.view(), std::forward<F>(handler), loc);
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
		return del(Path.view(), std::forward<F>(handler), loc);
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
		return options(Path.view(), std::forward<F>(handler), loc);
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &add_context(
		std::string_view method,
		std::string_view path,
		F &&handler) {
		router_.add_context(method, path, std::forward<F>(handler));
		return *this;
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &get_context(
		std::string_view path,
		F &&handler) {
		return add_context("GET", path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &post_context(
		std::string_view path,
		F &&handler) {
		return add_context("POST", path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &put_context(
		std::string_view path,
		F &&handler) {
		return add_context("PUT", path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &patch_context(
		std::string_view path,
		F &&handler) {
		return add_context("PATCH", path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &del_context(
		std::string_view path,
		F &&handler) {
		return add_context("DELETE", path, std::forward<F>(handler));
	}
	template<typename F>
		requires ContextHandlerFunction<F>
	App &options_context(
		std::string_view path,
		F &&handler) {
		return add_context("OPTIONS", path, std::forward<F>(handler));
	}
	template<typename F>
	App &use(
		F &&middleware) {
		router_.use(std::forward<F>(middleware));
		++middleware_count_;
		return *this;
	}
	template<typename F>
	App &sse(
		std::string_view path,
		F &&handler) {
		router_.sse(path, std::forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &ws(
		std::string_view path,
		F &&handler) {
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
	[[nodiscard]] State<T> state() const {
		auto const it = states_->find(std::type_index{typeid(T)});
		if (it == states_->end()) {
			return {};
		}
		return State<T>{.value = static_cast<T *>(it->second.get())};
	}
	template<typename F>
	App &group(
		std::string_view prefix,
		F &&fn) {
		router_.group(prefix, std::forward<F>(fn));
		return *this;
	}
	[[nodiscard]] Config &config() { return cfg_; }
	[[nodiscard]] Config const &config() const { return cfg_; }
	[[nodiscard]] Router &router() { return router_; }
	[[nodiscard]] Router const &router() const { return router_; }
	[[nodiscard]] std::vector<RouteInfo> route_infos() const { return router_.route_infos(); }
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
					.required_state_count = route.required_states.size(),
					.consumes = route.consumes,
					.produces = route.produces,
					.max_body_size = *route.max_body_size,
					.timeout = route.timeout,
					.middleware_count = route.middleware_count,
					.rate_limit = route.rate_limit,
					.auth_policy = route.auth_policy,
					.openapi_summary = route.openapi_summary});
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
		return out;
	}
	[[nodiscard]] std::string openapi_spec(
		std::string_view title = "API",
		std::string_view version = "1.0.0") const {
		auto json_str = [](std::string_view value) {
			std::string out = "\"";
			for (auto const ch: value) {
				auto const c = static_cast<unsigned char>(ch);
				if (c == '"') {
					out += "\\\"";
				} else if (c == '\\') {
					out += "\\\\";
				} else if (c == '\n') {
					out += "\\n";
				} else if (c == '\r') {
					out += "\\r";
				} else if (c == '\t') {
					out += "\\t";
				} else if (c < 0x20) {
					out += std::format("\\u{:04x}", c);
				} else {
					out += static_cast<char>(c);
				}
			}
			out += '"';
			return out;
		};
		auto method_key = [](std::string_view method) {
			std::string out;
			out.reserve(method.size());
			for (char const ch: method) {
				out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			}
			return out;
		};
		auto schema_for_path_type = [](std::string_view type) {
			if (type == "u64") {
				return std::string{R"({"type":"integer","format":"uint64","minimum":0})"};
			}
			if (type == "i64") {
				return std::string{R"({"type":"integer","format":"int64"})"};
			}
			if (type == "u32") {
				return std::string{R"({"type":"integer","format":"uint32","minimum":0})"};
			}
			if (type == "i32") {
				return std::string{R"({"type":"integer","format":"int32"})"};
			}
			return std::string{R"({"type":"string"})"};
		};

		std::string out;
		out += R"({"openapi":"3.0.0","info":{"title":)";
		out += json_str(title);
		out += R"(,"version":)";
		out += json_str(version);
		out += R"(})";
		bool has_auth_policy = false;
		for (auto const &route: route_metadata_) {
			if (!route.auth_policy.empty()) {
				has_auth_policy = true;
				break;
			}
		}
		if (has_auth_policy) {
			out += R"(,"components":{"securitySchemes":{"bearerAuth":{"type":"http","scheme":"bearer"}}})";
		}
		out += R"(,"paths":{)";
		std::vector<std::string> path_order;
		std::map<std::string, std::vector<AppRouteMetadata const *>> routes_by_path;
		for (auto const &route: route_metadata_) {
			auto [it, inserted] = routes_by_path.try_emplace(route.path);
			if (inserted) {
				path_order.push_back(route.path);
			}
			it->second.push_back(std::addressof(route));
		}
		for (std::size_t path_index = 0; path_index < path_order.size(); ++path_index) {
			auto const &path = path_order[path_index];
			auto const &routes = routes_by_path.at(path);
			if (path_index != 0) {
				out += ',';
			}
			out += json_str(path);
			out += ":{";
			for (std::size_t route_index = 0; route_index < routes.size(); ++route_index) {
				auto const &route = *routes[route_index];
				if (route_index != 0) {
					out += ',';
				}
				out += json_str(method_key(route.method));
				out += ":{";
				if (!route.name.empty()) {
					out += R"("operationId":)";
					out += json_str(route.name);
					out += ',';
				}
				if (!route.openapi_summary.empty()) {
					out += R"("summary":)";
					out += json_str(route.openapi_summary);
					out += ',';
				}
				if (!route.auth_policy.empty()) {
					out += R"("security":[{"bearerAuth":[]}],"x-auth-policy":)";
					out += json_str(route.auth_policy);
					out += ',';
				}
				if (route.timeout.count() != 0) {
					out += R"("x-timeout-ms":)";
					out += std::to_string(route.timeout.count());
					out += ',';
				}
				if (!route.rate_limit.empty()) {
					out += R"("x-rate-limit":)";
					out += json_str(route.rate_limit);
					out += ',';
				}
				out += R"("parameters":[)";
				for (std::size_t i = 0; i < route.path_params.size(); ++i) {
					if (i != 0) {
						out += ',';
					}
					out += R"({"name":)";
					out += json_str(route.path_params[i]);
					out += R"(,"in":"path","required":true,"schema":)";
					if (auto type = route.path_param_types.find(route.path_params[i]);
						type != route.path_param_types.end()) {
						out += schema_for_path_type(type->second);
					} else {
						out += schema_for_path_type({});
					}
					out += "}";
				}
				out += ']';
				if (!route.consumes.empty()) {
					out += R"(,"requestBody":{"content":{)";
					for (std::size_t i = 0; i < route.consumes.size(); ++i) {
						if (i != 0) {
							out += ',';
						}
						out += json_str(route.consumes[i]);
						out += R"(:{"schema":{"type":"object"}})";
					}
					out += "}}";
				}
				out += R"(,"responses":{"200":{"description":"OK")";
				if (!route.produces.empty()) {
					out += R"(","content":{)";
					for (std::size_t i = 0; i < route.produces.size(); ++i) {
						if (i != 0) {
							out += ',';
						}
						out += json_str(route.produces[i]);
						out += R"(:{"schema":{"type":"object"}})";
					}
					out += "}";
				}
				out += R"(}}})";
			}
			out += "}";
		}
		out += "}}";
		return out;
	}
	[[nodiscard]] Router::Handler openapi_handler(
		std::string_view title = "API",
		std::string_view version = "1.0.0") const {
		auto spec = openapi_spec(title, version);
		return [spec = std::move(spec)](RequestView const &) -> HttpResponse {
			auto response = HttpResponse::json(spec);
			return response;
		};
	}
	[[nodiscard]] ValidationReport validate() const {
		ValidationReport report;
		for (auto const &issue: state_issues_) {
			report.issues.push_back(ValidationIssue{.message = issue, .method = "APP", .path = "state"});
		}
		validate_tls_config(report);
		std::map<std::pair<std::string, std::string>, AppRouteMetadata const *> seen;
		std::map<std::pair<std::string, std::string>, AppRouteMetadata const *> seen_shapes;
		for (auto const &route: route_metadata_) {
			if (auto pattern_issue = validate_path_pattern(route.path)) {
				report.issues.push_back(
					ValidationIssue{
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
						.message = "duplicate route",
						.method = route.method,
						.path = route.path,
						.source_file = route.source_file,
						.source_line = route.source_line,
						.related_source_file = it->second->source_file,
						.related_source_line = it->second->source_line});
			}
			auto shape_key = std::pair{route.method, route_shape(route.path)};
			auto [shape_it, shape_inserted] = seen_shapes.emplace(shape_key, std::addressof(route));
			if (!shape_inserted && shape_it->second->path != route.path) {
				report.issues.push_back(
					ValidationIssue{
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
							.message = std::format("missing path parameter for Path<{}>", path_extractor),
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
							.message = std::format("missing path parameter for Path<{}>", index),
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
			if (route.method == "GET" && route.uses_body) {
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

	[[nodiscard]] static std::optional<std::string> validate_path_pattern(
		std::string_view path) {
		if (path.empty() || path.front() != '/') {
			return "invalid route pattern: path must start with /";
		}
		for (std::size_t pos = 0, segment_index = 0;; ++segment_index) {
			auto next = path.find('/', pos + 1);
			auto segment =
				path.substr(pos + 1, next == std::string_view::npos ? path.size() - pos - 1 : next - pos - 1);
			auto const open = segment.find('{');
			auto const close = segment.find('}');
			if ((open == std::string_view::npos) != (close == std::string_view::npos) || open > close) {
				return "invalid route pattern: unmatched path parameter braces";
			}
			if (open != std::string_view::npos) {
				if (open != 0 || close + 1 != segment.size()) {
					return "invalid route pattern: path parameter must occupy the full segment";
				}
				auto name = segment.substr(1, segment.size() - 2);
				bool const wildcard = name.starts_with('*');
				if (wildcard) {
					name.remove_prefix(1);
					if (next != std::string_view::npos) {
						return "invalid route pattern: wildcard parameter must be the final segment";
					}
				}
				if (name.empty()) {
					return "invalid route pattern: path parameter name is empty";
				}
			}
			(void)segment_index;
			if (next == std::string_view::npos) {
				break;
			}
			pos = next;
		}
		return std::nullopt;
	}

	[[nodiscard]] static std::string_view trim_ascii(
		std::string_view value) noexcept {
		while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
			value.remove_prefix(1);
		}
		while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
			value.remove_suffix(1);
		}
		return value;
	}

	[[nodiscard]] static std::optional<std::string_view> credentials_for_scheme(
		std::string_view auth,
		std::string_view scheme) noexcept {
		if (auth.size() <= scheme.size() || auth[scheme.size()] != ' ') {
			return std::nullopt;
		}
		if (!ascii_iequals(auth.substr(0, scheme.size()), scheme)) {
			return std::nullopt;
		}
		return trim_ascii(auth.substr(scheme.size() + 1));
	}

	[[nodiscard]] static std::string route_shape(
		std::string_view path) {
		std::string out;
		out.reserve(path.size());
		for (std::size_t pos = 0;;) {
			auto next = path.find('/', pos + 1);
			auto segment =
				path.substr(pos + 1, next == std::string_view::npos ? path.size() - pos - 1 : next - pos - 1);
			out += '/';
			if (segment.size() >= 2 && segment.front() == '{' && segment.back() == '}') {
				out += segment.starts_with("{*") ? "{*}" : "{}";
			} else {
				out += segment;
			}
			if (next == std::string_view::npos) {
				break;
			}
			pos = next;
		}
		return out.empty() ? "/" : out;
	}

	template<class Args, std::size_t... Is>
	static void append_required_states(
		std::vector<std::type_index> &out,
		std::index_sequence<Is...>) {
		(
			[&] {
				using Arg = std::tuple_element_t<Is, Args>;
				if constexpr (detail::StateArg<Arg>) {
					using StateValue = typename detail::StateType<std::remove_cvref_t<Arg>>::type;
					out.push_back(std::type_index{typeid(StateValue)});
				}
			}(),
			...);
	}

	template<class Arg>
	[[nodiscard]] static std::string extractor_name() {
		using Clean = std::remove_cvref_t<Arg>;
		if constexpr (detail::StateArg<Clean>) {
			return "State";
		} else if constexpr (detail::PathArg<Clean>) {
			return std::format("Path<{}>", detail::PathType<Clean>::name.view());
		} else if constexpr (detail::PathAtArg<Clean>) {
			return std::format("PathAt<{}>", detail::PathAtType<Clean>::index);
		} else if constexpr (detail::QueryArg<Clean>) {
			return std::format("Query<{}>", detail::QueryType<Clean>::name.view());
		} else if constexpr (detail::HeaderArg<Clean>) {
			return std::format("Header<{}>", detail::HeaderType<Clean>::name.view());
		} else if constexpr (detail::CookieArg<Clean>) {
			return std::format("Cookie<{}>", detail::CookieType<Clean>::name.view());
		} else if constexpr (detail::FormArg<Clean>) {
			return std::format("Form<{}>", detail::FormType<Clean>::name.view());
		} else if constexpr (detail::BodyTextArg<Clean>) {
			return "BodyText";
		} else if constexpr (detail::BodyBytesArg<Clean>) {
			return "BodyBytes";
		} else if constexpr (detail::OwnedBodyBytesArg<Clean>) {
			return "OwnedBodyBytes";
		} else if constexpr (detail::MultipartArg<Clean>) {
			return "Multipart";
		} else if constexpr (detail::RequestIdArg<Clean>) {
			return "RequestId";
		} else if constexpr (detail::ConnectionInfoArg<Clean>) {
			return "ConnectionInfo";
		} else if constexpr (detail::TraceContextArg<Clean>) {
			return "TraceContext";
		} else if constexpr (detail::BearerArg<Clean>) {
			return "Bearer";
		} else if constexpr (detail::BasicAuthArg<Clean>) {
			return "BasicAuth";
		} else if constexpr (detail::JsonArg<Clean>) {
			return "Json";
		} else if constexpr (detail::RequestViewArg<Clean>) {
			return "RequestView";
		} else if constexpr (detail::RequestArg<Clean>) {
			return "Request";
		} else {
			return "unknown";
		}
	}

	template<class Args, std::size_t... Is>
	static void append_extractors(
		std::vector<std::string> &out,
		std::index_sequence<Is...>) {
		(out.push_back(extractor_name<std::tuple_element_t<Is, Args>>()), ...);
	}

	template<class Args, std::size_t... Is>
	static void append_path_extractors(
		std::vector<std::string> &out,
		std::index_sequence<Is...>) {
		(
			[&] {
				using Arg = std::tuple_element_t<Is, Args>;
				using Clean = std::remove_cvref_t<Arg>;
				if constexpr (detail::PathArg<Clean>) {
					out.push_back(std::string{detail::PathType<Clean>::name.view()});
				}
			}(),
			...);
	}

	template<class T>
	[[nodiscard]] static consteval std::string_view route_type_tag() {
		using Clean = std::remove_cvref_t<T>;
		if constexpr (std::same_as<Clean, std::uint64_t>) {
			return "u64";
		} else if constexpr (std::same_as<Clean, std::int64_t>) {
			return "i64";
		} else if constexpr (std::same_as<Clean, std::uint32_t>) {
			return "u32";
		} else if constexpr (std::same_as<Clean, std::int32_t>) {
			return "i32";
		} else if constexpr (std::same_as<Clean, std::string> || std::same_as<Clean, std::string_view>) {
			return "string";
		} else {
			return "";
		}
	}

	template<class Args, std::size_t... Is>
	static void append_path_extractor_types(
		std::vector<std::pair<std::string, std::string>> &out,
		std::vector<std::pair<std::size_t, std::string>> &index_out,
		std::index_sequence<Is...>) {
		(
			[&] {
				using Arg = std::tuple_element_t<Is, Args>;
				using Clean = std::remove_cvref_t<Arg>;
				if constexpr (detail::PathArg<Clean>) {
					using PathValue = typename detail::PathType<Clean>::type;
					out.emplace_back(
						std::string{detail::PathType<Clean>::name.view()},
						std::string{route_type_tag<PathValue>()});
				} else if constexpr (detail::PathAtArg<Clean>) {
					using PathValue = typename detail::PathAtType<Clean>::type;
					index_out.emplace_back(detail::PathAtType<Clean>::index, std::string{route_type_tag<PathValue>()});
				}
			}(),
			...);
	}

	template<class Args, std::size_t... Is>
	[[nodiscard]] static consteval bool has_body_extractor_impl(
		std::index_sequence<Is...>) {
		return (
			false
			|| ...
			|| (detail::BodyTextArg<std::tuple_element_t<Is, Args>>
				|| detail::BodyBytesArg<std::tuple_element_t<Is, Args>>
				|| detail::OwnedBodyBytesArg<std::tuple_element_t<Is, Args>>
				|| detail::MultipartArg<std::tuple_element_t<Is, Args>>
				|| detail::JsonArg<std::tuple_element_t<Is, Args>>));
	}

	template<class Args>
	[[nodiscard]] static consteval bool has_body_extractor() {
		return has_body_extractor_impl<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{});
	}

	[[nodiscard]] static std::vector<std::string> collect_path_params(
		std::string_view path) {
		std::vector<std::string> out;
		for (std::size_t pos = 0; pos < path.size();) {
			auto open = path.find('{', pos);
			if (open == std::string_view::npos) {
				break;
			}
			auto close = path.find('}', open + 1);
			if (close == std::string_view::npos) {
				break;
			}
			auto name = path.substr(open + 1, close - open - 1);
			if (name.starts_with('*')) {
				name.remove_prefix(1);
			}
			if (auto colon = name.find(':'); colon != std::string_view::npos) {
				name = name.substr(0, colon);
			}
			if (!name.empty()) {
				out.emplace_back(name);
			}
			pos = close + 1;
		}
		return out;
	}

	[[nodiscard]] static std::map<std::string, std::string> collect_path_param_types(
		std::string_view path) {
		std::map<std::string, std::string> out;
		for (std::size_t pos = 0; pos < path.size();) {
			auto open = path.find('{', pos);
			if (open == std::string_view::npos) {
				break;
			}
			auto close = path.find('}', open + 1);
			if (close == std::string_view::npos) {
				break;
			}
			auto name = path.substr(open + 1, close - open - 1);
			if (name.starts_with('*')) {
				name.remove_prefix(1);
			}
			std::string_view type;
			if (auto colon = name.find(':'); colon != std::string_view::npos) {
				type = name.substr(colon + 1);
				name = name.substr(0, colon);
			}
			if (!name.empty()) {
				out.emplace(std::string{name}, std::string{type});
			}
			pos = close + 1;
		}
		return out;
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
			.middleware_count = middleware_count_};
		append_extractors<Args>(meta.extractors, std::make_index_sequence<std::tuple_size_v<Args>>{});
		append_path_extractors<Args>(meta.path_extractors, std::make_index_sequence<std::tuple_size_v<Args>>{});
		append_path_extractor_types<Args>(
			meta.path_extractor_types,
			meta.path_index_extractor_types,
			std::make_index_sequence<std::tuple_size_v<Args>>{});
		meta.path_params = collect_path_params(path);
		meta.path_param_types = collect_path_param_types(path);
		append_required_states<Args>(meta.required_states, std::make_index_sequence<std::tuple_size_v<Args>>{});
		meta.uses_body = has_body_extractor<Args>() || handler_kind == "json_body";
		route_metadata_.push_back(std::move(meta));
	}

	template<class Return>
	static void apply_return_metadata(
		AppRouteMetadata &meta) {
		using Clean = typename detail::ResponseMetadataType<Return>::type;
		if constexpr (detail::JsonArg<Clean>) {
			meta.produces = {"application/json"};
		}
	}

	template<class Return>
	void record_return_metadata() {
		apply_return_metadata<Return>(route_metadata_.back());
	}

	template<class Arg>
	[[nodiscard]] static auto field_problem(
		std::string_view extractor,
		HttpFieldError const &err) {
		auto kind = [err] {
			switch (err.kind) {
			case HttpFieldErrorKind::missing     : return "missing";
			case HttpFieldErrorKind::empty       : return "empty";
			case HttpFieldErrorKind::invalid     : return "invalid";
			case HttpFieldErrorKind::out_of_range: return "out_of_range";
			}
			return "invalid";
		}();
		auto body = std::format(
			R"({{"code":"invalid_field","extractor":"{}","source":"{}","name":"{}","kind":"{}","detail":"{}"}})",
			extractor,
			http_field_source_name(err.source),
			err.name,
			kind,
			err.message);
		return HttpResponse::json(std::move(body), kHttpBadRequest, "Bad Request");
	}

#if CONFLUX_HAS_JSON
	[[nodiscard]] static std::string json_escape(
		std::string_view value) {
		std::string out;
		out.reserve(value.size());
		for (char ch: value) {
			switch (ch) {
			case '"' : out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default  : out += ch; break;
			}
		}
		return out;
	}

	[[nodiscard]] static std::string_view json_error_stage_name(
		conflux::json::boundary::ErrorStage stage) noexcept {
		using enum conflux::json::boundary::ErrorStage;
		switch (stage) {
		case parse   : return "parse";
		case lookup  : return "lookup";
		case decode  : return "decode";
		case build   : return "build";
		case dump    : return "dump";
		case provider: return "provider";
		}
		return "provider";
	}

	[[nodiscard]] static std::string_view json_error_code_name(
		conflux::json::boundary::ErrorCode code) noexcept {
		using enum conflux::json::boundary::ErrorCode;
		switch (code) {
		case provider_failure      : return "provider_failure";
		case syntax_error          : return "syntax_error";
		case unexpected_eof        : return "unexpected_eof";
		case trailing_garbage      : return "trailing_garbage";
		case input_too_large       : return "input_too_large";
		case string_too_large      : return "string_too_large";
		case nesting_too_deep      : return "nesting_too_deep";
		case wrong_kind            : return "wrong_kind";
		case missing_member        : return "missing_member";
		case index_out_of_range    : return "index_out_of_range";
		case invalid_number        : return "invalid_number";
		case number_out_of_range   : return "number_out_of_range";
		case sign_mismatch         : return "sign_mismatch";
		case duplicate_member      : return "duplicate_member";
		case invalid_unicode_escape: return "invalid_unicode_escape";
		case invalid_utf8          : return "invalid_utf8";
		case invalid_pointer       : return "invalid_pointer";
		case constraint_violation  : return "constraint_violation";
		case invalid_value         : return "invalid_value";
		case output_too_large      : return "output_too_large";
		case resource_exhausted    : return "resource_exhausted";
		}
		return "provider_failure";
	}

	[[nodiscard]] static HttpResponse json_decode_problem(
		conflux::json::boundary::Error const &err) {
		std::string body = std::format(
			R"({{"code":"invalid_json","stage":"{}","kind":"{}","detail":"{}")",
			json_error_stage_name(err.stage),
			json_error_code_name(err.code),
			json_escape(err.message));
		if (err.member_name) {
			body += std::format(R"(,"member":"{}")", json_escape(*err.member_name));
		}
		if (err.source) {
			body += std::format(
				R"(,"source":{{"offset":{},"line":{},"column":{}}})",
				err.source->offset,
				err.source->line,
				err.source->column);
		}
		body += "}";
		return HttpResponse::json(std::move(body), kHttpBadRequest, "Bad Request");
	}
#endif

	template<class T>
	[[nodiscard]] static T extract_or_throw(
		std::expected<T, HttpFieldError> value,
		std::string_view extractor) {
		if (!value) {
			throw ExtractorFailure{field_problem<T>(extractor, value.error())};
		}
		return std::move(*value);
	}

	[[nodiscard]] static std::optional<std::pair<std::string_view, std::string_view>> path_param_at(
		RequestView const &req,
		std::size_t index) noexcept {
		std::size_t i = 0;
		for (auto const &[name, value]: req.params) {
			if (i == index) {
				return std::pair<std::string_view, std::string_view>{name, value};
			}
			++i;
		}
		return std::nullopt;
	}

	template<class T>
	[[nodiscard]] static std::expected<T, HttpFieldError> path_param_as_at(
		RequestView const &req,
		std::size_t index) {
		auto param = path_param_at(req, index);
		auto name = std::format("#{}", index);
		if (!param) {
			auto err = HttpFieldError{
				.kind = HttpFieldErrorKind::missing,
				.source = HttpFieldSource::params,
				.name = std::move(name),
				.message = std::format("params field '#{}' is missing", index)};
			return std::unexpected{std::move(err)};
		}
		return parse_http_field_value<T>(param->second, HttpFieldSource::params, param->first);
	}

	template<class Arg>
	[[nodiscard]] static auto make_handler_arg(
		StateMap const &states,
		RequestView const &req) {
		using Clean = std::remove_cvref_t<Arg>;
		if constexpr (detail::RequestViewArg<Clean>) {
			return req;
		} else if constexpr (detail::RequestArg<Clean>) {
			return req.to_owned();
		} else if constexpr (detail::StateArg<Clean>) {
			using StateValue = typename detail::StateType<Clean>::type;
			auto const it = states.find(std::type_index{typeid(StateValue)});
			if (it == states.end()) {
				return State<StateValue>{};
			}
			return State<StateValue>{.value = static_cast<StateValue *>(it->second.get())};
		} else if constexpr (detail::PathArg<Clean>) {
			using PathValue = typename detail::PathType<Clean>::type;
			if constexpr (std::same_as<PathValue, std::string_view>) {
				return Clean{.value = req.param(detail::PathType<Clean>::name.view())};
			} else {
				return Clean{
					.value = extract_or_throw(
						req.template param_as<PathValue>(detail::PathType<Clean>::name.view()),
						"Path")};
			}
		} else if constexpr (detail::PathAtArg<Clean>) {
			using PathValue = typename detail::PathAtType<Clean>::type;
			if constexpr (std::same_as<PathValue, std::string_view>) {
				auto param = path_param_at(req, detail::PathAtType<Clean>::index);
				return Clean{.value = param ? param->second : std::string_view{}};
			} else {
				return Clean{
					.value =
						extract_or_throw(path_param_as_at<PathValue>(req, detail::PathAtType<Clean>::index), "PathAt")};
			}
		} else if constexpr (detail::QueryArg<Clean>) {
			using QueryValue = typename detail::QueryType<Clean>::type;
			if constexpr (std::same_as<QueryValue, std::string_view>) {
				return Clean{.value = req.query_value(detail::QueryType<Clean>::name.view())};
			} else {
				return Clean{
					.value = extract_or_throw(
						req.template query_as<QueryValue>(detail::QueryType<Clean>::name.view()),
						"Query")};
			}
		} else if constexpr (detail::HeaderArg<Clean>) {
			using HeaderValue = typename detail::HeaderType<Clean>::type;
			if constexpr (std::same_as<HeaderValue, std::string_view>) {
				return Clean{.value = req.header(detail::HeaderType<Clean>::name.view())};
			} else {
				return Clean{
					.value = extract_or_throw(
						req.template header_as<HeaderValue>(detail::HeaderType<Clean>::name.view()),
						"Header")};
			}
		} else if constexpr (detail::CookieArg<Clean>) {
			using CookieValue = typename detail::CookieType<Clean>::type;
			if constexpr (std::same_as<CookieValue, std::string_view>) {
				return Clean{.value = req.cookie(detail::CookieType<Clean>::name.view())};
			} else {
				return Clean{
					.value = extract_or_throw(
						req.template cookie_as<CookieValue>(detail::CookieType<Clean>::name.view()),
						"Cookie")};
			}
		} else if constexpr (detail::FormArg<Clean>) {
			using FormValue = typename detail::FormType<Clean>::type;
			if constexpr (std::same_as<FormValue, std::string_view>) {
				return Clean{.value = req.form_value(detail::FormType<Clean>::name.view())};
			} else {
				return Clean{
					.value = extract_or_throw(
						req.template form_as<FormValue>(detail::FormType<Clean>::name.view()),
						"Form")};
			}
		} else if constexpr (detail::BodyTextArg<Clean>) {
			return BodyText{.value = req.body};
		} else if constexpr (detail::BodyBytesArg<Clean>) {
			return BodyBytes{.value = req.body};
		} else if constexpr (detail::OwnedBodyBytesArg<Clean>) {
			return OwnedBodyBytes{.value = std::string{req.body}};
		} else if constexpr (detail::MultipartArg<Clean>) {
			return Multipart{.form = req.form, .files = req.files};
		} else if constexpr (detail::RequestIdArg<Clean>) {
			return RequestId{.value = req.header("x-request-id")};
		} else if constexpr (detail::ConnectionInfoArg<Clean>) {
			return ConnectionInfo{.remote_addr = req.remote_addr, .is_tls = req.is_tls};
		} else if constexpr (detail::TraceContextArg<Clean>) {
			return TraceContext{.traceparent = req.header("traceparent")};
		} else if constexpr (detail::BearerArg<Clean>) {
			auto token = credentials_for_scheme(req.header("authorization"), "Bearer");
			return Bearer{.token = token.value_or(std::string_view{})};
		} else if constexpr (detail::BasicAuthArg<Clean>) {
			auto credentials = credentials_for_scheme(req.header("authorization"), "Basic");
			if (!credentials) {
				return BasicAuth{};
			}
			auto decoded = base64_decode(*credentials);
			auto colon = decoded.find(':');
			if (colon == std::string::npos) {
				return BasicAuth{};
			}
			return BasicAuth{.username = decoded.substr(0, colon), .password = decoded.substr(colon + 1)};
		} else {
			static_assert(
				kDependentFalse<Arg>,
				"HTTP app handler argument must be http::RequestView, http::Request, http::Path<...>, "
				"http::PathAt<...>, http::Query<...>, http::Header<...>, http::Cookie<...>, http::Form<...>, "
				"http::BodyText, "
				"http::BodyBytes, http::OwnedBodyBytes, http::Multipart, http::RequestId, http::ConnectionInfo, "
				"http::TraceContext, http::Bearer, "
				"http::BasicAuth, or http::State<T>");
		}
	}

	template<class Args, class Fn, std::size_t... Is>
	[[nodiscard]] static HttpResponse invoke_extracted(
		StateMap const &states,
		Fn &fn,
		RequestView const &req,
		std::index_sequence<Is...>) {
		try {
			return into_response(fn(make_handler_arg<std::tuple_element_t<Is, Args>>(states, req)...));
		} catch (ExtractorFailure &failure) { return std::move(failure).response(); }
	}

	template<class Fn, class Args, std::size_t... Is>
	void record_extracted_return_metadata(
		std::index_sequence<Is...>) {
		record_return_metadata<std::invoke_result_t<Fn &, std::tuple_element_t<Is, Args>...>>();
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
		router_.add(
			method,
			path,
			[states = states_, fn = Fn(std::forward<F>(handler))](RequestView const &req) mutable {
				return invoke_extracted<Args>(*states, fn, req, std::make_index_sequence<std::tuple_size_v<Args>>{});
			});
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
			return make_handler_arg<Arg>(states, req);
		}
	}

	template<class Args, class Body, class Fn, std::size_t... Is>
	[[nodiscard]] static HttpResponse invoke_json_extracted(
		StateMap const &states,
		Fn &fn,
		RequestView const &req,
		Json<Body> const &body,
		std::index_sequence<Is...>) {
		try {
			return into_response(fn(make_json_handler_arg<std::tuple_element_t<Is, Args>>(states, req, body)...));
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
		auto max_body_size = route_metadata_.back().max_body_size;
		auto json_options = json_options_;
		router_.add(
			method,
			path,
			[states = states_,
			 fn = Fn(std::forward<F>(handler)),
			 decode_opts = std::move(decode_opts),
			 max_body_size,
			 json_options](RequestView const &req) mutable -> HttpResponse {
				auto content_type = req.header("content-type");
				if (!content_type.starts_with("application/json")
					&& !content_type.starts_with("application/problem+json")) {
					return HttpResponse::json(
						R"({"error":"unsupported content type","expected":"application/json"})",
						kHttpBadRequest,
						"Bad Request");
				}
				auto const limit = *max_body_size != 0 ? *max_body_size : json_options->max_body_size;
				if (limit != 0 && req.body.size() > limit) {
					return HttpResponse::content_too_large();
				}
				auto const &effective_decode_opts = decode_opts ? *decode_opts : json_options->decode;
				auto decoded = conflux::json::boundary::decode_with<json::DefaultJsonProvider, BodyValue>(
					req.body,
					effective_decode_opts);
				if (!decoded) {
					return json_decode_problem(decoded.error());
				}
				auto body = Json<BodyValue>{std::move(*decoded)};
				return invoke_json_extracted<Args>(
					*states,
					fn,
					req,
					body,
					std::make_index_sequence<std::tuple_size_v<Args>>{});
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
		return HttpServer::try_create(cfg_, std::move(router_));
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
		AppRunOptions opts = {}) && {
		auto report = validate();
		if (!report) {
			std::cerr << "http app validation failed:\n" << report.summary() << '\n';
			return RunStatus::fatal_internal_exception;
		}
		cfg_.port = opts.port;
		HttpServer srv{cfg_, std::move(router_)};
		return srv.run();
	}

private:
	Config cfg_;
	Router router_;
	std::shared_ptr<StateMap> states_;
	std::vector<std::string> state_issues_;
	std::vector<AppRouteMetadata> route_metadata_;
	std::vector<StaticMountMetadata> static_mounts_;
	std::size_t middleware_count_{};
#if CONFLUX_HAS_JSON
	std::shared_ptr<AppJsonOptions> json_options_;
#endif
};

[[nodiscard]] App app(
	Config cfg = Config::public_server()) {
	return App{std::move(cfg)};
}

[[nodiscard]] RunStatus run(
	App app,
	AppRunOptions opts = {}) {
	return std::move(app).run(opts);
}

} // namespace conflux::http
