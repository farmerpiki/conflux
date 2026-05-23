module;
#include <chrono>
#include <memory>

export module conflux.http.extended;

export import conflux.http;
export import conflux.work;
import std;
import conflux.net.app.defer;

export namespace conflux::http {

template<typename F>
	requires(std::invocable<F &> && std::same_as<std::invoke_result_t<F &>, Response>)
[[nodiscard]] Response offload(
	std::shared_ptr<WorkPool> const &pool,
	F &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	return defer(pool, std::forward<F>(fn), timeout);
}

template<typename F>
	requires(std::invocable<F &> && std::same_as<std::invoke_result_t<F &>, Response>)
[[nodiscard]] Response offload(
	WorkPool &pool,
	F &&fn,
	std::chrono::milliseconds timeout = DeferredResponse::kDefaultTimeout) {
	return defer(pool, std::forward<F>(fn), timeout);
}

} // namespace conflux::http
