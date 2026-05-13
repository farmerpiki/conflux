# conflux.json API Reference

**Module:** `conflux.json`  
**Language:** C++26  
**Import:** `import conflux.json;`

All types live in namespace `conflux::json`. All fallible operations return
`std::expected<T, JsonError>`. Design-level invariants, including the single
permitted process-lifetime locale holder, are documented in `docs/json-design.md`.

---

## Parse

```cpp
expected<Document, JsonError> parse(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_borrowed(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_borrowed_unsafe(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_view(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_copy(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_copy(string&&      input, JsonParseOptions const& opts = {});
```

- `parse(string_view)` / `parse_view(string_view)` — performance-default,
  zero-copy view parse. String values borrow directly from `input`.
- `parse_borrowed(string_view)` / `parse_borrowed_unsafe(string_view)` —
  explicit aliases for `parse_view` when a review should notice that the
  returned `Document` contains views into caller-owned bytes.
- `parse_copy(string_view)` — copies input into the `Document`'s owned buffer.
- `parse_copy(string&&)` — moves the input string into the `Document`; no copy.
- `parse(string&&)`, `parse_view(string&&)`, and `parse_borrowed(string&&)` are
  `= delete` to prevent accidental dangling. Use `parse_copy(std::move(s))` for
  rvalue strings.

```cpp
auto doc = parse_view(R"({"x": 1, "y": 2})");
if (!doc) { /* doc.error() */ }
```

Borrowed/view parsing is only valid when the backing bytes are stable:

```cpp
std::string input = load_config();
auto doc = parse_view(input);
// Do not mutate, clear, resize, or destroy input while doc is alive.
```

Use explicit owning parsing when the source buffer will not outlive the document:

```cpp
auto doc = parse_copy(load_config());
```

### JsonParseOptions

```cpp
enum class ParseMode : u8 { strict, json5 };

enum class DuplicateKeyPolicy : u8 {
    reject,      // RFC 8259 recommended; default
    last_wins,   // keep last value; first occurrence's name position preserved
    first_wins,  // keep first value; duplicate parsed for syntax, then discarded
};

struct JsonParseOptions {
    LimitOption        max_depth;        // default 128
    LimitOption        max_input_size;   // default 128 MiB
    LimitOption        max_string_size;  // default 64 MiB
    ParseMode          mode{ParseMode::strict};
    DuplicateKeyPolicy duplicate_key{DuplicateKeyPolicy::reject};
    std::optional<u32> warm_threshold{}; // auto-warm object index when member count >= threshold
};
```

`LimitOption` accepts a `size_t` bound or `no_limit` sentinel:

```cpp
parse_view(input, { .max_depth = LimitOption{64} });
parse_view(input, { .max_string_size = no_limit });
```

**`ParseMode::json5`** accepts a subset of JSON5: single-line `//` and block `/* */` comments, trailing commas in objects and arrays, unquoted keys (identifier characters), and single-quoted strings. This is not full JSON5; the accepted subset matches what the test suite covers.

**`DuplicateKeyPolicy`** controls parser behavior when an object has repeated member names. Default is `reject` (returns `duplicate_member` error). `last_wins` and `first_wins` allow lossy ingestion of non-conforming inputs. Security note: use `reject` for untrusted input — duplicate-key ambiguity has been exploited in JSON security bypasses.

**`warm_threshold`** — if set and an object's member count is ≥ the threshold (and ≥ the internal `kHashThreshold`), the parser automatically builds a hash index for that object during parse rather than waiting for an explicit `warm_member_index` call.

---

## Document

`Document` is move-only. All borrowed handles (`NodeRef`, `ObjectView`,
`ArrayView`, `JsonNumberView`, `string_view` from `as_string()`) remain valid
across moves of the same `Document` and are invalidated when the `Document` is
destroyed.

```cpp
NodeRef    root()  const noexcept;
expected<string, JsonError> dump(JsonDumpOptions const& opts = {}) const;

expected<void, JsonError> warm_member_index(NodeRef node) const;
expected<void, JsonError> warm_member_indices(WarmIndexOptions const& opts = {}) const;
```

