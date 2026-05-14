# Auth password hashing

`conflux.net.password_hash` is the password-storage boundary for HTTP/auth code.
It replaces ad-hoc password digests and plaintext validator comparisons with a
single encoded hash format plus an explicit rehash path.

## Public surface

```cpp
import conflux.net.password_hash;

auto encoded = password_hash(password);              // Argon2id default
auto verified = password_verify(password, encoded);  // ok + needs_rehash
```

Primary APIs:

- `password_hash(password, opts)` creates a new encoded hash with a random salt.
- `password_hash_with_salt(password, salt, opts)` exists for tests, fixtures, and
  controlled migrations.
- `password_verify(password, encoded, current_opts)` verifies a stored hash and
  reports whether it should be replaced with the current algorithm/parameters.
- `password_needs_rehash(encoded, current_opts)` checks only the encoded metadata.
- `password_hash_argon2id_available()` reports whether the runtime `libargon2`
  backend was found.

## Format

Hashes use a modular-crypt-style string with algorithm/version/parameters encoded
beside the salt and derived key:

```text
$argon2id$v=19$m=65536,t=3,p=1$<salt-b64url>$<hash-b64url>
$pbkdf2-sha256$v=1$i=600000,l=32$<salt-b64url>$<hash-b64url>
```

The metadata is deliberately part of the stored value so DB rows can be upgraded
without a separate schema flag. On login:

1. Load the stored hash.
2. Call `password_verify(password, stored, current_opts)`.
3. If `ok && needs_rehash`, call `password_hash(password, current_opts)` and store
   the replacement in the same transaction/session flow.

## Algorithm policy

Default creation uses Argon2id parameters: 64 MiB memory, 3 iterations, 1 lane,
16-byte salt, 32-byte output. The implementation dynamically loads `libargon2` on
Linux so the module does not need Argon2 headers at compile time. If `libargon2`
is not available, Argon2id hashing returns a clear error instead of silently
falling back to a weaker algorithm.

`pbkdf2_sha256_password_hash_options()` remains available as a portable
compatibility/migration fallback and for test fixtures. Do not use it for new
production password rows when Argon2id is available.

## Basic Auth integration

`basic_auth_middleware` still accepts a validator callback because the framework
must not own user storage. Use the password hash API inside that callback:

```cpp
router.use(basic_auth_middleware([&](SV user, SV password) {
    auto stored = lookup_password_hash(user);
    if (!stored) {
        return false;
    }
    auto verified = password_verify(password, *stored);
    if (!verified || !verified->ok) {
        return false;
    }
    if (verified->needs_rehash) {
        replace_password_hash(user, *password_hash(password));
    }
    return true;
}));
```

The auth module intentionally does not add DB-specific migration code. The stored
hash string carries enough metadata for app-level login flows to migrate rows
incrementally after a successful verification.
