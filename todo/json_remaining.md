# conflux.json Remaining Work

Status: open TODO

Keep new JSON work narrow and tied to a measurable parser, DOM, or typed-serde
boundary.

## Open

- Perf-gated: full simdjson-style Stage-0 tokenizer only if benchmark gates justify the
  architectural cost.
- Toolchain-gated: replace `strtod_l` / `CLocaleHolder` only after libstdc++ `from_chars<double>`
  handles overflow/underflow correctly on supported toolchains.
- [x] Compile-time JSON literal parsing: design return type and `decode<T>`
  integration before implementation.
- Toolchain-gated: `debug-p2996-clang` preset only if a compatible reflection-capable Clang is
  available on the maintainer host.