`warm_member_index` builds a hash index for one object node, enabling O(1)
`find_member` instead of O(n) linear scan. `warm_member_indices` warms every
object in the document up to the supplied limits.

### JsonDumpOptions

```cpp
struct JsonDumpOptions {
    bool     pretty{false};
    unsigned indent{2};
    bool     sort_object_keys{false};
    bool     ascii_only{false};
};
```

### WarmIndexOptions

```cpp
struct WarmIndexOptions {
    size_t max_objects{SIZE_MAX};    // stop after warming this many objects
    size_t max_extra_bytes{SIZE_MAX}; // stop after allocating this many bytes
};
```

Partial warming is valid; `find_member` falls back to linear scan on un-warmed
objects.

---

## Read API

### NodeRef

Lightweight handle to a node inside a `Document`. Default-constructed handles
are null (kind `null`, all accessors return `wrong_kind`).

```cpp
JsonKind kind()    const noexcept;
bool     is_null() const noexcept;

expected<ObjectView,     JsonError> as_object() const;
expected<ArrayView,      JsonError> as_array()  const;
expected<bool,           JsonError> as_bool()   const;
expected<string_view,    JsonError> as_string()  const;
expected<JsonNumberView, JsonError> as_number()  const;

expected<NodeRef, JsonError> at(JsonPath const& path) const;
```

### JsonKind

```cpp
enum class JsonKind { object, array, string, number, boolean, null };
```

### ObjectView

```cpp
size_t                       size()        const noexcept;
optional<NodeRef>            find_member(string_view name) const noexcept; // O(1) if warmed, O(n) otherwise
expected<NodeRef, JsonError> member     (string_view name) const;          // missing_member on not found
ObjectMemberRange            members()     const noexcept;
```

`ObjectMember` holds `string_view name` and `NodeRef value`.

```cpp
auto obj = *doc->root().as_object();
for (auto [name, val] : obj.members()) { /* ... */ }
if (auto m = obj.find_member("key")) { /* use *m */ }
auto val = obj.member("key");  // error if missing
```

### ArrayView

```cpp
size_t                       size()           const noexcept;
expected<NodeRef, JsonError> element(size_t i) const; // index_out_of_range if i >= size()
ArrayElementRange            elements()        const noexcept;
```

```cpp
auto arr = *doc->root().as_array();
for (auto elem : arr.elements()) { /* NodeRef */ }
auto third = arr.element(2);
```

### JsonNumberView

Preserves the original lexeme; defers numeric conversion.

```cpp
string_view              lexeme() const noexcept;   // raw JSON text
JsonNumberForm           form()   const noexcept;   // integer or non_integer
expected<int64_t,  JsonError> to_i64() const;
expected<uint64_t, JsonError> to_u64() const;
expected<double,   JsonError> to_f64() const;
```

```cpp
enum class JsonNumberForm { integer, non_integer };
```

`to_i64`/`to_u64` require `form() == integer`; `to_f64` works for both.
Out-of-range values return `number_out_of_range`.

### JsonPath and `at()`

```cpp
struct JsonPathMember { string name; };
struct JsonPathIndex  { size_t index{}; };
using  JsonPathSegment = variant<JsonPathMember, JsonPathIndex>;

class JsonPath {
public:
    static JsonPath root();
    static expected<JsonPath, JsonError> from_pointer(string_view sv); // parses RFC 6901 JSON Pointer

    bool   empty()  const noexcept;
    size_t size()   const noexcept;
    void   push_member(string_view name);
    void   push_index (size_t idx);
    void   pop()          noexcept;
    string to_pointer()   const;  // serialise back to RFC 6901 string

    // range-for over JsonPathSegment elements
    auto begin() const noexcept;
    auto end()   const noexcept;
};

auto node = doc->root().at(JsonPath::from_pointer("/users/0/name").value());
```

`JsonPath` is a sequence of `JsonPathSegment`. `from_pointer` parses a JSON
Pointer (RFC 6901). `at()` walks the tree and returns `missing_member` or
`index_out_of_range` on failure.

---

## Build API

Build a `Document` programmatically via `value_builder()`:

