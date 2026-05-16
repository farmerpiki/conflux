# `conflux.json` implementation-unit split proposal

Date: 2026-05-15  
Status: **recommended, dedicated branch**  
Scope: split `src/json.cxx` while preserving the public `conflux::json` target and `import conflux.json;` API.

## Decision

Split `src/json.cxx` as an **internal implementation-unit split**, not as new public package components.

`conflux.json` is currently the right downstream ergonomic boundary: users want one JSON import for parser, DOM, builder, typed codec, SAX, and NDJSON. A public target split such as `json_core/json_dom/json_stream/json_codec` would add link/import choices without meaningful dependency savings, because every slice still depends only on `conflux.types` and most real consumers need the DOM plus parse/dump surface anyway.

The worthwhile split is therefore source-shape oriented:

- keep `conflux::json` and module name `conflux.json` stable;
- keep `conflux::json` liburing-free and independent from file I/O/logging globals;
- move large non-template method bodies and cold helpers into private module implementation units;
- leave templates, concepts, and user-specializable codec declarations in the primary interface where importers need them.

## Current source evidence

`src/json.cxx` is the largest module unit in the tree at roughly 9.1k lines / 278 KB. It currently mixes:

1. public vocabulary and options;
2. internal storage layout and object hash-indexing;
3. number classification and locale-backed conversion;
4. string decode and token reader logic;
5. DOM views and lookup helpers;
6. dump/pretty-printing;
7. parser/tokenizer/tree builder;
8. value/object/array builders and merge patch;
9. typed codec templates and validation/schema helpers;
10. SAX/NDJSON/accumulator streaming helpers.

That shape makes every change to parser, dump, or builder internals touch the primary BMI and makes diagnostics/review noisy. The current clean dependency boundary means this can be fixed without adding a new feature flag or forcing downstream code to choose between JSON subcomponents.

## Proposed source layout

```text
src/json.cxx
  Primary module interface. Owns exported enums, option structs, class declarations,
  lightweight inline accessors that are cheap and stable, codec templates/concepts,
  and public free-function declarations. Imports private partitions only as needed.

src/json_storage.cxx
  Private module partition or implementation unit for Node/MemberEntry/ObjHashTable,
  DocumentStorage, hash-index build/lookup, storage factory helpers, and destruction.

src/json_number.cxx
  Private implementation for JsonNumberView conversion, numeric classification,
  locale holder, strict number lexeme validation, and parse-side number-node creation.

src/json_reader.cxx
  JsonStringToken, JsonReader, JsonStreamReader bodies, UTF-8/string decode helpers,
  tokenizer byte-scan helpers, and optional std::experimental::simd scan glue.

src/json_dom.cxx
  NodeRef/ObjectView/ArrayView/range non-template bodies, equality/deep-compare,
  Document/ArenaDocument/JsonArena non-template bodies, warm-index traversal.

src/json_dump.cxx
  Dump implementation, string escaping, pretty-printing, depth truncation, and
  recursive serializer helpers.

src/json_parse.cxx
  Tokenizer + TreeBuilder + parse/parse_copy/parse_view/parse_dom/arena parse bodies.

src/json_builder.cxx
  ValueBuilder/ObjectBuilder/ArrayBuilder non-template bodies, merge_patch, primitive
  insertion/append helpers, and builder-side number validation calls.

src/json_stream.cxx
  SAX handler adapter bodies, NDJSON range, JsonAccumulator, and non-template streaming
  helpers. Keep handler concepts/templates in `src/json.cxx`.
```

The split can use private module partitions where multiple implementation units need shared internal layout. If compiler/toolchain behavior is fragile, prefer normal module implementation units plus a single private `json_storage` partition over exporting extra public modules.

## Public API and package graph

No downstream-facing change:

```text
conflux::json         -> primary module import: conflux.json
conflux::json_file    -> unchanged adapter above json + file_io_sync
conflux::json_reflect -> unchanged optional codec provider above json
```

Do **not** add public targets for the internal slices. The CMake target remains one static library:

```cmake
target_sources(conflux_json
  PUBLIC FILE_SET CXX_MODULES
    FILES src/json.cxx
  PRIVATE
    src/json_storage.cxx
    src/json_number.cxx
    src/json_reader.cxx
    src/json_dom.cxx
    src/json_dump.cxx
    src/json_parse.cxx
    src/json_builder.cxx
    src/json_stream.cxx)
```

If private partitions are used, they must remain private implementation detail and must not be installed/exported as package components.

## Implementation order

1. **Mechanical body extraction:** move only already out-of-class non-template definitions first. Keep behavior byte-for-byte equivalent and avoid API renames.
2. **De-inline hot/cold DOM methods:** move heavier `NodeRef`, `ObjectView`, `Document`, and `JsonArena` bodies out of the primary interface after tests still pass. Keep tiny accessors inline.
3. **Storage partition:** move `Node`, `MemberEntry`, `ObjHashTable`, `DocumentStorage`, and hash helpers behind a private partition once no exported inline body requires full definitions.
4. **Parser/dump/builder extraction:** move parser, dump, merge-patch, and builder bodies into private implementation units.
5. **Compile-time evidence pass:** record clean configure/build/test plus before/after size of the generated `conflux.json` BMI and incremental rebuild timings for parser-only and codec-only edits.

## Validation gates

Run at minimum:

```sh
cmake --preset debug-clang-libcxx
cmake --build --preset debug-clang-libcxx -j
ctest --test-dir <debug-clang-libcxx-build-dir> --output-on-failure
scripts/check-module-interface-regressions.sh
scripts/run-package-config-smoke.sh --prefix /tmp/conflux-install --components 'core;json;json_file'
```

Add targeted gates for this branch:

```text
- JSON parser, reader, SAX, NDJSON, dump, builder, schema/validate, merge_patch tests.
- `CONFLUX_FEATURE_SET=json` configures/builds without liburing.
- `conflux::json` install/export still exposes only `conflux.json` to consumers.
- No new imports of file I/O, mmap, logging, work scheduler, or io_uring from JSON.
```

## Expected benefits

- Smaller primary module interface churn for parser/dump/builder edits.
- Cleaner reviews: parser, dump, builder, DOM, and streaming changes land in focused files.
- Better compiler resilience: fewer cold implementation details in the primary module interface.
- No runtime cost target: object layout and algorithms stay unchanged.
- No downstream ergonomics tax: still one JSON target/import.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Template codec code still has to stay in the primary interface | Accept it; typed codec templates are importer-facing and are not the main parser/dump churn source. |
| Internal storage layout currently leaks into inline DOM accessors | Move heavy accessors out-of-line before hiding storage in a private partition. |
| C++ module compiler fragility around private partitions | Start with plain module implementation units; introduce a single private storage partition only when needed. |
| Accidental new public components | Keep all new files private sources under `conflux_json`; do not install/export internal slices. |
| Runtime regression from de-inlining hot accessors | Keep trivial accessors inline; benchmark lookup/dump/parse before and after de-inlining heavier paths. |

## Recommendation

Implement this after the public API alias cleanup or on a dedicated branch that avoids public rename churn. This is worth doing for maintainability and build ergonomics, but it should be treated as a zero-behavior-change refactor with compile-time evidence, not as a public component redesign.
