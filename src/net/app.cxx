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
	requires(std::invocable<Fn &> && same_as<std::invoke_result_t<Fn &>, HttpResponse>)
[[nodiscard]] HttpResponse defer(
	std::shared_ptr<WorkPool> const &pool,
	Fn &&fn,
	chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	if (!pool) {
		return HttpResponse::internal_error("defer: null pool");
	}
	auto deferred = make_shared<DeferredResponse>(timeout);
	bool const enqueued = pool->enqueue([deferred, work = std::decay_t<Fn>(forward<Fn>(fn))]() mutable {
		try {
			deferred->complete(work());
		} catch (exception const &ex) { deferred->complete(HttpResponse::internal_error(ex.what())); } catch (...) {
			deferred->complete(HttpResponse::internal_error());
		}
	});
	if (!enqueued) {
		return HttpResponse::internal_error("offload queue full");
	}
	return HttpResponse::deferred(move(deferred));
}
template<typename Fn>
	requires(std::invocable<Fn &> && same_as<std::invoke_result_t<Fn &>, HttpResponse>)
[[nodiscard]] HttpResponse defer(
	WorkPool &pool,
	Fn &&fn,
	chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	auto deferred = make_shared<DeferredResponse>(timeout);
	bool const enqueued = pool.enqueue([deferred, work = std::decay_t<Fn>(forward<Fn>(fn))]() mutable {
		try {
			deferred->complete(work());
		} catch (exception const &ex) { deferred->complete(HttpResponse::internal_error(ex.what())); } catch (...) {
			deferred->complete(HttpResponse::internal_error());
		}
	});
	if (!enqueued) {
		return HttpResponse::internal_error("offload queue full");
	}
	return HttpResponse::deferred(move(deferred));
}
struct AppRunOptions {
	u16 port = kConfigDefaultPort;
};
class App {
public:
	[[nodiscard]] static App default_server() { return App{Config::low_latency()}; }
	explicit App(
		Config cfg = Config::low_latency())
		: cfg_(move(cfg))
		, router_(cfg_) {}
	template<typename F>
	App &get(
		std::string_view path,
		F &&handler) {
		router_.get(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &post(
		std::string_view path,
		F &&handler) {
		router_.post(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &put(
		std::string_view path,
		F &&handler) {
		router_.put(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &patch(
		std::string_view path,
		F &&handler) {
		router_.patch(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &del(
		std::string_view path,
		F &&handler) {
		router_.del(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &options(
		std::string_view path,
		F &&handler) {
		router_.options(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &get_context(
		std::string_view path,
		F &&handler) {
		router_.get_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &post_context(
		std::string_view path,
		F &&handler) {
		router_.post_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &put_context(
		std::string_view path,
		F &&handler) {
		router_.put_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &patch_context(
		std::string_view path,
		F &&handler) {
		router_.patch_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &del_context(
		std::string_view path,
		F &&handler) {
		router_.del_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &options_context(
		std::string_view path,
		F &&handler) {
		router_.options_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &use(
		F &&middleware) {
		router_.use(forward<F>(middleware));
		return *this;
	}
	template<typename F>
	App &sse(
		std::string_view path,
		F &&handler) {
		router_.sse(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &on_not_found(
		F &&handler) {
		router_.on_not_found(forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &on_error(
		F &&handler) {
		router_.on_error(forward<F>(handler));
		return *this;
	}
	template<typename F>
	App &group(
		std::string_view prefix,
		F &&fn) {
		router_.group(prefix, forward<F>(fn));
		return *this;
	}
	[[nodiscard]] Config &config() { return cfg_; }
	[[nodiscard]] Config const &config() const { return cfg_; }
	[[nodiscard]] Router &router() { return router_; }
	[[nodiscard]] Router const &router() const { return router_; }
	[[nodiscard]] expected<std::unique_ptr<HttpServer>, std::string> try_server(
		AppRunOptions opts = {}) && {
		cfg_.port = opts.port;
		return HttpServer::try_create(cfg_, move(router_));
	}
	[[nodiscard]] expected<RunStatus, std::string> try_run(
		AppRunOptions opts = {}) && {
		auto srv = move(*this).try_server(opts);
		if (!srv) {
			return unexpected{move(srv.error())};
		}
		return (*srv)->run();
	}
	[[nodiscard]] RunStatus run(
		AppRunOptions opts = {}) && {
		cfg_.port = opts.port;
		HttpServer srv{cfg_, move(router_)};
		return srv.run();
	}

private:
	Config cfg_;
	Router router_;
};

} // namespace conflux::http
