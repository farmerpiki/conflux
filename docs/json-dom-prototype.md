# JSON parser/DOM policy design

Status: preview policy slice for `json/parser-dom-design` with historical branch notes.

This document records the parser/DOM shape that future implementation branches
should preserve. It deliberately does **not** start a full tokenizer or DOM
rewrite. The current code already has the required seams: borrowed documents,
PMR-injected documents, `JsonArena`, `JsonReader`, and boundary-provider traits.
The policy layer in `conflux.json` names those choices through `JsonDomPolicy`
and `parse_dom(...)` wrappers so later parser work can replace internals without
changing route/app JSON boundaries.

## Goals

- View-first parsing by default for hot paths.
- Explicit owning parse when bytes cannot outlive the document.
- Reusable arena storage for request/batch-scoped parse/process/reset loops.
- Provider-neutral framework integration through `conflux.json.boundary`.
- `std::expected<T, JsonError>` error path everywhere.
- Strict RFC 8259 UTF-8 and number handling by default.
- No process-global mutable parser state except the already-documented C-locale
  fallback used only for floating-point range classification.

## Non-goals for this slice

- No simdjson-style Stage-0 tokenizer rewrite.
- No new public DOM node representation.
- No parser/DOM rewrite for reflection serde; the reflected provider only maps through this facade.
- No JSON Pointer/Patch/schema expansion.
- No route/app code changes beyond using the existing boundary traits.

## Public policy surface

`conflux.json` now exports:

```cpp
enum class JsonDomInputOwnership {
    borrowed_view,
    owned_copy,
    owned_move,
};

enum class JsonDomStorageModel {
    standalone_document,
    caller_pmr_document,
    reusable_arena,
};

enum class JsonDomStringModel {
    view_unescaped_copy_decoded,
};

enum class JsonDomNumberModel {
    preserve_lexeme_parse_on_access,
};

enum class JsonDomUtf8Model {
    strict_validate_on_parse,
};

enum class JsonDomErrorModel {
    expected_json_error,
};

enum class JsonDomObjectIndexModel {
    preserve_order_warm_hash_on_demand,
};

struct JsonDomPolicy {
    JsonDomInputOwnership input;
    JsonDomStorageModel storage;
    JsonDomStringModel strings;
    JsonDomNumberModel numbers;
    JsonDomUtf8Model utf8;
    JsonDomErrorModel errors;
    JsonDomObjectIndexModel object_index;
    JsonParseOptions parse;

    static constexpr JsonDomPolicy view_first(JsonParseOptions = {});
    static constexpr JsonDomPolicy owning_document(JsonParseOptions = {});
    static constexpr JsonDomPolicy caller_pmr(JsonParseOptions = {});
    static constexpr JsonDomPolicy arena_reuse(JsonParseOptions = {});
    static constexpr JsonDomPolicy arena_borrowed(JsonParseOptions = {});
};
```

The wrappers are intentionally thin:

```cpp
expected<Document, JsonError> parse_dom(string_view, JsonDomPolicy = JsonDomPolicy::view_first());
expected<Document, JsonError> parse_dom(string&&, JsonDomPolicy = JsonDomPolicy::owning_document());
expected<Document, JsonError> parse_dom(string_view, pmr::memory_resource*, JsonDomPolicy = JsonDomPolicy::caller_pmr());
expected<ArenaDocument, JsonError> parse_dom(JsonArena&, string_view, JsonDomPolicy = JsonDomPolicy::arena_reuse());
expected<ArenaDocument, JsonError> parse_dom(JsonArena&, string&&, JsonDomPolicy = {.input = owned_move, .storage = reusable_arena});
```

Policy/storage mismatches return `JsonIssueCode::constraint_violation` through
`JsonError`; they do not assert or throw. This keeps the prototype safe for
branch experimentation and gives tests a stable error model.

## Memory model

### Standalone view document

`JsonDomPolicy::view_first()` maps to `parse_view(...)`.

- Nodes, child arrays, object-member arrays, and decoded strings live in
  `DocumentStorage`.