```cpp
ValueBuilder b = value_builder();
// set exactly one value, then finish
expected<Document, JsonError> doc = move(b).finish();
```

### ValueBuilder

```cpp
expected<void, JsonError>          set_null();
expected<void, JsonError>          set_bool  (bool v);
expected<void, JsonError>          set_string(string_view sv);   // copies into arena
expected<void, JsonError>          set_number(string_view lexeme); // raw JSON number text
expected<void, JsonError>          set_i64   (int64_t v);
expected<void, JsonError>          set_u64   (uint64_t v);
expected<void, JsonError>          set_f64   (double v);
expected<ObjectBuilder, JsonError> begin_object();
expected<ArrayBuilder,  JsonError> begin_array();

template<has_json_codec T>
expected<void, JsonError>          set(T const& v);  // uses JsonCodec<T>::encode

void                               reset()    noexcept; // discard pending value, reuse builder
void                               discard() && noexcept;
expected<Document, JsonError>      finish()  &&;       // consumes builder
```

### ObjectBuilder

```cpp
expected<void, JsonError>          insert_null  (string_view name);
expected<void, JsonError>          insert_bool  (string_view name, bool v);
expected<void, JsonError>          insert_string(string_view name, string_view value); // copies value
expected<void, JsonError>          insert_string_borrowed_name(string_view name, string_view value); // name NOT copied — must outlive Document; value is copied
expected<void, JsonError>          insert_string_borrowed(string_view name, string_view value);      // NEITHER copied — both must outlive Document
expected<void, JsonError>          insert_number(string_view name, string_view lexeme);
expected<void, JsonError>          insert_i64   (string_view name, int64_t v);
expected<void, JsonError>          insert_u64   (string_view name, uint64_t v);
expected<void, JsonError>          insert_f64   (string_view name, double v);
expected<ObjectBuilder, JsonError> insert_object(string_view name);
expected<ArrayBuilder,  JsonError> insert_array (string_view name);

template<has_json_codec T>
expected<void, JsonError>          insert(string_view name, T const& v);

void commit() && noexcept;  // must be called to attach to parent
```

Duplicate member names return `duplicate_member` error at insertion time.
`commit()` must be called (as an rvalue) before the parent builder or
`ValueBuilder::finish()` is invoked.

### ArrayBuilder

```cpp
expected<void, JsonError>          append_null();
expected<void, JsonError>          append_bool  (bool v);
expected<void, JsonError>          append_string(string_view v);
expected<void, JsonError>          append_number(string_view lexeme);
expected<void, JsonError>          append_i64   (int64_t v);
expected<void, JsonError>          append_u64   (uint64_t v);
expected<void, JsonError>          append_f64   (double v);
expected<ObjectBuilder, JsonError> append_object();
expected<ArrayBuilder,  JsonError> append_array();

template<has_json_codec T>
expected<void, JsonError>          append(T const& v);

void commit() && noexcept;
```

### Build example

```cpp
auto b  = value_builder();
auto ob = *b.begin_object();
ob.insert_i64   ("id",    42LL);
ob.insert_string("name", "alice");
auto ab = *ob.insert_array("tags");
ab.append_string("admin");
ab.append_string("user");
move(ab).commit();
move(ob).commit();
auto doc = move(b).finish();
// {"id":42,"name":"alice","tags":["admin","user"]}
```

---

## Codec System

### Built-in codecs

`decode<T>` and `encode` (via `ValueBuilder::set`) work out of the box for:

| Type | JSON |
|------|------|
| `bool` | `true` / `false` |
| `int64_t` | integer number |
| `uint64_t` | integer number |
| `double` | number |
| `string` | string (copied) |
| `string_view` | string (borrowed — caller ensures lifetime) |
| `optional<T>` | `null` or `T` |
| `Nullable<T>` | `null` or `T` (null-aware wrapper) |
| `vector<T>` | array |
| `array<T, N>` | array (exact size required) |
| `pair<A, B>` | two-element array |
| `tuple<Ts...>` | N-element array |
| `map<string, T>` | object |
| `unordered_map<string, T>` | object |

