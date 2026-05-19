# conflux.json.reflect — reflection-based JSON codec

**Module:** `conflux.json.reflect`  
**Provider module:** `conflux.json.reflect_provider`  
**CMake targets:** `conflux_json_reflect`, `conflux_json_reflect_provider`  
**CMake flag:** `CONFLUX_JSON_REFLECT` (opt-in)  
**Requires:** GCC 16+ with `-freflection` (P2996 static reflection)

Provides automatic `JsonCodec<T>` specialization for aggregate structs via C++26
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

The `debug-p2996-gcc` preset selects C++26 and enables `CONFLUX_JSON_REFLECT`.
The reflection targets add `-freflection`; the rest of the graph is kept on the
normal module path because GCC 16's reflected `std` BMI is still fragile for
non-reflection consumers.

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
// aggregate structs satisfy JsonCodec<T> through P2996 reflection.
auto doc = conflux::json::parse_view(R"({"x":3,"y":7})");
auto pt  = conflux::json::decode<Point>(doc->root());

conflux::json::ValueBuilder b;
(void)b.set(Point{1, 2});
auto encoded = std::move(b).finish();
```

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
`JsonMembers<T>` codecs:

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
- no manual `JsonMembers<T>` specialization
- all member types must be supported by `JsonCodec<T>` or by reflected primitive
  fallback handling

Non-aggregate types or types with invariants should use a manual `JsonCodec<T>`
or `JsonMembers<T>` specialization (`json-api.md`, Codec System section).

---

## Limitations

- GCC 16+ only. Clang does not implement the required P2996 surface as of
  2026-05.
- Only direct data members are reflected; base class members are not included.
- Reflected members are required unless the member type is `optional<T>`.
- `json::name` and `json::skip` are the only supported annotations.
- The reflection target is opt-in because `-freflection` still has separate
  toolchain/module fragility.
