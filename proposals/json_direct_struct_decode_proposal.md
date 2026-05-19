# Proposal: JSON direct-to-struct serde fast path

## Status

Implementation in progress.

Current sequencing note: benchmarking is intentionally deferred until the full
proposal is implemented. Compile/test validation may use clang for faster
feedback, while reflection-specific validation uses GCC 16 with P2996 enabled.

## Summary

`conflux.json` should add a first-class direct-to-struct serde path that parses JSON object members from `JsonReader` and assigns directly into the target C++ object. The goal is to close the gap with Glaze-style struct/reflection workloads by avoiding DOM construction, per-key string allocation, generic `NodeRef` lookup, and temporary `ValueBuilder`/`Document` construction when the target type is statically known.

The intended fast path is:

```text
input bytes -> JsonReader events -> compile-time field table -> direct member assignment -> T
```

The dynamic JSON path remains available for ad-hoc JSON, mutation-style construction, JSON Pointer/Patch, templates, schema validation, and other `Document`/`NodeRef` uses. Typed serde should not pay those costs unless the target type actually requires them.

## Problem

Current typed JSON decode has the right building blocks, but the fast path is not yet dominant enough:

- `JsonReader` already supports streaming events and `decode_full<T>(reader, ...)`.
- Manual `JsonMembers<T>` reader decode currently decodes every key into a fresh `std::string`, tracks required fields with `std::vector<bool>`, and linearly checks members.
- DOM decode walks through `Document`, `NodeRef`, and `ObjectView`, then does one lookup per field and an extra unknown-member scan under `UnknownMemberPolicy::reject`.
- Reflection decode is currently exposed as a `JsonCodec<T>` specialization, so it is naturally shaped around `NodeRef` decode and `ValueBuilder` encode.
- Provider-boundary serialization for typed values currently builds a `ValueBuilder`, finishes a `Document`, then dumps it.

This makes reflected/typed serde allocate and do generic JSON representation work that Glaze avoids. For routes and config payloads where the target C++ type is known, the common path should not construct a DOM at all.

## Goals

1. Decode `JsonMembers<T>` and reflected structs directly from `JsonReader` without DOM allocation.
2. Match object keys against compile-time field metadata without per-key owning string allocation on the unescaped-key path.
3. Track required fields without heap allocation for normal structs.
4. Serialize manual/reflected structs directly into a sink without `ValueBuilder`/`Document` when possible.
5. Make borrowed vs owned string behavior explicit in the API and HTTP integration.
6. Preserve existing DOM APIs and custom codec escape hatches.
7. Add benchmark rows that expose parse cost, decode cost, write cost, allocations, and memory bytes separately.

## Non-goals

- Do not rewrite the whole parser into a simdjson-style two-stage parser for this issue.
- Do not remove `Document`, `NodeRef`, `ValueBuilder`, JSON Pointer/Patch, or schema-oriented APIs.
- Do not introduce public macros or an external code generator.
- Do not make reflection mandatory; manual `JsonMembers<T>` remains the portable/explicit path.
- Do not hide lifetime-sensitive borrowed string behavior behind a convenience API.

## Design

### 1. Direct reader capability layer

Introduce a distinct internal capability for types that can decode from a `JsonReader` event stream:

```cpp
template<class T>
concept JsonReaderDecodable = /* primitive, container, JsonMembers, reflected, or custom reader codec */;

template<class T>
std::expected<T, JsonError> decode_direct(
    JsonReader& reader,
    JsonDecodeOptions const& opts = {},
    JsonDecodeScratch* scratch = nullptr);
```

`decode_full<T>(JsonReader&, ...)` should call this direct path for eligible types. The existing DOM fallback remains only for custom `JsonCodec<T>` specializations that have no reader-specific implementation.

### 2. Reusable decode scratch

Add a small scratch object for temporary decoded keys/strings, found-bit expansion, and lazy diagnostic materialization:

```cpp
struct JsonDecodeScratch {
    std::pmr::memory_resource* resource{};
    small_vector<char, 256> key_decode_buf;
    small_vector<char, 256> string_decode_buf;
    small_vector<std::uint64_t, 4> found_bits;
};
```

