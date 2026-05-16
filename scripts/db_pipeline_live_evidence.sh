#!/usr/bin/env bash
# Run the live PostgreSQL checks needed to close the DB pipeline evidence gap.
#
# Required env:
#   PG_TEST_CONNINFO  libpq conninfo for integration tests
#   PG_CONNINFO       libpq conninfo for db_pipeline_bench
#
# Optional env:
#   DB_PIPELINE_PRESET          CMake preset; default release-gcc-stdcxx
#   DB_PIPELINE_BATCHES         measured batches; default 60
#   DB_PIPELINE_BATCH_N         inserts per pipeline sync; default 100
#   DB_PIPELINE_WARMUP_BATCHES  warmup batches; default 10
#   DB_PIPELINE_REPS           repeated bench launches; default 5
#   DB_PIPELINE_ARTIFACT_DIR    output dir; default /tmp/<repo>/db-pipeline-evidence/<stamp>
set -euo pipefail

usage() {
	cat >&2 <<'USAGE'
usage: PG_TEST_CONNINFO=... PG_CONNINFO=... scripts/db_pipeline_live_evidence.sh

Runs:
  1. configure/build of DB integration + db_pipeline benchmark targets
  2. CTest DB integration slice via the db;integration label
  3. repeated db_pipeline_bench JSON runs into an artifact directory
USAGE
}

require_env() {
	local name=$1
	if [[ -z "${!name:-}" ]]; then
		printf 'required env not set: %s\n' "$name" >&2
		usage
		exit 2
	fi
}

script_repo_root() {
	local script_dir
	script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
	cd "$script_dir/.." && pwd
}

require_env PG_TEST_CONNINFO
require_env PG_CONNINFO

REPO_ROOT="${SOURCE_DIR:-$(git rev-parse --show-toplevel 2>/dev/null || script_repo_root)}"
cd "$REPO_ROOT"

PRESET="${DB_PIPELINE_PRESET:-release-gcc-stdcxx}"
BATCHES="${DB_PIPELINE_BATCHES:-60}"
BATCH_N="${DB_PIPELINE_BATCH_N:-100}"
WARMUP_BATCHES="${DB_PIPELINE_WARMUP_BATCHES:-10}"
REPS="${DB_PIPELINE_REPS:-5}"
RUN_STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
ARTIFACT_DIR="${DB_PIPELINE_ARTIFACT_DIR:-/tmp/$(basename "$REPO_ROOT")/db-pipeline-evidence/$RUN_STAMP}"
mkdir -p "$ARTIFACT_DIR"

require_positive_int() {
	local name=$1 value=$2
	if ! [[ "$value" =~ ^[1-9][0-9]*$ ]]; then
		printf '%s must be a positive integer: %s\n' "$name" "$value" >&2
		exit 2
	fi
}

require_positive_int DB_PIPELINE_BATCHES "$BATCHES"
require_positive_int DB_PIPELINE_BATCH_N "$BATCH_N"
require_positive_int DB_PIPELINE_WARMUP_BATCHES "$WARMUP_BATCHES"
require_positive_int DB_PIPELINE_REPS "$REPS"

configure_log="$ARTIFACT_DIR/configure.log"
build_log="$ARTIFACT_DIR/build.log"
ctest_log="$ARTIFACT_DIR/ctest-db-integration.log"
raw_ndjson="$ARTIFACT_DIR/db_pipeline.raw.ndjson"
summary_json="$ARTIFACT_DIR/db_pipeline.summary.json"
manifest_json="$ARTIFACT_DIR/manifest.json"

printf 'configuring preset %s\n' "$PRESET"
cmake --preset "$PRESET" -DCONFLUX_PG_TEST_CONNINFO="$PG_TEST_CONNINFO" \
	> "$configure_log" 2>&1
BUILD_DIR="$(sed -n 's/^-- Build files have been written to: //p' "$configure_log" | tail -1)"
if [[ -z "$BUILD_DIR" || ! -d "$BUILD_DIR" ]]; then
	printf 'configure failed; log=%s\n' "$configure_log" >&2
	tail -40 "$configure_log" >&2
	exit 2
fi

printf 'building DB integration and pipeline benchmark targets\n'
if ! cmake --build "$BUILD_DIR" --target conflux_db_integration conflux_db_pipeline_bench -- -j"$(nproc)" \
	> "$build_log" 2>&1; then
	printf 'build failed; log=%s\n' "$build_log" >&2
	tail -40 "$build_log" >&2
	exit 2
fi

printf 'running DB integration tests\n'
PG_TEST_CONNINFO="$PG_TEST_CONNINFO" ctest --test-dir "$BUILD_DIR" -L '^db$' --output-on-failure \
	> "$ctest_log" 2>&1 || {
		printf 'DB integration tests failed; log=%s\n' "$ctest_log" >&2
		tail -80 "$ctest_log" >&2
		exit 1
	}

