module;

export module conflux.net.app.defer;

import std;
import conflux.net.http.response;
import conflux.net.app.response;
import conflux.work;

export namespace conflux::http {

template<typename Fn>
	requires(std::invocable<Fn &> && IntoResponse<std::invoke_result_t<Fn &>>)
[[nodiscard]] Response defer(
	std::shared_ptr<WorkPool> const &pool,
	Fn &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	if (!pool) {
		return Response::internal_error("defer: null pool");
	}
	auto deferred = std::make_shared<DeferredResponse>(timeout);
	bool const enqueued = pool->enqueue([deferred, work = std::decay_t<Fn>(std::forward<Fn>(fn))]() mutable {
		try {
			deferred->complete(into_response(work()));
		} catch (std::exception const &ex) { deferred->complete(Response::internal_error(ex.what())); } catch (...) {
			deferred->complete(Response::internal_error());
		}
	});
	if (!enqueued) {
		return Response::internal_error("offload queue full");
	}
	return Response::deferred(std::move(deferred));
}

template<typename Fn>
	requires(std::invocable<Fn &> && IntoResponse<std::invoke_result_t<Fn &>>)
[[nodiscard]] Response defer(
	WorkPool &pool,
	Fn &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	auto deferred = std::make_shared<DeferredResponse>(timeout);
	bool const enqueued = pool.enqueue([deferred, work = std::decay_t<Fn>(std::forward<Fn>(fn))]() mutable {
		try {
			deferred->complete(into_response(work()));
		} catch (std::exception const &ex) { deferred->complete(Response::internal_error(ex.what())); } catch (...) {
			deferred->complete(Response::internal_error());
		}
	});
	if (!enqueued) {
		return Response::internal_error("offload queue full");
	}
	return Response::deferred(std::move(deferred));
}

} // namespace conflux::http
