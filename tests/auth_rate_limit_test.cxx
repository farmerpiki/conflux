// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.net.router;
import conflux.net.auth;

namespace {

using Clock = AuthThrottleClock;

[[nodiscard]] Clock::time_point at(
	int seconds) {
	return Clock::time_point{std::chrono::seconds{seconds}};
}

[[nodiscard]] RequestView make_auth_request() {
	static Request req;
	req = Request{};
	req.method = "POST";
	req.path = "/login";
	req.version = "HTTP/1.1";
	req.remote_addr = "127.0.0.1";
	req.form["username"] = "alice";
	req.query["user"] = "bob";
	req.headers["Authorization"] = "Bearer secret-token";
	return RequestView{req};
}

} // namespace

TEST_CASE(
	"auth throttle: failures lock a subject and expose retry/metrics",
	"[auth][rate-limit]") {
	AuthFailureLimiter limiter{
		AuthThrottleOptions{
							.max_failures = 2,
							.window = std::chrono::seconds{60},
							.lockout = std::chrono::seconds{30},
							.max_subjects = 16,
							}
    };

	CHECK(limiter.before_attempt("account:alice", at(100)).allowed);
	auto first = limiter.record_failure("account:alice", at(101));
	CHECK(first.allowed);
	CHECK(first.failures == 1);

	CHECK(limiter.before_attempt("account:alice", at(102)).allowed);
	auto second = limiter.record_failure("account:alice", at(103));
	CHECK_FALSE(second.allowed);
	CHECK(second.locked);
	CHECK(second.failures == 2);
	CHECK(second.retry_after == std::chrono::seconds{30});

	auto blocked = limiter.before_attempt("account:alice", at(110));
	CHECK_FALSE(blocked.allowed);
	CHECK(blocked.retry_after == std::chrono::seconds{23});

	CHECK(limiter.before_attempt("account:alice", at(133)).allowed);

	auto metrics = limiter.snapshot();
	CHECK(metrics.allowed_attempts == 3);
	CHECK(metrics.blocked_attempts == 1);
	CHECK(metrics.failures_recorded == 2);
	CHECK(metrics.tracked_subjects == 1);
}

TEST_CASE(
	"auth throttle: zero lockout blocks until the current window expires",
	"[auth][rate-limit]") {
	AuthFailureLimiter limiter{
		AuthThrottleOptions{
							.max_failures = 1,
							.window = std::chrono::seconds{10},
							.lockout = std::chrono::seconds{0},
							.max_subjects = 4,
							}
    };

	(void)limiter.record_failure("api-token:deadbeef", at(200));
	CHECK_FALSE(limiter.before_attempt("api-token:deadbeef", at(205)).allowed);
	CHECK(limiter.before_attempt("api-token:deadbeef", at(210)).allowed);
}

TEST_CASE(
	"auth throttle: success clears accumulated failures",
	"[auth][rate-limit]") {
	AuthFailureLimiter limiter{
		AuthThrottleOptions{
							.max_failures = 2,
							.window = std::chrono::seconds{60},
							.lockout = std::chrono::seconds{60},
							.max_subjects = 4,
							}
    };

	(void)limiter.record_failure("account:alice", at(1));
	limiter.record_success("account:alice");

	auto metrics = limiter.snapshot();
	CHECK(metrics.successes_recorded == 1);
	CHECK(metrics.tracked_subjects == 0);
	CHECK(limiter.before_attempt("account:alice", at(2)).allowed);
}

TEST_CASE(
	"auth throttle: subject store is bounded and evicts least-recently-used subjects",
	"[auth][rate-limit]") {
	AuthFailureLimiter limiter{
		AuthThrottleOptions{
							.max_failures = 3,
							.window = std::chrono::seconds{60},
							.lockout = std::chrono::seconds{60},
							.max_subjects = 2,
							}
    };

	(void)limiter.record_failure("account:a", at(1));
	(void)limiter.record_failure("account:b", at(2));
	(void)limiter.before_attempt("account:a", at(3)); // make a most-recently used
	(void)limiter.record_failure("account:c", at(4)); // evicts b

	auto metrics = limiter.snapshot();
	CHECK(metrics.subjects_evicted == 1);
	CHECK(metrics.tracked_subjects == 2);
	CHECK(limiter.before_attempt("account:b", at(5)).allowed);
}

TEST_CASE(
	"auth throttle: key helpers cover remote/account/query/bearer subjects",
	"[auth][rate-limit]") {
	auto req = make_auth_request();

	CHECK(auth_throttle_remote_key(req) == "remote:127.0.0.1");
	REQUIRE(auth_throttle_form_key(req, "username").has_value());
	CHECK(*auth_throttle_form_key(req, "username") == "account:alice");
	REQUIRE(auth_throttle_query_key(req, "user").has_value());
	CHECK(*auth_throttle_query_key(req, "user") == "account:bob");
	REQUIRE(auth_throttle_bearer_key(req).has_value());
	CHECK(auth_throttle_bearer_key(req)->starts_with("api-token:"));
	CHECK_FALSE(auth_throttle_bearer_key(req)->contains("secret-token"));
}

TEST_CASE(
	"auth throttle middleware: hooks record downstream auth failures",
	"[auth][rate-limit]") {
	AuthFailureLimiter limiter{
		AuthThrottleOptions{
							.max_failures = 1,
							.window = std::chrono::seconds{60},
							.lockout = std::chrono::seconds{60},
							.max_subjects = 4,
							}
    };
	auto middleware = auth_throttle_middleware(limiter, [](RequestView const &req) {
		return auth_throttle_form_key(req, "username");
	});
	auto req = make_auth_request();
	conflux::http::Router::Handler fail = [](RequestView const &) {
		Response r;
		r.status = 401;
		r.status_text = "Unauthorized";
		return r;
	};

	auto first = middleware(req, fail);
	CHECK(first.status == 401);
	auto second = middleware(req, fail);
	CHECK(second.status == 429);
	CHECK(second.headers["Retry-After"] == "60");

	auto metrics = limiter.snapshot();
	CHECK(metrics.failures_recorded == 1);
	CHECK(metrics.blocked_attempts == 1);
}
