# conflux.json Evolution Plan

**Goal:** Make `conflux.json` the reference C++26 JSON library — ergonomics and performance
both at the ceiling, neither traded for the other.

**Grounding:** conflux-feedback.json.md (15 user pain points), five rabidjson research docs,
perf_ideas.md, api_traps.md, seven adversarial review passes, and direct code audit of
`src/json.cxx`.

---

## Current State (Audited Against Source)

| What | Status |
|------|--------|
| Arena-backed `DocumentStorage` + `string_arena` | ✓ |
| SSE2 SIMD for string-content scan | ✓ |
| `parse(SV)` / `parse(S&&)` / `parse_borrowed(SV)` | ✓ |
| `JsonCodec<T>` + `JsonMembers<T>` struct serde | ✓ |
| `JsonError` with `JsonStage` + 20-code `JsonIssueCode` | ✓ |
| `warm_member_index(NodeRef)` + `warm_member_indices(WarmIndexOptions)` hash table | ✓ |
| `WarmIndexOptions { max_objects, max_extra_bytes }` — v16 shipped public type | ✓ |
| `kHashThreshold = 32` — minimum object size to build a hash table when warming | ✓ |
| Seeded `XXH3_64bits_withSeed` per-document (`getrandom` seed) for hash table | ✓ |
| `FI-1` build-failed sentinel `kHashBuildFailedSentinel` | ✓ |
| `JsonDumpOptions { pretty, indent, sort_object_keys, ascii_only }` | ✓ |
| `NodeRef` read-only view + separate builder hierarchy | ✓ |
| `JsonPath` + `NodeRef::at()` + `from_pointer()` | ✓ |
| `Nullable<T>` vs `Opt<T>` — null/missing distinguished | ✓ |
| `JsonNumberView` with `to_i64/to_u64/to_f64` range-checked | ✓ |
| Primary number parse via `from_chars` (locale-independent) | ✓ |
| `kMaxNumberLexemeLen = 1024` — reject number tokens > 1024 bytes at tokenizer | ✓ |
| `kValKindDeferred` — set when `from_chars` returns `result_out_of_range` AND lexeme ≤ 1024 bytes (= `kMaxNumberLexemeLen`); defers `strtod_l` to distinguish underflow-subnormal from overflow-infinite. `kSlowFloatLexemeCopyLimit = 4096` is now dead code (numbers > 1024 bytes are rejected before reaching it). | ✓ (replace in Phase 5.4) |
| `JsonParseOptions` with depth/input/string size limits | ✓ |
| Duplicate member rejection in parse and builder | ✓ |
| Module unit (`export module conflux.json`) | ✓ |
| Fuzz targets: parse, utf8, chunked decode | ✓ |
| `insert_string_view` — zero-copy external-name builder insert (v16 Item E) | ✓ |

**`JsonDumpOptions` existing fields:** `pretty`, `indent`, `sort_object_keys`, `ascii_only`.
Only `truncate_depth` and `indent_char` are missing.

**`MemberEntry` layout (line 383):** five fields — `name_off(u32)`, `name_len(u32)`,
`val_node(u32)`, `name_flags(u32)`, `name_ptr(char const*)`. The four `u32`s = 16 bytes;
`name_ptr` adds 8 → `sizeof == 24` (static_assert line 390). `name_flags` controls whether
the name is in arena (`0`), borrowed from input (`kStorageInputView = 0x01`), or an
external caller-owned pointer (`kMemberExternalView = 0x04`). `name_ptr` is dual-purpose:
warm-hash cache for arena names, and sole storage for `kMemberExternalView` names.

**`detail::make_object` / `detail::make_array`** (lines 520/514) are `inline Node`-returning
internal functions. Phase 1.4 introduces public `make_object`/`make_array` returning
`expected<Document>`. Rename internals to `detail::node_object` / `detail::node_array`
when Phase 1.4 lands.

---

## Gap Analysis

### Feedback items

| Feedback # | Mechanism | Missing piece |
|---|---|---|
| 1 (null vs missing) | `find_member` → `Opt<NodeRef>`; `is_null()` | Cookbook example |
| 2 (O(n) lookup) | `warm_member_index()` — threshold 32 | Cookbook: when to call; Phase 1.7 adds `warm_threshold` |
| 3 (typed decode) | `JsonCodec<T>` + `JsonMembers<T>` | End-to-end example |
| 4 (path access) | `NodeRef::at(JsonPath)` + `from_pointer()` | Cookbook |
| 6 (runtime errors) | `JsonError { stage, code, path, message }` | Error taxonomy doc |
| 11 (const/mutation) | `NodeRef` read-only by construction | Nothing to add |
| 5 (noisy builder) | Phase 1.4 — `make_object` / `make_array` | New code |
| 7 (dup-key policy) | Phase 1.3 — `DuplicateKeyPolicy` | New code |
| 8 (numeric coercion) | Phase 1.2 — `get_as<T>()` | New code |
| 9 (interop) | Phase 3.2 — `parse_sax` | New code |
| 10 (field helpers) | Phase 1.1 — `require_*` / `optional_*` | New code |
| 12 (pretty-print) | Phase 1.5 — `truncate_depth`, `indent_char` | New code |
| 13 (streaming) | Phase 7 — NDJSON + chunked | New code |
| 14 (arena reuse) | Phase 5 — `pmr` + `JsonArena` | New code |
| 15 (discoverability) | Phase 0 cookbook | Doc |

### Performance gaps

| Gap | Phase |
|-----|-------|
| No pull/on-demand parser (dominant HTTP workload) | 4 |
| `strtod_l` / `CLocaleHolder` global singleton | 5.4 |
| No `pmr::memory_resource` injection | 5.1 |
| No cross-parse arena reuse | 5.2 |
| `MemberEntry` = 24 bytes → 2.67 per cache line (should be 4) | 5.5 |
| SIMD: SSE2 string scan only, no AVX2 structural-char pass | 5.3 (measurement-gated) |
| `with_prefix` builds O(depth²) path on nested decode errors | 2.3 |

---

## Compatibility Matrix

| Parse mode | Storage | Status |
|---|---|---|
| `parse(SV)` / `parse(S&&)` | heap | ✓ existing |
| `parse_borrowed(SV)` | heap | ✓ existing |
| `parse(SV/S&&, opts, pmr*)` | caller `pmr` | ✓ Phase 5.1 |
| `parse_borrowed(SV, opts, pmr*)` | `pmr` for nodes; input still borrowed | ✓ Phase 5.1 |
| `JsonArena::parse_into(SV)` | arena slab (always copies input) | ✓ Phase 5.2 |
| `JsonReader` pull parse | stack only (zero-alloc) | ✓ Phase 4 |
| `parse_sax(SV, handler)` | none (events, wraps reader) | ✓ Phase 3.2 |
| `JsonArena::parse_into(SV, borrowed=true)` | dual-source lifetime trap | ✗ never |

---

## ABI and Stability Policy

