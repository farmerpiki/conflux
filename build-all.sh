#!/usr/bin/env bash
set -euo pipefail

PRESETS=(
    debug-clang-libcxx
    debug-gcc-stdcxx
    release-clang-libcxx
    release-gcc-stdcxx
    tsan-clang-libcxx
    tsan-gcc-stdcxx
)

PASS=()
FAIL=()

ln -svf /tmp/$(basename $PWD)/debug-clang-libcxx $PWD/build

for preset in "${PRESETS[@]}"; do
    echo "━━━ $preset ━━━"
    if cmake --preset "$preset" && cmake --build --preset "$preset"; then
        PASS+=("$preset")
    else
        FAIL+=("$preset")
    fi
done

echo ""
echo "━━━ results ━━━"
for p in "${PASS[@]+"${PASS[@]}"}"; do echo "  PASS  $p"; done
for f in "${FAIL[@]+"${FAIL[@]}"}"; do echo "  FAIL  $f"; done

[[ ${#FAIL[@]} -eq 0 ]]
