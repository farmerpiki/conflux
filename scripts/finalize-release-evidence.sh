#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s --evidence-dir DIR --log FILE [--full-sanitizer-sku release-json]\n' "$0" >&2
}

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
evidence_dir=""
log_file=""
full_sanitizer_sku="release-json"
skus=(release-json release-http-api release-web-server)

while (($# > 0)); do
    case "$1" in
        --evidence-dir)
            if (($# < 2)); then
                usage
                exit 2
            fi
            evidence_dir="$2"
            shift 2
            ;;
        --log)
            if (($# < 2)); then
                usage
                exit 2
            fi
            log_file="$2"
            shift 2
            ;;
        --full-sanitizer-sku)
            if (($# < 2)); then
                usage
                exit 2
            fi
            full_sanitizer_sku="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

if [[ -z "$evidence_dir" || -z "$log_file" ]]; then
    usage
    exit 2
fi

evidence_dir="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "$evidence_dir")"
log_file="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "$log_file")"
mkdir -p "$evidence_dir/logs"
cp "$log_file" "$evidence_dir/logs/evidence-run.log"

commit_sha="$(git -C "$source_root" rev-parse HEAD)"
commit_short="$(git -C "$source_root" rev-parse --short HEAD)"

scan_pattern='(^CMake Warning|(^|[[:space:]])warning:|(^|[[:space:]])WARNING:|(^|[[:space:]])error:|fatal error:|FAILED:|TEST_FAIL|TEST_WARNING|BUILD_FAIL|BUILD_WARNING|PRESET_SHAPE_FAIL|[1-9][0-9]* tests failed|The following tests FAILED|Errors while running CTest|No space left on device|AddressSanitizer|ThreadSanitizer)'
if grep -En "$scan_pattern" "$evidence_dir/logs/evidence-run.log" >"$evidence_dir/warning-error-scan.txt"; then
    {
        printf 'warning_error_scan=matched\n'
        cat "$evidence_dir/warning-error-scan.txt"
    } >"$evidence_dir/warning-error-scan.tmp"
    mv "$evidence_dir/warning-error-scan.tmp" "$evidence_dir/warning-error-scan.txt"
    printf 'finalize-release-evidence: warning/error scan matched; see %s\n' "$evidence_dir/warning-error-scan.txt" >&2
    exit 1
fi
printf 'clean: no warning/error/sanitizer/failure markers matched\n' >"$evidence_dir/warning-error-scan.txt"

{
    printf 'evidence_run=pre-evidence-release-closure\n'
    printf 'result=success\n'
    printf 'conflux_commit=%s\n' "$commit_sha"
    printf 'conflux_commit_short=%s\n' "$commit_short"
    printf 'warning_error_scan=clean\n'
    printf 'log=logs/evidence-run.log\n'
    printf 'skus=%s\n' "${skus[*]}"
    printf 'release_json_full_sanitizers=%s\n' "$([[ "$full_sanitizer_sku" == "release-json" ]] && printf enabled || printf disabled)"
    printf 'work_root=/tmp/gcc-16/pre-evidence\n'
    printf 'successful_work_trees=cleaned\n'
} >"$evidence_dir/evidence-run-summary.txt"

compiler_version() {
    local label="$1"
    local compiler="$2"
    if command -v "$compiler" >/dev/null 2>&1; then
        {
            printf '%s_version<<EOF\n' "$label"
            "$compiler" --version | sed -n '1,4p'
            printf 'EOF\n'
        }
    else
        printf '%s_version=not-found\n' "$label"
    fi
}

pkg_version() {
    local label="$1"
    local package="$2"
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "$package"; then
        printf '%s_version=%s\n' "$label" "$(pkg-config --modversion "$package")"
    else
        printf '%s_version=not-found\n' "$label"
    fi
}

{
    printf 'conflux_commit=%s\n' "$commit_sha"
    printf 'conflux_commit_short=%s\n' "$commit_short"
    printf 'kernel=%s\n' "$(uname -a)"
    if [[ -r /etc/os-release ]]; then
        distro="$(. /etc/os-release && printf '%s' "${PRETTY_NAME:-${NAME:-unknown}}")"
    else
        distro="unknown"
    fi
    printf 'distro=%s\n' "$distro"
    cpu_model="$(awk -F: '/model name/ {sub(/^[ \t]+/, "", $2); print $2; exit}' /proc/cpuinfo 2>/dev/null || true)"
    printf 'cpu_model=%s\n' "${cpu_model:-unknown}"
    ram_kb="$(awk '/MemTotal/ {print $2; exit}' /proc/meminfo 2>/dev/null || true)"
    if [[ -n "${ram_kb:-}" ]]; then
        printf 'ram_bytes=%s\n' "$((ram_kb * 1024))"
        printf 'ram_human=%sGB\n' "$(((ram_kb * 1024 + 999999999) / 1000000000))"
    else
        printf 'ram_bytes=unknown\n'
        printf 'ram_human=unknown\n'
    fi
    compiler_version clang /usr/lib/llvm/21/bin/clang++
    compiler_version gcc16 g++-16
    printf 'cmake_version=%s\n' "$(cmake --version | sed -n '1p')"
    printf 'ninja_version=%s\n' "$(ninja --version)"
    printf 'python_version=%s\n' "$(python3 --version)"
    pkg_version liburing liburing
    pkg_version openssl openssl
    pkg_version libssl libssl
    pkg_version libcrypto libcrypto
    pkg_version libnghttp2 libnghttp2
    pkg_version zlib zlib
    if command -v openssl >/dev/null 2>&1; then
        printf 'openssl_cli_version=%s\n' "$(openssl version -a | sed -n '1p')"
    else
        printf 'openssl_cli_version=not-found\n'
    fi
} >"$evidence_dir/environment.txt"

{
    printf 'conflux_commit=%s\n' "$commit_sha"
    printf '\nFull evidence regeneration (set EVIDENCE_DIR and SCRATCH_DIR env vars first):\n'
    printf 'cd <conflux-source> && { set -e; for sku in release-json release-http-api release-web-server; do if [[ "$sku" == release-json ]]; then scripts/check-pre-evidence-release-closure.sh --sku "$sku" --full-sanitizers --evidence-dir "$EVIDENCE_DIR"; else scripts/check-pre-evidence-release-closure.sh --sku "$sku" --evidence-dir "$EVIDENCE_DIR"; fi; done; } > "$SCRATCH_DIR/evidence-run.log" 2>&1\n'
    printf '\nEvidence finalization:\n'
    printf 'scripts/finalize-release-evidence.sh --evidence-dir "$EVIDENCE_DIR" --log "$SCRATCH_DIR/evidence-run.log"\n'
    printf '\nOr use the proof harness orchestrator (recommended):\n'
    printf 'cd <conflux_proof> && bash scripts/produce-evidence.sh\n'
} >"$evidence_dir/commands.txt"

{
    printf 'algorithm=sha256\n'
    printf 'zip_artifacts=none\n'
    for sku in "${skus[@]}"; do
        tarball="$evidence_dir/$sku/conflux-${sku}.tar.gz"
        if [[ -f "$tarball" ]]; then
            (cd "$evidence_dir" && sha256sum "$sku/conflux-${sku}.tar.gz")
        else
            printf 'missing  %s/conflux-%s.tar.gz\n' "$sku" "$sku"
            exit 1
        fi
    done
} >"$evidence_dir/checksums.txt"

rm -rf "$evidence_dir/build-cost"
mkdir -p "$evidence_dir/build-cost"
for sku in "${skus[@]}"; do
    mkdir -p "$evidence_dir/build-cost/$sku"
    cp "$evidence_dir/$sku/closure/build-cost/compile-time.json" "$evidence_dir/build-cost/$sku/compile-time.json"
    cp "$evidence_dir/$sku/closure/build-cost/measure-build-costs.json" "$evidence_dir/build-cost/$sku/measure-build-costs.json"
done

printf 'finalize-release-evidence: ok (%s)\n' "$commit_sha"
