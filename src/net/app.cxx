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

using ServerRequestView = ::HttpRequestView;
using ServerRequest = ::HttpRequest;
using ServerResponse = ::HttpResponse;
using HttpFieldError = ::HttpFieldError;
using FieldError = ::HttpFieldError;
using FieldErrorKind = ::HttpFieldErrorKind;
using FieldSource = ::HttpFieldSource;
using ::http_field_as;
using ::http_field_optional_as;
using ::http_field_source_name;
using ::parse_http_field_value;
using RequestView = ServerRequestView;
using OwnedRequest = ServerRequest;
// First-contact alias stays view-backed for sync handlers. Coroutine handlers
// that may suspend should accept OwnedRequest/ServerRequest instead.
using Request = RequestView;
using Response = ServerResponse;
template<typename Fn>
	requires(std::invocable<Fn &> && same_as<std::invoke_result_t<Fn &>, Response>)
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
		} catch (exception const &ex) { deferred->complete(Response::internal_error(ex.what())); } catch (...) {
			deferred->complete(Response::internal_error());
		}
	});
	if (!enqueued) {
		return Response::internal_error("offload queue full");
	}
	return Response::deferred(move(deferred));
}
template<typename Fn>
	requires(std::invocable<Fn &> && same_as<std::invoke_result_t<Fn &>, Response>)
[[nodiscard]] Response defer(
	WorkPool &pool,
	Fn &&fn,
	chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	auto deferred = make_shared<DeferredResponse>(timeout);
	bool const enqueued = pool.enqueue([deferred, work = std::decay_t<Fn>(forward<Fn>(fn))]() mutable {
		try {
			deferred->complete(work());
		} catch (exception const &ex) { deferred->complete(Response::internal_error(ex.what())); } catch (...) {
			deferred->complete(Response::internal_error());
		}
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
	[[nodiscard]] static App default_server() { return App{Config::low_latency()}; }
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
	requires ContextHandlerFunction<F>
	App &get_context(
		SV path,
		F &&handler) {
		router_.get_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &post_context(
		SV path,
		F &&handler) {
		router_.post_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &put_context(
		SV path,
		F &&handler) {
		router_.put_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &patch_context(
		SV path,
		F &&handler) {
		router_.patch_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &del_context(
		SV path,
		F &&handler) {
		router_.del_context(path, forward<F>(handler));
		return *this;
	}
	template<typename F>
	requires ContextHandlerFunction<F>
	App &options_context(
		SV path,
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
	[[nodiscard]] expected<UP<HttpServer>, S> try_server(
		AppRunOptions opts = {}) && {
		cfg_.port = opts.port;
		return HttpServer::try_create(cfg_, move(router_));
	}
	[[nodiscard]] expected<RunStatus, S> try_run(
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
