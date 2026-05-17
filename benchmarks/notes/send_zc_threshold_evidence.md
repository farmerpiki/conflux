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
  copied-notification rate, and submit-fallback rate.
- `pairs[]` — off-vs-`zc_auto` comparison for each response class, including
  median and best speedup. Concurrent rows additionally include RPS speedup,
  connection count, duration, and errors.
- `threshold_rollups[]` — per-threshold counts of usable pairs plus median/best
  speedups for rows whose counters are classified as `ok`.

Pair status is deliberately conservative:

- `ok` — SEND_ZC was attempted, no submit fallback/errors, no TLS bypass, copied
  notifications did not dominate.
- `no_zc_attempts` — response stayed below the tested threshold.
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
