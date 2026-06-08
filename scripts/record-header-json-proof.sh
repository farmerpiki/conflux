#!/usr/bin/env bash
# Records clang header-mode JSON compilation proof (json-proof-clang-header).
# In HEADER_INTERFACE mode, the CMakeLists.txt returns early after generating
# the header bridge, so runtime test executables (json_testsuite_gate, etc.)
# are not built. This proof instead verifies that the JSON public headers
# compile cleanly via the header smoke targets.
set -euo pipefail

usage() {
    printf 'usage: %s --evidence-dir DIR [--build-dir DIR] [--log-prefix PATH]\n' "$0" >&2
}

evidence_dir=""
build_dir="/tmp/gcc-16/json-proof-clang-header"
log_prefix="/tmp/gcc-16/json-proof-clang-header"

while (($# > 0)); do
    case "$1" in
        --evidence-dir)
            if (($# < 2)); then usage; exit 2; fi
            evidence_dir="$2"; shift 2 ;;
        --build-dir)
            if (($# < 2)); then usage; exit 2; fi
            build_dir="$2"; shift 2 ;;
        --log-prefix)
            if (($# < 2)); then usage; exit 2; fi
            log_prefix="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

if [[ -z "$evidence_dir" ]]; then usage; exit 2; fi

require_file() {
    local path="$1"
    if [[ ! -f "$path" ]]; then
        printf 'record-header-json-proof: missing required file: %s\n' "$path" >&2
        exit 1
    fi
}

require_dir() {
    local path="$1"
    if [[ ! -d "$path" ]]; then
        printf 'record-header-json-proof: missing required directory: %s\n' "$path" >&2
        exit 1
    fi
}

repo_root="$(git rev-parse --show-toplevel)"
commit="$(git -C "$repo_root" rev-parse HEAD)"
commit_short="$(git -C "$repo_root" rev-parse --short HEAD)"
proof_dir="$evidence_dir/json-proof-clang-header"
logs_dir="$proof_dir/logs"
configure_log="${log_prefix}-configure.log"
build_log="${log_prefix}-build.log"
ctest_log="${log_prefix}-ctest.log"

require_dir "$build_dir"
require_file "$configure_log"
require_file "$build_log"
require_file "$ctest_log"

scan_pattern='warning:|CMake Warning|error:|FAILED|Failure|Error|ERROR|FAIL|FAILURE|runtime error|AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|The following tests FAILED|Errors while running CTest|[1-9][0-9]* tests failed'
scan_output="$(rg -n "$scan_pattern" "$configure_log" "$build_log" "$ctest_log" || true)"
if [[ -n "$scan_output" ]]; then
    mkdir -p "$proof_dir"
    printf '%s\n' "$scan_output" >"$proof_dir/warning-error-scan.txt"
    printf 'record-header-json-proof: warning/error scan matched; leaving build tree at %s\n' "$build_dir" >&2
    exit 1
fi

clang_version="$(/usr/lib/llvm/21/bin/clang++ --version | head -1)"

rm -rf "$proof_dir"
mkdir -p "$logs_dir"
cp "$configure_log" "$logs_dir/configure.log"
cp "$build_log" "$logs_dir/build.log"
cp "$ctest_log" "$logs_dir/ctest.log"

{
    printf 'json_proof=clang_header_interface_json_compilation\n'
    printf 'result=success\n'
    printf 'variant=clang-header\n'
    printf 'conflux_commit=%s\n' "$commit"
    printf 'conflux_commit_short=%s\n' "$commit_short"
    printf 'compiler=%s\n' "$clang_version"
    printf 'interface_mode=HEADER_INTERFACE\n'
    printf 'targets_built=conflux_header_smoke_json conflux_header_smoke_public_includes\n'
    printf 'scope=header_api_compilation_only_not_runtime_proof\n'
    printf 'note=HEADER_INTERFACE mode builds the header bridge; runtime JSONTestSuite requires MODULE_INTERFACE (see json-proof/)\n'
    printf 'logs=logs/configure.log logs/build.log logs/ctest.log\n'
    printf 'successful_work_tree=cleaned\n'
} >"$proof_dir/summary.txt"

{
    printf 'variant=clang-header\n'
    printf 'mkdir -p /tmp/gcc-16\n'
    printf 'rm -rf %q\n' "$build_dir"
    printf 'cmake -Wno-dev -S . -B %q -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/lib/llvm/21/bin/clang++ -DCMAKE_CXX_FLAGS=%q -DCMAKE_EXE_LINKER_FLAGS=%q -DCMAKE_SHARED_LINKER_FLAGS=%q -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE -DCONFLUX_FEATURE_SET=dev-json -DCONFLUX_BUILD_TESTS=ON -DCONFLUX_BUILD_EXAMPLES=OFF -DCONFLUX_BUILD_BENCHMARKS=OFF -DCONFLUX_FETCH_TEST_DEPS=ON -DCONFLUX_TEST_CATCH2_PROVIDER=FETCH > %q 2>&1\n' \
        "$build_dir" \
        '-stdlib=libc++ -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0' \
        '-stdlib=libc++' \
        '-stdlib=libc++' \
        "$configure_log"
    printf 'cmake --build %q --target conflux_header_smoke_json conflux_header_smoke_public_includes > %q 2>&1\n' "$build_dir" "$build_log"
    printf 'ctest --test-dir %q --output-on-failure -E http-facade-header > %q 2>&1\n' "$build_dir" "$ctest_log"
    printf 'scripts/record-header-json-proof.sh --evidence-dir %q --build-dir %q --log-prefix %q\n' "$evidence_dir" "$build_dir" "$log_prefix"
} >"$proof_dir/commands.txt"

printf 'clean: no warning/error/sanitizer/failure markers matched\n' >"$proof_dir/warning-error-scan.txt"

(
    cd "$proof_dir"
    sha256sum summary.txt commands.txt warning-error-scan.txt logs/configure.log logs/build.log logs/ctest.log
) >"$proof_dir/checksums.txt"

rm -rf "$build_dir"

printf 'record-header-json-proof: ok (%s)\n' "$commit"
