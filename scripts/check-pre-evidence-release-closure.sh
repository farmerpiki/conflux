#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s [--sku release-json|release-http-api|release-web-server|release-pg] [--work-root DIR] [--skip-heavy] [--full-sanitizers] [--evidence-dir DIR]\n' "$0" >&2
}

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
release_sku="release-json"
work_root="${TMPDIR:-/tmp}/gcc-16/pre-evidence/$release_sku"
work_root_explicit=0
skip_heavy=0
full_sanitizers=0
evidence_dir=""

while (($# > 0)); do
    case "$1" in
        --sku)
            if (($# < 2)); then
                usage
                exit 2
            fi
            release_sku="$2"
            if ((work_root_explicit == 0)); then
                work_root="${TMPDIR:-/tmp}/gcc-16/pre-evidence/$release_sku"
            fi
            shift 2
            ;;
        --work-root)
            if (($# < 2)); then
                usage
                exit 2
            fi
            work_root="$2"
            work_root_explicit=1
            shift 2
            ;;
        --skip-heavy)
            skip_heavy=1
            shift
            ;;
        --full-sanitizers)
            full_sanitizers=1
            shift
            ;;
        --evidence-dir)
            if (($# < 2)); then
                usage
                exit 2
            fi
            evidence_dir="$2"
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

case "$release_sku" in
    release-json|release-http-api|release-web-server|release-pg) ;;
    *)
        printf 'check-pre-evidence-release-closure: unsupported release SKU: %s\n' "$release_sku" >&2
        exit 2
        ;;
esac

work_root="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "$work_root")"
evidence_root="$(python3 -c 'import pathlib, sys; print((pathlib.Path(sys.argv[1]).resolve().parent / "evidence").resolve())' "$source_root")"
case "$work_root" in
    "$evidence_root"|"$evidence_root"/*)
        printf 'check-pre-evidence-release-closure: refusing to write under ../evidence for scratch output: %s\n' "$work_root" >&2
        exit 2
        ;;
esac
if [[ -n "$evidence_dir" ]]; then
    evidence_dir="$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "$evidence_dir")"
fi

cleanup_success() {
    local status=$?
    if ((status == 0)); then
        rm -rf "$work_root"
    fi
}
trap cleanup_success EXIT

prepare_marked_dir() {
    local dir="$1"
    local marker="$dir/.conflux-closure-evidence"
    if [[ -d "$dir" && ! -f "$marker" ]]; then
        printf 'check-pre-evidence-release-closure: refusing to clean unmarked evidence directory: %s\n' "$dir" >&2
        exit 1
    fi
    rm -rf "$dir"
    mkdir -p "$dir"
    touch "$marker"
}

copy_if_exists() {
    local source="$1"
    local dest="$2"
    if [[ -e "$source" ]]; then
        mkdir -p "$(dirname "$dest")"
        cp -a "$source" "$dest"
    fi
}

stage_closure_evidence() {
    local dest="$1/$release_sku/closure"
    prepare_marked_dir "$dest"

    {
        printf 'closure=pre-evidence-release\n'
        printf 'result=success\n'
        printf 'release_sku=%s\n' "$release_sku"
        printf 'feature_set=%s\n' "$feature_set"
        printf 'components=%s\n' "$components"
        printf 'full_sanitizers=%s\n' "$full_sanitizers"
        printf 'heavy_checks_skipped=%s\n' "$skip_heavy"
        printf 'work_root=$SCRATCH_DIR/closure-%s\n' "$release_sku"
        printf 'scratch_cleanup=success-removes-work-root\n'
        printf 'build_cost=build-cost\n'
        printf 'capabilities=capabilities/capability-report.txt\n'
        printf 'logs=logs\n'
    } > "$dest/closure-summary.txt"

    copy_if_exists "$work_root/build-cost/compile-time.json" "$dest/build-cost/compile-time.json"
    copy_if_exists "$work_root/build-cost/measure-build-costs.json" "$dest/build-cost/measure-build-costs.json"
    copy_if_exists "$work_root/capabilities/capability-report.txt" "$dest/capabilities/capability-report.txt"
    copy_if_exists "$work_root/logs" "$dest/logs"
}

feature_set="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" "$release_sku" feature_set)"
components="$(python3 "$source_root/scripts/release-sku-field.py" "$source_root" "$release_sku" components)"
build_cost_target="conflux_quickstart_hello"
if [[ "$release_sku" == "release-json" ]]; then
    build_cost_target="conflux_header_smoke_json"
elif [[ "$release_sku" == "release-pg" ]]; then
    build_cost_target="conflux_db_basic"
fi

rm -rf "$work_root"
mkdir -p "$work_root/release-governance" "$work_root/source-archive" "$work_root/build-cost" "$work_root/capabilities" "$work_root/logs"

python3 "$source_root/scripts/check-release-docs.py"
python3 "$source_root/scripts/check-release-skus.py"
python3 "$source_root/scripts/check-release-sku-examples.py"
python3 "$source_root/scripts/check-release-notes.py"
python3 "$source_root/scripts/check-component-map.py"
python3 "$source_root/scripts/check-api-surface-map.py"
python3 "$source_root/scripts/check-package-docs.py"
python3 "$source_root/scripts/check-capability-report-docs.py"

CONFLUX_RELEASE_SOURCE_ARCHIVE_SKU="$release_sku" \
CONFLUX_RELEASE_SOURCE_ARCHIVE_STAGE="$work_root/source-archive/stage" \
    "$source_root/scripts/check-release-source-archive.sh"

CONFLUX_RELEASE_GENERATED_HEADERS_STAGE="$work_root/generated-headers/stage" \
    "$source_root/scripts/check-release-generated-headers-policy.sh"

if ((skip_heavy == 0)); then
    if ((full_sanitizers == 1)); then
        "$source_root/scripts/run-sanitizer-matrix.sh" \
            --full-release-gate \
            --log-root "$work_root/logs/full-sanitizers" \
            --build-root "$work_root/full-sanitizers"
    fi

    "$source_root/scripts/check-provider-policy-matrix.sh"

    CONFLUX_RELEASE_OFFLINE_SKU="$release_sku" \
    CONFLUX_RELEASE_OFFLINE_WORK="$work_root/offline-bootstrap" \
        "$source_root/scripts/check-release-offline-bootstrap.sh"

    CONFLUX_RELEASE_BOOTSTRAP_SKU="$release_sku" \
    CONFLUX_RELEASE_BOOTSTRAP_WORK="$work_root/artifact-bootstrap" \
        "$source_root/scripts/check-release-artifact-bootstrap.sh"

    case "$release_sku" in
        release-json)
            TMPDIR="$work_root/package-smoke" "$source_root/scripts/check-package-smoke-liburing-free.sh"
            TMPDIR="$work_root/package-smoke" "$source_root/scripts/check-package-smoke-json-standalone.sh"
            ;;
        release-http-api)
            TMPDIR="$work_root/package-smoke" "$source_root/scripts/check-package-smoke-api-surfaces.sh"
            TMPDIR="$work_root/package-smoke" "$source_root/scripts/check-package-smoke-runtime.sh"
            ;;
        release-web-server)
            TMPDIR="$work_root/package-smoke" "$source_root/scripts/check-package-smoke-runtime.sh"
            ;;
        release-pg)
            TMPDIR="$work_root/package-smoke" "$source_root/scripts/check-package-smoke-db.sh"
            ;;
    esac

    python3 "$source_root/scripts/compile_time_bench.py" \
        --source "$source_root" \
        --build "$work_root/build-cost/compile-time" \
        --feature-set "$feature_set" \
        --interface-mode HEADER_INTERFACE \
        --target "$build_cost_target" \
        --pretty >"$work_root/build-cost/compile-time.json"

    python3 "$source_root/scripts/measure-build-costs.py" \
        "$work_root/artifact-bootstrap/header-build" \
        --sku "$release_sku" \
        --json >"$work_root/build-cost/measure-build-costs.json"

    cmake -S "$source_root" -B "$work_root/capabilities/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCONFLUX_FEATURE_SET=release-web-server \
        -DCONFLUX_INTERFACE_MODE=HEADER_INTERFACE \
        -DCONFLUX_HEADER_LINK_EXAMPLES=ON \
        -DCONFLUX_BUILD_TESTS=OFF \
        -DCONFLUX_BUILD_EXAMPLES=ON \
        -DCONFLUX_BUILD_BENCHMARKS=OFF \
        -DCONFLUX_POSTGRES_PROVIDER=OFF
    cmake --build "$work_root/capabilities/build" --target conflux_capability_report_example
    cap_bin="$(find "$work_root/capabilities/build" -type f -name conflux_capability_report_example -perm /111 -print -quit)"
    if [[ -z "$cap_bin" ]]; then
        printf 'check-pre-evidence-release-closure: missing capability report executable\n' >&2
        exit 1
    fi
    timeout 10s "$cap_bin" >"$work_root/capabilities/capability-report.txt"
    grep -Fq 'conflux' "$work_root/capabilities/capability-report.txt"
fi

if find "$work_root" -path "$evidence_root" -print -quit | grep -q .; then
    printf 'check-pre-evidence-release-closure: generated output escaped into ../evidence\n' >&2
    exit 1
fi

if [[ -n "$evidence_dir" ]]; then
    mkdir -p "$evidence_dir"
    CONFLUX_RELEASE_ARTIFACTS_STAGE="$evidence_dir/$release_sku/stage" \
        "$source_root/scripts/stage-release-artifacts.sh" \
            --stage-dir "$evidence_dir/$release_sku/stage" \
            --tarball "$evidence_dir/$release_sku/conflux-${release_sku}.tar.gz" \
            --release-sku "$release_sku"
    stage_closure_evidence "$evidence_dir"
fi

printf 'check-pre-evidence-release-closure: ok (%s, %s, %s)\n' "$release_sku" "$components" "$work_root"
