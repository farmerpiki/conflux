# conflux.json — Remaining Work

_Collapsed from `JSON_PLAN.md`. Phases 0–4, 5.1/5.2/5.5, and 7 are complete._

---

## Phase 5 — Deferred items

### 5.3 AVX2 structural-character bitmasking (gate not met)

Gate: `twitter.json` ≥15% AND `github_events.json` ≥10% with bootstrap CI (≥1000 resamples, 95%), no regression on `canada.json`/`apache_builds.json` outside ±2%. **Gate failed 2026-05-02** (AVX2 string-scan only: 0%/+1.7% gains, −4% regression on canada).

**Stage-0 whitespace-skip tape: tested 2026-05-10, gate failed (neutral/no benefit).** Pre-scanned a `V<u32>` tape of token-start positions (AVX2 32B/iter) to make `skip_ws` O(1). Result: ws scanning is not the bottleneck (~0.06ms of 2.3ms for `pretty_ws`); tape heap allocation (~180KB) and cache pressure caused regression. Replaced with inline AVX2 `skip_ws` (no allocation); still neutral — confirms whitespace is not the bottleneck on this workload. Dropped: not worth maintenance cost for zero gain.

Full simdjson-style Stage-0 (scan all structural chars in 32-byte chunks, produce bitmask for state machine) is required to move the needle — not just widening the string scanner or skipping whitespace. Deferred until business need justifies the architectural change.

AVX2 code is in-tree, guarded by `CONFLUX_JSON_HAS_AVX2`. CMake preset `release-avx2-clang-libcxx` exists. Activate when implementing full Stage-0 tokenizer rewrite.

- [ ] Implement full simdjson-style Stage-0 tokenizer (32B/iter, all structural chars + quotes simultaneously).
- [ ] Re-run gate (K≥30, CPU pinned, turbo off, 5 warmup runs discarded).

### 5.4 Replace `strtod_l` / `CLocaleHolder` (platform-blocked)

**Gate failed 2026-05-05 (GCC 15.2.1/16.1.0 libstdc++ on Gentoo glibc):** `from_chars<double>` does not write `dv` on `result_out_of_range` — overflow vs underflow cannot be distinguished without `strtod_l`. Retain until libstdc++ `from_chars` sets `dv=inf` on overflow (C++ standard requires this).

- [ ] Revisit when libstdc++ `from_chars` correctly sets `dv=inf` on overflow. Run full number test suite against `from_chars` alone. Delete `strtod_l`/`CLocaleHolder`/`locale.h` only if all cases pass.

---

## Phase 6 — P2996 Reflection — DONE (424bbf0, modulo clang preset)

`src/json_reflect.cxx` (224 lines), `tests/json_reflection_test.cxx`, wired under `CONFLUX_JSON_REFLECT` in `tests/CMakeLists.txt`.

- [x] Implement `JsonCodec<T>` auto-derivation via P2996 (priority: explicit codec > explicit members > auto).
- [x] Annotations: `[[=conflux::json::name("id")]]`, `[[=conflux::json::skip]]`.
- [x] Tests in `tests/json_reflection_test.cxx` under `CONFLUX_JSON_REFLECT` guard.
- [x] CMake preset: `debug-p2996-gcc` (gcc-16 trunk).
- [ ] CMake preset: `debug-p2996-clang` (Bloomberg fork) — missing; add only if Bloomberg clang is available on host.

---

## Phase 8 — Advanced Features (P2)

### 8.1 JSON5 relaxed parse mode — DONE (424bbf0)

All subset items landed: `//`/`/* */` comments, trailing commas, single-quoted strings, unquoted ASCII identifier keys, mixed-key dedup (`DuplicateKeyPolicy::reject` on `{"a":1,a:2}` → `duplicate_member`), fuzz target.

- [x] Implement `ParseMode::json5` in `JsonParseOptions`.
- [x] Key normalization through `string_arena` for dedup.
- [x] Fuzz target `fuzz/fuzz_json5.cxx`.

### 8.2 Compile-time JSON literal parsing (design not finalized)

Blocked by: `from_chars<double>` not constexpr, `DocumentStorage` needing `constexpr new`. Return type and `decode<T>` integration not yet designed.

- [ ] Design internal consteval node type and return type of `parse_ct<"...">()`.
- [ ] Design `decode<T>` integration (new ABI-visible overload required).
- [ ] Implement subset: integers, booleans, null, no-escape strings, nested objects/arrays. No float literals.

### 8.3 JSON Schema lite — DONE (424bbf0)

`schema_for<T>()` and `validate(NodeRef root, NodeRef schema)` in `json.cxx:6750+`. 6 test cases in `json_test.cxx` under `[phase8.3]`. Note: signature is `validate(NodeRef, NodeRef)` — caller passes `schema_doc.root()`.

- [x] `Document schema_for<T>()` — emits field names, types, required/optional.
- [x] `expected<void, JsonError> validate(NodeRef, NodeRef schema)`.
- [x] Note: stateless fn-ptr constraints (Phase 2.2) are runtime-only and not reflected into schema.

---

## Fuzz gaps

- [x] `fuzz/fuzz_json_reader.cxx` — `JsonReader` pull parser. (landed 424bbf0)
- [x] `fuzz/fuzz_json_sax.cxx` — `parse_sax`. (landed 424bbf0)
- [x] `fuzz/fuzz_ndjson.cxx` — `NdjsonRange`. (landed 424bbf0; error path `__builtin_trap` fixed)
- [x] `fuzz/fuzz_json5.cxx` — JSON5. (landed 424bbf0)

---

## Benchmark gap

- [x] Add `SocketTaskRing` vs `FileReader` end-to-end JSON decode benchmark (deferred from JSON bench corpus — see `benchmarks/json_bench.cxx`). The benchmark now runs `JsonAccumulator` against the same large corpus via a temp file and a loopback socket source.

---

## Parallel JSON lane additions

### Parser/DOM policy facade — DONE

`json/parser-dom-design` added `JsonDomPolicy` and `parse_dom(...)` wrappers in
`conflux.json`, plus `docs/json-dom-prototype.md`. This names the intended
view-first, caller-PMR, and reusable-arena DOM integration surface without
starting a broad parser rewrite. Future tokenizer/DOM/reflection work should
use this facade and keep HTTP/app code on `conflux.json.boundary`.
