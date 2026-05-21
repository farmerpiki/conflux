module;

export module conflux.net.app.defer;

import std;
import conflux.net.http.response;
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

} // namespace conflux::http
