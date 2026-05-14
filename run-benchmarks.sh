#!/usr/bin/env bash
# Compatibility wrapper for the dedicated perf lane.
#
# Builds benchmark binaries from perf-* presets only. For measured DB-backed
# benchmark recording, call scripts/bench_record.sh directly.
set -euo pipefail

SOURCE_DIR="${SOURCE_DIR:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}"
exec "$SOURCE_DIR/scripts/run-perf-matrix.sh" "$@"
