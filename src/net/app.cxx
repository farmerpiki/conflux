export module conflux.net.app;

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.router;
import conflux.net.http_server;
import conflux.work;

export namespace conflux::http {

using Request = ::HttpRequestView;
using Response = ::HttpResponse;

template<typename Fn>
	requires(std::invocable<Fn &> && std::same_as<std::invoke_result_t<Fn &>, Response>)
[[nodiscard]] Response defer(
	SP<WorkPool> const &pool,
	Fn &&fn,
	chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	if (!pool) {
		return Response::internal_error("defer: null pool");
	}
	auto deferred = make_shared<DeferredResponse>(timeout);
	bool const enqueued = pool->enqueue([deferred, work = std::decay_t<Fn>(forward<Fn>(fn))]() mutable {
		try {
			deferred->complete(work());
		} catch (exception const &ex) {
			deferred->complete(Response::internal_error(ex.what()));
		} catch (...) { deferred->complete(Response::internal_error()); }
	});
	if (!enqueued) {
		return Response::internal_error("offload queue full");
	}
	return Response::deferred(move(deferred));
}

template<typename Fn>
	requires(std::invocable<Fn &> && std::same_as<std::invoke_result_t<Fn &>, Response>)
[[nodiscard]] Response defer(
	WorkPool &pool,
	Fn &&fn,
	chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	auto deferred = make_shared<DeferredResponse>(timeout);
	bool const enqueued = pool.enqueue([deferred, work = std::decay_t<Fn>(forward<Fn>(fn))]() mutable {
		try {
			deferred->complete(work());
		} catch (exception const &ex) {
			deferred->complete(Response::internal_error(ex.what()));
		} catch (...) { deferred->complete(Response::internal_error()); }
	});
	if (!enqueued) {
		return Response::internal_error("offload queue full");
	}
	return Response::deferred(move(deferred));
}

struct AppRunOptions {
	u16 port = kConfigDefaultPort;
};

class App {
public:
	[[nodiscard]] static App default_server() {
		return App{Config::low_latency()};
	}

	explicit App(
		Config cfg = Config::low_latency())
		: cfg_(move(cfg))
		, router_(cfg_) {}

	template<typename F>
	App &get(
		SV path,
		F &&handler) {
		router_.get(path, forward<F>(handler));
		return *this;
	}

	template<typename F>
	App &post(
		SV path,
		F &&handler) {
		router_.post(path, forward<F>(handler));
		return *this;
	}

	template<typename F>
	App &put(
		SV path,
		F &&handler) {
		router_.put(path, forward<F>(handler));
		return *this;
	}

	template<typename F>
	App &patch(
		SV path,
		F &&handler) {
		router_.patch(path, forward<F>(handler));
		return *this;
	}

	template<typename F>
	App &del(
		SV path,
		F &&handler) {
		router_.del(path, forward<F>(handler));
		return *this;
	}

	template<typename F>
	App &options(
		SV path,
		F &&handler) {
		router_.options(path, forward<F>(handler));
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
		SV path,
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
		SV prefix,
		F &&fn) {
		router_.group(prefix, forward<F>(fn));
		return *this;
	}

	[[nodiscard]] Config &config() { return cfg_; }
	[[nodiscard]] Config const &config() const { return cfg_; }

	[[nodiscard]] Router &router() { return router_; }
	[[nodiscard]] Router const &router() const { return router_; }

	void run(
		AppRunOptions opts = {}) && {
		cfg_.port = opts.port;
		HttpServer srv{cfg_, move(router_)};
		srv.run();
	}

private:
	Config cfg_;
	Router router_;
};

} // namespace conflux::http