Phases 1–2 are the stabilization window. Phase 2 completion is the ABI freeze point.
After that: additive only; removals require major bump.

- `JsonParseOptions`, `JsonDumpOptions`, `JsonDecodeOptions` — aggregate init; new fields
  append at end with defaults; existing field names frozen. Phases 1.3, 1.5, 1.7, and 2.1
  all add fields — all within the pre-freeze window.
- `JsonError` fields frozen after Phase 2. New `JsonIssueCode` values are additive.
- `Node::flags` byte has no public ABI guarantee — Documents do not survive a library upgrade.
- Phase 6 reflection path (`CONFLUX_JSON_REFLECT`) adds zero symbols to the default ABI.
- Phase 2.1 adds `opts = {}` as a defaulted parameter to the existing `decode<T>` template.
  Existing `JsonCodec<T>` specializations with `decode(NodeRef)` (no opts) continue to work via
  `if constexpr` detection — see Phase 2.1.

---

## Phase 0 — Discoverability Cookbook ✅ COMPLETE
**Scope:** `docs/json-cookbook.md` only. No code changes.
**Effort:** 3–4 days.
**Execute:** parallel with Phase 1.

Cover:
1. Null vs missing: `find_member` returns `Opt<NodeRef>`; `is_null()` on the result.
2. Large-object lookup: call `warm_member_index()` for objects with ≥32 members;
   `warm_member_indices()` for whole-document; contrast with `warm_threshold` option.
3. Typed struct decode: JSON string → `JsonMembers<T>` specialization → `decode<T>(doc)`.
4. Path traversal: `JsonPath::from_pointer("/results/0/id")` + `NodeRef::at()`.
5. Error taxonomy: `JsonStage`, common `JsonIssueCode` values, building user-facing messages.
6. Builder patterns: `ValueBuilder` → `begin_object()` / `begin_array()` → commit.
7. Integer literal note for `make_object` / `make_array`: `1` (int) is accepted via
   `JsonWritable`, encodes as `i64`. No suffix required.

---

## Phase 1 — Ergonomics Quick Wins ✓ COMPLETE
**Scope:** `src/json.cxx` only. No new data structures, no perf risk.
**Effort:** 1–2 weeks.

### 1.1 Field accessor helpers

Prototyping aids over `ObjectView`. **Document as such: prefer `decode<T>` in production.**
Target: one-off config readers and protocol glue.

All `*_string` / `optional_string` helpers return **owning `S`** (copies). `NodeRef::as_string()`
borrows from the `Document` input buffer — a silent UAF in HTTP handlers that discard the
document before using the string. The copy cost is acceptable for the one-off target use case.
View-returning variants (`*_string_view`) can be added later if benchmarks prove the copy
matters; they will carry explicit `// caller must keep Document alive` documentation.

```cpp
// Required: missing → missing_member, wrong type → wrong_kind
expected<S,      JsonError> require_string(ObjectView, SV name);
expected<i64,    JsonError> require_int   (ObjectView, SV name);
expected<u64,    JsonError> require_uint  (ObjectView, SV name);
expected<double, JsonError> require_double(ObjectView, SV name);
expected<bool,   JsonError> require_bool  (ObjectView, SV name);

// Optional: absent or explicit null → nullopt; wrong type → JsonError
expected<Opt<S>,      JsonError> optional_string(ObjectView, SV name);
expected<Opt<i64>,    JsonError> optional_int   (ObjectView, SV name);
expected<Opt<u64>,    JsonError> optional_uint  (ObjectView, SV name);
expected<Opt<double>, JsonError> optional_double(ObjectView, SV name);
expected<Opt<bool>,   JsonError> optional_bool  (ObjectView, SV name);
```

All push `name` onto `JsonError::path` before returning.

### 1.2 `JsonNumberView::get_as<T>()` — cross-numeric coercion

```cpp
template<class T>
    requires (std::integral<T> || std::floating_point<T>)
[[nodiscard]] expected<T, JsonError> JsonNumberView::get_as() const;
```

Routes through `to_i64()`, `to_u64()`, or `to_f64()` with narrowing range-check. Accepts
any arithmetic type (`int`, `float`, `unsigned`, etc.) not just `i64`/`u64`/`double`.
Surfaces `number_out_of_range` on overflow or narrowing failure.

Distinct from the strict `to_*()` accessors (which require matching number form) and from the
Phase 1.6 monadic `as_*()` aliases (which delegate to `to_*()` without cross-form coercion).

### 1.3 Duplicate-key parse policy

```cpp
enum class DuplicateKeyPolicy : u8 {
    reject,      // RFC 8259 recommended; current default
    last_wins,   // keep last value; first occurrence's name position preserved
    first_wins,  // keep first value; duplicate parsed fully then discarded
};

struct JsonParseOptions {
    LimitOption        max_depth;
    LimitOption        max_input_size;
    LimitOption        max_string_size;
    DuplicateKeyPolicy duplicate_key{DuplicateKeyPolicy::reject};  // NEW
};
```

**Storage rollback uses a `StorageMark`** capturing the size of all three storage vectors at
the point of detection. This ensures transactional correctness when a discarded value sub-tree
spans nodes, object-members entries, and string-arena bytes.

```cpp
struct StorageMark {
    SZ nodes;
    SZ object_members;
    SZ string_arena;
};
```

`first_wins` flow:
1. Detect duplicate key; record `StorageMark` before parsing the duplicate value.
2. Parse the duplicate value fully (syntax validated).
3. Rollback all three vectors to the mark (discard the duplicate sub-tree).
4. Continue parse.

`last_wins` flow:
1. Detect duplicate key; record the `staging_members` index of the first occurrence.
2. Parse the new duplicate value fully (new nodes/members/strings accumulate normally).
3. Update `staging_members[first_idx].val_node` to point at the new root node.
4. The **old** first-value sub-tree becomes unreachable garbage — **do not roll it back**.
   Compacting unreachable nodes during parse is not worth the complexity; the old nodes
   remain in storage until `Document` destruction. Document this as a known space cost.
5. The first occurrence's `name_off`/`name_len` are preserved (original key position kept).

No structural skip for either policy — "valid Document implies valid JSON" is unconditional.

**`last_wins` memory note:** old first-value sub-trees remain in storage until `Document`
destruction. A pathologically duplicate-heavy payload (all keys the same, N values) uses
O(N) storage even though the document contains 1 member. Document this limit. `reject` and
`first_wins` do not have this behaviour. `reject` (default) is the correct choice for
untrusted input.

Builder always rejects duplicates: wire-data duplicates come from sloppy generators; builder
duplicates are programmer error with no ambiguity about intent.

### 1.4 Compact builder factories: `make_object` / `make_array`

**`JsonWritable` concept** accepts a broader set of types than `has_json_codec`, covering natural
C++ literals without requiring codec specializations:

