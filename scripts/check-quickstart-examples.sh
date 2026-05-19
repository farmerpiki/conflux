#!/usr/bin/env bash
set -euo pipefail

root=${1:-examples/quickstart}

if grep -R -n -E 'conflux\.types|HttpRequest|HttpResponse|WorkPool|TaskSource|std::this_thread::sleep_for|json_response\(R"\(\{' "$root"; then
	printf 'quickstart examples use non-quickstart API surface\n' >&2
	exit 1
fi
