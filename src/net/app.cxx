module;
#include <memory>

export module conflux.net.app;

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http_server;
import conflux.work;
export namespace conflux::http {

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
class App {
public:
	[[nodiscard]] static App default_server() { return App{Config::low_latency()}; }
	explicit App(
		Config cfg = Config::low_latency())
		: cfg_(std::move(cfg))
		, router_(cfg_) {}
	template<typename F>
	App &add(
		std::string_view method,
		std::string_view path,
		F &&handler) {
		router_.add(method, path, std::forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &get(
		std::string_view path,
		F &&handler) {
		return add("GET", path, std::forward<F>(handler));
	}
	template<typename F>
	App &post(
		std::string_view path,
		F &&handler) {
		return add("POST", path, std::forward<F>(handler));
	}
	template<typename F>
	App &put(
		std::string_view path,
		F &&handler) {
		return add("PUT", path, std::forward<F>(handler));
	}
	template<typename F>
	App &patch(
		std::string_view path,
		F &&handler) {
		return add("PATCH", path, std::forward<F>(handler));
	}
	template<typename F>
	App &del(
		std::string_view path,
		F &&handler) {
		return add("DELETE", path, std::forward<F>(handler));
	}
	template<typename F>
	App &options(
		std::string_view path,
		F &&handler) {
		return add("OPTIONS", path, std::forward<F>(handler));
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
		StaticOptions const &sopts = {}) {
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
	[[nodiscard]] std::expected<std::unique_ptr<HttpServer>, std::string> try_server(
		AppRunOptions opts = {}) && {
		cfg_.port = opts.port;
		return HttpServer::try_create(cfg_, std::move(router_));
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
		cfg_.port = opts.port;
		HttpServer srv{cfg_, std::move(router_)};
		return srv.run();
	}

private:
	Config cfg_;
	Router router_;
};

} // namespace conflux::http
