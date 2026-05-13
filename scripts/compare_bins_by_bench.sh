#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/compare_bins_by_bench.sh [--yes] [--baseline-run-id ID] [--reps N] [--pguri URI] BENCH
  scripts/compare_bins_by_bench.sh [--yes] [--baseline-run-id ID] [--reps N] [--pguri URI] all

The launcher scans /tmp for runnable binaries matching conflux_<bench>_bench,
prompts before running, and calls scripts/bench_record.sh --compare-bins.
Pass --yes for unattended batch runs.
EOF
}

AUTO_YES=0
BASELINE_RUN_ID="${BENCH_ITERATIONS_FROM_RUN_ID:-}"
REPS="${BENCH_REPS:-1}"
PGURI="${PGURI:-postgres://postgres@localhost/conflux_bench}"
RECORD_SCRIPT="${RECORD_SCRIPT:-./scripts/bench_record.sh}"
BENCH_NAME=""
START_FROM=""

while (($#)); do
  case "$1" in
    --yes|-y)
      AUTO_YES=1
      ;;
    --baseline-run-id)
      shift
      [[ $# -gt 0 ]] || { echo "--baseline-run-id needs a value" >&2; exit 2; }
      BASELINE_RUN_ID="$1"
      ;;
    --reps)
      shift
      [[ $# -gt 0 ]] || { echo "--reps needs a value" >&2; exit 2; }
      REPS="$1"
      ;;
    --pguri)
      shift
      [[ $# -gt 0 ]] || { echo "--pguri needs a value" >&2; exit 2; }
      PGURI="$1"
      ;;
    --record-script)
      shift
      [[ $# -gt 0 ]] || { echo "--record-script needs a value" >&2; exit 2; }
      RECORD_SCRIPT="$1"
      ;;
    --start-from)
      shift
      [[ $# -gt 0 ]] || { echo "--start-from needs a value" >&2; exit 2; }
      START_FROM="$1"
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      if [[ -z "$BENCH_NAME" ]]; then
        BENCH_NAME="$1"
      else
        echo "unexpected extra argument: $1" >&2
        exit 2
      fi
      ;;
  esac
  shift
done

[[ -n "$BENCH_NAME" ]] || { usage >&2; exit 2; }

normalize_tree() {
  local tree="$1"
  case "$tree" in
    gcc-16) printf '%s\n' gcc16 ;;
    *) printf '%s\n' "$tree" ;;
  esac
}

normalize_preset() {
  local preset="$1"
  preset="${preset#release-}"
  case "$preset" in
    clang*) printf '%s\n' clang ;;
    gcc16*) printf '%s\n' gcc16 ;;
    gcc*) printf '%s\n' gcc ;;
    *) printf '%s\n' "${preset%%-*}" ;;
  esac
}

discover_candidates() {
  local bench="$1"
  local -a out=()
  while IFS= read -r -d '' bin; do
    local info name tree preset label
    info=$("$bin" --bench-info 2>/dev/null) || continue
    name=$(jq -r '.name // empty' <<< "$info")
    [[ "$name" == "$bench" ]] || continue
    tree=$(normalize_tree "$(basename "$(dirname "$(dirname "$bin")")")")
    preset=$(normalize_preset "$(basename "$(dirname "$bin")")")
    label="${tree}-${preset}"
    out+=("$label:$bin")
  done < <(find /tmp -path "*/release-*/benchmarks/conflux_${bench}_bench*" -type f -executable -print0 2>/dev/null | sort -z)
  printf '%s\n' "${out[@]}"
}

run_one() {
  local bench="$1"
  if [[ -n "$START_FROM" ]]; then
    if [[ "$bench" == "$START_FROM" ]]; then
      START_FROM=""
    else
      echo "skipping $bench (before start-from $START_FROM)"
      return 0
    fi
  fi
  mapfile -t candidates < <(discover_candidates "$bench")
  if (( ${#candidates[@]} == 0 )); then
    echo "no runnable binaries found for $bench under /tmp" >&2
    return 1
  fi

  echo "benchmark: $bench"
  if [[ -n "$BASELINE_RUN_ID" ]]; then
    echo "baseline run id: $BASELINE_RUN_ID"
  fi
  echo "candidates:"
  for c in "${candidates[@]}"; do
    local label="${c%%:*}"
    local bin="${c#*:}"
    local info warmup
    info=$("$bin" --bench-info 2>/dev/null)
    warmup=$(jq -r '[.configs[].args[]?] | any(. == "--warmup")' <<< "$info")
    printf '  [%s] %s warmup=%s\n' "$label" "$bin" "$warmup"
  done

  if (( ! AUTO_YES )); then
    read -r -p "Run compare-bins for $bench on ${#candidates[@]} binaries? [y/N] " reply
    case "$reply" in
      y|Y|yes|YES) ;;
      *) echo "skipped $bench"; return 0 ;;
    esac
  fi

  env_args=()
  [[ -n "$BASELINE_RUN_ID" ]] && env_args+=(BENCH_ITERATIONS_FROM_RUN_ID="$BASELINE_RUN_ID")
  [[ -n "$REPS" ]] && env_args+=(BENCH_REPS="$REPS")
  env_args+=(PGURI="$PGURI")

  echo "running compare-bins for $bench"
  if ! env "${env_args[@]}" "$RECORD_SCRIPT" --compare-bins "${candidates[@]}"; then
    echo "compare-bins failed for $bench; continuing" >&2
    return 1
  fi
}

if [[ "$BENCH_NAME" == "all" ]]; then
  mapfile -t benches < <(
    find /tmp -path '*/release-*/benchmarks/conflux_*_bench*' -type f -executable -print0 2>/dev/null \
      | xargs -0 -n1 basename \
      | sed -n 's/^conflux_\(.*\)_bench$/\1/p' \
      | sort -u
  )
  if (( ${#benches[@]} == 0 )); then
    echo "no benchmark binaries found under /tmp" >&2
    exit 1
  fi
  for bench in "${benches[@]}"; do
    if ! run_one "$bench"; then
      echo "continuing after failure in $bench" >&2
    fi
  done
else
  run_one "$BENCH_NAME"
fi
