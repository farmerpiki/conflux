# conflux.json API Reference

**Module:** `conflux.json`  
**Language:** C++23 baseline; optional reflection provider edges require C++26  
**Import:** `import conflux.json;`

All types live in namespace `conflux::json`. Core JSON parser and DOM operations
return `std::expected<T, JsonError>`; boundary and file helpers map failures to
`conflux::json::boundary::Error` and `JsonFileError`. Design-level invariants,
including the single permitted process-lifetime locale holder, are documented in
`docs/json-design.md`.
For ownership, allocation, and borrowed-string lifetime summaries across JSON
and HTTP JSON helpers, see [`cost-lifetime-model.md`](cost-lifetime-model.md).

---

## Provider boundary traits

`conflux.json.boundary` is the provider-neutral JSON serde boundary used by
framework/app-facing components. It intentionally does not pick a parser or DOM.
Provider modules satisfy the concepts and can be swapped without changing route
helpers.

```cpp
import conflux.json.boundary;

namespace jb = conflux::json::boundary;

template<class Provider, class T>
concept JsonDumpProvider = requires(T const& value, jb::DumpOptions const& opts) {
    { Provider::dump_json(value, opts) } -> same_as<expected<string, jb::Error>>;
};

template<class Provider, class T>
concept JsonDecodeProvider = requires(string_view input, jb::DecodeOptions const& opts) {
    { Provider::template decode_json<T>(input, opts) } -> same_as<expected<T, jb::Error>>;
};
```

`conflux.json.native_provider` adapts the current `conflux.json` parser/DOM to
that boundary:

```cpp
import conflux.json.native_provider;

using Provider = conflux::json::boundary::NativeJsonProvider;

auto doc  = Provider::parse_json_document(body, {.copy_input = true});
auto json = conflux::json::boundary::dump_with<Provider>(*doc);
auto id   = conflux::json::boundary::decode_with<Provider, i64>("42");

std::string out;
auto ok = conflux::json::boundary::write_with<Provider>(
    *doc,
    [&](std::string_view chunk) { out.append(chunk); });
```

`write_with` is the route/transport-facing writer adaptor. Providers may expose
a direct `write_json(value, opts, sink)` fast path; otherwise the boundary falls
back to `dump_json` and forwards the single produced chunk. The fallback keeps
the current native provider behavior explicit while allowing streaming providers
to avoid an intermediate owning JSON string later.

HTTP JSON framework helpers depend on `conflux.json.boundary`, not the native
provider. Provider-explicit APIs use the `*_with<Provider>` suffix. Typed
app/router helpers live in `conflux.net.http.app_json` and require an explicit
provider for both request-body decode and response serialization. The native
convenience edge lives in `conflux.net.http.native_json`, which imports
`conflux.json.native_provider` and restores default-provider overloads for app
code that intentionally chooses the current native adapter. Custom providers
should implement the same static `dump_json` / `decode_json` /
`parse_json_document` shape, and may add `write_json(value, opts, sink)` for
direct chunked output. Providers return `conflux::json::boundary::Error` rather
than leaking provider-specific errors through framework boundaries.

Boundary `DecodeOptions` includes `copy_input` and `unknown_members`. The native
provider maps `copy_input` onto `JsonDomPolicy::owning_document()` or
`JsonDomPolicy::view_first()`, and maps `unknown_members` onto native
`JsonDecodeOptions`.

Reflection serde remains an optional provider edge. Import
`conflux.json.reflect_provider` and pass
`conflux::json::boundary::NativeReflectJsonProvider` to boundary/app helpers when
aggregate P2996 serde is desired. Framework modules should not import
`conflux.json.reflect` directly.

---

## Parse

```cpp
expected<Document, JsonError> parse(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse(string&&      input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_borrowed(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_borrowed_unsafe(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_view(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_copy(string_view input, JsonParseOptions const& opts = {});
expected<Document, JsonError> parse_copy(string&&      input, JsonParseOptions const& opts = {});
```

