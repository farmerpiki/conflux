# conflux.json Cookbook

Practical patterns for the most common tasks. For the full API surface see
`docs/json-api.md`.

```cpp
import conflux.json;
using namespace conflux::json;
```

---

## 1. Null vs Missing

`find_member` returns `std::optional<NodeRef>` — `nullopt` when the key is absent,
`NodeRef` (possibly null-kind) when the key exists.

```cpp
auto doc = *parse_view(R"({"a": null, "b": 1})");
auto obj = *doc.root().as_object();

// absent key
std::optional<NodeRef> c = obj.find_member("c");   // nullopt
bool missing = !c;                        // true

// explicit null
std::optional<NodeRef> a = obj.find_member("a");
bool present = a.has_value();             // true
bool is_null = a && a->is_null();         // true

// present non-null
std::optional<NodeRef> b = obj.find_member("b");
bool is_null2 = b && b->is_null();        // false
```

The helper accessors reflect the same rule: `optional_*` returns `nullopt` for
both absent **and** explicit-null; `require_*` returns an error for absent.

```cpp
auto val = optional_int(obj, "count");  // std::optional<i64>: nullopt if absent or null
auto req = require_int(obj, "id");      // expected<i64, JsonError>: error if absent
```

---

## 2. Large-Object Lookup

Linear scan is the default. Objects with ≥ 32 members benefit from a hash
table. Build it explicitly or let the parser do it automatically.

### Explicit warm (ad-hoc use)

```cpp
// Warm one object:
auto err = doc.warm_member_index(node);   // expected<void, JsonError>

// Warm every object in the document:
WarmIndexOptions opts{.max_objects = no_limit, .max_extra_bytes = no_limit};
auto err2 = doc.warm_member_indices(opts);
```

After warming, `find_member` / `member` on that object use hash lookup.
Objects with fewer than 32 members are skipped — warming them is a no-op.

### Auto-warm at parse time

```cpp
JsonParseOptions opts{.warm_threshold = 8u};   // warm any object with ≥ 8 members
auto doc = *parse_view(input, opts);
// No explicit warm_member_index call needed.
```

`warm_threshold = nullopt` (default) disables auto-warm — preserves existing
behaviour and avoids paying hash-table construction cost on one-shot documents.
Use `warm_threshold` on API-response paths with consistent-shape objects that
are accessed many times per parse.

---

## 3. Typed Struct Decode

Define a `JsonMembers<T>` specialization. The library generates encode/decode
automatically from the member map.

```cpp
struct Config {
    std::string host;
    int         port;
    bool        tls;
};

template<>
struct JsonMembers<Config> {
    static auto members() {
        return std::make_tuple(
            std::pair{"host", &Config::host},
            std::pair{"port", &Config::port},
            std::pair{"tls",  &Config::tls}
        );
    }
};
```

Decode from a parsed document:

```cpp
auto doc = *parse_view(json_text);
auto cfg = decode<Config>(doc);   // expected<Config, JsonError>
if (!cfg) { /* cfg.error() */ }
```

Decode from a pull-parser reader (zero-DOM path):

```cpp
JsonReader reader{json_text};
auto cfg = decode<Config>(reader);
```

Unknown keys produce an error by default. To ignore them:

```cpp
JsonDecodeOptions opts{.unknown_members = UnknownMemberPolicy::ignore};
auto cfg = decode<Config>(doc, opts);
```

To encode back to JSON:

```cpp
// JsonMembers<T> gives encode via ObjectBuilder internally; use make_object for
// ad-hoc construction; for struct round-trip use ValueBuilder + insert<T>:
ValueBuilder vb;
auto ob = *vb.begin_object();
*ob.insert("config", cfg_value);   // T with has_json_codec<T>
std::move(ob).commit();
auto out_doc = *std::move(vb).finish();
```

---

## 4. Path Traversal

`JsonPath::from_pointer` parses a JSON Pointer (RFC 6901) into a typed path.
`NodeRef::at(path)` walks the document.

```cpp
auto doc  = *parse_view(json_text);
auto path = *JsonPath::from_pointer("/results/0/id");
auto node = doc.root().at(path);   // expected<NodeRef, JsonError>
if (node) {
    auto id = *node->as_i64();
}
```

Build paths programmatically:

```cpp
JsonPath p;
p.push_member("results");
p.push_index(0);
p.push_member("id");
auto node = doc.root().at(p);
```

On error, `JsonError::path` contains the path prefix up to the failing segment.

---

## 5. Error Taxonomy

Every fallible operation returns `expected<T, JsonError>`.

```cpp
struct JsonError {
    JsonStage       stage;    // where the error occurred
    JsonIssueCode   code;     // what went wrong
    JsonPath        path;     // location in the document (may be empty)
    std::string     message;  // human-readable detail
    JsonSourceLocation src;   // source file/line of the throw site (library-internal)
};
```

