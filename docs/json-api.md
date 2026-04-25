# conflux.json API Reference

**Module:** `conflux.json`  
**Language:** C++26  
**Import:** `import conflux.json;`

All types live in namespace `conflux::json`. All fallible operations return
`std::expected<T, JsonError>`.

---

## Parse

```cpp
expected<Document, JsonError> parse(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse(string&&      input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_borrowed(string_view input, JsonParseOptions const& opts = {});
```

- `parse(string_view)` — copies input into the Document's owned buffer.
- `parse(string&&)` — moves the string; no copy.
- `parse_borrowed(string_view)` — zero-copy: string values borrow directly from
  `input`. The caller must keep `input` alive for the Document's lifetime.
  `parse_borrowed(string&&)` is `= delete` to prevent accidental dangling.

```cpp
auto doc = parse(R"({"x": 1, "y": 2})");
if (!doc) { /* doc.error() */ }
```

### JsonParseOptions

```cpp
struct JsonParseOptions {
    LimitOption max_depth;       // default 128
    LimitOption max_input_size;  // default 4 GiB
    LimitOption max_string_size; // default unlimited
};
```

`LimitOption` accepts a `size_t` bound or `no_limit` sentinel:

```cpp
parse(input, { .max_depth = LimitOption{64} });
parse(input, { .max_string_size = no_limit });
```

---

## Document

`Document` is move-only. All borrowed handles (`NodeRef`, `ObjectView`,
`ArrayView`, `JsonNumberView`, `string_view` from `as_string()`) remain valid
across moves of the same `Document` and are invalidated when the `Document` is
destroyed.

```cpp
NodeRef    root()  const noexcept;
expected<string, JsonError> dump(JsonDumpOptions const& opts = {}) const;

expected<void, JsonError> warm_member_index (NodeRef node,                         ) const;
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
auto node = doc->root().at(JsonPath::from_pointer("/users/0/name").value());
```

`JsonPath` is a sequence of `JsonPathSegment` (`JsonPathMember` or
`JsonPathIndex`). `from_pointer` parses a JSON Pointer (RFC 6901). `at()` walks
the tree and returns `missing_member` or `index_out_of_range` on failure.

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
expected<void, JsonError>          insert_string_view(string_view name, string_view value); // borrows value; caller guarantees lifetime >= Document
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

auto doc = parse(R"({"x":3,"y":7})");
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

`has_json_codec<T>` (concept) is true when either `JsonMembers<T>` or
`JsonCodec<T>` is specialised.

### Free decode helpers

```cpp
template<has_json_codec T> expected<T, JsonError> decode(NodeRef node);
template<has_json_codec T> expected<T, JsonError> decode(Document const& d); // decodes root
```

---

## Error Handling

### JsonError

```cpp
struct JsonError {
    JsonStage             stage;
    JsonIssueCode         code;
    JsonPath              path;
    optional<JsonSourceLocation> source;      // byte offset + line/column
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
| `constraint_violation` | internal invariant broken |
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

Nullable<int64_t> v = 42LL;
v.has_value();        // true
*v;                   // 42
v.value_or(0);        // 42
```

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

- `Document` is immutable after `parse` or `ValueBuilder::finish` returns.
  The only internal mutation is lazy hash-index construction via atomic CAS.
- All borrowed handles (`NodeRef`, `ObjectView`, `ArrayView`, etc.) are valid
  across moves of the same `Document`.
- `parse_borrowed` string values point into the original `input` buffer.
  Destroying or mutating that buffer after parse is undefined behaviour.
- `insert_string_view` / `ObjectBuilder` stores a `string_view` reference
  without copying. The pointed-to data must outlive the `Document`.
- Builder sub-builders (`ObjectBuilder`, `ArrayBuilder`) must be `commit()`'d
  before the parent or `ValueBuilder::finish()` is called. Dropping without
  commit is a logic error (debug assertion in debug builds).
- Duplicate member names are rejected at insertion time (`duplicate_member`).
  Detection uses decoded UTF-8 bytes compared byte-for-byte.

---

## Known Deviations from RFC 8259

- Number lexemes longer than 4 KiB that hit the range-error slow path are
  conservatively classified as overflow. `to_f64()` returns `number_out_of_range`
  where RFC 8259 is silent on precision. Numbers ≤ 4 KiB are correctly handled
  including extreme underflow (e.g. `"0.` + 4000 zeros + `1"`).
- `dump()` always round-trips number lexemes verbatim; no precision loss.