- `parse(string_view)` / `parse_view(string_view)` — performance-default,
  zero-copy view parse. String values borrow directly from `input`.
- `parse(string&&)` — convenience owning parse for temporary `std::string`
  values; equivalent to `parse_copy(std::move(input))`.
- `parse_borrowed(string_view)` / `parse_borrowed_unsafe(string_view)` —
  explicit aliases for `parse_view` when a review should notice that the
  returned `Document` contains views into caller-owned bytes.
- `parse_copy(string_view)` — copies input into the `Document`'s owned buffer.
- `parse_copy(string&&)` — moves the input string into the `Document`; no copy.
- `parse_view(string&&)` and `parse_borrowed(string&&)` are
  `= delete` to prevent accidental dangling, including PMR overloads that take
  a caller-supplied memory resource.

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

### Compile-time JSON literal direction

Compile-time JSON literals are not exported in the current preview. The planned
shape is a constexpr-validated, zero-runtime-parse literal wrapper rather than a
second DOM type:

```cpp
constexpr auto spec = json_literal<R"({"enabled":true,"limit":4})">();
static_assert(spec.valid());

auto doc = spec.document();          // runtime Document view over static bytes
auto cfg = spec.decode<Config>();    // same decode<T> contract and errors
```

The return type should be an immutable `JsonLiteral<N>` value that stores the
source bytes, compile-time validation status, and compact structural metadata
needed to build a normal `Document` view or drive typed decode. It should not
own allocator state and should not expose a parallel node API. Runtime-facing
code should still see `Document`, `JsonError`, and `decode<T>` semantics.

`decode<T>` integration should reuse the existing typed decode machinery and
`JsonDecodeOptions`. Literal decode may specialize supported scalar/aggregate
paths when the metadata is available at compile time, but error reporting must
still identify the JSON path and decode stage in the same terms as runtime
decode. Types containing borrowed fields may borrow from the static literal
bytes; owning decode should keep the existing `decode_owned<T>` lifetime rules.

Implementation should start with strict JSON only, duplicate-key rejection, and
the existing default parser limits. JSON5 literals, custom duplicate-key
policies, and provider-boundary literal support should be separate follow-up
work.

### Parser/DOM prototype facade

`JsonDomPolicy` names the planned parser/DOM architecture without replacing the
current parser yet. It is useful for code and tests that need to choose the
memory model explicitly while preserving one future-compatible API.

```cpp
JsonDomPolicy view   = JsonDomPolicy::view_first();       // borrowed bytes
JsonDomPolicy owned  = JsonDomPolicy::owning_document();  // copy/move bytes
JsonDomPolicy pmr    = JsonDomPolicy::caller_pmr();       // caller resource
JsonDomPolicy arena  = JsonDomPolicy::arena_reuse();      // JsonArena storage

auto a = parse_dom(body, view);
auto b = parse_dom(std::string{body}, owned);
auto c = parse_dom(body, resource, pmr);
auto d = parse_dom(json_arena, body, arena);
```

The facade fixes these design choices for future parser work: strings use
`view_unescaped_copy_decoded`, numbers use `preserve_lexeme_parse_on_access`,
UTF-8 is `strict_validate_on_parse`, errors are `expected_json_error`, and object
lookup preserves order with on-demand hash warming. Policy/storage mismatches
return `JsonIssueCode::constraint_violation`. See `docs/json-dom-prototype.md`
for the branch design notes.

### Optional sync file helpers

File parsing is intentionally kept out of `conflux.json`. Import
`conflux.json.file` and link `conflux::json_file` when the convenience boundary
is useful:

```cpp
import conflux.json.file;

expected<Document, JsonFileError> blocking_parse_file_at(
    int root_fd,
    string_view contained_relative_path,
    JsonParseOptions const& opts = {});
expected<Document, JsonFileError> blocking_parse_file(
    string_view path,
    JsonParseOptions const& opts = {});
```

