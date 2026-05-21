# Auth rate-limit hooks

Branch: `auth/rate-limit-hooks`

This slice adds auth-side abuse controls for login and token surfaces that need a
subject key more specific than the generic IP-only `rate_limit_middleware`.

## What exists

`AuthFailureLimiter` is a bounded, LRU-evicting failed-attempt limiter. It is not
coupled to Basic auth or to a specific route shape.

Use it for:

- login/account throttling (`account:<username>` plus optional IP context in the
  caller's key),
- API-token throttling (`api-token:<sha256(token)>`),
- remote-address fallback throttling for unauthenticated flows,
- custom auth policies that want to call `before_attempt`, `record_failure`, and
  `record_success` directly.

The limiter exports a `snapshot()` with counters for allowed attempts, blocked
attempts, recorded failures, recorded successes, evictions, and currently tracked
subjects.

## Subject keys

Helpers are provided for common policy hooks:

- `auth_throttle_key(scope, subject)` builds stable scope-prefixed keys.
- `auth_throttle_remote_key(req)` normalizes parseable peer IPs.
- `auth_throttle_form_key(req, "username")` reads a parsed form field.
- `auth_throttle_query_key(req, "user")` reads a parsed query field.
- `auth_throttle_bearer_key(req)` hashes the bearer token before storing it.

The token helper deliberately stores a SHA-256 digest, not the raw bearer token.

## Middleware adapter

`auth_throttle_middleware(limiter, selector, opts)` is a small adapter for normal
route middleware. The selector returns a key (`std::string`, `std::string_view`,
or `std::optional<std::string>`). Empty or missing keys pass through.

The adapter:

1. calls `before_attempt(key)`,
2. returns `429 Too Many Requests` with `Retry-After` if blocked,
3. calls the downstream handler,
4. records a failure for configured auth-failure statuses (`401`, `403` by
   default),
5. clears the subject on configured success responses (`2xx`/`3xx` by default).

Example:

```cpp
AuthFailureLimiter login_limiter{AuthThrottleOptions{
    .max_failures = 5,
    .window = std::chrono::minutes{10},
    .lockout = std::chrono::minutes{15},
}};

router.use(auth_throttle_middleware(
    login_limiter,
    [](RequestView const& req) {
        return auth_throttle_form_key(req, "username");
    }));
```

For account+IP policies, compose the subject in the selector:

```cpp
router.use(auth_throttle_middleware(
    login_limiter,
    [](RequestView const& req) -> std::optional<std::string> {
        auto account = auth_throttle_form_key(req, "username");
        if (!account) {
            return std::nullopt;
        }
        return auth_throttle_key("login", *account + ":" + auth_throttle_remote_key(req));
    }));
```

## Defaults and non-goals

This does not replace application-specific lockout policy, CAPTCHA policy, or
session storage. It provides low-level hooks and a safe default adapter so route
code does not need to hand-roll per-account or per-token throttle state.

The existing Basic-auth failed-attempt limiter remains in place for Basic-auth
middleware. `AuthFailureLimiter` is the reusable primitive for login forms,
OAuth/API-token endpoints, or custom auth flows.