```cpp
template<class T>
concept JsonWritable =
    std::same_as<std::remove_cvref_t<T>, bool>               ||
    std::same_as<std::remove_cvref_t<T>, S>                  ||
    std::convertible_to<std::remove_cvref_t<T>, SV>          ||
    (std::integral<std::remove_cvref_t<T>> &&
     !std::same_as<std::remove_cvref_t<T>, char> &&
     !std::same_as<std::remove_cvref_t<T>, char8_t> &&
     !std::same_as<std::remove_cvref_t<T>, signed char> &&
     !std::same_as<std::remove_cvref_t<T>, unsigned char> &&
     !std::same_as<std::remove_cvref_t<T>, wchar_t> &&
     !std::same_as<std::remove_cvref_t<T>, char16_t> &&
     !std::same_as<std::remove_cvref_t<T>, char32_t>)        ||
    std::floating_point<std::remove_cvref_t<T>>              ||
    has_json_codec<std::remove_cvref_t<T>>;
```

**Dispatch priority** in the builder (order matters — `bool` must precede `integral`; codec
wins over implicit `SV` conversion for user-defined types):
1. `bool` → `insert_bool`
2. `has_json_codec<T>` → `JsonCodec<T>::encode` — explicit codec always wins over implicit
   string conversion; a type that is both `SV`-convertible and codec-equipped encodes via codec
3. `S` / `SV`-convertible (no codec) → `insert_string` (copy). `char const*` is accepted via
   this branch; a null `char const*` returns `invalid_value` without constructing a `string_view`
4. Signed integral (non-bool, non-char-type) → `insert_i64` via `static_cast<i64>`. Standard
   types (`short`, `int`, `long`, `long long`) are ≤ 64 bits and always fit; no runtime check
   needed. Non-standard types wider than 64 bits require a runtime range check; builder returns
   `number_out_of_range` on overflow.
5. Unsigned integral (non-bool, non-char-type) → `insert_u64`
6. Floating-point → `insert_f64`; non-finite (`NaN`, `Inf`) → `invalid_value` error

`char`, `char8_t`, `signed char`, `unsigned char`, `wchar_t`, `char16_t`, `char32_t` are all
excluded (ambiguous intent). Enum types are not handled by default; define a `JsonCodec<MyEnum>`
specialization.

**Large integer note:** values > 2^31 must fit in `i64`/`u64` range; the builder will return
`number_out_of_range` if the value overflows. Literals above 2^31 require explicit cast if the
compiler deduces `int` — e.g., `4294967296LL`.

**`JsonObjectPair` concept** for the pair-based API:

```cpp
template<class P>
concept JsonObjectPair =
    std::tuple_size<std::remove_cvref_t<P>>::value == 2 &&
    std::convertible_to<std::tuple_element_t<0, std::remove_cvref_t<P>>, SV> &&
    JsonWritable<std::tuple_element_t<1, std::remove_cvref_t<P>>>;
```

**Primary: explicit pair per arg (heterogeneous):**
```cpp
template<class... Pairs>
    requires (JsonObjectPair<std::remove_cvref_t<Pairs>> && ...)
expected<Document, JsonError> make_object(Pairs&&... pairs);
```

**Convenience: initializer_list (homogeneous value type):**
```cpp
template<class V>
    requires JsonWritable<V>
expected<Document, JsonError> make_object(std::initializer_list<std::pair<SV, V>> pairs);
```

Both call the same internal builder loop. Integer and floating-point literals work directly:

```cpp
auto doc = *make_object(
    pair{"model",       "gpt-4o"sv},
    pair{"temperature", 0.7},
    pair{"max_tokens",  4096},   // int → i64; no suffix required
    pair{"stream",      true});

auto doc = *make_object({{"role", "user"sv}, {"content", body}});
```

**`make_array`:**
```cpp
template<class... Elems>
    requires (JsonWritable<std::remove_cvref_t<Elems>> && ...)
expected<Document, JsonError> make_array(Elems&&... elems);

template<class V>
    requires JsonWritable<V>
expected<Document, JsonError> make_array(std::initializer_list<V> elems);
```

**Implementation note:** rename `detail::make_object` / `detail::make_array` to
`detail::node_object` / `detail::node_array` to eliminate same-TU shadowing.

### 1.5 Extended `JsonDumpOptions`

```cpp
struct JsonDumpOptions {
    bool      pretty{false};
    unsigned  indent{2};
    bool      sort_object_keys{false};   // existing
    bool      ascii_only{false};         // existing
    char      indent_char{' '};          // NEW — '\t' for tab-indented output
    Opt<SZ>   truncate_depth{};          // NEW — replace nodes deeper than N with null
};
```

`truncate_depth`: nodes at depth > N emit `null` instead of their actual value. Depth is
zero-based: root = 0, root's direct children = 1, etc.

| `truncate_depth` | Effect |
|---|---|
| `nullopt` (default) | no truncation |
| `0` | entire output is `null` |
| `1` | root emitted, all children replaced with `null` |
| `N` | nodes at depth ≤ N emitted, depth > N → `null` |

Output is semantically **lossy** — must not be fed to any parser expecting the original data.
Named `truncate_depth` (not `max_depth`) so the lossy nature is visible at every call site.

### 1.6 Monadic `NodeRef` typed accessors

Strict **aliases** only — no new semantics beyond `as_number().to_i64()` etc. Exist for
ergonomic monadic chaining; document the two-step form as canonical in non-chain contexts
(api_traps.md §1.20 — two ways to do the same thing).

```cpp
[[nodiscard]] expected<i64,    JsonError> NodeRef::as_i64()    const;  // → as_number().to_i64()
[[nodiscard]] expected<u64,    JsonError> NodeRef::as_u64()    const;  // → as_number().to_u64()
[[nodiscard]] expected<double, JsonError> NodeRef::as_double() const;  // → as_number().to_f64()
```

```cpp
auto v = doc.root().as_object()
    .and_then([](ObjectView o) { return o.member("count"); })
    .and_then([](NodeRef n)    { return n.as_i64(); });
```

### 1.7 `warm_threshold` in `JsonParseOptions`

```cpp
struct JsonParseOptions {
    // ... existing fields ...
    Opt<u32> warm_threshold{};  // NEW
    // nullopt (default) = never auto-warm; preserves existing behaviour
    // Some(N)           = auto-warm during parse for objects with >= N members
    // Some(1)           = warm every non-empty object
};
```

`u32` with `SIZE_MAX` semantics was invalid (`u32` cannot represent `SIZE_MAX`). Using
`Opt<u32>` cleanly separates disabled (`nullopt`) from "warm all" (`Some(1)`).

Default `nullopt` preserves existing behaviour — no auto-warm. Opt-in: `warm_threshold = 8`
on API-response paths with consistent-shape objects. The explicit `warm_member_index()` call
remains the recommended path for ad-hoc use; `warm_threshold` is for parse-time automation.

---

## Phase 2 — Decode Layer Evolution ✓ COMPLETE
**Scope:** `JsonCodec` / `JsonMembers` decode layer. No parse engine changes.
**Effort:** 1 week.