`blocking_parse_file_at` reads through `conflux.file_io_sync`, then calls
`parse_copy(std::string&&)`, so the returned `Document` owns the bytes. The read
limit mirrors `JsonParseOptions::max_input_size`: default 128 MiB, explicit bound
when supplied, or unbounded only when `max_input_size = no_limit`.

`blocking_parse_file_at` is a contained-path helper: `contained_relative_path`
must be relative to `root_fd` and must not escape it. `blocking_parse_file` is
the generic convenience helper and accepts the same absolute or cwd-relative
paths as `conflux::file_io_sync::blocking_read_text_file`.

The preview API advertises `blocking_parse_file_at` and
`blocking_parse_file` for file-backed parsing.

### JsonParseOptions

```cpp
enum class ParseMode : u8 { strict, json5 };

enum class DuplicateKeyPolicy : u8 {
    reject,      // RFC 8259 recommended; default
    last_wins,   // keep last value; first occurrence's name position preserved
    first_wins,  // keep first value; duplicate parsed for syntax, then discarded
};

struct JsonParseOptions {
    LimitOption                 max_depth;        // default 128
    LimitOption                 max_input_size;   // default 128 MiB
    LimitOption                 max_string_size;  // default 64 MiB
    DuplicateKeyPolicy          duplicate_key{DuplicateKeyPolicy::reject};
    std::optional<std::uint32_t> warm_threshold{}; // auto-warm object index when member count >= threshold
    ParseMode                   mode{ParseMode::strict};
};
```

`LimitOption` accepts a `size_t` bound or `no_limit` sentinel:

```cpp
parse_view(input, { .max_depth = LimitOption{64} });
parse_view(input, { .max_string_size = no_limit });
```

**`ParseMode::json5`** accepts a subset of JSON5: single-line `//` and block `/* */` comments, trailing commas in objects and arrays, unquoted keys (identifier characters), and single-quoted strings. This is not full JSON5; the accepted subset matches what the test suite covers.

**`DuplicateKeyPolicy`** controls parser behavior when an object has repeated member names. Default is `reject` (returns `duplicate_member` error). `last_wins` and `first_wins` allow lossy ingestion of non-conforming inputs. Security note: use `reject` for untrusted input — duplicate-key ambiguity has been exploited in JSON security bypasses. Streaming typed decode honors this policy for duplicate known fields. Unknown duplicate fields under `UnknownMemberPolicy::ignore` are consumed and validated but are not tracked for duplicate-key rejection.

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
    bool                       pretty{false};
    unsigned                   indent{2};
    bool                       sort_object_keys{false};
    bool                       ascii_only{false};
    char                       indent_char{' '};
    std::optional<std::size_t> truncate_depth{};
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
are invalid; call `is_valid()` before using a stored handle that may not have
come from a `Document`.

```cpp
bool     is_valid() const noexcept;
JsonKind kind()    const noexcept;
bool     is_null() const noexcept;

expected<ObjectView,     JsonError> as_object() const;
expected<ArrayView,      JsonError> as_array()  const;
expected<bool,           JsonError> as_bool()   const;
expected<string_view,    JsonError> as_string()  const;
expected<JsonNumberView, JsonError> as_number()  const;

expected<NodeRef, JsonError> at(JsonPath const& path) const;
expected<string, JsonError> dump(JsonDumpOptions const& opts = {}) const;
```

`NodeRef::dump()` serializes that node and its descendants as a JSON subtree.
It uses the same `JsonDumpOptions` contract as `Document::dump()`: formatting is
regenerated, original whitespace is not preserved, and number lexemes follow the
dumper's existing round-trip behavior.

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
`index_out_of_range` on failure. `NodeRef::at_pointer(string_view)` is the
convenience form when the caller already has an RFC 6901 string.

### JSON Merge Patch

