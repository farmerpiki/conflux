#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s --evidence-dir DIR [--build-dir DIR] [--log-prefix PATH]\n' "$0" >&2
}

evidence_dir=""
build_dir="/tmp/gcc-16/json-proof-clang-clean"
log_prefix="/tmp/gcc-16/json-proof-clang-clean"

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
        --build-dir)
            if (($# < 2)); then
                usage
                exit 2
            fi
            build_dir="$2"
            shift 2
            ;;
        --log-prefix)
            if (($# < 2)); then
                usage
                exit 2
            fi
            log_prefix="$2"
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

if [[ -z "$evidence_dir" ]]; then
    usage
    exit 2
fi

require_file() {
    local path="$1"
    if [[ ! -f "$path" ]]; then
        printf 'record-json-proof-evidence: missing required file: %s\n' "$path" >&2
        exit 1
    fi
}

require_dir() {
    local path="$1"
    if [[ ! -d "$path" ]]; then
        printf 'record-json-proof-evidence: missing required directory: %s\n' "$path" >&2
        exit 1
    fi
}

repo_root="$(git rev-parse --show-toplevel)"
commit="$(git -C "$repo_root" rev-parse HEAD)"
commit_short="$(git -C "$repo_root" rev-parse --short HEAD)"
proof_dir="$evidence_dir/json-proof"
logs_dir="$proof_dir/logs"
configure_log="${log_prefix}-configure.log"
build_log="${log_prefix}-build.log"
ctest_log="${log_prefix}-ctest.log"

require_dir "$build_dir"
require_file "$configure_log"
require_file "$build_log"
require_file "$ctest_log"
require_dir "$build_dir/_deps/jsontestsuite-src"
require_dir "$build_dir/_deps/catch2-src"

scan_pattern='warning:|CMake Warning|error:|FAILED|Failure|Error|ERROR|FAIL|FAILURE|runtime error|AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|The following tests FAILED|Errors while running CTest|[1-9][0-9]* tests failed'
scan_output="$(rg -n "$scan_pattern" "$configure_log" "$build_log" "$ctest_log" || true)"
if [[ -n "$scan_output" ]]; then
    mkdir -p "$proof_dir"
    printf '%s\n' "$scan_output" >"$proof_dir/warning-error-scan.txt"
    printf 'record-json-proof-evidence: warning/error scan matched; leaving build tree at %s\n' "$build_dir" >&2
    exit 1
fi

rm -rf "$proof_dir"
mkdir -p "$logs_dir"
cp "$configure_log" "$logs_dir/configure.log"
cp "$build_log" "$logs_dir/build.log"
cp "$ctest_log" "$logs_dir/ctest.log"

json_testsuite_commit="$(git -C "$build_dir/_deps/jsontestsuite-src" rev-parse HEAD)"
catch2_commit="$(git -C "$build_dir/_deps/catch2-src" rev-parse HEAD)"
node_version="$(node --version 2>/dev/null || printf 'not_available')"
python_version="$(python3 --version 2>&1)"
clang_version="$(/usr/lib/llvm/21/bin/clang++ --version | head -1)"

{
    printf 'json_proof=JSONTestSuite_and_differential_smoke\n'
    printf 'result=success\n'
    printf 'conflux_commit=%s\n' "$commit"
    printf 'conflux_commit_short=%s\n' "$commit_short"
    printf 'compiler=%s\n' "$clang_version"
    printf 'python=%s\n' "$python_version"
    printf 'node=%s\n' "$node_version"
    printf 'JSONTestSuite_commit=%s\n' "$json_testsuite_commit"
    printf 'Catch2_commit=%s\n' "$catch2_commit"
    printf 'tests=JSONTestSuite_y_accept JSONTestSuite_n_reject json_differential_smoke_node_python\n'
    printf 'scope=parser_accept_reject_only_not_public_benchmark_proof\n'
    printf 'logs=logs/configure.log logs/build.log logs/ctest.log\n'
    printf 'successful_work_tree=cleaned\n'
} >"$proof_dir/summary.txt"

{
    printf 'mkdir -p /tmp/gcc-16\n'
    printf 'rm -rf %q\n' "$build_dir"
    printf 'cmake -Wno-dev -S . -B %q -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/lib/llvm/21/bin/clang++ -DCMAKE_CXX_FLAGS=%q -DCMAKE_EXE_LINKER_FLAGS=%q -DCMAKE_SHARED_LINKER_FLAGS=%q -DCONFLUX_FEATURE_SET=dev-json -DCONFLUX_BUILD_TESTS=ON -DCONFLUX_BUILD_EXAMPLES=OFF -DCONFLUX_BUILD_BENCHMARKS=OFF -DCONFLUX_FETCH_TEST_DEPS=ON -DCONFLUX_TEST_CATCH2_PROVIDER=FETCH -DCONFLUX_ENABLE_JSON_TESTSUITE=ON > %q 2>&1\n' \
        "$build_dir" \
        '-stdlib=libc++ -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0' \
        '-stdlib=libc++' \
        '-stdlib=libc++' \
        "$configure_log"
    printf 'cmake --build %q --target conflux_json_testsuite_gate conflux_json_differential_smoke > %q 2>&1\n' "$build_dir" "$build_log"
    printf 'ctest --test-dir %q --output-on-failure -R %q > %q 2>&1\n' "$build_dir" '^(JSONTestSuite:|json differential smoke:)' "$ctest_log"
    printf 'scripts/record-json-proof-evidence.sh --evidence-dir %q --build-dir %q --log-prefix %q\n' "$evidence_dir" "$build_dir" "$log_prefix"
} >"$proof_dir/commands.txt"

printf 'clean: no warning/error/sanitizer/failure markers matched\n' >"$proof_dir/warning-error-scan.txt"

(
    cd "$proof_dir"
    sha256sum summary.txt commands.txt warning-error-scan.txt logs/configure.log logs/build.log logs/ctest.log
) >"$proof_dir/checksums.txt"

rm -rf "$build_dir"

printf 'record-json-proof-evidence: ok (%s)\n' "$commit"