Rules:

- Default construction should use inline storage only for common structs.
- Heap allocation from `scratch.resource` is allowed only when escaped keys/strings or very wide structs exceed inline capacity.
- Do not allocate diagnostic paths/messages until an error is actually returned.

### 3. Compile-time field metadata

For `JsonMembers<T>` and reflected structs, generate a shared field table shape:

```cpp
struct JsonFieldInfo {
    std::string_view name;
    std::uint64_t hash;
    std::uint32_t ordinal;
    bool required;
    bool skipped;
};
```

The metadata source differs by path:

- `JsonMembers<T>`: from `json_member("name", &T::field)` entries plus constraints.
- Reflection: from P2996 member names and annotations such as `json::name` / `json::skip`.

The decode loop must use this metadata to find the destination field once per input key, then immediately decode the following value into the target member.

### 4. Key matching policy

Key matching should avoid runtime maps in the typed path:

```text
N <= 8       -> unrolled direct string_view comparisons
9 <= N <= 32 -> hash-first switch/table, then string equality for collision safety
N > 32      -> sorted constexpr table or generated compact lookup table
```

Incoming key handling:

1. If `JsonStringToken::unescaped_borrow()` succeeds, compare the borrowed `std::string_view` directly.
2. If the key is escaped, decode into `JsonDecodeScratch::key_decode_buf`, then compare the scratch view.
3. Never allocate `std::string key_name` on the unescaped-key success path.

Unknown-member behavior:

- `reject`: return immediately after reading the unknown key, before decoding or scanning the rest of the object.
- `ignore`: consume the next value with `skip_next_value()` or the existing validated skip helper.

### 5. Required-field tracking

Replace `std::vector<bool>` in the success path with fixed-width bitsets:

```text
N <= 64      -> uint64_t found_mask
65..256      -> std::array<uint64_t, K>
N > 256      -> scratch.found_bits
```

At object end, scan required fields and produce the existing `missing_member` error shape. Optional fields remain optional. Skipped fields are ignored for required tracking.

### 6. Direct manual `JsonMembers<T>` decode

`JsonMembers<T>` should become the first implementation target because it does not need P2996 toolchain support.

Decode shape:

```cpp
T result{};
while (reader object has key) {
    auto key = borrowed_or_scratch_key(reader.key_token(), scratch);
    auto field = find_field<T>(key);
    if (!field) { reject_or_skip_unknown(); continue; }
    mark_found(field.ordinal);
    decode_member_direct(result, field, reader, opts, scratch);
    run_field_constraint_if_any(result, field);
}
check_required_fields<T>(found_bits);
return result;
```

This is the baseline that should close most allocation overhead even before reflection work.

### 7. Direct reflected decode

Add a reflection-specific direct reader codec parallel to the current `JsonCodec<T>` specialization:

```cpp
template<class T>
concept ReflectJsonReaderDecodable = ReflectJsonAggregate<T> && /* all members decodable */;

template<ReflectJsonReaderDecodable T>
std::expected<T, JsonError> decode_reflect_direct(
    JsonReader& reader,
    JsonDecodeOptions const& opts = {},
    JsonDecodeScratch* scratch = nullptr);
```

Behavior:

- Reflect direct data members.
- Respect `[[= conflux::json::name("...")]]` and `[[= conflux::json::skip{}]]`.
- Assign directly with reflected member access.
- Treat `std::optional<T>` as optional; all other non-skipped fields are required.
- Use primitive/direct container reader paths where available.
- Recurse into nested reflected structs.
- Fall back to DOM only for custom codec types that have no reader codec.

The reflection provider should prefer this direct path whenever possible.

### 8. Borrowed vs owned string policy

Add an explicit typed decode split:

```cpp
template<class T>
std::expected<T, JsonError> decode_owned(
    std::string_view input,
    JsonParseOptions const& parse_opts = {},
    JsonDecodeOptions const& decode_opts = {});

template<class T>
std::expected<T, JsonError> decode_borrowed(
    std::string_view input,
    JsonParseOptions const& parse_opts = {},
    JsonDecodeOptions const& decode_opts = {});
```