```cpp
expected<Document, JsonError> merge_patch(NodeRef target, NodeRef patch);
expected<Document, JsonError> merge_patch(Document const& target, Document const& patch);
```

`merge_patch` implements RFC 7396 semantics against the immutable DOM by
building a new owning `Document`: non-object patches replace the target, object
patch members with `null` delete target members, object/object pairs merge
recursively, and arrays are replaced as whole values. Target member order is
preserved for unchanged/replaced members; new patch members are appended in patch
order.

### JSON Patch

```cpp
namespace conflux::json {

enum class JsonPatchOp { add, remove, replace, move, copy, test };

struct JsonPatchOptions {
    size_t max_operations = 1024;
    size_t max_pointer_depth = 128;
    size_t max_result_nodes = 1'000'000;
    bool reject_duplicate_object_members = true;
    bool allow_missing_remove = false;
};

expected<Document, JsonError>
apply_patch(NodeRef target, NodeRef patch, JsonPatchOptions opts = {});
expected<Document, JsonError>
apply_patch(Document const& target, Document const& patch, JsonPatchOptions opts = {});
expected<void, JsonError>
validate_patch(NodeRef patch, JsonPatchOptions opts = {});

}
```

`apply_patch` implements RFC 6902 over the immutable DOM and returns a new
owning `Document`. `add`, `remove`, `replace`, `move`, `copy`, and `test` are
supported. Root replacement is allowed through `add`/`replace`; root removal is
rejected with `patch_remove_document_root`. `max_result_nodes` bounds the
expanded candidate tree after each operation so small patches cannot amplify
`copy` operations into unbounded memory growth. Failed operations do not
mutate the input document. Patch diagnostics use `JsonStage::json_patch` and stable
`JsonIssueCode::patch_*` values, with operation index and pointer text attached
when available.

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
expected<void, JsonError>          set(T const& v);  // uses conflux::json::JsonCodec<T>::encode

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

For compact construction, `conflux::json::object` and `conflux::json::array`
layer lambda sugar over the same builders:

```cpp
auto doc = conflux::json::object([](auto& obj) {
    obj("id", 42);
    obj("name", "alice");
    obj.array("tags", [](auto& arr) {
        arr("admin");
        arr("user");
    });
});
```

The helper returns `expected<Document, JsonError>`. Ignored insert/append errors
are captured by the writer and returned by the outer helper.

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
| `string` / `basic_string<char, Traits, Alloc>` | string (copied; allocator-aware strings use their own allocator) |
| `string_view` | string (borrowed — caller ensures lifetime) |
| `optional<T>` | `null` or `T` |
| `Nullable<T>` | `null` or `T` (null-aware wrapper) |
| `vector<T, Alloc>` | array (allocator-aware vectors use their own allocator) |
| `array<T, N>` | array (exact size required) |
| `pair<A, B>` | two-element array |
| `tuple<Ts...>` | N-element array |
| `map<string, T>` | object |
| `unordered_map<string, T>` | object |

### `conflux::json::json_member` helper

```cpp
namespace conflux::json {

template<class T, class M>
struct JsonMember {
    string_view  name;
    M T::*       pointer;
};

template<class T, class M>
constexpr JsonMember<T, M> json_member(string_view name, M T::* p);

} // namespace conflux::json
```

Convenience factory used inside `conflux::json::JsonMembers<T>::members()` tuples.

### Struct decode via `conflux::json::JsonMembers`

Specialise `conflux::json::JsonMembers<T>` to decode structs. Unknown members and missing
required members are errors. `optional<T>` fields are optional; all others
are required. Reader-path struct decode writes supported field types directly
into the destination field. Allocator-aware `std::basic_string<char, Traits, Alloc>`
and `std::vector<T, Alloc>` fields are accepted; they keep the allocator of
the field object being decoded. The current public return-by-value decode APIs
still default-construct `T`, so PMR fields use the default PMR resource unless
the caller owns the destination object through a future resource-aware API.
No PMR performance claim is implied by this support.