### 2.1 `JsonDecodeOptions` — runtime decode policy

```cpp
enum class UnknownMemberPolicy : u8 {
    reject,   // current default — unknown key → JsonError
    ignore,   // silently skip unknown keys
};

struct JsonDecodeOptions {
    UnknownMemberPolicy unknown_members{UnknownMemberPolicy::reject};
};
```

Add `opts = {}` as a defaulted trailing parameter to existing exported `decode` templates:

```cpp
template<class T>
expected<T, JsonError> decode(NodeRef root, JsonDecodeOptions const& opts = {});

template<class T>
expected<T, JsonError> decode(Document const& doc, JsonDecodeOptions const& opts = {});
```

**Backward compatibility:** existing `JsonCodec<T>` specializations with `decode(NodeRef)`
(no opts parameter) continue to work. The internal dispatch uses `if constexpr`:

```cpp
if constexpr (requires { JsonCodec<T>::decode(node, opts); })
    return JsonCodec<T>::decode(node, opts);
else
    return JsonCodec<T>::decode(node);
```

Existing custom codecs require zero changes. Only `JsonMembers<T>`-derived codecs are updated
to accept and forward `opts`.

`decode<T>(reader, opts)` (Phase 4.2) takes the same `JsonDecodeOptions`.

### 2.2 Field-level compile-time validation constraints

Constraints are **stateless function pointers** — no captures, no heap allocation, no
module dependency on `small_move_only_function`. This is intentional: stateless fn-ptr
constraints cover the common case (range checks, enum membership) without importing
`conflux.work` machinery into the JSON codec metadata.

```cpp
template<class M>
using JsonConstraintFn = expected<void, JsonError> (*)(M const&);
```

Extend the `JsonMembers<T>::members()` tuple with an optional third element:

```cpp
// Backward-compatible: existing 2-element tuples keep working.
// New 3-element form adds a constraint checked post-decode:
make_tuple(
    pair{"age", &Person::age},
    static_cast<JsonConstraintFn<int>>([](int v) -> expected<void, JsonError> {
        if (v < 0) return unexpected(JsonError{.stage=JsonStage::decode,
                                               .code=JsonIssueCode::constraint_violation,
                                               .message="age must be non-negative"});
        return {};
    })
)
```

**Stateful / runtime-config validation** (e.g., max value from config) does NOT belong in
tuple metadata. Use post-decode composition instead:

```cpp
decode<Person>(node, opts)
    .and_then([&](Person p) -> expected<Person, JsonError> {
        if (p.age > config.max_age) return unexpected(JsonError{...});
        return p;
    });
```

This is idiomatic, zero-overhead, and does not require any library support.

### 2.3 `JsonError::path` append-only fix (O(depth²) → O(depth), zero success-path allocations)

Fixes the `with_prefix` rebuild in generated `JsonMembers<T>` decode only. Custom `JsonCodec`
specializations keep `with_prefix` for compatibility.

**Key constraint:** `JsonPathMember { S name; }` owns a `string`. Pushing a `JsonPathMember`
for every member during successful decode would add N string allocations on the hot path.
This is unacceptable. Solution: use a non-owning internal path stack during decode and
materialize a `JsonPath` only when an error occurs.

```cpp
// Internal path frame — no heap allocation
namespace detail {
struct PathFrame {
    enum class Kind : u8 { member, index } kind;
    SV  member_name;  // SV into input or arena (valid for the duration of decode)
    SZ  index;
};

// Materialize only on error path
JsonPath materialize_path(std::span<PathFrame const> frames);
}
```

The decode loop maintains a `SmallVector<detail::PathFrame, 16>` shared across recursion
levels. Depths ≤ 16 use the inline buffer (zero heap). Depths > 16 may spill to heap —
acceptable since deep nesting only occurs on error paths. Each member pushes a
`PathFrame{.kind=member, .member_name=name}`, recurses, pops on return — no string allocation.
On error return, `materialize_path` copies each `member_name` SV into `JsonPathMember { S
name }` once. Total: zero allocs on success, one alloc per depth level on error.

```cpp
namespace detail {
template<class T>
expected<T, JsonError> decode_with_frames(
    NodeRef node,
    SmallVector<PathFrame, 16>& frames,  // shared across recursion; .size() == current depth
    JsonDecodeOptions const& opts);
}
```

Public `decode<T>(node, opts)` stack-allocates the `SmallVector`, calls `decode_with_frames`,
materializes path only on error. The `with_prefix` calls in `JsonMembers<T>` recursion are
removed; `with_prefix` stays for custom codecs.

---

## Phase 3 — SAX / Event Interface ✅ COMPLETE
**Dependency:** Phase 4 (`JsonReader`) must ship first.
**Effort:** 1 week (after Phase 4).

### 3.1 `HandlerReturn` concept helper

```cpp
template<class R>
concept HandlerReturn =
    std::same_as<R, void> ||
    std::convertible_to<R, expected<void, JsonError>>;
```

### 3.2 `JsonHandler` concept + `JsonDefaultHandler`

```cpp
template<class H>
concept JsonHandler =
    requires(H& h, SV sv, i64 i, u64 u, double d, bool b) {
        requires HandlerReturn<decltype(h.on_null())>;
        requires HandlerReturn<decltype(h.on_bool(b))>;
        requires HandlerReturn<decltype(h.on_string(sv))>;
        requires HandlerReturn<decltype(h.on_i64(i))>;
        requires HandlerReturn<decltype(h.on_u64(u))>;
        requires HandlerReturn<decltype(h.on_double(d))>;
        requires HandlerReturn<decltype(h.on_begin_object())>;
        requires HandlerReturn<decltype(h.on_key(sv))>;
        requires HandlerReturn<decltype(h.on_end_object())>;
        requires HandlerReturn<decltype(h.on_begin_array())>;
        requires HandlerReturn<decltype(h.on_end_array())>;
        // on_number_raw is OPTIONAL — see number dispatch below
    };
```

**Number dispatch:** `parse_sax` uses `if constexpr (requires { h.on_number_raw(sv); })` to
detect whether the handler provides `on_number_raw`:
- Provided: call `on_number_raw(lexeme)` only. Typed callbacks (`on_i64`/`on_u64`/`on_double`)
  not called — no typed conversion cost. Handler wanting typed conversion calls it from within
  `on_number_raw`.
- Not provided (including handlers inheriting `JsonDefaultHandler` which omits `on_number_raw`):
  dispatch typed callbacks only.

When `on_number_raw` is present, `parse_sax` validates its return type at compile time:
`static_assert(HandlerReturn<decltype(h.on_number_raw(sv))>)`. A handler defining
`int on_number_raw(SV)` fails with a clear `static_assert` rather than a delayed template error.

`on_number_raw` is NOT part of the `JsonHandler` concept — it is an optional opt-in. This
avoids tag-return tricks in the base class and keeps `HandlerReturn` clean.