Rules:

- `decode_owned<T>` owns/copies data required by owning C++ fields.
- `decode_borrowed<T>` may populate `std::string_view` fields from input when the token is unescaped.
- Escaped string values cannot be borrowed. Strict borrowed mode should return a clear error, while arena-backed borrowed mode may materialize into caller-provided storage.
- `std::string` fields always materialize decoded strings.
- `std::string_view` fields require an explicit borrowed mode or explicit policy; do not silently return dangling views from temporary input.

This split keeps the Glaze-like zero-allocation case available without making lifetimes ambiguous.

### 9. Direct writer

Add a sink-oriented writer for manual/reflected structs:

```cpp
template<class T, class Sink>
std::expected<void, JsonError> write_json_direct(
    Sink& sink,
    T const& value,
    JsonDumpOptions const& opts = {});
```

Rules:

- Do not build `ValueBuilder` or `Document` for direct-serializable structs.
- Write field names from compile-time metadata.
- Write primitive values directly.
- Recurse into direct-writable nested structs and containers.
- Fall back to `ValueBuilder` only for custom codec types without direct writer support.
- When `Sink` is `std::string`, reserve using a cheap size estimate.
- Preserve `pretty`, `indent`, `sort_object_keys`, and `ascii_only` behavior where feasible. If sorted output would require buffering, either document the fallback or route sorted direct writes through a temporary only when requested.

### 10. Provider boundary changes

Update native providers to prefer direct typed serde:

```cpp
NativeJsonProvider::decode_json<T>(input, opts)
NativeReflectJsonProvider::decode_json<T>(input, opts)
NativeJsonProvider::dump_json<T>(value, opts)
NativeReflectJsonProvider::dump_json<T>(value, opts)
```

Decision policy:

```text
copy_input == false && T is direct-decodable -> JsonReader direct decode
copy_input == true  && T needs owning strings   -> direct decode with owned policy
T requires DOM fallback                         -> parse_dom + decode(NodeRef)
Document requested                              -> parse_dom
```

For writing:

```text
T is direct-writable -> write_json_direct into string/sink
T requires DOM       -> ValueBuilder + Document + dump fallback
Document             -> existing doc.dump path
```

Provider-neutral HTTP/framework code remains bound to `conflux.json.boundary` and should not import reflection internals directly.

### 11. HTTP integration

Typed HTTP JSON should use direct serde by default:

```cpp
http::Json<T>      // owned/direct decode into T
http::JsonView<T>  // borrowed direct decode; handler-scope lifetime
http::JsonBody     // dynamic Document/NodeRef path
```

Expected behavior:

- `Json<T>` does not parse a DOM unless `T` requires fallback.
- `JsonView<T>` is explicit and documented as handler-scope borrowed data.
- Decode errors map to consistent typed HTTP problem responses.
- Max body size and max JSON depth still use existing parser limits.
- OpenAPI/schema generation can continue to use `JsonMembers<T>`/reflection metadata without depending on runtime DOM decode.

## Implementation plan

### Phase A — final measurement

Status: implemented for in-tree manual and P2996 reflection benchmark rows.

Files likely touched:

- `benchmarks/json_bench.cxx`
- `benchmarks/bench_common.cxx` or a new benchmark-only allocation counter helper
- `benchmarks/corpus/route_payloads/*` if more typed payloads are needed

Tasks:

1. Add rows for manual/reflected struct decode through DOM and reader paths.
2. Add rows for direct writer vs `ValueBuilder`/`Document` writer once implemented.
3. Add allocation counters: allocation count, bytes allocated, peak bytes if practical.
4. Add benchmark models: small object, 8-16 field object, nested object, array of objects, out-of-order keys, escaped strings, missing required fields, unknown fields.
5. Add optional Glaze comparison target guarded by a CMake option so the core tree remains dependency-light.

Gate:

- Final report must show throughput and allocation counts for DOM, reader,
  reflection, direct decode, and direct write paths after implementation is
  complete.

Implementation notes:

- `conflux_json_bench` now reports allocation count and allocated bytes per
  iteration for the manual DOM/direct struct decode and writer matrix.
- Added `conflux_json_reflect_bench`, enabled only when `CONFLUX_JSON_REFLECT`
  is on, for P2996 reflected DOM/direct decode and writer rows.
- Added a `release-p2996-gcc` preset so reflected serde benchmarks run in a
  release build with the `std` BMI compiled under `-freflection`.
- `scripts/run-build-artifact.sh` accepts the new release P2996 preset, keeping
  benchmark execution on the same helper path as the rest of the tree.
- Reflected direct decode benchmark coverage includes primitive, string,
  optional, aggregate, vector, and fixed-array members.

### Phase B — direct `JsonMembers<T>` reader decode

Status: implemented for manual `JsonMembers<T>` reader decode.

Files likely touched:

- `src/json.cxx`
- `docs/json-api.md`
- JSON codec tests

Tasks:

1. Add `JsonDecodeScratch`.
2. Add borrowed/scratch key helper around `JsonStringToken`.
3. Replace `std::string key_name` and `std::vector<bool>` in reader `JsonMembers<T>` decode.
4. Add field dispatch helper for small `N`; keep larger table policy simple at first.
5. Add tests for unescaped keys, escaped keys, unknown reject, unknown ignore, missing required field, optional field, constraint failure.

Gate:

- Manual `JsonMembers<T>` reader decode performs zero heap allocations for primitive-only structs with unescaped keys.

Implementation notes:

- Added `JsonDecodeScratch` with inline key/string storage, overflow PMR buffers,
  and reusable found-bit storage.
- Replaced reader-path object keys with borrowed `JsonStringToken` views when
  possible, with scratch-backed escaped-key decode.
- Replaced per-object found tracking with inline bitsets for small member counts.
- Added direct reader tests for caller scratch use, escaped keys, unknown members,
  missing members, optionals, and constraints through existing JSON coverage.

### Phase C — reader codec customization point

Status: implemented for `JsonCodec<T>::decode(JsonReader&, Event, Options, Scratch*)` overload detection.

Files likely touched:

- `src/json.cxx`
- `docs/json-api.md`
- `docs/json-boundary-guide.md`

Tasks:

1. Add explicit custom reader codec concept, e.g. `JsonReaderCodec<T>` or optional `JsonCodec<T>::decode(JsonReader&, ...)` overload detection.
2. Route primitive/container/manual-member decode through direct reader dispatch.
3. Keep DOM fallback for old custom codecs.
4. Document how custom codec authors opt into direct reader decode.

Gate:

- Custom enum/value types can avoid DOM fallback if they provide direct reader decode.

Implementation notes:

- Added reader-event codec detection to the `conflux.json` dispatch path.
- Existing primitive/container/manual-member decode remains direct from
  `JsonReader`; legacy codecs without the reader overload still fall back through
  DOM parsing.

### Phase D — direct reflected reader decode

Status: implemented and validated under `debug-p2996-gcc`.

Files likely touched:

- `src/json_reflect.cxx`
- `src/json_reflect_provider.cxx`
- `docs/json-reflect.md`
- reflection examples/tests

Tasks:

1. Build reflected field table from P2996 metadata.
2. Implement direct object decode using the same dispatch/found-bit helpers as manual members.
3. Respect annotations and optional/skipped fields.
4. Prefer direct reflected decode in `NativeReflectJsonProvider::decode_json<T>`.
5. Keep DOM-shaped reflected `JsonCodec<T>` for APIs that explicitly decode from `NodeRef`.

Gate:

- Reflected primitive/string structs no longer allocate DOM storage on `decode_json<T>(input, copy_input=false)`.

Implementation notes:

- Added reflected reader-event decode overload parallel to the DOM-shaped
  reflected `JsonCodec<T>::decode(NodeRef, ...)`.
