#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

rg -n \
    --glob '!tests/**' \
    --glob '!benchmarks/**' \
    --glob '!build/**' \
    --glob '!cmake-build-*/**' \
    '\(void\)|\bauto[[:space:]]+_[[:space:]]*=' \
    src examples fuzz "$@"