### `json_member` helper

```cpp
template<class T, class M>
struct JsonMember {
    string_view  name;
    M T::*       pointer;
};

template<class T, class M>
constexpr JsonMember<T, M> json_member(string_view name, M T::* p);
```

Convenience factory used inside `JsonMembers<T>::members()` tuples.

### Struct decode via `JsonMembers`

Specialise `JsonMembers<T>` to decode structs. Unknown members and missing
required members are errors. `optional<T>` fields are optional; all others
are required.

```cpp
struct Point { int64_t x{}; int64_t y{}; };

template<>
struct JsonMembers<Point> {
    static constexpr auto members() {
        return std::tuple{
            json_member("x", &Point::x),
            json_member("y", &Point::y),
        };
    }
    static constexpr string_view type_name() { return "Point"; }
};

auto doc = parse_view(R"({"x":3,"y":7})");
auto pt  = decode<Point>(doc->root());  // expected<Point, JsonError>
```

### Custom codec via `JsonCodec`

Specialise `JsonCodec<T>` for types that need custom decode/encode logic (e.g.
enums, types with invariants).

```cpp
template<>
struct JsonCodec<Color> {
    static expected<Color, JsonError> decode(NodeRef n) {
        auto s = n.as_string();
        if (!s) return unexpected(move(s).error());
        if (*s == "red")   return Color::red;
        if (*s == "green") return Color::green;
        return unexpected(JsonError{
            .stage = JsonStage::decode,
            .code  = JsonIssueCode::invalid_value,
            .message = std::format("unknown Color: {}", *s)});
    }
    static expected<void, JsonError> encode(ValueBuilder& b, Color c) {
        switch (c) {
        case Color::red:   return b.set_string("red");
        case Color::green: return b.set_string("green");
        }
        return unexpected(JsonError{.stage = JsonStage::build,
                                    .code  = JsonIssueCode::invalid_value});
    }
    static constexpr string_view type_name() { return "Color"; }
};
```

`has_json_codec<T>` (concept) and `has_json_codec_v<T>` (variable template)
are true when either `JsonMembers<T>` or `JsonCodec<T>` is specialised.

### Free decode helpers

```cpp
template<has_json_codec T> expected<T, JsonError> decode(NodeRef node);
template<has_json_codec T> expected<T, JsonError> decode(Document const& d); // decodes root
template<class T> expected<T, JsonError> decode(JsonReader& r);              // full document
template<class T> expected<T, JsonError> decode_full(JsonReader& r);
template<class T> expected<T, JsonError> decode_full(string_view input);
template<class T> expected<T, JsonError> decode_next(JsonReader& r);          // streaming
```

`decode(JsonReader&)` and `decode_full(...)` require EOF after one decoded root
value and return `trailing_garbage` if another top-level value remains.
`decode_next(...)` is the explicit streaming form for NDJSON-like loops or other
multi-value inputs.

`JsonReader::skip_next_value()` validates the skipped value by consuming normal
reader events and returns the byte range it consumed.

---

## Error Handling

### JsonError

```cpp
struct JsonError {
    JsonStage             stage;
    JsonIssueCode         code;
    JsonPath              path;
    optional<JsonSourceLocation> source;      // byte offset + line/column (1-based)
    optional<JsonKind>    expected_kind;
    optional<JsonKind>    actual_kind;
    optional<string>      member_name;
    optional<string>      target_type;
    optional<size_t>      requested_index;
    optional<size_t>      container_size;
    string                message;

    JsonError with_prefix(JsonPath const& prefix) const&;
    JsonError with_prefix(JsonPath const& prefix) &&;
};
```

### JsonSourceLocation

```cpp
struct JsonSourceLocation {
    size_t offset{};    // byte offset from start of input
    size_t line{1};     // 1-based line number
    size_t column{1};   // 1-based column number
};
```

### JsonStage

```cpp
enum class JsonStage { parse, lookup, decode, build, dump };
```

### JsonIssueCode