- The streaming reflected path uses borrowed/scratch key decode, respects
  annotations, optional fields, skipped fields, unknown-member policy, nested
  reflected aggregates, vector/fixed-array members, and primitive/string member
  decode.
- Added reflection tests for escaped annotation keys, ignored nested unknown
  values, and rejected unknown members.
- Fixed the `debug-p2996-gcc` preset/build wiring so reflection selects C++26
  and builds the `std` BMI with `-freflection`; this makes `std::meta` available
  through `import std` without a separate `<meta>` include in the reflection
  module.
- Removed an unnecessary global-fragment standard header from `conflux.types`
  so the reflected `std` BMI can be imported cleanly by the JSON reflection
  test graph.
- `conflux_json_reflect_tests` passes through
  `./scripts/run-build-artifact.sh /tmp/gcc-16/debug-p2996-gcc/tests/conflux_json_reflect_tests`.

### Phase E — direct writer

Status: implemented for manual `JsonMembers<T>` and reflected compact/default
provider output.

Files likely touched:

- `src/json_dump.cxx` or `src/json.cxx`, depending on current writer split
- `src/json_native_provider.cxx`
- `src/json_reflect_provider.cxx`
- `docs/json-api.md`
- `docs/json-boundary-guide.md`

Tasks:

1. Add sink concept and direct writer entry point.
2. Implement primitives, strings, arrays/ranges, optionals, manual members.
3. Implement reflected direct writer.
4. Provider `dump_json<T>` should use direct writer for eligible types.
5. Preserve fallback for dynamic/custom DOM-only codecs.

Gate:

- Reflected/manual typed JSON response serialization avoids `ValueBuilder` and `Document` on the default compact output path.

Implementation notes:

- Added `write_json_direct<T>` and `dump_direct<T>` for primitives, strings,
  optionals, nullable values, vectors, arrays, and manual `JsonMembers<T>`.
- `NativeJsonProvider::dump_json<T>` now prefers the direct writer for eligible
  manual types and falls back to the existing `ValueBuilder`/`Document` path for
  sorted output or unsupported codecs.
- Added `write_reflect_json_direct<T>` and `dump_reflect_direct<T>` for reflected
  aggregates, including annotations, skipped fields, optionals, nested reflected
  aggregates, and primitive/string members.
- `NativeReflectJsonProvider::dump_json<T>` now prefers the reflected direct
  writer for reflected aggregates and falls back to the native DOM writer for
  sorted output or unsupported codecs.
- Added compact direct writer tests for manual member structs and reflected
  structs.

### Phase F — borrowed/owned APIs and HTTP typed JSON

Status: implemented.

Files likely touched:

- `src/json_native_provider.cxx`
- `src/json_reflect_provider.cxx`
- `src/net/http_app_json.cxx`
- `docs/json-boundary-guide.md`
- `docs/http-server-api.md` or typed app docs

Tasks:

1. Add explicit `decode_owned<T>` / `decode_borrowed<T>` APIs.
2. Map boundary provider options onto owned/borrowed decode behavior.
3. Add HTTP `Json<T>`, `JsonView<T>`, and `JsonBody` guidance if not already present.
4. Add diagnostics for invalid borrowed string cases.

Gate:

- Typed HTTP JSON decode is direct by default and lifetime-safe by construction.

Implementation notes:

- Added explicit `decode_borrowed<T>` and `decode_owned<T>` entry points in
  `conflux.json`; owned string-view decode is rejected at compile time.
- Existing native boundary decode already maps `copy_input=false` to the direct
  `JsonReader` path and `copy_input=true` to the owning DOM path. The reflected
  provider preserves that split and delegates the concrete read behavior to the
  native provider.
- Typed HTTP app/router body helpers now default to
  `DecodeOptions{.copy_input = false}`, so typed route decode selects the direct
  reader path by default while keeping the request body alive for the decode
  call. Callers can still pass `copy_input=true` explicitly when they need an
  owning DOM fallback.
- Added HTTP route helper coverage proving the default decode option is borrowed.

## Benchmark matrix

Minimum rows:

