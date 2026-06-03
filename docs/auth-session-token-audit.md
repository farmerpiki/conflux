# Auth Session and Token Audit

Status: implemented for the current JWT/cookie/session-secret surface.

## Scope

This audit covers the auth surfaces that currently create or verify bearer/session material:

- `conflux.net.jwt` for stateless bearer/session tokens.
- `conflux.net.cookie_signing` for tamper-evident cookie values.
- `conflux.net.config` auth secret sources for JWT, cookie, and session secret rotation.
- `conflux.net.csrf` for double-submit CSRF tokens.

There is no server-side session store in the framework yet. Applications that need opaque sessions should store only a random session id in the cookie, keep expiry/revocation server-side, and sign the cookie with `cookie_signing_middleware` or a future dedicated session middleware.

## JWT policy

`conflux::http::JwtOptions` now separates compatibility defaults from stricter session-token policy knobs.
Use `JwtOptions::public_server()` or the config-derived `jwt_options_from_config(...)`
default when a token is accepted by HTTP middleware or other public-server entrypoints.

- `verify_exp`: reject expired tokens when `exp` exists.
- `verify_nbf`: reject not-yet-valid tokens when `nbf` exists.
- `require_exp`: reject tokens missing `exp`.
- `require_iat`: reject tokens missing `iat`.
- `require_jti`: reject tokens missing `jti`.
- `clock_skew`: tolerate small clock differences for `exp`/`nbf`.
- `max_token_lifetime`: require `exp` + `iat` and cap `exp - iat`.
- `revoked_jti`: optional lookup hook; returning `true` rejects the token.

Recommended session/bearer-token policy:

```cpp
auto opts = conflux::http::JwtOptions::public_server();
opts.secrets = resolve_secret_rotation(cfg.auth_secrets.jwt, "jwt").value();
opts.require_jti = true;
opts.revoked_jti = [&](std::string_view jti) {
    return revoked_token_store.contains(jti);
};
```

Long-lived refresh tokens should use a different secret/config slot from short-lived access tokens, or a dedicated issuer/audience policy, so rotation and revocation can be scoped independently.

## Expiry

Raw default-constructed `JwtOptions` still accept tokens without `exp` for compatibility tests and non-session uses. `jwt_options_from_config(...)` defaults to `JwtOptions::public_server()`, which requires `exp` and `iat` and caps token lifetime at 15 minutes unless the caller explicitly supplies a different policy. `max_token_lifetime` rejects missing `exp`/`iat`, rejects negative registered timestamps, rejects `exp < iat`, and caps the token lifetime even when `exp` is far in the future.

## Revocation

JWTs are stateless, so revocation requires one of these application-level strategies:

1. Keep short access-token lifetimes and rotate the signing secret when broad invalidation is acceptable.
2. Include `jti` and use `conflux::http::JwtOptions::revoked_jti` for targeted deny-list checks.
3. Use opaque server-side session ids for high-control sessions; sign only the cookie envelope.

`conflux::http::jwt_middleware` now benefits from these policy knobs because it delegates to `conflux::http::jwt_decode(opts)`.

## Secret rotation

JWT and signed-cookie verification accepts the active secret and all configured previous secrets. Signing always uses the active secret. This allows rolling deployment:

1. Add the old active secret to `*_previous_secret`.
2. Configure the new `*_secret` as active.
3. Wait longer than the maximum token/cookie lifetime.
4. Remove the previous secret.

## Cookie/session storage

Signed cookies only provide integrity. They do not encrypt contents, enforce expiry by themselves, or provide revocation. Store only low-sensitivity opaque identifiers unless the payload has its own expiry and revocation policy.

Cookie signing should be combined with explicit cookie attributes, usually `Path=/; Secure; HttpOnly; SameSite=Lax` or stricter. CSRF tokens should not be treated as authentication tokens.

## Current non-goals

- No built-in opaque session database.
- No refresh-token table.
- No account/login throttling beyond the current Basic-auth failed-attempt limiter.
- No JWT encryption; HS256 signing only.

Those belong in later `auth/rate-limit-hooks` or dedicated session-store branches.
