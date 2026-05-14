export module conflux.net.auth;
import std;
import conflux.types;
import conflux.crypto;
import conflux.net.http.types;
import conflux.net.router;
import conflux.utils;

export struct BasicAuthOptions {
	S realm{"Restricted"};

	// Failed Basic-auth attempts allowed per remote address during the window.
	// 0 disables this guard for deployments that enforce login throttling elsewhere.
	unsigned failed_attempts{10};
	chrono::seconds failed_window{chrono::minutes{5}};

	// Maximum remote addresses tracked by the failure limiter.
	SZ max_failed_clients{65536};
};

namespace auth_detail {

using Clock = chrono::steady_clock;

struct FailedAuthBucket {
	unsigned failures{};
	Clock::time_point window_start{Clock::now()};
};

struct FailedAuthStore {
	explicit FailedAuthStore(
		SZ max)
		: max_(max) {}

	[[nodiscard]] FailedAuthBucket *find(
		SV key) noexcept {
		auto it = map_.find(key);
		if (it == map_.end()) {
			return nullptr;
		}
		order_.splice(order_.end(), order_, it->second.order_it);
		return &it->second.bucket;
	}

	[[nodiscard]] FailedAuthBucket &touch(
		SV key,
		Clock::time_point now) {
		if (auto *bucket = find(key); bucket != nullptr) {
			return *bucket;
		}
		if (map_.size() >= max_) {
			map_.erase(order_.front());
			order_.pop_front();
		}
		auto owned = S{key};
		order_.push_back(owned);
		auto [it, _] = map_.emplace(
			move(owned),
			Entry{
				.bucket = FailedAuthBucket{.failures = 0, .window_start = now},
				.order_it = std::prev(order_.end()),
			});
		return it->second.bucket;
	}

	void erase(
		SV key) noexcept {
		auto it = map_.find(key);
		if (it == map_.end()) {
			return;
		}
		order_.erase(it->second.order_it);
		map_.erase(it);
	}

private:
	struct TransparentHash {
		using is_transparent = void;
		SZ operator ()(
			SV s) const noexcept {
			return hash<SV>{}(s);
		}
		SZ operator ()(
			S const &s) const noexcept {
			return hash<SV>{}(s);
		}
	};
	struct Entry {
		FailedAuthBucket bucket{};
		std::list<S>::iterator order_it;
	};

	SZ max_;
	std::list<S> order_;
	std::unordered_map<S, Entry, TransparentHash, std::equal_to<>> map_;
};

struct FailedAuthState {
	mutex mtx;
	FailedAuthStore store;

	explicit FailedAuthState(
		SZ max)
		: store(max) {}
};

Opt<SV> credentials_for_scheme(
	SV auth,
	SV scheme) noexcept {
	if (auth.size() <= scheme.size() || auth[scheme.size()] != ' ') {
		return nullopt;
	}
	if (!conflux::http::ascii_iequals(auth.substr(0, scheme.size()), scheme)) {
		return nullopt;
	}
	return auth.substr(scheme.size() + 1);
}

[[nodiscard]] S failed_auth_key(
	HttpRequestView const &req) {
	if (req.remote_addr.empty()) {
		return "unknown";
	}
	return S{req.remote_addr};
}

HttpResponse unauthorized(
	SV www_auth) {
	HttpResponse r;
	r.status = kHttpUnauthorized;
	r.status_text = "Unauthorized";
	r.content_type = "text/plain; charset=utf-8";
	r.set_text_body("Unauthorized");
	r.headers["WWW-Authenticate"] = S{www_auth};
	return r;
}

HttpResponse too_many_auth_attempts(
	chrono::seconds retry_after) {
	HttpResponse r;
	r.status = 429;
	r.status_text = "Too Many Requests";
	r.content_type = "text/plain; charset=utf-8";
	r.set_text_body("Too Many Requests");
	r.headers["Retry-After"] = format(
		"{}",
		std::max<chrono::seconds::rep>(chrono::seconds::rep{1}, retry_after.count()));
	return r;
}

[[nodiscard]] bool basic_auth_limiter_enabled(
	BasicAuthOptions const &opts) noexcept {
	return opts.failed_attempts != 0U && opts.failed_window.count() > 0;
}

[[nodiscard]] Opt<chrono::seconds> basic_auth_retry_after(
	FailedAuthState &state,
	BasicAuthOptions const &opts,
	SV key,
	Clock::time_point now) {
	if (!basic_auth_limiter_enabled(opts)) {
		return nullopt;
	}
	SL const lock{state.mtx};
	auto *bucket = state.store.find(key);
	if (bucket == nullptr) {
		return nullopt;
	}
	auto elapsed = chrono::duration_cast<chrono::seconds>(now - bucket->window_start);
	if (elapsed >= opts.failed_window) {
		state.store.erase(key);
		return nullopt;
	}
	if (bucket->failures < opts.failed_attempts) {
		return nullopt;
	}
	return max(chrono::seconds{1}, opts.failed_window - elapsed);
}

void record_basic_auth_failure(
	FailedAuthState &state,
	BasicAuthOptions const &opts,
	SV key,
	Clock::time_point now) {
	if (!basic_auth_limiter_enabled(opts)) {
		return;
	}
	SL const lock{state.mtx};
	auto &bucket = state.store.touch(key, now);
	auto elapsed = chrono::duration_cast<chrono::seconds>(now - bucket.window_start);
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
	SV key) {
	if (!basic_auth_limiter_enabled(opts)) {
		return;
	}
	SL const lock{state.mtx};
	state.store.erase(key);
}

} // namespace auth_detail

