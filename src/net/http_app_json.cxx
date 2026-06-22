module;
#include <concepts>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

export module conflux.net.http.app_json;

import conflux.types;
import conflux.utils;
import conflux.net.app.response;
import conflux.net.app.types;
import conflux.net.router;
import conflux.net.http.response_json;

export namespace conflux::http::codec::json {

[[nodiscard]] inline Response decode_error_response() {
	auto response = Response::json(
		R"({"code":"json.decode.type_mismatch","detail":"json decode failed"})",
		kHttpBadRequest,
		"Bad Request");
	response.content_type = "application/problem+json";
	return response;
}

namespace detail {

struct StoredResponseOptions {
	int status{kHttpOk};
	std::string status_text{"OK"};
	conflux::json::boundary::DumpOptions dump{};

	[[nodiscard]] ResponseOptions view() const noexcept {
		return ResponseOptions{.status = status, .status_text = status_text, .dump = dump};
	}
};

[[nodiscard]] inline StoredResponseOptions store_response_options(
	ResponseOptions opts) {
	return StoredResponseOptions{
		.status = opts.status,
		.status_text = std::string{opts.status_text},
		.dump = opts.dump};
}

} // namespace detail

template<class Provider, class F>
concept JsonViewHandler =
	requires(std::decay_t<F> &fn, conflux::http::RequestView const &req, ResponseOptions const &opts) {
		{ response_or_internal_error_with<Provider>(std::invoke(fn, req), opts) } -> std::same_as<Response>;
	};

template<class Provider, class F>
concept JsonNullaryHandler = requires(std::decay_t<F> &fn, ResponseOptions const &opts) {
	{ response_or_internal_error_with<Provider>(std::invoke(fn), opts) } -> std::same_as<Response>;
};

template<class Provider, class F>
concept JsonRouteHandler = JsonViewHandler<Provider, F> || JsonNullaryHandler<Provider, F>;

template<class Provider, class Body, class F>
concept JsonBodyViewHandler = requires(
	std::decay_t<F> &fn,
	conflux::http::RequestView const &req,
	std::remove_cvref_t<Body> const &body,
	ResponseOptions const &opts) {
	{ response_or_internal_error_with<Provider>(std::invoke(fn, req, body), opts) } -> std::same_as<Response>;
};

template<class Provider, class Body, class F>
concept JsonBodyHandler =
	requires(std::decay_t<F> &fn, std::remove_cvref_t<Body> const &body, ResponseOptions const &opts) {
		{ response_or_internal_error_with<Provider>(std::invoke(fn, body), opts) } -> std::same_as<Response>;
	};

template<class Provider, class Body, class F>
concept JsonDecodedRouteHandler = conflux::json::boundary::JsonDecodeProvider<Provider, std::remove_cvref_t<Body>>
							   && (JsonBodyViewHandler<Provider, Body, F> || JsonBodyHandler<Provider, Body, F>);

template<class Derived, class Provider>
struct JsonRouteVerbAccessors {
	template<class F>
	auto &get(
		std::string_view path,
		F &&fn,
		ResponseOptions opts = {})
		requires JsonRouteHandler<Provider, F>
	{
		return static_cast<Derived &>(*this).add("GET", path, std::forward<F>(fn), opts);
	}

	template<class F>
	auto &post(
		std::string_view path,
		F &&fn,
		ResponseOptions opts = {})
		requires JsonRouteHandler<Provider, F>
	{
		return static_cast<Derived &>(*this).add("POST", path, std::forward<F>(fn), opts);
	}

	template<class Body, class F>
	auto &post_body(
		std::string_view path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {.copy_input = false})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return static_cast<Derived &>(*this)
			.template add_body<Body>("POST", path, std::forward<F>(fn), opts, decode_opts);
	}

	template<class Body, class F>
	auto &put_body(
		std::string_view path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {.copy_input = false})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return static_cast<Derived &>(*this)
			.template add_body<Body>("PUT", path, std::forward<F>(fn), opts, decode_opts);
	}

	template<class Body, class F>
	auto &patch_body(
		std::string_view path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {.copy_input = false})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return static_cast<Derived &>(*this)
			.template add_body<Body>("PATCH", path, std::forward<F>(fn), opts, decode_opts);
	}
};