For GCC 16+ P2996 builds, `conflux.json.reflect` provides a zero-boilerplate
aggregate path instead. See `json-reflect.md` and
`examples/quickstart/json_reflect_crud.cxx`.

```cpp
struct Point { int64_t x{}; int64_t y{}; };

template<>
struct conflux::json::JsonMembers<Point> {
    static constexpr auto members() {
        return std::tuple{
            conflux::json::json_member("x", &Point::x),
            conflux::json::json_member("y", &Point::y),
        };
    }
    static constexpr string_view type_name() { return "Point"; }
};

auto doc = parse_view(R"({"x":3,"y":7})");
auto pt  = decode<Point>(doc->root());  // expected<Point, JsonError>
```

### Custom codec via `conflux::json::JsonCodec`

Specialise `conflux::json::JsonCodec<T>` for types that need custom decode/encode logic (e.g.
enums, types with invariants).

```cpp
template<>
struct conflux::json::JsonCodec<Color> {
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

Custom codecs can opt into direct reader decode by adding this overload:

```cpp
static expected<Color, JsonError> decode(JsonReader& r,
                                         JsonReader::Event event,
                                         JsonDecodeOptions const& opts,
                                         JsonDecodeScratch* scratch);
```

The overload receives the already-read event for the value. It should consume
only the remaining events belonging to that value. Types without this overload
still work through the DOM-shaped `decode(NodeRef)` fallback.

`has_json_codec<T>` (concept) and `has_json_codec_v<T>` (variable template)
are true when either `conflux::json::JsonMembers<T>` or `conflux::json::JsonCodec<T>` is specialised.

### Free decode helpers

```cpp
template<has_json_codec T> expected<T, JsonError> decode(NodeRef node);
template<has_json_codec T> expected<T, JsonError> decode(Document const& d); // decodes root
template<class T> expected<T, JsonError> decode(JsonReader& r);              // full document
template<class T> expected<T, JsonError> decode_full(JsonReader& r);
template<class T> expected<T, JsonError> decode_full(string_view input);
template<class T> expected<T, JsonError> decode_direct(JsonReader& r, JsonDecodeOptions const& opts = {},
                                                       JsonDecodeScratch* scratch = nullptr);
template<class T> expected<T, JsonError> decode_borrowed(string_view input, JsonParseOptions const& parse = {},
                                                         JsonDecodeOptions const& decode = {});
template<class T> expected<T, JsonError> decode_owned(string_view input, JsonParseOptions const& parse = {},
                                                      JsonDecodeOptions const& decode = {});
template<class T> expected<T, JsonError> decode_next(JsonReader& r);          // streaming
```

`decode(JsonReader&)` and `decode_full(...)` require EOF after one decoded root
value and return `trailing_garbage` if another top-level value remains.
`decode_next(...)` is the explicit streaming form for NDJSON-like loops or other
multi-value inputs.
`decode_direct(...)` exposes the reader/scratch path used by typed providers.
`decode_borrowed(...)` names the view-backed input lifetime explicitly;
`decode_owned(...)` rejects targets that contain borrowed-view fields such as
`std::string_view` at compile time. Provider boundaries that copy input reject
the same borrowed-view target shapes at runtime because the temporary owning DOM
would otherwise be destroyed before the returned value.

For direct output, `write_json_direct(out, value, opts)` appends compact JSON to
an existing string and `dump_direct(value, opts)` returns a new string. The
native provider uses this path for eligible compact `conflux::json::JsonMembers<T>` values and
falls back to the DOM writer for sorted or unsupported output.

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
populated state via `conflux::json::JsonCodec<T>::decode`.

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
- `parse` on lvalue/view input, `parse_view`, `parse_borrowed`, and
  `parse_borrowed_unsafe` string values point into the original `input` buffer.
  Destroying or mutating that buffer after parse is undefined behaviour.
- `parse_copy` and `parse(std::string&&)` store the bytes inside the returned
  `Document`; use an owning parse whenever the source buffer cannot remain
  alive and immutable.
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

`JsonArena` reuses a monotonic allocation region for parsed JSON storage. Object hash indexes use a separate reclaimable PMR pool by default so repeated `parse_into` reuse does not retain stale indexes; pass `hash_index_resource` only when you want to own that allocation policy. Useful for request-scoped JSON: parse, process, then `reset()` the arena rather than allocating and freeing `Document` per request.

```cpp
struct JsonArenaOptions {
    std::size_t               initial_slab{64 * 1024}; // pre-allocated slab size
    std::pmr::memory_resource *hash_index_resource{nullptr};
};