### `JsonStage`

| Value | When |
|---|---|
| `lex` | Tokenizer: bad UTF-8, unterminated string, illegal character |
| `parse` | Parser: structural error (mismatched brackets, unexpected token) |
| `decode` | `decode<T>`: type mismatch, missing required field, constraint violation |
| `build` | Builder: duplicate key, invalid value (NaN/Inf), builder misuse |
| `query` | `at()`, `as_object()`, etc.: wrong kind or missing path segment |

### Common `JsonIssueCode` values

| Code | Meaning |
|---|---|
| `missing_member` | Required key not found in object |
| `wrong_kind` | Node kind does not match expected (e.g., asked for int, got string) |
| `number_out_of_range` | Numeric value overflows the target type |
| `duplicate_key` | Duplicate object key with `DuplicateKeyPolicy::reject` |
| `constraint_violation` | Builder misuse or field constraint failure |
| `invalid_value` | Non-finite float in builder, null `char const*` |
| `input_too_large` | Input exceeds `max_input_size` limit |
| `depth_exceeded` | Nesting deeper than `max_depth` |
| `string_too_large` | String exceeds `max_string_size` limit |
| `unexpected_token` | Structural parse error |

### Building user-facing error messages

```cpp
auto result = decode<Config>(doc);
if (!result) {
    auto const& e = result.error();
    // path is empty for top-level errors
    auto loc = e.path.empty()
        ? std::string{"(root)"}
        : e.path.to_pointer();   // "/field/subfield" RFC 6901 style
    log_error("JSON {}: {} at {}", e.stage, e.message, loc);
}
```

`JsonError::path` is populated by `decode<T>` on field-level errors and by
`NodeRef::at()` on path traversal errors. Parse-stage errors (lex/parse)
populate `src` (internal location) but not `path`.

---

## 6. Builder Patterns

### `make_object` / `make_array` — one-shot construction

```cpp
auto doc = *make_object(
    std::pair{"model",       "gpt-4o"sv},
    std::pair{"temperature", 0.7},
    std::pair{"max_tokens",  4096},   // int literal → i64, no suffix needed
    std::pair{"stream",      true});

auto arr = *make_array(1, 2, 3);

// Homogeneous initializer_list form:
auto doc2 = *make_object({{"role", "user"sv}, {"content", body}});
```

`make_object` / `make_array` accept any `JsonWritable` value: `bool`, `string`
/ `string_view`-convertible, non-char integrals (→ `i64`/`u64`), floating-point,
and any type with a `JsonCodec<T>` specialization. Character types (`char`,
`wchar_t`, etc.) and enums without a codec are excluded.

### `ValueBuilder` — incremental construction

Use `ValueBuilder` when the structure is dynamic or contains nested sub-trees.

```cpp
ValueBuilder vb;

// Scalar:
*vb.set_string("hello");
auto doc = *std::move(vb).finish();

// Nested object:
ValueBuilder vb2;
auto ob = *vb2.begin_object();
*ob.insert_string("key", "value");
*ob.insert_i64("n", 42);
{
    auto inner = *ob.insert_object("nested");
    *inner.insert_bool("flag", true);
    std::move(inner).commit();
}
std::move(ob).commit();
auto doc2 = *std::move(vb2).finish();
```

`commit()` is `&&`-qualified and `noexcept`. Call it exactly once per builder.
Destroying an uncommitted `ObjectBuilder` / `ArrayBuilder` rolls back its partial
state — the parent `ValueBuilder` is left in a usable state.

**Duplicate keys** are rejected by the builder regardless of `DuplicateKeyPolicy`
— duplicate insertion returns a `constraint_violation` error.

---

## 7. Integer Literals in `make_object` / `make_array`

Unadorned integer literals (`1`, `100`, `4096`) are deduced as `int` by the
compiler. `JsonWritable` accepts all non-char signed integrals via `i64` encoding,
so no suffix is required for values that fit in `int` range:

```cpp
auto doc = *make_object(std::pair{"count", 1});   // fine — int → i64
```

Values above 2^31 − 1 that the compiler deduces as `int` overflow and are
undefined behaviour before `make_object` is even called. Use an explicit suffix:

```cpp
auto doc = *make_object(std::pair{"big", 4294967296LL});   // i64 literal
```

Values above 2^63 − 1 must use `u64` encoding; provide `uint64_t` explicitly:

```cpp
auto doc = *make_object(std::pair{"flags", uint64_t{0xFFFF'FFFF'FFFF'FFFFull}});
```

Non-finite floats (`NaN`, `Inf`) are rejected at insertion time with
`invalid_value` — JSON has no representation for them.
