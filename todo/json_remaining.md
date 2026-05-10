# conflux.json — Remaining Work

_Collapsed from `JSON_PLAN.md`. Phases 0–4, 5.1/5.2/5.5, and 7 are complete._

---

## Phase 5 — Deferred items

### 5.3 AVX2 structural-character bitmasking (gate not met)

Gate: `twitter.json` ≥15% AND `github_events.json` ≥10% with bootstrap CI (≥1000 resamples, 95%), no regression on `canada.json`/`apache_builds.json` outside ±2%. **Gate failed 2026-05-02** (AVX2 string-scan only: 0%/+1.7% gains, −4% regression on canada).

Full simdjson-style Stage-0 (scan all structural chars in 32-byte chunks, produce bitmask for state machine) is required to move the needle — not just widening the string scanner. Deferred until business need justifies the architectural change.

AVX2 code is in-tree, guarded by `CONFLUX_JSON_HAS_AVX2`. CMake preset `release-avx2-clang-libcxx` exists. Activate when implementing full Stage-0 tokenizer rewrite.

- [ ] Implement full simdjson-style Stage-0 tokenizer (32B/iter, all structural chars + quotes simultaneously).
- [ ] Re-run gate (K≥30, CPU pinned, turbo off, 5 warmup runs discarded).

### 5.4 Replace `strtod_l` / `CLocaleHolder` (platform-blocked)

**Gate failed 2026-05-05 (GCC 15.2.1/16.1.0 libstdc++ on Gentoo glibc):** `from_chars<double>` does not write `dv` on `result_out_of_range` — overflow vs underflow cannot be distinguished without `strtod_l`. Retain until libstdc++ `from_chars` sets `dv=inf` on overflow (C++ standard requires this).

- [ ] Revisit when libstdc++ `from_chars` correctly sets `dv=inf` on overflow. Run full number test suite against `from_chars` alone. Delete `strtod_l`/`CLocaleHolder`/`locale.h` only if all cases pass.

---

## Phase 6 — P2996 Reflection (toolchain-gated, P2)

Not in release compilers as of 2026-05. Guarded by `CONFLUX_JSON_REFLECT` + `__cpp_reflection`/`__has_feature(reflection)`.

- [ ] Implement `JsonCodec<T>` auto-derivation via P2996 (priority: explicit codec > explicit members > auto).
- [ ] Annotations: `[[=conflux::json::name("id")]]`, `[[=conflux::json::skip]]`.
- [ ] Tests in `tests/json_reflection_test.cxx` under `CONFLUX_JSON_REFLECT` guard.
- [ ] CMake presets: `debug-p2996-clang` (Bloomberg fork), `debug-p2996-gcc` (gcc-16 trunk).

---

## Phase 8 — Advanced Features (P2)

### 8.1 JSON5 relaxed parse mode

Subset: `//`/`/* */` comments, trailing commas, single-quoted strings, unquoted ASCII identifier keys. Mixed quoted/unquoted duplicate key detection required. Fuzz required before ship (`fuzz/fuzz_json5.cxx`).

- [ ] Implement `ParseMode::json5` in `JsonParseOptions`.
- [ ] Key normalization through `string_arena` for dedup.
- [ ] Fuzz target `fuzz/fuzz_json5.cxx`.

### 8.2 Compile-time JSON literal parsing (design not finalized)

Blocked by: `from_chars<double>` not constexpr, `DocumentStorage` needing `constexpr new`. Return type and `decode<T>` integration not yet designed.

- [ ] Design internal consteval node type and return type of `parse_ct<"...">()`.
- [ ] Design `decode<T>` integration (new ABI-visible overload required).
- [ ] Implement subset: integers, booleans, null, no-escape strings, nested objects/arrays. No float literals.

### 8.3 JSON Schema lite

- [ ] `Document schema_for<T>()` — dry-run `decode<T>()` with side effects suppressed; emits field names, types, required/optional.
- [ ] `expected<void, JsonError> validate(NodeRef, Document const& schema)`.
- [ ] Note: stateless fn-ptr constraints (Phase 2.2) are runtime-only and not reflected into schema.

---

## Fuzz gaps

- [ ] `fuzz/fuzz_json_reader.cxx` — `JsonReader` pull parser.
- [ ] `fuzz/fuzz_json_sax.cxx` — `parse_sax`.
- [ ] `fuzz/fuzz_ndjson.cxx` — `NdjsonRange`.
- [ ] `fuzz/fuzz_json5.cxx` — JSON5 (after 8.1).

---

## Benchmark gap

- [ ] Add `SocketTaskRing` vs `FileReader` end-to-end JSON decode benchmark (deferred from JSON bench corpus — see `benchmarks/json_bench.cxx`).
