# Ergonomics Potential Not Covered by Current Proposals

Date: 2026-05-16

Scope: review gaps that are not already tracked by `todo/proposal_state.md`,
`todo/parallel_priority_plan.md`, `docs/naming-audit.md`, or the current module
split proposals. This is an evaluation note, not an implementation mandate.

## Highest-value candidates

| Priority | Candidate branch | Decision | Why it is not already covered |
|---|---|---|---|
| P1 | `http/request-response-name-split` | Worth doing before public API freeze. | Existing naming docs cover blocking/sync/async vocabulary, but not the server/client HTTP type collision. |
| P1 | `http/app-first-contact-imports` | Worth doing as docs/examples cleanup, with one source-doc correction. | Component docs say to prefer narrow targets, but server docs/examples still normalize the broad HTTP umbrella. |
| P2 | `http/fallible-setup-factories` | Worth adding if API cleanup is active. | Existing docs accept throwing setup helpers; expected-heavy components make this an ergonomic outlier. |
| P2 | `http/context-route-method-sugar` | Worth adding if context dispatch remains public. | Context dispatch naming is tracked; method-level route registration convenience is not. |
| P2 | `template/namespace-polish` | Worth fixing pre-v1. | Component map documents a template component, but namespace shape is currently a global `tmpl`. |
| P2 | `docs/component-map-guard` | Worth adding as a small CI/doc guard. | Package smoke tests verify installed config; they do not guard component-map drift. |
| P3 | `http/typed-field-extractors` | Useful, but defer until higher-priority API cleanup settles. | Request fields are string maps only; no existing proposal covers typed extraction helpers. |

## P1: split HTTP server/client request and response names

Current shape creates two different public concepts named `HttpRequest` and two
different public concepts named `HttpResponse`:

- server-owned request: global `HttpRequest` in `conflux.net.http.server_types`;
- server request view: global `HttpRequestView`;
- server response: global `HttpResponse` in `conflux.net.http.response`;
- client request: `conflux::http::HttpRequest` in `conflux.net.http.request`;
- client response/result: `conflux::http::HttpResponse` / `HttpResult` in
  `conflux.net.client`;
- `conflux.net.app` then aliases server types into `conflux::http` as
  `RequestView`, `OwnedRequest`, `Request`, and `Response`.

This is worse than ordinary shorthand cleanup because the broad umbrella imports
server and client APIs together. A user can see `HttpRequest` in server docs and
`http::HttpRequest` in client docs, while `http::Request` means server request
view. That makes code search, autocomplete, and examples harder to trust.

Suggested final shape:

- server: `conflux::http::RequestView`, `conflux::http::OwnedRequest`,
  `conflux::http::Response` as first-contact names;
- client: `conflux::http::ClientRequest`, `conflux::http::ClientResponse`,
  `conflux::http::ClientResult`;
- keep `HttpClient` as-is;
- remove or quarantine unqualified `HttpRequest` / `HttpResponse` examples before
  the public surface freezes.

Acceptance criteria:

- server docs/examples use `http::RequestView` / `http::OwnedRequest` /
  `http::Response`, not unqualified `HttpRequest` / `HttpResponse`;
- client docs/examples use `http::ClientRequest` / `http::ClientResponse`;
- the umbrella import no longer creates two plausible meanings for one request or
  response name in the same namespace;
- compile tests cover both server and client snippets in one translation unit.

## P1: make app imports the server first-contact path

`conflux.net.http` currently re-exports the app/server stack plus client,
OpenAPI, policy, auth, compression, vhost, and protocol modules. That is fine as
a complete HTTP umbrella, but poor as the default teaching import for server-only
examples.

Recommended cleanup:

- make `conflux.net.app` / `conflux::http_app` the first-contact server import in
  hello/middleware/basic docs;
- reserve `conflux.net.http` for “complete HTTP stack” examples;
- fix the server API doc note that says `conflux.net.openapi` is not included in
  the umbrella, because the source currently exports it from `conflux.net.http`.

Acceptance criteria:

- simple server examples do not import the client request builder by default;
- docs distinguish `http_app` from the complete `http` umbrella;
- OpenAPI umbrella wording matches source.

## P2: add non-throwing setup factories

The project generally favors `expected<T, E>` for recoverable public operations,
but some setup APIs still require exceptions:

- `HttpRequest::get/post/...` builder factories parse URLs through a throwing
  helper;
- `config_from_ini(path)` throws on file or parse failure.

Recommended additions:

- client request: `try_get`, `try_post`, `try_method`, or `Builder::try_url(...)`
  returning `expected<..., HttpError>` / URL parse error;
- config: `try_config_from_ini(path) -> expected<Config, string>` or a small
  typed config error;
- leave throwing wrappers as convenience only, not as the only path.

Acceptance criteria:

- examples can load config and build client requests without `try/catch`;
- throwing wrappers are thin wrappers over the expected-returning functions;
- no hot-path changes.

## P2: add per-method context route sugar

`Router` has `get/post/put/...` helpers for ordinary handlers, but context-aware
routes require `add_context(method, path, handler)`.

Recommended additions:

- `get_context`, `post_context`, `put_context`, `patch_context`, `del_context`,
  `options_context` on `Router`;
- matching `App` forwarding helpers only if `App` already forwards the ordinary
  methods in the public API.

Acceptance criteria:

- context-route examples read like ordinary routes;
- helpers are inline delegates to `add_context`, with no behavior or perf change.

## P2: polish template namespace before v1

`conflux.templates` exports a global `tmpl` namespace while the component is named
`template` in the public component map. That is a small but visible API shape
mismatch.

Recommended cleanup:

- introduce `conflux::templates` as the canonical namespace;
- keep `tmpl` only as a transitional alias if needed;
- update examples and tests to use the canonical namespace.

Acceptance criteria:

- module name, component docs, and examples all point to one namespace spelling;
- no global-only namespace is required for first-contact use.

## P2: guard component-map drift

`docs/component-map.md` asks maintainers to keep component entries synced with
`conflux_public_component(...)`, but there is no direct check for that table.

Recommended cleanup:

- add a small script that extracts public component names from `CMakeLists.txt`
  and compares them against the component-map tables;
- wire it into the existing smoke/check scripts or a lightweight CTest.

Acceptance criteria:

- adding/removing a public component fails the check until docs are updated;
- support/private targets remain explicitly allowlisted.

## P3: typed field extractors for route params/query/form/headers

Server request ergonomics currently push users toward string-map access for route
params, query fields, form fields, headers, and cookies. Typed parsing is likely
to be repeated in every application.

Recommended shape:

- a small helper layer such as `field_as<T>(fields, key)` returning `expected<T,
  FieldParseError>`;
- optional defaults: `field_or<T>(fields, key, fallback)`;
- support integer, bool, enum/string_view conversion first; leave JSON/body
  binding out of scope.

Acceptance criteria:

- handlers can parse common path/query values without hand-rolled `from_chars`;
- missing key vs malformed value are distinct errors;
- helpers are header/module-only, allocation-light, and not coupled to routing.

## Defer / avoid for now

- Do not start a broad namespace cleanup across unrelated modules from this note;
  the HTTP request/response collision is a specific, user-visible hotspot.
- Do not change feature defaults just for ergonomics; modular target selection is
  already documented and should remain evidence-driven.
- Do not add a route macro/DSL layer yet. The current route surface is simple;
  smaller helpers above address the main repetition without committing to a DSL.
