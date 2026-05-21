module;

export module conflux.net.app.policies;

import std;
import conflux.net.http.types;
import conflux.net.http.response;
import conflux.net.http.request;
import conflux.net.http.server_types;
import conflux.net.app.route_helpers;

export namespace conflux::http {

struct AppRateLimitOptions {
	unsigned requests{100};
	std::chrono::seconds window{60};
	unsigned burst{0};
	std::size_t max_clients{65536};
};

struct AppRouteRateLimit {
	std::string name;
	AppRateLimitOptions options{};
	bool enabled{};

	struct Bucket {
		unsigned tokens{};
		std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};
	};

	std::mutex mutex;
	std::unordered_map<std::string, Bucket> buckets;
};

namespace detail {

[[nodiscard]] std::optional<HttpResponse> route_auth_failure(
	std::string_view policy,
	RequestView const &req) {
	if (policy.empty()) {
		return std::nullopt;
	}
	auto token = detail::credentials_for_scheme(req.header("authorization"), "Bearer");
	if (!token || token->empty()) {
		return HttpResponse::unauthorized("Bearer");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<HttpResponse> route_rate_limit_failure(
	AppRouteRateLimit &policy,
	RequestView const &req) {
	if (!policy.enabled) {
		return std::nullopt;
	}
	auto const capacity = policy.options.requests + policy.options.burst;
	if (capacity == 0) {
		HttpResponse response;
		response.status = 429;
		response.status_text = "Too Many Requests";
		response.content_type = "text/plain; charset=utf-8";
		response.set_text_body("Too Many Requests");
		response.headers["Retry-After"] = std::format("{}", policy.options.window.count());
		return response;
	}

	auto const now = std::chrono::steady_clock::now();
	auto const key = req.remote_addr.empty() ? std::string{"unknown"} : std::string{req.remote_addr};
	auto retry_after = static_cast<unsigned>(policy.options.window.count());

	{
		std::scoped_lock const lock{policy.mutex};
		auto const max_clients = std::max<std::size_t>(policy.options.max_clients, 1);
		if (policy.buckets.size() >= max_clients && !policy.buckets.contains(key)) {
			policy.buckets.erase(policy.buckets.begin());
		}
		auto [it, inserted] =
			policy.buckets.try_emplace(key, AppRouteRateLimit::Bucket{.tokens = capacity, .window_start = now});
		auto &bucket = it->second;
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
			return std::nullopt;
		}
		auto remaining = policy.options.window - elapsed;
		retry_after = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::seconds>(remaining).count());
	}

	HttpResponse response;
	response.status = 429;
	response.status_text = "Too Many Requests";
	response.content_type = "text/plain; charset=utf-8";
	response.set_text_body("Too Many Requests");
	response.headers["Retry-After"] = std::format("{}", retry_after);
	return response;
}

[[nodiscard]] HttpResponse apply_route_timeout(
	HttpResponse response,
	std::chrono::milliseconds timeout) {
	if (timeout.count() > 0 && response.is_deferred()) {
		if (auto const &deferred = response.deferred_response_ptr()) {
			deferred->set_deadline(std::chrono::steady_clock::now() + timeout);
		}
	}
	return response;
}

} // namespace detail

} // namespace conflux::http
