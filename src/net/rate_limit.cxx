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
export namespace conflux::http::detail {

template<class Bucket>
class ShardedRateLimitStore {
	static constexpr std::size_t target_shards = 64;
	static constexpr std::size_t min_clients_per_shard = 1024;
	struct Shard {
		std::mutex mutex;
		conflux::support::StringLruMap<Bucket> buckets;
		explicit Shard(
			std::size_t capacity)
			: buckets{capacity} {}
	};
	std::vector<std::unique_ptr<Shard>> shards_;

public:
	explicit ShardedRateLimitStore(
		std::size_t max_clients) {
		auto const total = std::max<std::size_t>(max_clients, 1);
		auto const shard_count = std::min(target_shards, std::max<std::size_t>(1, total / min_clients_per_shard));
		shards_.reserve(shard_count);
		auto const base = total / shard_count;
		auto const extra = total % shard_count;
		for (std::size_t i = 0; i < shard_count; ++i) {
			shards_.push_back(std::make_unique<Shard>(base + (i < extra ? 1U : 0U)));
		}
	}
	[[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }
	template<class Factory, class Fn>
	[[nodiscard]] auto with_bucket(
		std::string_view key,
		Factory &&factory,
		Fn &&fn) {
		auto &shard = *shards_[std::hash<std::string_view>{}(key) % shards_.size()];
		std::scoped_lock const lock{shard.mutex};
		auto touched = shard.buckets.get_or_create(key, std::forward<Factory>(factory));
		return std::invoke(std::forward<Fn>(fn), *touched.value, touched.inserted);
	}
};

} // namespace conflux::http::detail
// Middleware factory: token-bucket rate limiter keyed on remote_addr.
// Thread-safe — shared across all rings via captured SP.
export Router::Middleware rate_limit_middleware(
	RateLimitOptions opts = {}) {
	struct State {
		conflux::http::detail::ShardedRateLimitStore<Bucket> store;
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
			auto r = Response::text("Too Many Requests", kHttpTooManyRequests);
			r.headers["Retry-After"] = std::format("{}", retry_after);
			return r;
		}
		return next(req);
	};
}
