export module conflux.net.auth;
import std;
import conflux.types;
import conflux.crypto;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;
import conflux.utils;

export struct BasicAuthOptions {
	std::string realm{"Restricted"};

	// Failed Basic-auth attempts allowed per remote address during the window.
	// 0 disables this guard for deployments that enforce login throttling elsewhere.
	unsigned failed_attempts{10};
	std::chrono::seconds failed_window{std::chrono::minutes{5}};

	// Maximum remote addresses tracked by the failure limiter.
	std::size_t max_failed_clients{65536};
};

export struct BasicCredentials {
	std::string username;
	std::string password;
};

export std::optional<BasicCredentials> parse_basic_credentials(
	std::string_view authorization) {
	auto credentials = conflux::http::credentials_for_auth_scheme(authorization, "Basic");
	if (!credentials) {
		return std::nullopt;
	}
	auto decoded = base64_decode(*credentials);
	auto colon = decoded.find(':');
	if (colon == std::string::npos) {
		return std::nullopt;
	}
	return BasicCredentials{.username = decoded.substr(0, colon), .password = decoded.substr(colon + 1)};
}

namespace auth_detail {

using Clock = std::chrono::steady_clock;

struct FailedAuthBucket {
	unsigned failures{};
	Clock::time_point window_start{Clock::now()};
};

struct FailedAuthStore {
	explicit FailedAuthStore(
		std::size_t max_entries)
		: buckets_(max_entries) {}

	[[nodiscard]] FailedAuthBucket *find(
		std::string_view key) noexcept {
		return buckets_.find(key);
	}

	[[nodiscard]] FailedAuthBucket &touch(
		std::string_view key,
		Clock::time_point now) {
		return *buckets_.get_or_create(key, [now] { return FailedAuthBucket{.failures = 0, .window_start = now}; })
					.value;
	}

	void erase(
		std::string_view key) noexcept {
		(void)buckets_.erase(key);
	}

private:
	conflux::support::StringLruMap<FailedAuthBucket> buckets_;
};

struct FailedAuthState {
	std::mutex mtx;
	FailedAuthStore store;

	explicit FailedAuthState(
		std::size_t max_attempts)
		: store(std::max<std::size_t>(max_attempts, 1)) {}
};

[[nodiscard]] std::string failed_auth_key(
	conflux::http::RequestView const &req) {
	if (req.remote_addr.empty()) {
		return "unknown";
	}
	return std::string{req.remote_addr};
}

conflux::http::Response unauthorized(
	std::string_view www_auth) {
	auto r = conflux::http::Response::text("Unauthorized", kHttpUnauthorized, "Unauthorized");
	r.headers["WWW-Authenticate"] = std::string{www_auth};
	return r;
}

conflux::http::Response too_many_auth_attempts(
	std::chrono::seconds retry_after) {
	auto r = conflux::http::Response::text("Too Many Requests", kHttpTooManyRequests);
	r.headers["Retry-After"] =
		std::format("{}", std::max<std::chrono::seconds::rep>(std::chrono::seconds::rep{1}, retry_after.count()));
	return r;
}

[[nodiscard]] bool basic_auth_limiter_enabled(
	BasicAuthOptions const &opts) noexcept {
	return opts.failed_attempts != 0U && opts.failed_window.count() > 0;
}

[[nodiscard]] std::optional<std::chrono::seconds> basic_auth_retry_after(
	FailedAuthState &state,
	BasicAuthOptions const &opts,
	std::string_view key,
	Clock::time_point now) {
	if (!basic_auth_limiter_enabled(opts)) {
		return std::nullopt;
	}
	std::scoped_lock const lock{state.mtx};
	auto *bucket = state.store.find(key);
	if (bucket == nullptr) {
		return std::nullopt;
	}
	auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - bucket->window_start);
	if (elapsed >= opts.failed_window) {
		state.store.erase(key);
		return std::nullopt;
	}
	if (bucket->failures < opts.failed_attempts) {
		return std::nullopt;
	}
	return std::max(std::chrono::seconds{1}, opts.failed_window - elapsed);
}