- Unescaped strings and number lexemes reference caller-owned input bytes.
- Caller must keep bytes stable for the lifetime of `Document` and all `NodeRef`s.
- Rvalue `std::string` input is rejected for borrowed-view policy.

Use this for request bodies or mapped files whose byte lifetime is already tied
to the operation.

### Standalone owned document

`JsonDomPolicy::owning_document()` maps to `parse_copy(...)`.

- Input is copied or moved into the `Document`.
- Unescaped strings and number lexemes reference the owned input buffer.
- Decoded strings still materialize into the string arena.

Use this when the source buffer will be destroyed or mutated after parse.

### Caller-PMR document

`JsonDomPolicy::caller_pmr()` maps to the existing `parse_*` overloads that take
`std::pmr::memory_resource*`.

- Structural arrays and decoded-string arena allocate from caller-provided PMR.
- Hash-index allocation also follows the document storage resource.
- Resource lifetime must exceed the document and all node views.

This is the bridge for custom request allocators and later arena replacement
work without forcing `JsonArena` on every caller.

### Reusable arena document

`JsonDomPolicy::arena_reuse()` / `arena_borrowed()` map to `JsonArena`.

- One arena owns reusable `DocumentStorage`.
- `ArenaDocument` is a handle into that arena.
- `JsonArena::reset()` invalidates all arena documents and releases the monotonic
  buffer region.
- `owned_move` is supported for arena parses when moving request/body storage into
  the arena document is better than copying.

Use this for parse/process/reset loops and batch ingestion.

## String policy

`view_unescaped_copy_decoded` is the only supported string model for now.

- Raw unescaped strings may remain as views into input storage.
- Escaped strings are decoded into `DocumentStorage::string_arena`.
- Builder-inserted strings follow the existing builder copy/borrow API.

Do not add APIs that pretend every string is owning or every string is a view.
The split is fundamental to keeping hot-path allocation low while preserving
correct decoded UTF-8 semantics.

## Number policy

`preserve_lexeme_parse_on_access` is the only supported number model for now.

- Parser validates JSON number syntax.
- Number bytes are preserved as lexemes.
- `JsonNumberView::to_i64()`, `to_u64()`, and `to_f64()` perform typed conversion
  on demand.
- Integer conversion must remain exact.
- Floating-point conversion must remain locale-independent. Current libstdc++
  overflow/underflow behavior still requires the documented `strtod_l` C-locale
  fallback for a narrow slow path.

Do not eagerly convert all numbers during parse unless benchmark evidence proves
it wins on representative corpora and does not weaken exact integer behavior.

## UTF-8 policy

`strict_validate_on_parse` is the only supported UTF model for now.

- Strict JSON rejects invalid UTF-8 and invalid Unicode escape sequences.
- JSON5 mode is explicitly opt-in through `JsonParseOptions::mode`.
- Malformed input must report `JsonStage::parse` with a precise issue code and
  source location where possible.

## Object lookup policy

`preserve_order_warm_hash_on_demand` is the only supported object model for now.

- Object member order is preserved.
- Duplicate-key behavior follows `JsonParseOptions::duplicate_key`; default is
  reject.
- Small objects stay linear.
- Larger objects may build a hash index through `warm_member_index(...)` or parser
  `warm_threshold`.
- Hash-index failure is cached per object and returned as `resource_exhausted`.

This avoids yyjson-style linked-list lookup costs for large objects without
turning every small object into a hash table.

## Integration API

Route/framework code should continue to depend on `conflux.json.boundary`, not
on this native DOM prototype. Native application edges can choose this provider
through `conflux.json.native_provider` / `conflux.net.http.native_json`.

Future provider work should add a provider adapter around this policy surface
rather than importing HTTP/app modules into the parser layer.

## Implementation sequence after this slice

1. Keep the policy/facade tests green while refactoring internals.
2. Extract duplicated parser storage reset/BOM setup into private helpers.
3. Prototype a Stage-0 tokenizer behind the same `JsonDomPolicy` surface.
4. Measure with the route/edge/malformed fixtures before changing defaults.
5. Only then decide whether to introduce a new public DOM representation.
