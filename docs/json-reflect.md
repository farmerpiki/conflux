# conflux.json.reflect — Reflection-based JSON codec

**Module:** `conflux.json.reflect`  
**CMake target:** `conflux_json_reflect`  
**CMake flag:** `CONFLUX_JSON_REFLECT` (opt-in)  
**Requires:** GCC 16+ with `-freflection` (P2996 static reflection)

Provides automatic `JsonCodec<T>` specialization for aggregate structs via C++26 static reflection. No manual member registration.

---

## Build setup

```cmake
# CMakeLists.txt
find_package(conflux REQUIRED COMPONENTS json_reflect)
target_link_libraries(mytarget PRIVATE conflux::json_reflect)
```

The `conflux_json_reflect` target automatically adds `-freflection` to the compile flags. The feature gate `CONFLUX_JSON_REFLECT` controls whether the module is built.

---

## Usage

```cpp
import conflux.json.reflect;
import conflux.json;

struct Point {
    int64_t x{};
    int64_t y{};
};

// Opt in with CONFLUX_JSON_REFLECT macro
CONFLUX_JSON_REFLECT(Point);

// Now decode/encode work automatically
auto doc = conflux::json::parse_view(R"({"x":3,"y":7})");
auto pt  = conflux::json::decode<Point>(doc->root()); // expected<Point, JsonError>

auto encoded = conflux::json::encode(Point{1, 2});    // expected<Document, JsonError>
```

---

## `json::name` annotation

Override the JSON key for a member:

```cpp
import conflux.json.reflect;

struct User {
    int64_t     id{};
    std::string [[json::name("full_name")]] name;  // serializes as "full_name"
};
CONFLUX_JSON_REFLECT(User);
```

Without `json::name`, the serialized key is the C++ member name verbatim.

---

## Aggregate constraints

The reflection codec requires:
- The type is an aggregate (no user-declared constructor, no virtual functions, no private/protected non-static data members).
- All member types have a `JsonCodec<T>` specialization (built-in or registered).
- `CONFLUX_JSON_REFLECT(T)` is invoked at namespace scope in a TU that imports `conflux.json.reflect`.

Non-aggregate types or types with invariants should use `JsonCodec<T>` manual specialization (`json-api.md`, Codec System section).

---

## Limitations

- GCC 16+ only. Clang does not implement P2996 as of 2026-05.
- Only direct data members are reflected; base class members are not.
- Reflected members are always required in decode (no `optional<T>`-as-optional semantics unless the field type is `optional<T>`).
- `json::name` is the only supported annotation; ordering, skip, and default-value annotations are not yet implemented.
- Build fails cleanly on compilers without `-freflection` — the module is not compiled unless `CONFLUX_JSON_REFLECT` is set.
