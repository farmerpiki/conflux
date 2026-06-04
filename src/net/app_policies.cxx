export module conflux.net.app.policies;

export import conflux.net.app.types;
import std;
import conflux.net.auth;
import conflux.net.http.response;
import conflux.net.http.server_types;
import conflux.net.http.types;
import conflux.net.rate_limit;
import conflux.utils;

export namespace conflux::http::detail {

struct AppRouteRateLimit {
	std::string name;
	AppRateLimitOptions options{};
	bool enabled{};

	struct Bucket {
		unsigned tokens{};
		std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};
	};

	std::optional<conflux::http::detail::ShardedRateLimitStore<Bucket>> buckets;
};

[[nodiscard]] std::optional<Response> route_auth_failure(
	std::string_view policy,
	conflux::http::RequestView const &req) {
	if (policy.empty()) {
		return std::nullopt;
	}
	auto token = credentials_for_auth_scheme(req.header("authorization"), "Bearer");
	if (!token || token->empty()) {
		return Response::unauthorized("Bearer");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<BasicAuth> parse_basic_auth(
	conflux::http::RequestView const &req) {
	auto credentials = conflux::http::parse_basic_credentials(req.header("authorization"));
	if (!credentials) {
		return std::nullopt;
	}
	return BasicAuth{.username = std::move(credentials->username), .password = std::move(credentials->password)};
}

[[nodiscard]] std::optional<Response> route_rate_limit_failure(
	AppRouteRateLimit &policy,
	conflux::http::RequestView const &req) {
	if (!policy.enabled) {
		return std::nullopt;
	}
	auto const capacity = policy.options.requests + policy.options.burst;
	if (capacity == 0) {
		auto response = Response::text("Too Many Requests", kHttpTooManyRequests);
		response.headers["Retry-After"] = std::format("{}", policy.options.window.count());
		return response;
	}

	auto const now = std::chrono::steady_clock::now();
	auto const key = req.remote_addr.empty() ? std::string{"unknown"} :
											   conflux::utils::parse_ip(req.remote_addr)
												   .transform(conflux::utils::ip_to_string)
												   .value_or(std::string{req.remote_addr});
	auto retry_after = static_cast<unsigned>(policy.options.window.count());
	bool allowed = false;

	auto _ = policy.buckets->with_bucket(
		key,
		[capacity, now] { return AppRouteRateLimit::Bucket{.tokens = capacity, .window_start = now}; },
		[&](AppRouteRateLimit::Bucket &bucket, bool inserted) {
			if (inserted) {
				bucket.tokens = capacity;
			}
			auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - bucket.window_start);
			if (elapsed >= policy.options.window) {
				bucket.tokens = capacity;
				bucket.window_start = now;
				elapsed = std::chrono::seconds{0};
			}

			if (bucket.tokens > 0) {
				--bucket.tokens;
				allowed = true;
			} else {
				auto remaining = policy.options.window - elapsed;
				retry_after =
					static_cast<unsigned>(std::chrono::duration_cast<std::chrono::seconds>(remaining).count());
			}
			return 0;
		});

	if (allowed) {
		return std::nullopt;
	}

	auto response = Response::text("Too Many Requests", kHttpTooManyRequests);
	response.headers["Retry-After"] = std::format("{}", retry_after);
	return response;
}

} // namespace conflux::http::detail
