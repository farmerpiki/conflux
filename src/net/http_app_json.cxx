export module conflux.net.http.app_json;

import std;
import conflux.types;
import conflux.net.app;
import conflux.net.router;
import conflux.net.http.response_json;

export namespace conflux::http::json {

[[nodiscard]] inline HttpResponse decode_error_response() {
	return HttpResponse::json(R"({"error":"json decode failed"})", kHttpBadRequest, "Bad Request");
}

namespace detail {

struct StoredResponseOptions {
	int status{kHttpOk};
	S status_text{"OK"};
	conflux::json::boundary::DumpOptions dump{};

	[[nodiscard]] ResponseOptions view() const noexcept {
		return ResponseOptions{.status = status, .status_text = status_text, .dump = dump};
	}
};

[[nodiscard]] inline StoredResponseOptions store_response_options(
	ResponseOptions opts) {
	return StoredResponseOptions{.status = opts.status, .status_text = S{opts.status_text}, .dump = opts.dump};
}

} // namespace detail

template<class Provider, class F>
concept JsonViewHandler = requires(std::decay_t<F> &fn, HttpRequestView const &req, ResponseOptions const &opts) {
	{ response_or_internal_error_with<Provider>(std::invoke(fn, req), opts) } -> same_as<HttpResponse>;
};

template<class Provider, class F>
concept JsonNullaryHandler = requires(std::decay_t<F> &fn, ResponseOptions const &opts) {
	{ response_or_internal_error_with<Provider>(std::invoke(fn), opts) } -> same_as<HttpResponse>;
};

template<class Provider, class F>
concept JsonRouteHandler = JsonViewHandler<Provider, F> || JsonNullaryHandler<Provider, F>;

template<class Provider, class Body, class F>
concept JsonBodyViewHandler = requires(
	std::decay_t<F> &fn,
	HttpRequestView const &req,
	std::remove_cvref_t<Body> const &body,
	ResponseOptions const &opts) {
	{ response_or_internal_error_with<Provider>(std::invoke(fn, req, body), opts) } -> same_as<HttpResponse>;
};

template<class Provider, class Body, class F>
concept JsonBodyHandler = requires(
	std::decay_t<F> &fn,
	std::remove_cvref_t<Body> const &body,
	ResponseOptions const &opts) {
	{ response_or_internal_error_with<Provider>(std::invoke(fn, body), opts) } -> same_as<HttpResponse>;
};

template<class Provider, class Body, class F>
concept JsonDecodedRouteHandler =
	conflux::json::boundary::JsonDecodeProvider<Provider, std::remove_cvref_t<Body>>
	&& (JsonBodyViewHandler<Provider, Body, F> || JsonBodyHandler<Provider, Body, F>);

template<class Provider, class F>
[[nodiscard]] Router::Handler make_handler_with(
	F &&fn,
	ResponseOptions opts = {})
	requires JsonRouteHandler<Provider, F>
{
	using Fn = std::decay_t<F>;
	auto stored_opts = detail::store_response_options(opts);
	return Router::Handler{[fn = Fn(forward<F>(fn)), opts = move(stored_opts)](HttpRequestView const &req) mutable -> HttpResponse {
		if constexpr (JsonViewHandler<Provider, Fn>) {
			return response_or_internal_error_with<Provider>(std::invoke(fn, req), opts.view());
		} else {
			return response_or_internal_error_with<Provider>(std::invoke(fn), opts.view());
		}
	}};
}

template<class Provider, class Body, class F>
[[nodiscard]] Router::Handler make_decode_handler_with(
	F &&fn,
	ResponseOptions opts = {},
	conflux::json::boundary::DecodeOptions decode_opts = {})
	requires JsonDecodedRouteHandler<Provider, Body, F>
{
	using Fn = std::decay_t<F>;
	using BodyValue = std::remove_cvref_t<Body>;
	auto stored_opts = detail::store_response_options(opts);
	return Router::Handler{[fn = Fn(forward<F>(fn)), opts = move(stored_opts), decode_opts](HttpRequestView const &req) mutable -> HttpResponse {
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
class RouterJsonRoutes {
public:
	explicit RouterJsonRoutes(
		Router &router)
		: router_(&router) {}

	template<class F>
	Router &add(
		SV method,
		SV path,
		F &&fn,
		ResponseOptions opts = {})
		requires JsonRouteHandler<Provider, F>
	{
		return router_->add(method, path, make_handler_with<Provider>(forward<F>(fn), opts));
	}

	template<class Body, class F>
	Router &add_body(
		SV method,
		SV path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return router_->add(method, path, make_decode_handler_with<Provider, Body>(forward<F>(fn), opts, decode_opts));
	}

	template<class F>
	Router &get(
		SV path,
		F &&fn,
		ResponseOptions opts = {})
		requires JsonRouteHandler<Provider, F>
	{
		return add("GET", path, forward<F>(fn), opts);
	}

	template<class F>
	Router &post(
		SV path,
		F &&fn,
		ResponseOptions opts = {})
		requires JsonRouteHandler<Provider, F>
	{
		return add("POST", path, forward<F>(fn), opts);
	}

	template<class Body, class F>
	Router &post_body(
		SV path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return add_body<Body>("POST", path, forward<F>(fn), opts, decode_opts);
	}

	template<class Body, class F>
	Router &put_body(
		SV path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return add_body<Body>("PUT", path, forward<F>(fn), opts, decode_opts);
	}

	template<class Body, class F>
	Router &patch_body(
		SV path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return add_body<Body>("PATCH", path, forward<F>(fn), opts, decode_opts);
	}

private:
	Router *router_{};
};

template<class Provider>
class AppJsonRoutes {
public:
	explicit AppJsonRoutes(
		App &app)
		: app_(&app) {}

	template<class F>
	App &add(
		SV method,
		SV path,
		F &&fn,
		ResponseOptions opts = {})
		requires JsonRouteHandler<Provider, F>
	{
		app_->router().add(method, path, make_handler_with<Provider>(forward<F>(fn), opts));
		return *app_;
	}

	template<class Body, class F>
	App &add_body(
		SV method,
		SV path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		app_->router().add(method, path, make_decode_handler_with<Provider, Body>(forward<F>(fn), opts, decode_opts));
		return *app_;
	}

	template<class F>
	App &get(
		SV path,
		F &&fn,
		ResponseOptions opts = {})
		requires JsonRouteHandler<Provider, F>
	{
		return add("GET", path, forward<F>(fn), opts);
	}

	template<class F>
	App &post(
		SV path,
		F &&fn,
		ResponseOptions opts = {})
		requires JsonRouteHandler<Provider, F>
	{
		return add("POST", path, forward<F>(fn), opts);
	}

	template<class Body, class F>
	App &post_body(
		SV path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return add_body<Body>("POST", path, forward<F>(fn), opts, decode_opts);
	}

	template<class Body, class F>
	App &put_body(
		SV path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return add_body<Body>("PUT", path, forward<F>(fn), opts, decode_opts);
	}

	template<class Body, class F>
	App &patch_body(
		SV path,
		F &&fn,
		ResponseOptions opts = {},
		conflux::json::boundary::DecodeOptions decode_opts = {})
		requires JsonDecodedRouteHandler<Provider, Body, F>
	{
		return add_body<Body>("PATCH", path, forward<F>(fn), opts, decode_opts);
	}

private:
	App *app_{};
};

template<class Provider>
[[nodiscard]] RouterJsonRoutes<Provider> routes(
	Router &router) {
	return RouterJsonRoutes<Provider>{router};
}

template<class Provider>
[[nodiscard]] AppJsonRoutes<Provider> routes(
	App &app) {
	return AppJsonRoutes<Provider>{app};
}

} // namespace conflux::http::json