template<class Provider, class F>
[[nodiscard]] conflux::http::Router::Handler make_handler_with(
	F &&fn,
	ResponseOptions opts = {})
	requires JsonRouteHandler<Provider, F>
{
	using Fn = std::decay_t<F>;
	auto stored_opts = detail::store_response_options(opts);
	return conflux::http::Router::Handler{
		[fn = Fn(std::forward<F>(fn)),
		 opts = std::move(stored_opts)](conflux::http::RequestView const &req) mutable -> Response {
			if constexpr (JsonViewHandler<Provider, Fn>) {
				return response_or_internal_error_with<Provider>(std::invoke(fn, req), opts.view());
			} else {
				return response_or_internal_error_with<Provider>(std::invoke(fn), opts.view());
			}
		}};
}

template<class Provider, class Body, class F>
[[nodiscard]] conflux::http::Router::Handler make_decode_handler_with(
	F &&fn,
	ResponseOptions opts = {},
	conflux::json::boundary::DecodeOptions decode_opts = {.copy_input = false})
	requires JsonDecodedRouteHandler<Provider, Body, F>
{
	using Fn = std::decay_t<F>;
	using BodyValue = std::remove_cvref_t<Body>;
	auto stored_opts = detail::store_response_options(opts);
	return conflux::http::Router::Handler{
		[fn = Fn(std::forward<F>(fn)), opts = std::move(stored_opts), decode_opts](
			conflux::http::RequestView const &req) mutable -> Response {
			auto decoded = conflux::json::boundary::decode_with<Provider, BodyValue>(req.body, decode_opts);
			if (!decoded) {
				return decode_error_response();
			}
			if constexpr (JsonBodyViewHandler<Provider, BodyValue, Fn>) {
				return response_or_internal_error_with<Provider>(std::invoke(fn, req, *decoded), opts.view());
			} else {
				return response_or_internal_error_with<Provider>(std::invoke(fn, *decoded), opts.view());
			}
		}};
}

template<class Provider>
class RouterJsonRoutes : public JsonRouteVerbAccessors<RouterJsonRoutes<Provider>, Provider> {
public:
	explicit RouterJsonRoutes(
		conflux::http::Router &router)
		: router_(&router) {}

	template<class F>
	conflux::http::Router &add(
		std::string_view method,
		std::string_view path,
		F &&fn,
		ResponseOptions opts = {})
		requires JsonRouteHandler<Provider, F>
	{
		return router_->add(method, path, make_handler_with<Provider>(std::forward<F>(fn), opts));
	}

	template<class Body, class F>
	conflux::http::Router &add_body(
		std::string_view method,
		std::string_view path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {.copy_input = false})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return router_->add(
			method,
			path,
			make_decode_handler_with<Provider, Body>(std::forward<F>(fn), opts, decode_opts));
	}

private:
	conflux::http::Router *router_{};
};

template<class Provider, class AppLike>
class AppJsonRoutes : public JsonRouteVerbAccessors<AppJsonRoutes<Provider, AppLike>, Provider> {
public:
	explicit AppJsonRoutes(
		AppLike &app)
		: app_(&app) {}

	template<class F>
	AppLike &add(
		std::string_view method,
		std::string_view path,
		F &&fn,
		ResponseOptions opts = {})
		requires JsonRouteHandler<Provider, F>
	{
		app_->add(method, path, make_handler_with<Provider>(std::forward<F>(fn), opts));
		return *app_;
	}

	template<class Body, class F>
	AppLike &add_body(
		std::string_view method,
		std::string_view path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {.copy_input = false})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		app_->add(method, path, make_decode_handler_with<Provider, Body>(std::forward<F>(fn), opts, decode_opts));
		return *app_;
	}

private:
	AppLike *app_{};
};

template<class Provider>
[[nodiscard]] RouterJsonRoutes<Provider> routes(
	conflux::http::Router &router) {
	return RouterJsonRoutes<Provider>{router};
}

template<class Provider, class AppLike>
	requires(!std::same_as<std::remove_cvref_t<AppLike>, conflux::http::Router>)
[[nodiscard]] AppJsonRoutes<Provider, AppLike> routes(
	AppLike &app) {
	return AppJsonRoutes<Provider, AppLike>{app};
}

} // namespace conflux::http::codec::json
