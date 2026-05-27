#!/usr/bin/env bash
# Guardrails for GCC module CMI fragility regressions.
#
# The expensive/coroutine-heavy bodies for fragile modules must live in normal
# implementation units, not in exported module interface CMIs.  This script is a
# cheap source-shape test; it intentionally complements, not replaces, compiler
# coverage.
set -euo pipefail

script_repo_root() {
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cd "$script_dir/.." && pwd
}

SOURCE_DIR="${1:-${SOURCE_DIR:-$(script_repo_root)}}"
SOURCE_DIR="$(realpath "$SOURCE_DIR")"

failures=0

fail() {
    printf 'module-regression: %s\n' "$*" >&2
    failures=$((failures + 1))
}

require_file() {
    local path="$1"
    [[ -f "$SOURCE_DIR/$path" ]] || fail "missing required file: $path"
}

require_contains() {
    local path="$1" pattern="$2" why="$3"
    if ! grep -Eq "$pattern" "$SOURCE_DIR/$path"; then
        fail "$path: expected $why"
    fi
}

reject_contains() {
    local path="$1" pattern="$2" why="$3"
    if grep -Eq "$pattern" "$SOURCE_DIR/$path"; then
        fail "$path: must not contain $why"
    fi
}

for path in \
    CMakeLists.txt \
    src/net/cancel.cxx \
    src/net/cancel_impl.cxx \
    src/socket_io/socket_io_coro.cxx \
    src/socket_io/socket_io_coro_impl.cxx
    do
    require_file "$path"
done

# conflux.net.cancel is the known-good pattern: exported declarations only;
# state, mutex/atomic, and coroutine bodies live in cancel_impl.cxx.
require_contains src/net/cancel.cxx '^export[[:space:]]+module[[:space:]]+conflux\.net\.cancel;' \
    'the conflux.net.cancel exported module declaration'
reject_contains src/net/cancel.cxx '^[[:space:]]*module;' \
    'a global module fragment'
reject_contains src/net/cancel.cxx '^[[:space:]]*#[[:space:]]*include\b' \
    'textual includes'
reject_contains src/net/cancel.cxx '^[[:space:]]*import[[:space:]]+std(\.compat)?[[:space:]]*;' \
    'direct std module imports'
reject_contains src/net/cancel.cxx '\bco_(await|yield|return)\b' \
    'coroutine bodies'
reject_contains src/net/cancel.cxx 'std::(mutex|atomic|optional|lock_guard|unique_lock|condition_variable)' \
    'heavy std implementation state'

require_contains src/net/cancel_impl.cxx '^[[:space:]]*module;' \
    'a global module fragment for textual implementation headers'
require_contains src/net/cancel_impl.cxx '^module[[:space:]]+conflux\.net\.cancel;' \
    'the conflux.net.cancel implementation-unit declaration'
require_contains src/net/cancel_impl.cxx 'std::(mutex|atomic|optional|lock_guard)' \
    'implementation-only synchronization/state'
require_contains src/net/cancel_impl.cxx '\bco_await\b' \
    'implementation-only coroutine bodies'

# conflux.socket_io.coro is not fully declaration-only yet. It names std types in
# its exported API, so it uses explicit standard headers in the global module
# fragment instead of importing std into this public CMI. GCC 16 has ICEd while
# deserializing larger imported CMIs; keeping std out of this interface CMI was
# verified against debug-gcc16-stdcxx. Keep method bodies in the implementation
# unit so coroutine frames and synchronization state do not grow the public CMI.
require_contains src/socket_io/socket_io_coro.cxx '^export[[:space:]]+module[[:space:]]+conflux\.socket_io\.coro;' \
    'the conflux.socket_io.coro exported module declaration'
reject_contains src/socket_io/socket_io_coro.cxx '^[[:space:]]*import[[:space:]]+std(\.compat)?[[:space:]]*;' \
    'direct std module imports; use explicit standard headers here so GCC does not deserialize std through this public socket coroutine CMI'
require_contains src/socket_io/socket_io_coro_impl.cxx '^module[[:space:]]+conflux\.socket_io\.coro;' \
    'the conflux.socket_io.coro implementation-unit declaration'

if ! python3 - "$SOURCE_DIR/CMakeLists.txt" <<'PY'
from __future__ import annotations
import re
import sys
from pathlib import Path

cmake = Path(sys.argv[1]).read_text()
cmake_for_match = cmake.replace("${CONFLUX_SRC_ROOT}/", "src/")
failures: list[str] = []


def calls_for(command: str, name: str) -> list[str]:
    out: list[str] = []
    needle = f"{command}({name}"
    pos = 0
    while True:
        start = cmake_for_match.find(needle, pos)
        if start < 0:
            return out
        depth = 0
        end = start
        seen_open = False
        while end < len(cmake_for_match):
            ch = cmake_for_match[end]
            if ch == '(':
                depth += 1
                seen_open = True
            elif ch == ')':
                depth -= 1
                if seen_open and depth == 0:
                    end += 1
                    break
            end += 1
        out.append(cmake_for_match[start:end])
        pos = end


def require_private_impl(target: str, path: str) -> None:
    calls = calls_for("target_sources", target) + calls_for("conflux_add_module_library", target)
    if not calls:
        failures.append(f"CMakeLists.txt: missing source registration for {target}")
        return
    public_hits = [call for call in calls if path in call and "PUBLIC FILE_SET CXX_MODULES" in call]
    if public_hits:
        failures.append(
            f"CMakeLists.txt: {path} is in a PUBLIC CXX_MODULES file set for {target}; "
            "fragile implementation units must stay PRIVATE"
        )
    private_hits = [
        call
        for call in calls
        if path in call and (re.search(r"\bPRIVATE\b", call) or "PRIVATE_SOURCES" in call)
    ]
    if not private_hits:
        failures.append(
            f"CMakeLists.txt: {path} must be registered as a PRIVATE source for {target}"
        )


require_private_impl("conflux_net_cancel", "src/net/cancel_impl.cxx")
require_private_impl("conflux_socket_io", "src/socket_io/socket_io_coro_impl.cxx")

for msg in failures:
    print(f"module-regression: {msg}", file=sys.stderr)
sys.exit(1 if failures else 0)
PY
then
    failures=$((failures + 1))
fi

if ((failures != 0)); then
    exit 1
fi

printf 'module-regression: fragile module interface checks passed.\n'