void record_basic_auth_failure(
	FailedAuthState &state,
	BasicAuthOptions const &opts,
	std::string_view key,
	Clock::time_point now) {
	if (!basic_auth_limiter_enabled(opts)) {
		return;
	}
	std::scoped_lock const lock{state.mtx};
	auto &bucket = state.store.touch(key, now);
	auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - bucket.window_start);
	if (elapsed >= opts.failed_window) {
		bucket.failures = 0;
		bucket.window_start = now;
	}
	if (bucket.failures < std::numeric_limits<unsigned>::max()) {
		++bucket.failures;
	}
}

void clear_basic_auth_failures(
	FailedAuthState &state,
	BasicAuthOptions const &opts,
	std::string_view key) {
	if (!basic_auth_limiter_enabled(opts)) {
		return;
	}
	std::scoped_lock const lock{state.mtx};
	state.store.erase(key);
}

} // namespace auth_detail

// Middleware factory: HTTP Basic Authentication guard.
// validator(username, password) → true = allow, false = 401.

export using AuthThrottleClock = std::chrono::steady_clock;

export struct AuthThrottleOptions {
	// Failed attempts allowed per subject during the window. 0 disables throttling.
	unsigned max_failures{5};
	std::chrono::seconds window{std::chrono::minutes{5}};
	// Additional lockout after the threshold is reached. 0 blocks until the current
	// window expires instead of starting a separate lockout period.
	std::chrono::seconds lockout{std::chrono::minutes{5}};
	// Maximum distinct subjects tracked simultaneously. Clamped to at least one.
	std::size_t max_subjects{65536};
};

export struct AuthThrottleOutcome {
	bool allowed{true};
	std::chrono::seconds retry_after{0};
	unsigned failures{0};
	bool locked{false};
};

export struct AuthThrottleMetrics {
	std::uint64_t allowed_attempts{};
	std::uint64_t blocked_attempts{};
	std::uint64_t failures_recorded{};
	std::uint64_t successes_recorded{};
	std::uint64_t subjects_evicted{};
	std::size_t tracked_subjects{};
};

export class AuthFailureLimiter {
public:
	AuthFailureLimiter();
	explicit AuthFailureLimiter(AuthThrottleOptions opts);

	[[nodiscard]] AuthThrottleOutcome
	before_attempt(std::string_view subject, AuthThrottleClock::time_point now = AuthThrottleClock::now());
	[[nodiscard]] AuthThrottleOutcome
	record_failure(std::string_view subject, AuthThrottleClock::time_point now = AuthThrottleClock::now());
	void record_success(std::string_view subject);
	void clear(std::string_view subject);
	[[nodiscard]] AuthThrottleMetrics snapshot() const;
	[[nodiscard]] AuthThrottleOptions options() const noexcept { return opts_; }

private:
	std::shared_ptr<void> state_{};
	AuthThrottleOptions opts_{};
};

export struct AuthThrottleMiddlewareOptions {
	std::vector<unsigned> failure_statuses{401, 403};
	bool clear_on_success{true};
	unsigned success_status_min{200};
	unsigned success_status_max{399};
};

namespace auth_detail {

struct AuthThrottleBucket {
	unsigned failures{};
	AuthThrottleClock::time_point window_start{AuthThrottleClock::now()};
	AuthThrottleClock::time_point locked_until{};
};

struct AuthThrottleState {
	std::mutex mtx;
	conflux::support::StringLruMap<AuthThrottleBucket> buckets;
	AuthThrottleMetrics metrics{};

	explicit AuthThrottleState(
		std::size_t max_subjects_hint)
		: buckets(max_subjects_hint) {}

	[[nodiscard]] AuthThrottleBucket *find(
		std::string_view subject) noexcept {
		return buckets.find(subject);
	}

	[[nodiscard]] AuthThrottleBucket &touch(
		std::string_view subject,
		AuthThrottleClock::time_point now) {
		auto result =
			buckets.get_or_create(subject, [now] { return AuthThrottleBucket{.failures = 0, .window_start = now}; });
		if (result.evicted) {
			++metrics.subjects_evicted;
		}
		return *result.value;
	}