class JsonArena {
public:
    explicit JsonArena(JsonArenaOptions const&);

    expected<ArenaDocument, JsonError> parse_into      (string_view,  JsonParseOptions const& = {});
    expected<ArenaDocument, JsonError> parse_borrowed_into(string_view, JsonParseOptions const& = {});
    expected<ArenaDocument, JsonError> parse_moved_into(std::string,  JsonParseOptions const& = {});

    void   reset();           // invalidates all ArenaDocuments; reuses memory
    size_t slab_capacity() const;
    size_t slab_used()     const;
};
```

`ArenaDocument` is a handle into the arena's storage. **All `ArenaDocument` handles are invalidated by `reset()`** — do not hold them across a reset.
`parse_borrowed_into` borrows caller-owned bytes and rejects temporary `std::string` inputs; use `parse_moved_into(std::move(input))` when the arena should own an rvalue string.

```cpp
JsonArena arena{JsonArenaOptions{.initial_slab = 1024 * 1024}};

for (auto const& raw : requests) {
    auto doc = arena.parse_into(raw);
    if (doc) process(doc->root());
    arena.reset();
}
```

---

## NDJSON / Streaming

### `JsonStreamReader`

Incremental event reader for byte streams that arrive in chunks. It owns the
fed bytes, reuses the existing `JsonReader` event grammar, and rolls back when a
partial token reaches the current end of the buffer. `next()` returns
`nullopt` when no complete event is available yet; call `close()` when the input
source reaches EOF so a terminal number can be emitted and unterminated
containers/strings become parse errors.

```cpp
class JsonStreamReader {
public:
    using Event = JsonReader::Event;

    explicit JsonStreamReader(JsonParseOptions const& = {});

    expected<void, JsonError> feed(string_view chunk);
    expected<void, JsonError> feed(span<byte const> chunk);
    expected<void, JsonError> close();

    expected<optional<Event>, JsonError> next();

    JsonStringToken key_token() const;
    JsonStringToken string_token() const;
    JsonNumberView  number_val() const;
    bool            bool_val() const;

    string_view input() const;
    size_t      buffered_bytes() const;
    bool        closed() const;
    void        reset();
};
```

Example:

```cpp
JsonStreamReader reader;
reader.feed(R"({"items":[1)");

while (auto ev = reader.next()) {
    if (!*ev) break; // need more bytes or closed EOF
    handle(**ev, reader);
}

reader.feed(R"(,2,3]})");
reader.close();

while (auto ev = reader.next()) {
    if (!*ev) break;
    handle(**ev, reader);
}
```

String tokens and key tokens are views into the stream reader's internal buffer
when they do not require unescaping. Treat token views as valid until the next
`feed()` or `reset()` call; copy/append decoded strings if they must outlive the
stream buffer. The current implementation does not compact consumed bytes, so
`max_input_size` applies to the total fed stream, not just unread bytes.

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

The schema reflects the same members and types that `decode<T>` would accept. Optional fields become non-required schema properties. Custom `conflux::json::JsonCodec<T>` specializations are not introspected — only `conflux::json::JsonMembers<T>` yields a meaningful schema.

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
