# conflux.json Remaining Work

Status: open TODO

Keep new JSON work narrow and tied to a measurable parser, DOM, or typed-serde
boundary.

## Open

- Perf-gated: full simdjson-style Stage-0 tokenizer only if benchmark gates justify the
  architectural cost.
- Toolchain-gated: replace `strtod_l` / `CLocaleHolder` only after libstdc++ `from_chars<double>`
  handles overflow/underflow correctly on supported toolchains.
- Deferred: implement/export the compile-time JSON literal API only if a real
  preview user or docs path needs it. The return type and `decode<T>`
  integration design is already documented; no exported literal API exists in
  the current preview.