`on_begin_object()` and `on_begin_array()` take no hint parameter — the pull parser does not
scan ahead to count members. If a future pre-scanned source needs hints, an overloaded variant
can be added without breaking existing handlers.

**`JsonDefaultHandler`** — base with no-op implementations so handlers only override what
they care about:

```cpp
struct JsonDefaultHandler {
    expected<void, JsonError> on_null()           { return {}; }
    expected<void, JsonError> on_bool(bool)       { return {}; }
    expected<void, JsonError> on_string(SV)       { return {}; }
    expected<void, JsonError> on_i64(i64)         { return {}; }
    expected<void, JsonError> on_u64(u64)         { return {}; }
    expected<void, JsonError> on_double(double)   { return {}; }
    expected<void, JsonError> on_begin_object()   { return {}; }
    expected<void, JsonError> on_key(SV)          { return {}; }
    expected<void, JsonError> on_end_object()     { return {}; }
    expected<void, JsonError> on_begin_array()    { return {}; }
    expected<void, JsonError> on_end_array()      { return {}; }
    // on_number_raw intentionally absent — define it to opt into raw dispatch
};
```

`void`-returning handlers cannot abort the walk — documented at the concept definition site.
Handlers needing abort capability must return `expected<void, JsonError>`.

### 3.3 `parse_sax`

```cpp
template<JsonHandler H>
expected<void, JsonError> parse_sax(
    SV input,
    H& handler,
    JsonParseOptions const& opts = {});
```

Wraps `JsonReader`. Zero extra allocation. `parse_sax` is the loop-free ergonomic wrapper;
`JsonReader` is for callers needing interleaved logic. One calls the other.

---

## Phase 4 — Pull Parser ✅ COMPLETE
**Priority:** P0 — dominant `conflux` HTTP handler workload.
**Effort:** 3–4 weeks.
**Status:** Implemented and all tests pass (clang-libcxx + gcc-stdcxx).

### 4.1 `JsonReader`

```cpp
// Half-open byte range [start, end) within a JsonReader's input() slice.
struct JsonByteRange { SZ start; SZ end; };

// Handle for a JSON string (key or value). Remains valid until the next call to
// next() or skip_next_value(). Does not allocate.
struct JsonStringToken {
    // Raw bytes of the lexeme including surrounding quotes; borrowed from input.
    [[nodiscard]] SV   raw_lexeme()      const noexcept;

    // True if the string contains backslash escape sequences.
    [[nodiscard]] bool has_escapes()     const noexcept;

    // SV into input when !has_escapes(); nullopt when has_escapes().
    // Valid until next()/skip_next_value() — do not outlive the JsonReader event.
    [[nodiscard]] Opt<SV> unescaped_borrow() const noexcept;

    // Append decoded string to caller-owned buffer. Handles escapes.
    // Fails with string_too_large if result would exceed opts.max_string_size.
    expected<void, JsonError> append_decoded_to(S& out) const;

    // Upper bound on decoded length in bytes (not code points): raw_lexeme().size()-2
    // (quotes stripped). Actual decoded length ≤ this value (escape sequences shrink).
    // Use to pre-size a buffer for decode_into without a two-pass scan.
    [[nodiscard]] SZ max_decoded_size() const noexcept;

    // Decode into caller-provided buffer; returns SV into buf.
    // buf.size() must be ≥ max_decoded_size(). Writes exactly decoded_length bytes;
    // does NOT append '\0'. Returns SV whose .size() is the exact decoded byte count
    // (may be < buf.size() when escapes were present).
    // Fails with string_too_large if decoded length exceeds opts.max_string_size.
    expected<SV, JsonError> decode_into(std::span<char> buf) const;
};

class JsonReader {
public:
    enum class Event : u8 {
        begin_object, end_object,
        begin_array,  end_array,
        key,
        string_value, number_value, bool_value, null_value,
    };

    explicit JsonReader(SV input, JsonParseOptions const& opts = {});

    [[nodiscard]] expected<Opt<Event>, JsonError> next();

    [[nodiscard]] JsonStringToken key_token()    const noexcept;
    [[nodiscard]] JsonStringToken string_token() const noexcept;
    [[nodiscard]] JsonNumberView  number_val()   const noexcept;  // see note below
    [[nodiscard]] bool            bool_val()     const noexcept;

    // Full input slice passed to the constructor. Used to extract sub-ranges from
    // JsonByteRange values returned by skip_next_value().
    [[nodiscard]] SV input() const noexcept;

    // Advance past the next complete JSON value; return its byte range in input().
    // Valid at any structural position — does not require a preceding next() call.
    // Invalidates the previous JsonStringToken (same rules as next()).
    // Once has_error() is set, returns the same error without advancing.
    [[nodiscard]] expected<JsonByteRange, JsonError> skip_next_value();

    // Current structural depth.
    [[nodiscard]] SZ depth() const noexcept;

    // True after any next()/skip_next_value() returns an error.
    // Once set, next() and skip_next_value() return the same error without advancing.
    [[nodiscard]] bool has_error() const noexcept;

    // Reposition to start. Clears has_error(). Always succeeds.
    // THE ONLY guaranteed recovery path from any error state.
    // Invalidates all JsonStringTokens from key_token() / string_token().
    void reset() noexcept;
};
```

`skip_to_depth()` is **not provided in v1**. Recovery from malformed input in a streaming
context is complex; `reset()` is the only guaranteed recovery. If future workloads prove
the need for partial-document recovery, `skip_to_depth` can be added in v2.

`JsonReader` is stack-only (no heap allocation). It does not build a DOM.

**`JsonNumberView` is storage-independent** — source-confirmed at `json.cxx:823`: it stores
`{SV lexeme_, u64 raw_payload_, u8 flags_}` with no `DocumentStorage*` dependency. It can be
constructed directly inside `JsonReader` without a separate `JsonNumberToken` type. The existing
`to_i64()`/`to_u64()`/`to_f64()` and Phase 1.2 `get_as<T>()` work identically from both DOM
and pull-parser contexts.

### 4.2 `decode<T>(JsonReader&)` — direct struct decode

```cpp
template<class T>
expected<T, JsonError> decode(JsonReader& reader, JsonDecodeOptions const& opts = {});
```

`decode<T>(reader)` **consumes the next complete value** from the reader (calls `next()`
internally as its first step). This is consistent with how `decode<T>(node)` consumes a DOM
node — the caller does not need to pre-position the reader.

`JsonMembers<T>` decode:
1. `reader.next()` → `begin_object`
2. Loop: `reader.next()` → `key`; match name
3. Matched: `decode<M>(reader, opts)` recursively
4. Unknown: `reader.skip_next_value()` (discard result) or error per `opts.unknown_members`
5. `end_object` → return `T`

Recovery on failure: `has_error()` is set. Caller calls `reset()` — the only reliable path.
After reset, reader is at start of input; caller retries or propagates.

