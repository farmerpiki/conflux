module;

export module conflux.net.rate_limit;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;
export struct RateLimitOptions {
	// Maximum requests allowed per window.
	unsigned requests{100};

	// Window duration. Tokens refill fully at each window boundary.
	std::chrono::seconds window{60};

	// Maximum burst above the base rate (extra tokens at window start).
	// Total capacity = requests + burst.
	unsigned burst{0};

	// Maximum number of distinct client IPs tracked simultaneously.
	// When the limit is reached, the least-recently-seen client is evicted.
	// Prevents unbounded memory growth under IP-spoofing DoS.
	std::size_t max_clients{65536};
};
using Clock = std::chrono::steady_clock;
struct Bucket {
	unsigned tokens{};
	Clock::time_point window_start{Clock::now()};
};
// LRU-bounded token bucket store. Not exported — module scope to satisfy GCC.
struct RateLimitStore {
	explicit RateLimitStore(
		std::size_t max_clients)
		: buckets_(max_clients) {}
	// Returns a reference to the bucket for `key`, creating it if absent.
	// Evicts the least-recently-seen entry when the store is full.
	// Transparent std::hash/equal lets the hot find() path work from SV
	// without allocating.
	Bucket &touch(
		std::string_view key,
		unsigned capacity,
		Clock::time_point now) {
		return *buckets_.get_or_create(key, [capacity, now] { return Bucket{.tokens = capacity, .window_start = now}; })
					.value;
	}

private:
	conflux::support::StringLruMap<Bucket> buckets_;
};
// Middleware factory: token-bucket rate limiter keyed on remote_addr.
// Thread-safe — shared across all rings via captured SP.
export Router::Middleware rate_limit_middleware(
	RateLimitOptions opts = {}) {
	struct State {
		std::mutex mtx;
		RateLimitStore store;
		explicit State(
			std::size_t max_clients)
			: store(max_clients) {}
	};
	auto state = std::make_shared<State>(std::max<std::size_t>(opts.max_clients, 1));
	unsigned const capacity = opts.requests + opts.burst;

	return [opts, capacity, state](RequestView const &req, Router::Handler const &next) -> Response {
		auto const now = Clock::now();
		auto const key = req.remote_addr.empty() ?
							 std::string{"unknown"} :
							 parse_ip(req.remote_addr).transform(ip_to_string).value_or(std::string{req.remote_addr});

		bool allowed = false;
		auto retry_after = static_cast<unsigned>(opts.window.count());

		{
			std::scoped_lock const lock{state->mtx};
			auto &bucket = state->store.touch(key, capacity, now);

			// Refill: new window → reset tokens to full capacity.
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
		}

		if (!allowed) {
			Response r;
			r.status = 429;
			r.status_text = "Too Many Requests";
			r.content_type = "text/plain; charset=utf-8";
			r.set_text_body("Too Many Requests");
			r.headers["Retry-After"] = std::format("{}", retry_after);
			return r;
		}
		return next(req);
	};
}