// Middleware factory: HTTP Basic Authentication guard.
// validator(username, password) → true = allow, false = 401.
export template<typename Validator>
Router::Middleware basic_auth_middleware(
	Validator &&validator,
	BasicAuthOptions opts) {
	auto state = make_shared<auth_detail::FailedAuthState>(max<SZ>(opts.max_failed_clients, 1));
	return [v = std::decay_t<Validator>(forward<Validator>(validator)),
			opts = move(opts),
			state](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		S const limiter_key = auth_detail::failed_auth_key(req);
		auto const now = auth_detail::Clock::now();
		if (auto retry_after = auth_detail::basic_auth_retry_after(*state, opts, limiter_key, now)) {
			return auth_detail::too_many_auth_attempts(*retry_after);
		}

		auto auth = req.headers["authorization"];
		auto credentials = auth_detail::credentials_for_scheme(auth, "Basic");
		if (!credentials) {
			auth_detail::record_basic_auth_failure(*state, opts, limiter_key, now);
			return auth_detail::unauthorized(format("Basic realm=\"{}\"", opts.realm));
		}
		auto decoded = base64_decode(*credentials);
		auto colon = decoded.find(':');
		if (colon == S::npos) {
			auth_detail::record_basic_auth_failure(*state, opts, limiter_key, now);
			return auth_detail::unauthorized(format("Basic realm=\"{}\"", opts.realm));
		}
		SV const sv{decoded};
		SV const user = sv.substr(0, colon);
		SV const pass = sv.substr(colon + 1);
		if (!v(user, pass)) {
			auth_detail::record_basic_auth_failure(*state, opts, limiter_key, now);
			return auth_detail::unauthorized(format("Basic realm=\"{}\"", opts.realm));
		}
		auth_detail::clear_basic_auth_failures(*state, opts, limiter_key);
		return next(req);
	};
}

export template<typename Validator>
Router::Middleware basic_auth_middleware(
	Validator &&validator,
	S realm = "Restricted") {
	return basic_auth_middleware(
		forward<Validator>(validator),
		BasicAuthOptions{.realm = move(realm)});
}

// Middleware factory: Bearer token Authentication guard.
// validator(token) → true = allow, false = 401.
export template<typename Validator>
Router::Middleware bearer_auth_middleware(
	Validator &&validator) {
	return [v = std::decay_t<Validator>(forward<Validator>(
				validator))](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto auth = req.headers["authorization"];
		auto credentials = auth_detail::credentials_for_scheme(auth, "Bearer");
		if (!credentials) {
			return auth_detail::unauthorized("Bearer");
		}
		auto token = trim(*credentials);
		if (!v(token)) {
			return auth_detail::unauthorized("Bearer");
		}
		return next(req);
	};
}