`decode<std::string_view>(reader)` is explicitly deleted. With `JsonStringToken` there is no
hidden scratch buffer, but the deletion is still correct: escaped strings have no stable
contiguous decoded representation to borrow, and making the behaviour data-dependent (borrow
when unescaped, error when escaped) is unsafe for generic struct fields where callers cannot
predict which case applies. Use `std::string` instead.

### 4.3 `JsonCodec<Document>::decode(JsonReader&)` — sub-tree capture

For structs containing `Document` fields:

```cpp
template<>
struct JsonCodec<Document> {
    static expected<Document, JsonError> decode(JsonReader& reader);
};
```

Implementation: call `reader.skip_next_value()` to advance past the complete sub-tree in one
step, receiving `JsonByteRange {start, end}`. No recursive `decode<Document>` call — forward
progress guaranteed by the skip. Slice `reader.input().substr(start, end-start)` and re-parse
into a new heap `Document`. Cost is explicit (one alloc). The parent `decode<T>` continues
with tokens after the captured sub-tree.

### 4.4 Benchmark corpus — gates all SIMD decisions

**Must land before any Phase 5 SIMD work begins.**

Add to `benchmarks/json_bench.cxx`:
- `twitter.json` (~631 KB, nested, string-heavy)
- `canada.json` (~2.2 MB, float arrays)
- `github_events.json` (~65 KB, repeated-key API response)
- `apache_builds.json` (~1.6 MB, mixed)

All run **in-process over in-memory buffers**. Report for each benchmark:
- MB/s throughput
- allocations/op and bytes_allocated/op
- p50/p95 latency (especially for small 8–200 byte API payloads)
- compare: `parse()` DOM vs `decode<T>(reader)` pull path

Store baseline in `benchmarks/baseline.json` as `{median, p25, p75}` per benchmark.

---

## Phase 5 — Memory Model & Performance Hardening ✅ COMPLETE (5.1/5.2/5.4/5.5; 5.3 DEFERRED — measurement-gated)
**Effort:** 3 weeks.

### 5.1 `pmr::memory_resource` injection

```cpp
expected<Document, JsonError> parse(
    SV input, JsonParseOptions const& opts, pmr::memory_resource* resource);

expected<Document, JsonError> parse(
    S&& input, JsonParseOptions const& opts, pmr::memory_resource* resource);

expected<Document, JsonError> parse_borrowed(
    SV input, JsonParseOptions const& opts, pmr::memory_resource* resource);
```

**Contract:** the `memory_resource` must outlive every `Document` allocated from it.
This is the same rule as `parse_borrowed` input lifetime — document both prominently.

`DocumentStorage` allocates node vector, `object_members`, and `string_arena` from `resource`
when non-null.

### 5.2 `JsonArena` — cross-parse arena reuse

Thin adapter over `pmr::monotonic_buffer_resource` + persistent `DocumentStorage`.

`parse_into` returns **`ArenaDocument`** — a RAII handle holding a non-owning reference to
the arena's storage. `ArenaDocument` exposes the same query interface as `Document`.

**Lifetime rule:** `NodeRef`s and other borrowed views extracted from an `ArenaDocument` have
the same lifetime as the `ArenaDocument` itself — which is bounded by the owning `JsonArena`.
There are no runtime checks for extracted `NodeRef`s; this is a documented ownership rule,
not a detected invariant. In debug builds, the `ArenaDocument` holds a generation counter;
accessing a stale `ArenaDocument` (one whose arena has been reset) aborts. `NodeRef`s
extracted from a stale `ArenaDocument` are undefined behaviour — enforce through code review
and documentation, not runtime checks (adding generation to every `NodeRef` would bloat it).

> **COOKBOOK RULE (must appear as a callout in docs):**
> Do NOT store a `NodeRef` extracted from an `ArenaDocument` beyond the scope owning the
> `JsonArena`. Do NOT cache `NodeRef` values across `arena.reset()`. Debug builds abort on
> stale `ArenaDocument` access; `NodeRef` staleness is undetected — undefined behaviour.
> If you need values to outlive the arena, copy them into owned types (`S`, `i64`, etc.)
> before calling `reset()`.

```cpp
struct JsonArenaOptions {
    SZ   initial_slab{64 * 1024};
    bool intern_keys{false};
};

class ArenaDocument {
public:
    [[nodiscard]] NodeRef root() const noexcept;
    // ... same query interface as Document ...
};

class JsonArena {
public:
    explicit JsonArena(JsonArenaOptions const& opts = {});

    [[nodiscard]] expected<ArenaDocument, JsonError> parse_into(
        SV input, JsonParseOptions const& opts = {});

    // Invalidates all ArenaDocuments. Debug builds abort on stale ArenaDocument access.
    void reset() noexcept;

    [[nodiscard]] SZ slab_capacity() const noexcept;
    [[nodiscard]] SZ slab_used()     const noexcept;
};
```

**Key interning (`intern_keys = true`):** hash set in a separate sub-slab not rewound by
`reset()`. Keys seen in parse N are reused in parse N+1. No user-visible SV lifetime
extension — interning is an internal optimization.

### 5.3 AVX2 structural-character bitmasking (measurement-gated) — GATE NOT MET

**Gate:** `twitter.json` ≥15% AND `github_events.json` ≥10%, both with bootstrap CI
(≥1000 resamples, 95%) excluding zero, AND no regression on `canada.json` or
`apache_builds.json` outside ±2%. All four conditions AND. Benchmark protocol: K≥30 runs,
CPU pinned, turbo disabled, first 5 warmup runs discarded.

**Gate result (2026-05-02, Ryzen 7 5800X, K=30):**
| Corpus | SSE2 baseline | AVX2 (string-scan only) | Required | Result |
|--------|--------------|------------------------|----------|--------|
| twitter.json | 540 MB/s | 540 MB/s | +15% → 621 | ❌ 0% |
| github\_events.json | 357 MB/s | 363 MB/s | +10% → 393 | ❌ +1.7% |
| canada.json | 218 MB/s | 209 MB/s | ≤±2% | ❌ −4% |

Widening the string scanner from 16→32 bytes is insufficient. The plan's "Stage-0" means
scanning ALL structural characters (`{`, `}`, `[`, `]`, `,`, `:`) + quotes simultaneously
in 32-byte chunks, producing a bitmask the state-machine walks — i.e., full simdjson-style
two-pass tokenizer restructure. That architectural change is deferred pending business need.

AVX2 code is in-tree and guarded by `#if defined(CONFLUX_JSON_HAS_AVX2)` + `CONFLUX_JSON_AVX2`
cmake option. CMake preset `release-avx2-clang-libcxx` exists. Activate when implementing
the full Stage-0 tokenizer rewrite.

**Approach:** simdjson Stage-0 — 32 bytes/iteration with `_mm256_cmpeq_epi8` for structural
characters + quote bytes simultaneously, feeding bitmask positions into the state-machine.

**CPU floor:** `x86-64-v3` (AVX2+BMI2). Separate CMake preset `release-avx2-clang-libcxx`
with `-DCONFLUX_JSON_AVX2=ON`. Default stays SSE2. Guard: `#if defined(CONFLUX_JSON_HAS_AVX2)`.