| Code | When |
|------|------|
| `syntax_error` | malformed JSON text |
| `unexpected_eof` | input ends mid-value |
| `trailing_garbage` | extra content after root value |
| `invalid_utf8` | illegal UTF-8 byte sequence |
| `invalid_unicode_escape` | bad `\uXXXX` escape |
| `invalid_pointer` | malformed JSON Pointer |
| `duplicate_member` | object has repeated key |
| `missing_member` | required key absent in decode |
| `wrong_kind` | accessor kind mismatch |
| `index_out_of_range` | array index ≥ size |
| `invalid_number` | number lexeme not parseable |
| `number_out_of_range` | value overflows target type |
| `sign_mismatch` | negative number into `uint64_t` |
| `invalid_value` | value rejected by codec logic |
| `constraint_violation` | internal invariant or user codec constraint violated |
| `nesting_too_deep` | nesting exceeds `max_depth` |
| `input_too_large` | input exceeds `max_input_size` |
| `string_too_large` | string exceeds `max_string_size` |
| `output_too_large` | dump output overflows |
| `resource_exhausted` | allocation or effort limit hit |

---

## Nullable\<T\>

A null-aware wrapper for types that must distinguish JSON `null` from
"absent". Unlike `optional<T>`, `Nullable<T>` is always present in a decoded
struct — it just may be null.

```cpp
Nullable<int64_t> n = nullptr;
n.is_null();          // true
n.has_value();        // false
bool(n);              // false

Nullable<int64_t> v = 42LL;
v.has_value();        // true
bool(v);              // true
*v;                   // 42 (UB if null)
v.value();            // 42 (UB if null)
v.value_or(0);        // 42
v->some_method();     // arrow access (UB if null)
```

Supports `==` and `<=>` comparison (null compares less than any value;
two nulls compare equal).

Decoded from JSON `null` → null state. Decoded from any non-null JSON value →
populated state via `JsonCodec<T>::decode`.

---

## Node Identity Utilities

```cpp
bool is_same_node       (NodeRef a, NodeRef b) noexcept; // same physical node
bool is_value_equal     (NodeRef a, NodeRef b);          // structural equality, numbers compared as f64
bool is_value_equal_exact(NodeRef a, NodeRef b);         // structural equality, numbers compared by lexeme

struct NodeIdentityHash  { size_t operator()(NodeRef) const noexcept; };
struct NodeIdentityEqual { bool   operator()(NodeRef, NodeRef) const noexcept; };
```

`NodeIdentityHash` / `NodeIdentityEqual` allow using `NodeRef` as a key in
`unordered_map` / `unordered_set` based on physical identity.

---

## Contracts and Lifetime Rules

- `Document` is immutable after `parse`, `parse_view`, `parse_copy`, or `ValueBuilder::finish` returns.
  The only internal mutation is lazy hash-index construction via atomic CAS.
- All borrowed handles (`NodeRef`, `ObjectView`, `ArrayView`, etc.) are valid
  across moves of the same `Document`.
- `parse`, `parse_view`, `parse_borrowed`, and `parse_borrowed_unsafe` string values point into the original `input` buffer.
  Destroying or mutating that buffer after parse is undefined behaviour.
- `parse_copy` stores the bytes inside the returned `Document`; use it whenever
  the source buffer cannot remain alive and immutable.
- `NdjsonRange` returns per-line `Document` values that borrow from the original
  NDJSON buffer. The entire NDJSON input must remain alive and immutable while
  any iterator result is alive.
- `insert_string_borrowed_name` stores the member *name* pointer without
  copying. The name data must outlive the `Document`.
- `insert_string_borrowed` / `append_string_borrowed` store neither name nor
  value — both pointers must outlive the `Document`. Zero-copy path for
  caller-owned strings (e.g. mapped file content, arena-allocated data).
- Builder sub-builders (`ObjectBuilder`, `ArrayBuilder`) must be `commit()`'d
  before the parent or `ValueBuilder::finish()` is called. Dropping without
  commit is a logic error (debug assertion in debug builds).
- Duplicate member names in the **builder** are always rejected at insertion time (`duplicate_member`). Detection uses decoded UTF-8 bytes compared byte-for-byte.
- Duplicate member names during **parsing** follow `JsonParseOptions::duplicate_key` (default: `reject`).

