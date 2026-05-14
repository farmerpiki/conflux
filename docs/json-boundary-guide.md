# JSON boundary trait guide

`conflux.json.boundary` is the provider-neutral JSON seam. It is intentionally
thin: HTTP/app code should depend on boundary concepts and option/error types,
while concrete parser/DOM modules stay behind provider adapters.

## Modules

| Module | Role | Concrete JSON dependency |
|---|---|---|
| `conflux.json.boundary` | Provider concepts, neutral options/errors, `dump_with`, `decode_with`, `write_with`. | None |
| `conflux.json.native_provider` | Adapter from the current `conflux.json` parser/DOM/codecs to the boundary. | `conflux.json` |
| `conflux.net.http.json` | Request body helpers that require an explicit provider (`*_with`). | None beyond boundary |
| `conflux.net.http.response_json` | Response serialization helpers that require an explicit provider (`*_with`). | None beyond boundary |
| `conflux.net.http.app_json` | App/router route helpers that decode typed request bodies and encode typed responses through an explicit provider. | None beyond boundary |
| `conflux.net.http.native_json` | Native-provider convenience overloads and `DefaultJsonProvider`. | `conflux.json.native_provider` |
| `conflux.json.reflect_provider` | Optional P2996 reflected native provider for aggregate serde. | `conflux.json.reflect` + `conflux.json.native_provider` |

Use the `*_with<Provider>` APIs in framework code and reusable modules. Import
`conflux.net.http.native_json` only at the application/default-integration edge
where choosing the current native JSON provider is intentional.

## Provider shape

A provider only implements the operations it supports:

```cpp
struct MyProvider {
    template<class T>
    static expected<std::string, Error> dump_json(T const&, DumpOptions const&);

    template<class T>
    static expected<T, Error> decode_json(std::string_view, DecodeOptions const&);

    template<class T, class Sink>
    static expected<void, Error> write_json(T const&, DumpOptions const&, Sink&&);
};
```

`write_json` is optional. If it exists, `write_with` streams chunks directly to
the sink. Otherwise it falls back through `dump_json`. That keeps response code
provider-neutral while allowing future providers to avoid an intermediate string.

## Decode options

`DecodeOptions` is intentionally small and provider-neutral:

- `copy_input = true` means the provider may retain parsed views by owning/copying
  input bytes.
- `copy_input = false` means the caller keeps bytes alive for the decode operation
  and allows view/reader paths.
- `unknown_members = reject|ignore` controls typed-object decode behavior.

Native providers map those fields onto `JsonDomPolicy` and `JsonDecodeOptions`;
other providers should preserve the semantics even if their internal parser shape
differs.

## HTTP usage

Provider-neutral response code:

```cpp
import conflux.net.http.response_json;

auto r = conflux::http::json::try_response_with<MyProvider>(payload);
```

Provider-neutral app/router code:

```cpp
import conflux.net.http.app_json;

conflux::http::json::routes<MyProvider>(app)
    .get("/status", [] { return StatusDto{.ok = true}; })
    .post_body<CreateUserDto>("/users", [](CreateUserDto const& body) {
        return CreatedUserDto{.id = body.name};
    });
```

Native convenience edge:

```cpp
import conflux.net.http.native_json;

auto r = conflux::http::json::try_response(payload);
```

The convenience module exists for ergonomics, but it is not the framework seam.
New route/app infrastructure should use the provider-explicit helpers so later
parser/DOM replacement remains local to adapter wiring.

## Rules for new code

- Do not import `conflux.json` from HTTP/app framework modules merely to emit or
  parse JSON.
- Do not expose `Document`, `NodeRef`, `JsonError`, or `JsonDumpOptions` from
  HTTP/app framework APIs unless the API is explicitly native-provider-specific.
- Boundary-facing APIs use `conflux::json::boundary::Error`, `DumpOptions`, and
  `DecodeOptions`.
- Parser/DOM/reflection work stays behind provider adapters until the app and
  route boundaries no longer depend on concrete JSON types. For reflected native
  serde, use `conflux::json::boundary::NativeReflectJsonProvider` from
  `conflux.json.reflect_provider`.
- App/router typed JSON helpers live in `conflux.net.http.app_json` and require
  an explicit provider parameter. Native-provider convenience remains outside
  that framework seam.