### 5.4 Replace `strtod_l` / `CLocaleHolder`

Current: `CLocaleHolder` singleton + `strtod_l` for `kValKindDeferred` nodes (subnormal
float edge cases where `from_chars` returned `result_out_of_range` at parse time, lexeme
≤ 4 KB).

**Fix:** in `JsonNumberView::to_f64()`, for `kValKindDeferred` nodes, call
`from_chars(chars_format::general)` on the preserved lexeme. On glibc (platform target),
`from_chars<double>` correctly handles subnormals and is locale-independent.

**Required proof gate before deleting `strtod_l`:** run the full existing number test suite
(all number-related cases in `tests/json_test.cxx`) using `from_chars` alone on the target
platform (glibc). All tests must pass — the existing regression suite is the authoritative
gate, no separate magic-value list needed. Note: `"0." + N×'0' + "1"` for N ≥ 1024 will
never reach `kValKindDeferred` (rejected by `kMaxNumberLexemeLen = 1024` at the tokenizer)
and need not be tested via this path.

If the full suite passes: delete `CLocaleHolder`, `strtod_l`, `locale.h`. If any case fails:
keep `strtod_l` for those cases and document why. Do not delete until the suite is clean.

**Preserve `kValKindDeferred` flag** — stored in `Node::flags`; removing it would break
Documents read back across a library upgrade. No change to `Node::flags` layout. No public
API change.

### 5.5 `MemberEntry` cache-line packing ✅ COMPLETE (Option B)

Target: 16 bytes (4 per cache line, vs current 2.67). Requires removing `name_ptr`.

`name_ptr` is dual-purpose:
- **Arena names:** warm-hash cache (`build_table` sets it; `find_member_warmed` uses it as Item C).
- **`kMemberExternalView` names:** only storage. Cannot be recomputed.

**Option A (implemented, reverted) — `benchmarks/notes/5.5-memberentry-pack-v1.patch`:**
Side-table `V<pair<u32,char const*>> external_ptrs_` in `DocumentStorage`. `member_name()`
uses `lower_bound` by absolute member index for `kMemberExternalView`. `build_table()` drops
the `name_ptr` caching; `find_member_warmed()` uses `member_name()` for the hash-match
comparison instead of direct `SV{name_ptr, len}`.

**Gate result (2026-05-02, K=30, 20s CPU settle):**
| Path | 24B baseline | 16B Option A | Required | Result |
|------|-------------|--------------|----------|--------|
| github\_events warmed hit | 50.3 ns | 52.1 ns | no regression | ❌ +3.6% |
| github\_events warmed miss | 55.9 ns | 61.5 ns | no regression | ❌ +10% |
| apache\_builds warmed hit/miss | 161 ns | 168 ns | no regression | ❌ +4.4% |
| find\_member/1024 (hash path) | 10.8 ns | 11.8 ns | no regression | ❌ +9% |

Root cause: dropping Item C (`name_ptr` as pre-cached pointer in `find_member_warmed`)
forces a `member_name()` dispatch call per hash-match comparison. This dominates the warmed
hash-table path regression.

**Option B (implemented 2026-05-04):** 16-byte `MemberEntry` (name_ptr removed);
ptr_cache appended after slots in `ObjHashTable` allocation; `DocumentStorage::external_ptrs_`
side table for `kMemberExternalView` names (name_off reused as index). `ChildFrame::local_external_ptrs_`
accumulates ptrs during build; `commit()` relocates them into `DocumentStorage::external_ptrs_`.

**Gate result (2026-05-04, 5 runs each, release-clang-libcxx):**
| Path | 24B baseline | 16B Option B | Required | Result |
|------|-------------|--------------|----------|--------|
| 31-member linear scan (per lookup) | 38.5 ns | 34.5 ns | ≥5% gain | ✅ −10.4% |
| 1024-member warmed hash (per lookup) | 9.7 ns | 9.6 ns | no regression | ✅ ≈0% |

---

## Phase 6 — P2996 Reflection (Toolchain-Gated)
**Priority:** P2. Not in release compilers as of 2026-05.

```cpp
#if defined(CONFLUX_JSON_REFLECT) && \
    (defined(__cpp_reflection) || __has_feature(reflection))

template<class T>
    requires std::is_aggregate_v<T>
          && (!detail::has_members_spec<T>::value)
          && (!detail::has_codec_spec<T>::value)
struct JsonCodec<T> { /* P2996-derived encode/decode */ };

#endif
```

Priority: explicit `JsonCodec<T>` > explicit `JsonMembers<T>` > P2996 auto-derivation.
Annotations: `[[=conflux::json::name("id")]]`, `[[=conflux::json::skip]]`.

CMake presets: `debug-p2996-clang` (Bloomberg fork), `debug-p2996-gcc` (gcc-16 trunk).
Tests in `tests/json_reflection_test.cxx`, compiled only under `CONFLUX_JSON_REFLECT`.

---

## Phase 7 — Streaming & NDJSON ✅ COMPLETE
**Effort:** 2 weeks.

### 7.1 NDJSON iterator

Single-pass `std::ranges::input_range`. Splitting on raw 0x0A is correct — RFC 8259 §7
forbids unescaped 0x0A in strings; a raw 0x0A in a string is a parse error caught per-line.

`operator*()` caches the parsed `expected<Document, JsonError>` for the current line. Multiple
dereferences before `++` return the same cached result without re-parsing.

```cpp
class NdjsonRange {
public:
    explicit NdjsonRange(SV input, JsonParseOptions const& opts = {});

    struct Iterator {
        using iterator_category = std::input_iterator_tag;
        using value_type        = expected<Document, JsonError>;
        using difference_type   = std::ptrdiff_t;

        value_type const& operator*() const;  // cached; no re-parse on repeat calls
        Iterator& operator++();               // advance; clears cache
        Iterator  operator++(int);
        bool operator==(std::default_sentinel_t) const noexcept;
    };

    Iterator begin();                           // single-pass; calling twice is UB
    std::default_sentinel_t end() const noexcept;
};
```

### 7.2 `JsonAccumulator` — buffered document assembly

Not an incremental parser. An accumulating buffer that assembles a complete document from
chunks when the document boundary is known externally (HTTP `Content-Length`, NDJSON line,
WebSocket frame). Named `JsonAccumulator` to make the non-incremental nature explicit.

```cpp
class JsonAccumulator {
public:
    explicit JsonAccumulator(JsonParseOptions const& opts = {});

    // Returns input_too_large if accumulated size exceeds opts.max_input_size.
    expected<void, JsonError> feed(SV chunk);

    // Calls parse(std::move(buffer_), opts) — no extra copy.
    expected<Document, JsonError> finish();

    void reset() noexcept;
    SZ   buffered_bytes() const noexcept;
};
```

