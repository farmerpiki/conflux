# conflux.json Remaining Work

Status: open inventory

Completed phase detail was pruned. Keep new JSON work narrow and tied to a
measurable parser, DOM, or typed-serde boundary.

## Open

- Full simdjson-style Stage-0 tokenizer only if benchmark gates justify the
  architectural cost.
- Replace `strtod_l` / `CLocaleHolder` only after libstdc++ `from_chars<double>`
  handles overflow/underflow correctly on supported toolchains.
- Compile-time JSON literal parsing: design return type and `decode<T>`
  integration before implementation.
- `debug-p2996-clang` preset only if a compatible reflection-capable Clang is
  available on the maintainer host.

## Done Reference

Completed: reflection support under GCC, JSON5 subset, schema lite, fuzz target
coverage, JSON stream reader, parser/DOM facade, and socket/file JSON benchmark
coverage.
