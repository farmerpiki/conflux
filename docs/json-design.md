# conflux.json Design Notes

## Process-lifetime state

`conflux.json` has one permitted process-lifetime singleton: the internal
`CLocaleHolder` used by the slow floating-point range classifier.

The parser normally uses `std::from_chars` for JSON numbers. On current
libstdc++ builds, `from_chars` may report `std::errc::result_out_of_range`
without leaving enough information in the destination `double` to distinguish
finite underflow from overflow-to-infinity. The slow classifier therefore falls
back to `strtod_l` with an explicit C locale for that narrow error path.

The C locale object is intentionally cached for the process lifetime instead of
being created and destroyed per number parse. That keeps locale setup out of the
hot path, avoids repeated libc allocation, and keeps JSON decimal parsing
independent from the process locale. It is never freed because there is no
benefit to doing so during normal shutdown and because freeing it would add
cross-thread lifetime risk for an internal fallback path.

Design constraints:

- no other hidden global or singleton state in `conflux.json`;
- no parser behavior may depend on the process locale;
- all per-document mutable state belongs to `DocumentStorage`, `JsonArena`, or
  caller-provided storage;
- any future process-lifetime cache must be documented here before it is added.

Removal condition: delete `CLocaleHolder`, `strtod_l`, and this exception once
the supported libstdc++/libc++ matrix provides `from_chars` behavior that fully
passes the number overflow/underflow gate without the locale fallback.

## Parser/DOM policy

`json/parser-dom-design` introduced an explicit preview policy surface in
`conflux.json`: `JsonDomPolicy` plus `parse_dom(...)` overloads. The policy names
the architecture that future parser rewrites must preserve:

- memory model: borrowed view document, owned document, caller-PMR document, or
  reusable `JsonArena` document;
- error model: `std::expected<T, JsonError>` only;
- string model: unescaped input strings may be views; escaped strings decode into
  document storage;
- number model: preserve lexemes and convert on typed access;
- UTF model: strict validation during parse;
- object model: preserve member order and warm hash indexes on demand.

Detailed branch notes live in `docs/json-dom-prototype.md`. This design is
intentionally a facade over the current parser and arena implementation, not a
new parser. Stage-0 tokenizer work, arena DOM replacement, and reflection serde
must build behind this surface rather than adding new HTTP/app JSON dependencies.