`feed()` checks `buffered_bytes() + chunk.size()` against `opts.max_input_size` and returns
`input_too_large` before appending — prevents unbounded memory accumulation on spoofed
`Content-Length`.

`finish()` moves the buffer into `parse()` — no copy of the accumulated data.

Option B (true incremental tokenizer with state-machine save/restore): deferred to v2.

---

## Phase 8 — Advanced Features

### 8.1 JSON5 relaxed parse mode

```cpp
enum class ParseMode : u8 { strict, json5 };
// Added to JsonParseOptions: ParseMode mode{ParseMode::strict};
```

Subset: `//` / `/* */` comments, trailing commas, single-quoted strings, unquoted ASCII
identifier keys (`[A-Za-z_$][A-Za-z0-9_$]*` only; Unicode identifier escapes not supported).

**Key normalization:** quoted and unquoted keys are both written through `string_arena` before
dedup check. A mixed document `{"a": 1, a: 2}` must be detected as a duplicate.

Fuzzing required before ship (`fuzz/fuzz_json5.cxx`).

### 8.2 Compile-time JSON literal parsing

**Status: design not finalized.** This section records requirements; the internal consteval
node type is not yet specified.

Blocked by: `from_chars<double>` not constexpr, `DocumentStorage` needing `constexpr new`.

Planned subset: integers, booleans, null, no-escape strings, nested objects/arrays.
No floating-point literals (static_assert with message if encountered).

The return type of `parse_ct<"...">()` and its integration with `decode<T>` (would require
a new ABI-visible overload) must be designed before implementation.

### 8.3 JSON Schema lite

```cpp
template<class T>
    requires (detail::has_members_spec<T>::value || detail::has_codec_spec<T>::value)
Document schema_for();

expected<void, JsonError> validate(NodeRef root, Document const& schema);
```

Dry-run `decode<T>()` with side effects suppressed. Covers required/optional fields and type
constraints.

**Constraint note:** Phase 2.2 stateless fn-ptr constraints are runtime-only — they cannot be
reflected into the schema document. `schema_for<T>()` emits field names, types, and
required/optional status only. Value constraints (range checks, enum membership) are omitted;
document this at the schema API call site.

---

## Infrastructure

### Benchmark statistical gate

- K ≥ 30 runs, dedicated machine, CPU pinned, turbo disabled, first 5 warmup runs dropped
- Gate: `(new_median - baseline_median) / baseline_median > 0.05` AND bootstrap CI
  (≥1000 resamples, 95%) excludes zero
- Report: MB/s throughput, allocations/op, bytes_allocated/op, p50/p95 latency (small
  8–200 byte payloads), branch misses, L1/L2 cache misses, compile time for
  `JsonMembers`-heavy TUs
- Refresh baseline on green main merge; store in `benchmarks/baseline.json`

### JSONTestSuite conformance gate

- Gate CI on 100% pass for `y_` (must-accept) and `n_` (must-reject)
- Track `i_` without gating

### Fuzz expansion

- `fuzz/fuzz_json_reader.cxx` — Phase 4 `JsonReader`
- `fuzz/fuzz_json_sax.cxx` — Phase 3.2 `parse_sax`
- `fuzz/fuzz_ndjson.cxx` — Phase 7.1 `NdjsonRange`
- `fuzz/fuzz_json5.cxx` — Phase 8.1

### PGO preset

`release-pgo-clang-libcxx`: instrument → train on all four corpora → use.
Expected 10–20% on branchy dispatch. Optional lane.

---

## Non-Goals

| Item | Reason |
|------|--------|
| Binary formats (CBOR, MessagePack) | Separate module |
| Full JSON Schema ($ref, oneOf, allOf) | Schema-lite covers practical needs |
| Thread-safe mutable DOM | No use case |
| Cross-platform / Windows / macOS | Linux-only |
| `basic_json<...>` template parameter | `pmr` is the answer |
| `operator[]` mutation on `NodeRef` | nlohmann footgun |
| `parse_into(SV, borrowed=true)` | Dual-source lifetime trap |
| `reset_keep_keys()` on `JsonArena` | Ambiguous SV validity post-reset |
| `skip_to_depth()` on `JsonReader` | Deferred to v2; `reset()` is v1 recovery |
| Chunked parse Option B | Deferred to v2 |
| P2996 as required | Library must work without it |
| `strtod_l` retention after proof gate | Replaced in Phase 5.4 |
| Eisel-Lemire | Not applicable to subnormal edge cases (`kValKindDeferred` path) |
| Captured lambdas in `JsonMembers` tuple metadata | Post-decode `.and_then()` for stateful validation |
| `NodeRef` generation counter for `ArenaDocument` stale detection | Bloats NodeRef; ownership rule enforced by docs |

---

## Phase Summary

Execution order: **0 → 1 → 2 → 4 → 3 → 5 → 7 → 6 → 8**
Phase 3 after Phase 4 (SAX wraps reader). All others independent.

| Phase | Deliverable | Effort | Priority |
|-------|-------------|--------|----------|
| ~~0~~ ✅ | Cookbook (integer literal note, warm_threshold guidance, error taxonomy) | 3–4 days | P0 |
| 1 | `require_*` (owning S), `get_as<T>`, `DuplicateKeyPolicy` + `StorageMark` rollback, `make_object`/`make_array` (`JsonWritable`), `truncate_depth`/`indent_char`, `as_i64/double` aliases, `warm_threshold{Opt<u32>}` | 1–2 wk | P0 |
| 2 | `JsonDecodeOptions` (`if constexpr` compat), compile-time constraints (fn-ptr), O(depth²) fix (pass-down accumulator for `JsonMembers` only) | 1 wk | P0 |
| 4 | `JsonReader` (`JsonStringToken`, `skip_next_value() → JsonByteRange`, `input()`, no `skip_to_depth`) + `decode<T>(reader)` (consume-on-entry, `string_view` deleted) + `JsonCodec<Document>::decode(reader)` (via `skip_next_value`) + corpus baseline (MB/s + alloc/op + latency) | 3–4 wk | P0 |
| 3 | `JsonDefaultHandler` + `JsonHandler` concept + `parse_sax` (after Phase 4) | 1 wk | P1 |
| 5 | `pmr` injection (lifetime contract), `JsonArena`+`ArenaDocument` (doc-only NodeRef lifetime), key interning, AVX2 (K≥30 gate), `from_chars` deferred (proof gate), `MemberEntry` 16B (Option A + measurement gate) | 3 wk | P1 |
| 7 | `NdjsonRange` (caching `operator*`) + `JsonAccumulator` (move-finish, feed size check) | 2 wk | P1 |
| 6 | P2996 reflection (`__cpp_reflection` + `__has_feature`) | 2 wk | P2 |
| 8 | JSON5 (ASCII idents + key normalization), compile-time mini-parser (design first), schema lite | 3 wk | P2 |
| Infra | JSONTestSuite gate, fuzz per-phase, benchmark gate (K≥30, alloc/op, latency), PGO preset | ongoing | P1 |
