# conflux.json.reflect — reflection-based JSON codec

**Module:** `conflux.json.reflect`  
**Provider module:** `conflux.json.reflect_provider`  
**CMake targets:** `conflux_json_reflect`, `conflux_json_reflect_provider`  
**CMake flag:** `CONFLUX_JSON_REFLECT` (opt-in)  
**Requires:** GCC 16+ or reflection-capable Clang with `-freflection` (P2996 static reflection)

Provides automatic `conflux::json::JsonCodec<T>` specialization for aggregate structs via C++26
static reflection. It is provider-neutral at the HTTP/app boundary: callers that
want reflected native JSON serde should use
`conflux::json::boundary::NativeReflectJsonProvider` with the existing boundary
traits.

---

## Build setup

```cmake
find_package(conflux REQUIRED COMPONENTS json_reflect_provider)
target_link_libraries(mytarget PRIVATE conflux::json_reflect_provider)
```

The `debug-p2996-gcc` and `debug-p2996-clang` presets select C++26 and enable
`CONFLUX_JSON_REFLECT`. The build keeps `-freflection` scoped to CMake's
generated `std` BMI and the JSON reflection targets so the full runtime graph
does not inherit the flag.
Reflection modules should use `import std;`; Clang P2996 builds may include
`<meta>` in the global module fragment for vendor reflection declarations.

---

## Usage

```cpp
import conflux.json;
import conflux.json.reflect;

struct Point {
    int64_t x{};
    int64_t y{};
};

// No macro registration. Importing conflux.json.reflect makes eligible
// aggregate structs satisfy conflux::json::JsonCodec<T> through P2996 reflection.
auto doc = conflux::json::parse_view(R"({"x":3,"y":7})");
auto pt  = conflux::json::decode<Point>(doc->root());

conflux::json::ValueBuilder b;
(void)b.set(Point{1, 2});
auto encoded = std::move(b).finish();
```

HTTP handlers can use the same direct aggregate serde. This is the no-registration
variant of the JSON CRUD quickstart:

```cpp
import conflux.http;
import conflux.json.reflect;
import std;

namespace http = conflux::http;

struct Todo {
    int64_t id{};
    std::string title;
    bool done{};
};

struct CreateTodo {
    std::string title;
};

app.get<"/todos/{id:i64}">([](std::int64_t id) -> http::Json<Todo> {
    return http::json(Todo{.id = id, .title = "write docs"});
});

app.post("/todos", [](http::Json<CreateTodo> const& body) {
    return http::created(Todo{.id = 1, .title = body->title});
});
```

See `examples/quickstart/json_reflect_crud.cxx` for the full route set. The
portable `examples/quickstart/json_crud.cxx` still uses `conflux::json::JsonMembers<T>` because
reflection is toolchain-gated.

Provider-boundary usage:

```cpp
import conflux.json.reflect_provider;

using Provider = conflux::json::boundary::NativeReflectJsonProvider;

auto pt = conflux::json::boundary::decode_with<Provider, Point>(
    R"({"x":3,"y":7})");

auto body = conflux::json::boundary::dump_with<Provider>(Point{1, 2});
```

Route/app helpers should use `Provider` explicitly rather than importing the
native parser into framework code.

`NativeReflectJsonProvider::dump_json<T>` uses the reflected direct writer for
compact reflected aggregates. Sorted output falls back to the native DOM writer.
Callers that want the reflected writer directly can use
`dump_reflect_direct(value)` or `write_reflect_json_direct(out, value)`.

---

## Annotations

Override the JSON key for a member:

```cpp
import conflux.json.reflect;

struct User {
    int64_t id{};
    [[= conflux::json::name("full_name")]] std::string name;
};
```

Skip a member during encode/decode:

```cpp
struct InternalUser {
    std::string name;
    [[= conflux::json::skip{}]] int internal_id{};
};
```

Without `json::name`, the serialized key is the C++ member name verbatim.
Skipped members keep their C++ default value during decode and are omitted during
encode. A skipped key present in input is treated as an unknown member and follows
`JsonDecodeOptions::unknown_members`.

---

## Decode policy

Reflected codecs now support the same `JsonDecodeOptions` path as manual
`conflux::json::JsonMembers<T>` codecs:

```cpp
auto pt = conflux::json::decode<Point>(
    doc->root(),
    conflux::json::JsonDecodeOptions{
        .unknown_members = conflux::json::UnknownMemberPolicy::ignore});
```

The provider boundary maps `conflux::json::boundary::DecodeOptions` onto the
native parser/DOM policy:

- `copy_input = true` -> `JsonDomPolicy::owning_document()`
- `copy_input = false` -> `JsonDomPolicy::view_first()` / reader path
- `unknown_members` -> native `JsonDecodeOptions::unknown_members`

---

## Aggregate constraints

The reflection codec requires:

- aggregate type
- default-initializable type
- no manual `conflux::json::JsonMembers<T>` specialization
- member types must be supported by `conflux::json::JsonCodec<T>`, reflected aggregate decode,
  reflected primitive/string fallback handling, `std::optional<T>`,
  `std::vector<T>`, or fixed `std::array<T, N>`

For `copy_input = false`, the reflected reader path decodes supported members
directly from `JsonReader`, including nested reflected aggregates and vector or
fixed-array members. For `copy_input = true`, the provider keeps the owning DOM
fallback available.

Non-aggregate types or types with invariants should use a manual `conflux::json::JsonCodec<T>`
or `conflux::json::JsonMembers<T>` specialization (`json-api.md`, Codec System section).

---

## Limitations

- Reflection-capable GCC 16+ and local P2996 Clang builds are the only checked
  toolchains.
- Only direct data members are reflected; base class members are not included.
- Reflected members are required unless the member type is `optional<T>`.
- `json::name` and `json::skip` are the only supported annotations.
- The reflection target is opt-in because `-freflection` still has separate
  toolchain/module fragility.