	void erase(
		std::string_view subject) noexcept {
		(void)buckets.erase(subject);
	}
};

[[nodiscard]] bool auth_throttle_enabled(
	AuthThrottleOptions const &opts) noexcept {
	return opts.max_failures != 0U && opts.window.count() > 0;
}

[[nodiscard]] std::chrono::seconds retry_after_until(
	AuthThrottleClock::time_point deadline,
	AuthThrottleClock::time_point now) {
	if (deadline <= now) {
		return std::chrono::seconds{1};
	}
	auto const remaining = std::chrono::ceil<std::chrono::seconds>(deadline - now);
	return std::max(std::chrono::seconds{1}, remaining);
}

[[nodiscard]] AuthThrottleState &auth_throttle_state(
	std::shared_ptr<void> const &state) noexcept {
	return *std::static_pointer_cast<AuthThrottleState>(state);
}

void refresh_auth_bucket_window(
	AuthThrottleBucket &bucket,
	AuthThrottleOptions const &opts,
	AuthThrottleClock::time_point now) {
	if (bucket.locked_until != AuthThrottleClock::time_point{} && now >= bucket.locked_until) {
		bucket.failures = 0;
		bucket.window_start = now;
		bucket.locked_until = {};
		return;
	}
	auto const elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - bucket.window_start);
	if (elapsed >= opts.window) {
		bucket.failures = 0;
		bucket.window_start = now;
		bucket.locked_until = {};
	}
}

[[nodiscard]] AuthThrottleOutcome auth_bucket_outcome(
	AuthThrottleBucket const &bucket,
	AuthThrottleOptions const &opts,
	AuthThrottleClock::time_point now) {
	if (!auth_throttle_enabled(opts)) {
		return {.allowed = true, .failures = bucket.failures};
	}
	if (bucket.locked_until != AuthThrottleClock::time_point{} && now < bucket.locked_until) {
		return {
			.allowed = false,
			.retry_after = retry_after_until(bucket.locked_until, now),
			.failures = bucket.failures,
			.locked = true,
		};
	}
	if (bucket.failures >= opts.max_failures) {
		auto const deadline = bucket.window_start + opts.window;
		return {
			.allowed = false,
			.retry_after = retry_after_until(deadline, now),
			.failures = bucket.failures,
			.locked = true,
		};
	}
	return {.allowed = true, .failures = bucket.failures};
}

template<typename Key>
[[nodiscard]] std::optional<std::string> normalize_auth_throttle_key(
	Key &&key) {
	using K = std::remove_cvref_t<Key>;
	if constexpr (std::same_as<K, std::optional<std::string>>) {
		return std::forward<Key>(key);
	} else if constexpr (std::same_as<K, std::string>) {
		if (key.empty()) {
			return std::nullopt;
		}
		return std::string{std::forward<Key>(key)};
	} else if constexpr (std::same_as<K, std::string_view>) {
		if (key.empty()) {
			return std::nullopt;
		}
		return std::string{key};
	} else {
		if (!key || key->empty()) {
			return std::nullopt;
		}
		return std::string{*key};
	}
}

[[nodiscard]] bool auth_response_is_failure(
	conflux::http::Response const &response,
	AuthThrottleMiddlewareOptions const &opts) {
	return response.status >= 0
		&& std::ranges::find(opts.failure_statuses, static_cast<unsigned>(response.status))
			   != opts.failure_statuses.end();
}

[[nodiscard]] bool auth_response_is_success(
	conflux::http::Response const &response,
	AuthThrottleMiddlewareOptions const &opts) noexcept {
	return response.status >= 0
		&& static_cast<unsigned>(response.status) >= opts.success_status_min
		&& static_cast<unsigned>(response.status) <= opts.success_status_max;
}

} // namespace auth_detail

AuthFailureLimiter::AuthFailureLimiter()
	: AuthFailureLimiter(AuthThrottleOptions{}) {}

