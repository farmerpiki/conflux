module conflux.net.rate_limit;

import std;
namespace conflux::http {

Router::Middleware rate_limit_middleware(
	RateLimitOptions opts) {
	struct State {
		conflux::http::detail::ShardedRateLimitStore<Bucket> store;
		explicit State(
			std::size_t max_clients)
			: store(max_clients) {}
	};
	auto state = std::shared_ptr<State>{new State(std::max<std::size_t>(opts.max_clients, 1))};
	unsigned const capacity = opts.requests + opts.burst;

	return [opts, capacity, state](
			   conflux::http::RequestView const &req,
			   conflux::http::Router::Handler const &next) -> conflux::http::Response {
		auto const now = Clock::now();
		auto const key = detail::rate_limit_remote_key(req.remote_addr);

		bool allowed = false;
		auto retry_after = static_cast<unsigned>(opts.window.count());

		auto _ = state->store.with_bucket(
			key,
			[capacity, now] { return Bucket{.tokens = capacity, .window_start = now}; },
			[&](Bucket &bucket, bool) {
				auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - bucket.window_start);
				if (elapsed >= opts.window) {
					bucket.tokens = capacity;
					bucket.window_start = now;
					elapsed = std::chrono::seconds{0};
				}

				if (bucket.tokens > 0) {
					--bucket.tokens;
					allowed = true;
				} else {
					auto remaining = opts.window - elapsed;
					retry_after =
						static_cast<unsigned>(std::chrono::duration_cast<std::chrono::seconds>(remaining).count());
				}
				return 0;
			});

		if (!allowed) {
			auto r = conflux::http::Response::text("Too Many Requests", kHttpTooManyRequests);
			r.headers["Retry-After"] = std::format("{}", retry_after);
			return r;
		}
		return next(req);
	};
}

} // namespace conflux::http
