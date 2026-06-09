#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s [--output FILE]\n' "$0" >&2
}

output=""
while (($# > 0)); do
    case "$1" in
        --output)
            if (($# < 2)); then
                usage
                exit 2
            fi
            output="$2"
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

emit_tool() {
    local name="$1"
    local purpose="$2"
    local path
    path="$(command -v "$name" 2>/dev/null || true)"
    if [[ -n "$path" ]]; then
        printf 'tool=%s\tstatus=available\tpath=%s\tpurpose=%s\n' "$name" "$path" "$purpose"
    else
        printf 'tool=%s\tstatus=missing\tpath=\tpurpose=%s\n' "$name" "$purpose"
    fi
}

write_report() {
    printf 'external_proof_tools=availability\n'
    printf 'note=missing tools mean the corresponding external proof lane is not natively attached; see docker_conformance below\n'
    printf 'docker_conformance=see conformance-docker/ in the evidence repo (h2spec+wstest+spectral via Docker)\n'
    emit_tool h2spec 'HTTP/2 protocol conformance'
    emit_tool wstest 'Autobahn WebSocket conformance'
    emit_tool spectral 'OpenAPI external validation'
    emit_tool swagger-cli 'OpenAPI external validation'
    emit_tool redocly 'OpenAPI external validation'
    emit_tool openapi-generator-cli 'OpenAPI external validation'
    emit_tool swagger-codegen 'OpenAPI external validation'
    emit_tool wrk 'HTTP benchmark client'
    emit_tool wrk2 'constant-throughput latency benchmark client'
    emit_tool h2load 'HTTP/2 benchmark client'
    emit_tool bombardier 'HTTP benchmark client'
}

if [[ -n "$output" ]]; then
    mkdir -p "$(dirname "$output")"
    write_report >"$output"
else
    write_report
fi
