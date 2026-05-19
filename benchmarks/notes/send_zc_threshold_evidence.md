# SEND_ZC threshold evidence

`http/send-threshold-bench` is a measurement branch. It must not change
`Config::send_zc_threshold` unless same-host artifacts show a stable win and the
SEND_ZC counters rule out copied-notification or fallback-heavy behavior.

## Wrapper

Use the standalone artifact wrapper when a benchmark database is unnecessary:

```sh
SEND_ZC_PRESET=perf-clang-libcxx \
SEND_ZC_THRESHOLDS="4096 16384 65536" \
SEND_ZC_ITERATIONS=1000 \
SEND_ZC_WARMUP=100 \
SEND_ZC_REPS=5 \
SEND_ZC_LOAD=1 \
SEND_ZC_CONNECTIONS=64 \
SEND_ZC_DURATION=2 \
  scripts/send_zc_threshold_evidence.sh
```

Artifacts are written under:

```text
/tmp/conflux/send-zc-threshold-evidence/<UTC-stamp>/
```

Expected files:

- `configure.log` and `build.log` — selected perf-preset build evidence.
- `send_zc_threshold.raw.ndjson` — raw repeated `conflux_send_zc_bench` rows.
  Each row is annotated with `rep`, `send_zc_threshold`, and `sweep_mode`.
- `send_zc_threshold.summary.json` — grouped timing/counter summary.
- `manifest.json` — preset, build dir, threshold list, repeat counts, load shape,
  commit, and branch where available.

## Summary shape

`scripts/send_zc_threshold_summary.py` validates that every configured sweep has
paired `/off` and `/zc_auto` rows, then emits:

- `variants[]` — per config/variant sample count, median/best `ns_per_iter`,
  optional median/best `requests_per_sec`, aggregate `zc_*` counters,
  `zc_capable_rings` / `zc_enabled_rings` when emitted by the benchmark,
  copied-notification rate, submit-fallback rate, parsed `body_bytes`, and
  `zero_copy_candidate` when the body size is at or above the tested threshold.
- `pairs[]` — off-vs-`zc_auto` comparison for each response class, including
  median and best speedup. Concurrent rows additionally include RPS speedup,
  connection count, duration, errors, parsed `body_bytes`, and
  `zero_copy_candidate`.
- `threshold_rollups[]` — per-threshold counts of usable pairs plus median/best
  speedups for rows whose counters are classified as `ok`.

Pair status is deliberately conservative:

- `ok` — SEND_ZC was attempted, no submit fallback/errors, no TLS bypass, copied
  notifications did not dominate.
- `below_threshold` — parsed response body size stayed below the tested threshold.
- `zc_unsupported` — parsed body size crossed the threshold, but benchmark
  telemetry reported zero SEND_ZC-capable rings.
- `zc_disabled` — parsed body size crossed the threshold, capability was present,
  but telemetry reported zero SEND_ZC-enabled rings.
- `zc_inactive_candidate` — parsed body size crossed the threshold but no
  SEND_ZC attempt was recorded; older raw rows without ring-capability fields use
  this status when they cannot prove whether capability or activation was missing.
- `no_zc_attempts` — retained only for rows whose response size could not be
  parsed, so the summary cannot decide whether the row was below threshold.
- `mostly_copied` — notification CQEs show the kernel mostly copied payloads.
- `submit_fallback` / `zc_errors` — submission or CQE errors made the result
  unsuitable for a threshold-default change.
- `tls_bypass` — TLS response crossed the threshold but intentionally stayed on
  the regular TLS send path.

## Promotion rule

Keep the current default unless host-local artifacts show:

1. a stable median improvement for at least one realistic 64 KiB or 1 MiB plain
   or mapped response class;
2. no regression for smaller sub-threshold responses that would become SEND_ZC
   candidates;
3. low copied-notification and fallback rates;
4. load rows with no material error rate and matching or better RPS;
5. repeated results from the same perf preset, CPU governor, pinned CPU setup,
   and kernel/NIC environment.

Recorded DB path remains:

```sh
ONLY_BENCH=send_zc BENCH_PRESET=perf-clang-libcxx \
  scripts/bench_record.sh send-zc-threshold-local
```

The recorder preserves the same `zc_*` counters in `results.extra` for raw rows.

## Host evidence note: 2026-05-18 upload

Uploaded artifacts under `send-zc-threshold/` were captured on branch `main` at
commit `f85b6739d1c929db61e535c75bf37422013f9ecb` with preset
`release-clang-libcxx`, thresholds 4 KiB / 16 KiB / 64 KiB, five reps, and
64-connection load rows. Configure/build completed with OpenSSL 3.5.5 and
HTTP/3 enabled.

This run still does not justify changing `Config::send_zc_threshold`: every
threshold rollup reported zero `ok` pairs. Status totals across all 72 off-vs-ZC
pairs were balanced across `below_threshold`, `mostly_copied`,
`zc_inactive_candidate`, and `tls_bypass` (18 each).

Important interpretation details:

- Plain-body candidate rows did attempt SEND_ZC, but every candidate notification
  was copied (`copied_notification_rate = 1.0`), so they are intentionally
  classified as `mostly_copied`.
- Static-file rows labelled `mapped` by the benchmark crossed the threshold with
  `zc_capable_rings = 1` and `zc_enabled_rings = 1`, but recorded zero
  `mapped_attempts`. On the current server shape, `serve_static` normally selects
  the async streamed/splice path when a ring-local `FileReader` exists, so these
  rows are path diagnostics rather than threshold evidence.
- TLS rows correctly stay on the TLS send path and are classified as `tls_bypass`.

Keep the 16 KiB default. The next useful SEND_ZC branch is not threshold tuning;
it is either a benchmark naming/coverage cleanup that separates static streamed
from mmap-backed responses, or a focused investigation into whether a true mmap
response path still needs live SEND_ZC evidence.
