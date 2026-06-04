# Auth password hashing

`conflux.net.password_hash` is the password-storage boundary for HTTP/auth code.
It keeps stored password rows in a single encoded format, exposes an explicit
verify/rehash path, and centralizes expensive KDF admission control.

## Public surface

```cpp
import conflux.net.password_hash;

auto encoded = conflux::http::password_hash(password);              // Argon2id default
auto verified = conflux::http::password_verify(password, encoded);  // ok + needs_rehash
```

Primary APIs:

- `conflux::http::password_hash(password, opts, secrets)` creates a new encoded hash with a random salt.
- `conflux::http::password_hash_with_salt(password, salt, opts, secrets)` exists for tests,
  fixtures, and controlled migrations.
- `conflux::http::password_verify(password, encoded, current_opts, secrets)` verifies a stored
  hash and reports whether it should be replaced with the current algorithm,
  parameters, or pepper policy.
- `conflux::http::password_needs_rehash(encoded, current_opts, secrets)` checks only the encoded
  metadata.
- `conflux::http::password_hash_argon2id_available()` reports whether the configured Argon2id
  backend is usable.
- `conflux::http::password_hash_configure_resource_limits(limits)` bounds simultaneous KDF work
  and queued hash callers.

## Format

Hashes use a modular-crypt-style string with algorithm/version/parameters encoded
beside the salt and derived key:

```text
$argon2id$v=19$m=65536,t=3,p=1$<salt-b64url>$<hash-b64url>
$argon2id$v=19$m=65536,t=3,p=1,k=1$<salt-b64url>$<hash-b64url>
$pbkdf2-sha256$v=1$i=600000,l=32$<salt-b64url>$<hash-b64url>
$pbkdf2-sha256$v=1$i=600000,l=32,k=1$<salt-b64url>$<hash-b64url>
```

`k=1` means the stored hash was post-processed with the verifier-only secret
carried in `conflux::http::PasswordHashSecrets`, not in the stored password row and not in
`conflux::http::PasswordHashOptions`. A `k=1` row fails closed if verification is attempted
without that secret. Rows without `k=1` are unpeppered rows and must be verified
without a verifier secret.

The metadata is deliberately part of the stored value so DB rows can be upgraded
without a separate schema flag. On login:

1. Load the stored hash.
2. Resolve `conflux::http::PasswordHashSecrets` from typed auth config.
3. Call `conflux::http::password_verify(password, stored, current_opts, secrets)`.
4. If `ok && needs_rehash`, call `conflux::http::password_hash(password, current_opts, secrets)` and store
   the replacement in the same transaction/session flow.

Introducing a verifier secret to an existing unpeppered password table requires
a controlled migration before enabling the secret for login verification.

## Algorithm policy

Default creation uses Argon2id parameters: 64 MiB memory, 3 iterations, 1 lane,
16-byte salt, 32-byte output. This stays above the OWASP floor while keeping the
cost bounded enough for a small server when paired with admission control.

Argon2 provider selection is controlled at configure time:

```text
-DCONFLUX_PASSWORD_HASH_ARGON2_PROVIDER=AUTO     # default: link libargon2 if found, else runtime loader
-DCONFLUX_PASSWORD_HASH_ARGON2_PROVIDER=SYSTEM   # require pkg-config libargon2 and link it
-DCONFLUX_PASSWORD_HASH_ARGON2_PROVIDER=RUNTIME  # use dlopen only
-DCONFLUX_PASSWORD_HASH_ARGON2_PROVIDER=OFF      # disable Argon2id backend
```

`SYSTEM` is preferred for release packaging because deployment failures are found
at configure/link time. `RUNTIME` remains available for draft/demo builds that
want to run on systems where `libargon2` may or may not be installed. Missing
Argon2id support fails closed; it never silently falls back to PBKDF2 for a new
Argon2id hash.

`conflux::http::pbkdf2_sha256_password_hash_options()` remains a FIPS-oriented
fallback and for test fixtures. The PBKDF2 path uses a no-allocation streaming
SHA-256/HMAC implementation for the 600k-iteration inner loop. Do not use PBKDF2
for new production password rows when Argon2id is available.

## Verifier secret / pepper

Configure a verifier-only secret outside the password-hash storage table and pass
it separately from the algorithm parameters:

```cpp
auto cfg = config_from_ini("/etc/conflux.ini");
auto secrets = conflux::http::password_hash_secrets_from_config(cfg);
if (!secrets) {
    return unexpected{secrets.error()}; // explicit missing/short secret error
}

conflux::http::PasswordHashOptions opts;
auto encoded = conflux::http::password_hash(password, opts, *secrets);
auto verified = conflux::http::password_verify(password, encoded, opts, *secrets);
```

The secret must be stable across restarts and deployments that need to verify old
rows. Store it separately from DB password hashes: env/secret manager/KMS-backed
config, not in the user table. The default config has no production secret;
helpers such as `conflux::http::password_hash_secrets_from_config`, `conflux::http::jwt_options_from_config`,
and `cookie_signing_options_from_config` return explicit missing-secret errors
until a source is configured. JWT, cookie, and session secrets use typed rotation
config: one active source plus optional previous sources for verification-only
rollover.

See `docs/auth-session-token-audit.md` for JWT/session expiry and revocation policy.

## Resource and abuse controls

Password hashing is intentionally expensive. Configure KDF admission limits during
server startup:

```cpp
auto ok = conflux::http::password_hash_configure_resource_limits({
    .max_concurrent_hashes = 2,
    .max_waiting_hashes = 64,
});
```

`max_concurrent_hashes == 0` uses the library default
`min(max(hardware_concurrency / 2, 1), 4)`. `max_waiting_hashes == 0` makes excess
hash callers fail fast with `password hash: concurrency limit reached`. This gate
protects CPU/RAM from concurrent Argon2id/PBKDF2 work; it is not a substitute for
request-level throttling.

Basic auth now has a failed-attempt limiter enabled by default:

```cpp
router.use(conflux::http::basic_auth_middleware(
    [&](std::string_view user, std::string_view password) { return verify_user_password(user, password); },
    conflux::http::BasicAuthOptions{
        .realm = "Restricted",
        .failed_attempts = 10,
        .failed_window = chrono::minutes{5},
        .max_failed_clients = 65536,
    }));
```

Set `failed_attempts = 0` only when another layer already enforces failed-login
rate limiting. For login forms or API-token endpoints, use `conflux::http::AuthFailureLimiter`
and the helpers documented in `docs/auth-rate-limit-hooks.md` to throttle by
account, API-token digest, remote address, or a composed application key.

## Basic Auth integration

`conflux::http::basic_auth_middleware` still accepts a validator callback because the framework
must not own user storage. Use the password hash API inside that callback:

```cpp
conflux::http::PasswordHashOptions current = current_password_hash_options();

router.use(conflux::http::basic_auth_middleware([&](std::string_view user, std::string_view password) {
    auto stored = lookup_password_hash(user);
    if (!stored) {
        return false;
    }
    auto verified = conflux::http::password_verify(password, *stored, current);
    if (!verified || !verified->ok) {
        return false;
    }
    if (verified->needs_rehash) {
        auto replacement = conflux::http::password_hash(password, current);
        if (replacement) {
            replace_password_hash(user, *replacement);
        }
    }
    return true;
}));
```

The auth module intentionally does not add DB-specific migration code. The stored
hash string carries enough metadata for app-level login flows to migrate rows
incrementally after a successful verification.