AuthFailureLimiter::AuthFailureLimiter(
	AuthThrottleOptions opts)
	: state_(std::make_shared<auth_detail::AuthThrottleState>(std::max<std::size_t>(opts.max_subjects, 1)))
	, opts_(opts) {}

AuthThrottleOutcome AuthFailureLimiter::before_attempt(
	std::string_view subject,
	AuthThrottleClock::time_point now) {
	if (subject.empty() || !auth_detail::auth_throttle_enabled(opts_)) {
		return {.allowed = true};
	}
	auto &state = auth_detail::auth_throttle_state(state_);
	std::scoped_lock const lock{state.mtx};
	auto *bucket = state.find(subject);
	if (bucket == nullptr) {
		++state.metrics.allowed_attempts;
		return {.allowed = true};
	}
	auth_detail::refresh_auth_bucket_window(*bucket, opts_, now);
	auto out = auth_detail::auth_bucket_outcome(*bucket, opts_, now);
	if (out.allowed) {
		++state.metrics.allowed_attempts;
	} else {
		++state.metrics.blocked_attempts;
	}
	return out;
}

AuthThrottleOutcome AuthFailureLimiter::record_failure(
	std::string_view subject,
	AuthThrottleClock::time_point now) {
	if (subject.empty() || !auth_detail::auth_throttle_enabled(opts_)) {
		return {.allowed = true};
	}
	auto &state = auth_detail::auth_throttle_state(state_);
	std::scoped_lock const lock{state.mtx};
	auto &bucket = state.touch(subject, now);
	auth_detail::refresh_auth_bucket_window(bucket, opts_, now);
	if (bucket.failures < std::numeric_limits<unsigned>::max()) {
		++bucket.failures;
	}
	++state.metrics.failures_recorded;
	if (bucket.failures >= opts_.max_failures && opts_.lockout.count() > 0) {
		bucket.locked_until = now + opts_.lockout;
	}
	return auth_detail::auth_bucket_outcome(bucket, opts_, now);
}

void AuthFailureLimiter::record_success(
	std::string_view subject) {
	if (subject.empty()) {
		return;
	}
	auto &state = auth_detail::auth_throttle_state(state_);
	std::scoped_lock const lock{state.mtx};
	state.erase(subject);
	++state.metrics.successes_recorded;
}

void AuthFailureLimiter::clear(
	std::string_view subject) {
	if (subject.empty()) {
		return;
	}
	auto &state = auth_detail::auth_throttle_state(state_);
	std::scoped_lock const lock{state.mtx};
	state.erase(subject);
}

AuthThrottleMetrics AuthFailureLimiter::snapshot() const {
	auto &state = auth_detail::auth_throttle_state(state_);
	std::scoped_lock const lock{state.mtx};
	auto out = state.metrics;
	out.tracked_subjects = state.buckets.size();
	return out;
}

export [[nodiscard]] std::string auth_throttle_key(
	std::string_view scope,
	std::string_view subject) {
	return std::format("{}:{}", scope, subject);
}

export [[nodiscard]] std::string auth_throttle_remote_key(
	conflux::http::RequestView const &req,
	std::string_view scope = "remote") {
	auto subject = req.remote_addr.empty() ?
					   std::string{"unknown"} :
					   parse_ip(req.remote_addr).transform(ip_to_string).value_or(std::string{req.remote_addr});
	return auth_throttle_key(scope, subject);
}

export [[nodiscard]] std::optional<std::string> auth_throttle_form_key(
	conflux::http::RequestView const &req,
	std::string_view field,
	std::string_view scope = "account") {
	auto value = req.form[field];
	if (value.empty()) {
		return std::nullopt;
	}
	return auth_throttle_key(scope, value);
}

export [[nodiscard]] std::optional<std::string> auth_throttle_query_key(
	conflux::http::RequestView const &req,
	std::string_view field,
	std::string_view scope = "account") {
	auto value = req.query[field];
	if (value.empty()) {
		return std::nullopt;
	}
	return auth_throttle_key(scope, value);
}