```text
decode/manual/dom/small
decode/manual/reader/current-or-direct/small
decode/manual/reader/direct/medium
decode/manual/reader/direct/nested
decode/manual/reader/direct/array_objects
decode/manual/reader/direct/out_of_order
decode/manual/reader/direct/escaped_strings
decode/reflection/dom/small
decode/reflection/direct/small
decode/reflection/direct/medium
write/manual/dom
write/manual/direct
write/reflection/dom
write/reflection/direct
```

Metrics:

```text
ns/iter
MB/s
allocations/iter
allocated bytes/iter
peak bytes/iter where available
binary size delta for serde-heavy TU
compile time delta for serde-heavy TU
```

Comparison policy:

- Compare Glaze on the same structs and same input bytes.
- Include both ordered and out-of-order key payloads.
- Include malformed/error-path cases so the direct path does not only optimize happy-path input.
- Keep all raw benchmark runs, not only selected summaries.

## Correctness tests

Required tests:

- object decode into primitives
- object decode into strings
- borrowed `string_view` decode with unescaped input
- borrowed `string_view` decode with escaped input rejects or materializes according to policy
- nested struct decode
- array of structs
- optional fields
- missing required fields
- unknown-member reject
- unknown-member ignore
- duplicate JSON keys under existing duplicate-key policy
- escaped object keys
- constraint failure preserves member name
- custom codec fallback still works
- direct writer round-trip
- pretty/sorted/ascii-only writer behavior or documented fallback

## Diagnostics requirements

Direct serde errors should remain as actionable as DOM decode errors:

```text
JSON decode failed at $.users[3].id
expected: unsigned integer
actual: string
member: id
target: User::id
```

Implementation notes:

- Do not eagerly build full paths on the success path.
- Maintain a lightweight stack of field ordinals/names only when diagnostics are enabled or an error occurs.
- Preserve `member_name`, `target_type`, `source.offset`, and existing `JsonIssueCode` values where possible.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Duplicate direct/DOM codec logic diverges | Share field metadata helpers and primitive decode/write helpers. Keep DOM path as fallback, not a competing generated system. |
| Compile-time cost grows | Keep tables simple, avoid large template recursion, benchmark serde-heavy compile time, prefer module-contained implementation where possible. |
| Borrowed views dangle | Require explicit borrowed API and document HTTP handler-scope lifetime. Delete/disable borrowed decode from rvalue strings. |
| Escaped strings force allocation | Use scratch/arena; expose strict borrowed rejection for zero-allocation users. |
| Sorted/pretty writer conflicts with streaming | Compact direct writer first; sorted output may intentionally use fallback or bounded buffering. |
| Custom codecs remain slow | Add reader/writer customization points so hot custom codecs can opt in without abandoning existing `JsonCodec<T>`. |
| Error quality regresses | Add tests asserting error code/member/path/source for direct decode failures. |

## Acceptance criteria

A change series implementing this proposal is complete when:

1. Manual `JsonMembers<T>` reader decode of primitive-only structs performs zero heap allocations on unescaped-key success cases.
2. Reflected struct decode through `NativeReflectJsonProvider` avoids DOM construction when `copy_input=false` and all members are direct-decodable.
3. Direct writer avoids `ValueBuilder`/`Document` for manual and reflected structs in compact output mode.
4. Typed provider APIs preserve current boundary concepts and DOM fallback compatibility.
5. Borrowed string decode is explicit and lifetime-safe.
6. Benchmark output reports allocations and throughput for DOM vs direct decode/write.
7. Error-path tests cover missing, unknown, escaped, malformed, and constraint-failure cases.
8. Documentation explains which APIs allocate, borrow, copy, and fallback to DOM.

## Recommended priority

1. Benchmark and allocation counters.
2. Manual `JsonMembers<T>` direct reader cleanup.
3. Reader/writer codec customization points.
4. Reflected direct reader decode.
5. Direct writer.
6. Borrowed/owned API polish.
7. HTTP typed JSON integration.
8. Parser/SIMD stage work only after new profiles prove parsing dominates.
