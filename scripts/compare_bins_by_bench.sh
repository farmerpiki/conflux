#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/compare_bins_by_bench.sh [--yes] [--baseline-run-id ID] [--reps N] [--pguri URI] [--dir LABEL:DIR ...] BENCH
  scripts/compare_bins_by_bench.sh [--yes] [--baseline-run-id ID] [--reps N] [--pguri URI] [--dir LABEL:DIR ...] all

When --dir is provided, DIR may be a build root or its benchmarks directory.
The launcher finds matching --bench-info binaries in each labeled directory,
prompts once per benchmark unless --yes is passed, and calls
scripts/bench_record.sh --compare-bins. Without --dir, it preserves the old
/tmp auto-discovery behavior. Pass --yes for unattended batch runs.
EOF
}

AUTO_YES=0
BASELINE_RUN_ID="${BENCH_ITERATIONS_FROM_RUN_ID:-}"
REPS="${BENCH_REPS:-5}"
PGURI="${PGURI:-postgres://postgres@localhost/conflux_bench}"
RECORD_SCRIPT="${RECORD_SCRIPT:-./scripts/bench_record.sh}"
BENCH_NAME=""
START_FROM=""
DIR_SPECS=()

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
    --dir)
      shift
      [[ $# -gt 0 ]] || { echo "--dir needs LABEL:DIR" >&2; exit 2; }
      [[ "$1" == *:* ]] || { echo "--dir needs LABEL:DIR (got $1)" >&2; exit 2; }
      DIR_SPECS+=("$1")
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
  preset="${preset#perf-}"
  case "$preset" in
    clang*) printf '%s\n' clang ;;
    gcc16*) printf '%s\n' gcc16 ;;
    gcc*) printf '%s\n' gcc ;;
    *) printf '%s\n' "${preset%%-*}" ;;
  esac
}

dir_label() {
  local spec="$1"
  printf '%s\n' "${spec%%:*}"
}

dir_path() {
  local spec="$1"
  printf '%s\n' "${spec#*:}"
}

bench_dir_for_spec() {
  local path="$1"
  if [[ -d "$path/benchmarks" ]]; then
    printf '%s\n' "$path/benchmarks"
  else
    printf '%s\n' "$path"
  fi
}

discover_from_dir_specs() {
  local wanted="${1:-}"
  local -a out=()
  local spec label root bench_dir
  for spec in "${DIR_SPECS[@]}"; do
    label=$(dir_label "$spec")
    root=$(dir_path "$spec")
    bench_dir=$(bench_dir_for_spec "$root")
    if [[ ! -d "$bench_dir" ]]; then
      echo "benchmark directory not found for $label: $bench_dir" >&2
      continue
    fi
    while IFS= read -r -d '' bin; do
      local info name
      info=$("$bin" --bench-info 2>/dev/null) || continue
      name=$(jq -r '.name // empty' <<< "$info")
      [[ -n "$wanted" && "$name" != "$wanted" ]] && continue
      out+=("$name"$'\t'"$label:$bin")
    done < <(find "$bench_dir" -maxdepth 1 -type f -executable -name 'conflux_*bench*' -print0 | sort -z)
  done
  printf '%s\n' "${out[@]}"
}

discover_candidates_auto() {
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
  done < <(
    find /tmp \
      \( -path "*/release-*/benchmarks/conflux_${bench}_bench*" \
         -o -path "*/perf-*/benchmarks/conflux_${bench}_bench*" \) \
      -type f -executable -print0 2>/dev/null | sort -z
  )
  printf '%s\n' "${out[@]}"
}

discover_candidates() {
  local bench="$1"
  if (( ${#DIR_SPECS[@]} > 0 )); then
    discover_from_dir_specs "$bench" | awk -F '\t' '{print $2}'
  else
    discover_candidates_auto "$bench"
  fi
}

discover_benches() {
  if (( ${#DIR_SPECS[@]} > 0 )); then
    discover_from_dir_specs "" | awk -F '\t' '{print $1}' | sort -u
  else
    find /tmp \
      \( -path '*/release-*/benchmarks/conflux_*bench*' \
         -o -path '*/perf-*/benchmarks/conflux_*bench*' \) \
      -type f -executable -print0 2>/dev/null \
      | while IFS= read -r -d '' bin; do
          info=$("$bin" --bench-info 2>/dev/null) || continue
          jq -r '.name // empty' <<< "$info"
        done \
      | sed '/^$/d' \
      | sort -u
  fi
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
  if (( ${#candidates[@]} < 2 )); then
    echo "skipping $bench: need at least 2 candidate binaries, found ${#candidates[@]}"
    return 0
  fi

  echo "benchmark: $bench"
  if [[ -n "$BASELINE_RUN_ID" ]]; then
    echo "baseline run id: $BASELINE_RUN_ID"
  fi
  echo "candidates:"
  for c in "${candidates[@]}"; do
    local label="${c%%:*}"
    local bin="${c#*:}"
    local info warmup calibrates target_ms max_iters
    info=$("$bin" --bench-info 2>/dev/null)
    warmup=$(jq -r '[.configs[].args[]?] | any(. == "--warmup")' <<< "$info")
    calibrates=$(jq -r '[.configs[]? | (.args // []) as $args | any(range(0; ($args|length) - 1); $args[.] == "--iterations" and $args[.+1] == "0")] | any' <<< "$info")
    target_ms=$(jq -r '[.configs[]? | .target_ms?] | map(tostring) | unique | join(",")' <<< "$info")
    max_iters=$(jq -r '[.configs[]? | .max_iterations?] | map(tostring) | unique | join(",")' <<< "$info")
    printf '  [%s] %s warmup=%s calibrates=%s target_ms=%s max_iterations=%s\n' \
      "$label" "$bin" "$warmup" "$calibrates" "${target_ms:-default}" "${max_iters:-default}"
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
  mapfile -t benches < <(discover_benches)
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