export [[nodiscard]] std::optional<std::string> auth_throttle_bearer_key(
	conflux::http::RequestView const &req,
	std::string_view scope = "api-token") {
	auto credentials = conflux::http::credentials_for_auth_scheme(req.headers["authorization"], "Bearer");
	if (!credentials) {
		return std::nullopt;
	}
	if (credentials->empty()) {
		return std::nullopt;
	}
	auto digest = sha256(to_unsigned_span(*credentials));
	return auth_throttle_key(scope, base64url_encode(digest));
}

export inline conflux::http::Response auth_throttle_too_many_requests(
	AuthThrottleOutcome const &outcome) {
	return auth_detail::too_many_auth_attempts(outcome.retry_after);
}

export template<typename KeySelector>
conflux::http::Router::Middleware auth_throttle_middleware(
	AuthFailureLimiter limiter,
	KeySelector &&selector,
	AuthThrottleMiddlewareOptions opts = {}) {
	return [limiter = std::move(limiter),
			selector = std::decay_t<KeySelector>(std::forward<KeySelector>(selector)),
			opts = std::move(opts)](
			   conflux::http::RequestView const &req,
			   conflux::http::Router::Handler const &next) mutable -> conflux::http::Response {
		auto key = auth_detail::normalize_auth_throttle_key(selector(req));
		if (!key) {
			return next(req);
		}
		if (auto gate = limiter.before_attempt(*key); !gate.allowed) {
			return auth_throttle_too_many_requests(gate);
		}
		auto response = next(req);
		if (auth_detail::auth_response_is_failure(response, opts)) {
			(void)limiter.record_failure(*key);
		} else if (opts.clear_on_success && auth_detail::auth_response_is_success(response, opts)) {
			limiter.record_success(*key);
		}
		return response;
	};
}
export template<typename Validator>
conflux::http::Router::Middleware basic_auth_middleware(
	Validator &&validator,
	BasicAuthOptions opts) {
	auto state = std::make_shared<auth_detail::FailedAuthState>(std::max<std::size_t>(opts.max_failed_clients, 1));
	return [v = std::decay_t<Validator>(std::forward<Validator>(validator)),
			opts = std::move(opts),
			state](conflux::http::RequestView const &req, conflux::http::Router::Handler const &next) -> conflux::http::Response {
		std::string const limiter_key = auth_detail::failed_auth_key(req);
		auto const now = auth_detail::Clock::now();
		if (auto retry_after = auth_detail::basic_auth_retry_after(*state, opts, limiter_key, now)) {
			return auth_detail::too_many_auth_attempts(*retry_after);
		}

		auto credentials = parse_basic_credentials(req.headers["authorization"]);
		if (!credentials) {
			auth_detail::record_basic_auth_failure(*state, opts, limiter_key, now);
			return auth_detail::unauthorized(std::format("Basic realm=\"{}\"", opts.realm));
		}
		if (!v(credentials->username, credentials->password)) {
			auth_detail::record_basic_auth_failure(*state, opts, limiter_key, now);
			return auth_detail::unauthorized(std::format("Basic realm=\"{}\"", opts.realm));
		}
		auth_detail::clear_basic_auth_failures(*state, opts, limiter_key);
		return next(req);
	};
}

export template<typename Validator>
conflux::http::Router::Middleware basic_auth_middleware(
	Validator &&validator,
	std::string realm = "Restricted") {
	return basic_auth_middleware(std::forward<Validator>(validator), BasicAuthOptions{.realm = std::move(realm)});
}

// Middleware factory: Bearer token Authentication guard.
// validator(token) → true = allow, false = 401.
export template<typename Validator>
conflux::http::Router::Middleware bearer_auth_middleware(
	Validator &&validator) {
	return [v = std::decay_t<Validator>(std::forward<Validator>(validator))](
			   conflux::http::RequestView const &req,
			   conflux::http::Router::Handler const &next) -> conflux::http::Response {
		auto auth = req.headers["authorization"];
		auto credentials = conflux::http::credentials_for_auth_scheme(auth, "Bearer");
		if (!credentials) {
			return auth_detail::unauthorized("Bearer");
		}
		if (!v(*credentials)) {
			return auth_detail::unauthorized("Bearer");
		}
		return next(req);
	};
}