bench_bin="$BUILD_DIR/benchmarks/conflux_db_pipeline_bench"
: > "$raw_ndjson"
for rep in $(seq 1 "$REPS"); do
	printf 'running db_pipeline bench rep %s/%s\n' "$rep" "$REPS"
	rep_ndjson="$ARTIFACT_DIR/db_pipeline.rep${rep}.tmp.ndjson"
	PG_CONNINFO="$PG_CONNINFO" "$bench_bin" \
		--batches "$BATCHES" \
		--batch-n "$BATCH_N" \
		--warmup-batches "$WARMUP_BATCHES" \
		--config-name "b${BATCHES}_n${BATCH_N}" \
		--json > "$rep_ndjson"
	jq -c --argjson rep "$rep" '. + {rep: $rep}' "$rep_ndjson" >> "$raw_ndjson"
	rm -f "$rep_ndjson"
done

validate_pipeline_rows() {
	local raw=$1 expected=$2
	jq -s -e --argjson expected "$expected" '
	  def valid_row:
	    (.variant == "plain" or .variant == "pipeline")
	    and (.rep | type == "number")
	    and (.rep >= 1 and .rep <= $expected)
	    and (.iterations | type == "number")
	    and (.total_ns | type == "number")
	    and (.ns_per_iter | type == "number")
	    and (.iterations > 0)
	    and (.total_ns >= 0)
	    and (.ns_per_iter > 0);
	  . as $rows
	  | ($rows | length) == ($expected * 2)
	    and all($rows[]; valid_row)
	    and all(range(1; $expected + 1);
	      . as $rep
	      | (($rows | map(select(.rep == $rep and .variant == "plain")) | length) == 1)
	        and (($rows | map(select(.rep == $rep and .variant == "pipeline")) | length) == 1))
	' "$raw" >/dev/null
}

if ! validate_pipeline_rows "$raw_ndjson" "$REPS"; then
	printf 'db_pipeline_bench produced invalid/partial NDJSON; raw=%s\n' "$raw_ndjson" >&2
	jq -r -R 'try fromjson catch "invalid-json"' "$raw_ndjson" >&2 || true
	exit 1
fi

jq -n \
	--slurpfile rows <(jq -s '.' "$raw_ndjson") \
	--arg preset "$PRESET" \
	--arg build_dir "$BUILD_DIR" \
	--arg artifact_dir "$ARTIFACT_DIR" \
	--argjson reps "$REPS" \
	--argjson batches "$BATCHES" \
	--argjson batch_n "$BATCH_N" \
	--argjson warmup_batches "$WARMUP_BATCHES" \
	'
	  def median: sort | .[(length / 2 | floor)];
	  $rows[0] as $raw
	  | ($raw
	     | group_by(.variant)
	     | map({variant: .[0].variant,
	            samples: length,
	            median_ns_per_iter: (map(.ns_per_iter) | median),
	            best_ns_per_iter: (map(.ns_per_iter) | min)})) as $variants
	  | ($raw
	     | group_by(.rep)
	     | map({rep: .[0].rep,
	            plain_ns_per_iter: (map(select(.variant == "plain"))[0].ns_per_iter),
	            pipeline_ns_per_iter: (map(select(.variant == "pipeline"))[0].ns_per_iter)})
	     | map(. + {speedup: (.plain_ns_per_iter / .pipeline_ns_per_iter)})) as $paired
	  | {preset:$preset, build_dir:$build_dir, artifact_dir:$artifact_dir,
	     reps:$reps, batches:$batches, batch_n:$batch_n, warmup_batches:$warmup_batches,
	     variants:$variants,
	     paired_speedups:$paired,
	     speedup_median: ($paired | map(.speedup) | median),
	     speedup_best: ($paired | map(.speedup) | max)}' \
	> "$summary_json"

jq -n \
	--arg preset "$PRESET" \
	--arg build_dir "$BUILD_DIR" \
	--arg artifact_dir "$ARTIFACT_DIR" \
	--arg raw_ndjson "$raw_ndjson" \
	--arg summary_json "$summary_json" \
	--arg ctest_log "$ctest_log" \
	--arg commit "$(git rev-parse HEAD 2>/dev/null || echo unknown)" \
	--arg branch "$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)" \
	'{preset:$preset, build_dir:$build_dir, artifact_dir:$artifact_dir,
	  raw_ndjson:$raw_ndjson, summary_json:$summary_json, ctest_log:$ctest_log,
	  commit:$commit, branch:$branch}' \
	> "$manifest_json"

printf 'DB pipeline live evidence written to %s\n' "$ARTIFACT_DIR"
printf 'summary: %s\n' "$summary_json"
