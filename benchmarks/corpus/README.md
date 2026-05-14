# JSON benchmark corpus

`conflux_json_bench` loads these files through a source-relative path derived
from `benchmarks/json_bench.cxx`, so normal in-tree builds do not need a data
copy step.

Corpus groups:

- root `*.json`: nativejson-style real-world corpora used for parse/dump
  throughput comparisons.
- `route_payloads/*.json`: application-shaped request/response payloads for
  route-boundary JSON costs.
- `edge/*.json`: valid adversarial inputs that stress number lexemes, escaped
  strings, out-of-order object keys, and duplicate-key policy handling.
- `malformed/*.json`: strict-JSON rejection fixtures. These should fail with
  default `JsonParseOptions` and are timed through the parser error path.

Keep files deterministic and hand-reviewable. Prefer adding a new focused file
over mutating existing corpus files, so benchmark history remains comparable.