---

## Arena-backed parsing (`JsonArena`)

`JsonArena` reuses a single monotonic allocation region across multiple parse calls. Useful for request-scoped JSON: parse, process, then `reset()` the arena rather than allocating and freeing `Document` per request.

```cpp
struct JsonArenaOptions {
    size_t initial_slab_bytes; // pre-allocated slab size
};

class JsonArena {
public:
    explicit JsonArena(JsonArenaOptions const&);

    expected<ArenaDocument, JsonError> parse_into      (string_view,  JsonParseOptions const& = {});
    expected<ArenaDocument, JsonError> parse_borrowed_into(string_view, JsonParseOptions const& = {});
    expected<ArenaDocument, JsonError> parse_moved_into(string&&,     JsonParseOptions const& = {});

    void   reset();           // invalidates all ArenaDocuments; reuses memory
    size_t slab_capacity() const;
    size_t slab_used()     const;
};
```

`ArenaDocument` is a handle into the arena's storage. **All `ArenaDocument` handles are invalidated by `reset()`** — do not hold them across a reset.

```cpp
JsonArena arena{JsonArenaOptions{.initial_slab_bytes = 1024 * 1024}};

for (auto const& raw : requests) {
    auto doc = arena.parse_into(raw);
    if (doc) process(doc->root());
    arena.reset();
}
```

---

## NDJSON / Streaming

### `NdjsonRange`

Streaming range over newline-delimited JSON. Each iteration parses one line and yields `expected<Document, JsonError>`.

```cpp
class NdjsonRange {
public:
    explicit NdjsonRange(string_view input, JsonParseOptions const& = {});

    // range-for: yields expected<Document, JsonError> per line
    auto begin() const;
    default_sentinel_t end() const;
};
```

The entire `input` buffer must remain alive and immutable while any iterator or document derived from it is alive (same borrowed-parse lifetime rule).

```cpp
for (auto& result : NdjsonRange{raw_ndjson}) {
    if (!result) { /* result.error() */ continue; }
    process(result->root());
}
```

### `JsonAccumulator`

Incremental buffer accumulator for streaming input where the full JSON value arrives in chunks.

```cpp
class JsonAccumulator {
public:
    explicit JsonAccumulator(JsonParseOptions const& = {});

    expected<void, JsonError>     feed  (string_view chunk); // append chunk; validates size
    expected<Document, JsonError> finish();                  // parse accumulated buffer
    void                          reset();                   // clear buffer for reuse
};
```

---

## Schema validation

Generate a JSON Schema (draft-07 subset) from a codec-registered type, then validate any `NodeRef` against it.

```cpp
template<class T>
requires(has_members_spec<T> || has_codec_spec<T>)
expected<Document, JsonError> schema_for();

[[nodiscard]] expected<void, JsonError> validate(NodeRef root, NodeRef schema);
```

`schema_for<T>()` returns a `Document` containing the schema. Pass its root to `validate` along with the data node to check.

```cpp
auto schema_doc = schema_for<MyStruct>();
// schema_doc->root() is the schema NodeRef

auto data_doc = parse_view(raw_json);
auto result = validate(data_doc->root(), schema_doc->root());
if (!result) { /* result.error() describes the violation */ }
```

The schema reflects the same members and types that `decode<T>` would accept. Optional fields become non-required schema properties. Custom `JsonCodec<T>` specializations are not introspected — only `JsonMembers<T>` yields a meaningful schema.

---

## Known Deviations from RFC 8259

- Number lexemes longer than 1024 bytes are rejected at parse time with
  `invalid_number`. RFC 8259 imposes no length limit; this is a DoS-hardening
  measure. Real-world numbers fit in well under 100 bytes.
- Number lexemes longer than 4 KiB that hit the range-error slow path are
  conservatively classified as overflow. `to_f64()` returns `number_out_of_range`
  where RFC 8259 is silent on precision. Numbers ≤ 4 KiB are correctly handled
  including extreme underflow (e.g. `"0.` + 4000 zeros + `1"`).
- `dump()` always round-trips number lexemes verbatim; no precision loss.
