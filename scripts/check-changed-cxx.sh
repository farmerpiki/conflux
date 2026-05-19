#!/usr/bin/env bash

set -u -o pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET_ROOT="/tmp/$(basename "${ROOT_DIR}")"
CLANG_TIDY_BUILD_DIR="${PRESET_ROOT}/debug-clang-libcxx"
REPORT_ROOT="${ROOT_DIR}/build/hygiene"
TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_DIR="${REPORT_ROOT}/${TIMESTAMP}"
LATEST_LINK="${REPORT_ROOT}/latest"

mkdir -p "${RUN_DIR}"
rm -f "${LATEST_LINK}"
ln -s "${RUN_DIR}" "${LATEST_LINK}"

SUMMARY_FILE="${RUN_DIR}/summary.txt"
CHANGED_FILE="${RUN_DIR}/changed-files.txt"

touch "${SUMMARY_FILE}" "${CHANGED_FILE}"

declare -a CHANGED_CXX=()
declare -a TIDY_CXX=()
declare -A SEEN=()

while IFS= read -r -d '' path; do
	if [[ -n "${path}" && -z "${SEEN["${path}"]+x}" ]]; then
		SEEN["${path}"]=1
		CHANGED_CXX+=("${path}")
	fi
done < <(git -C "${ROOT_DIR}" diff --name-only -z --diff-filter=ACMR -- '*.cxx')

while IFS= read -r -d '' path; do
	if [[ -n "${path}" && -z "${SEEN["${path}"]+x}" ]]; then
		SEEN["${path}"]=1
		CHANGED_CXX+=("${path}")
	fi
done < <(git -C "${ROOT_DIR}" ls-files --others --exclude-standard -z -- '*.cxx')

for path in "${CHANGED_CXX[@]}"; do
	printf '%s\n' "${path}" >> "${CHANGED_FILE}"
	# clang-tidy 21 crashes in bugprone-reserved-identifier on this module unit.
	# Keep it in compiler builds/findings, but do not pass it to clang-tidy.
	if [[ "${path}" != tests/* && "${path}" != "src/net/trailing_slash.cxx" ]]; then
		TIDY_CXX+=("${path}")
	fi
done

append_section() {
	local title="$1"
	{
		printf '== %s ==\n' "${title}"
		cat
		printf '\n'
	} >> "${SUMMARY_FILE}"
}

run_logged() {
	local name="$1"
	shift
	local log_file="${RUN_DIR}/${name}.log"
	printf 'Running %s\n' "${name}"
	{
		printf '$'
		for arg in "$@"; do
			printf ' %q' "${arg}"
		done
		printf '\n\n'
	} > "${log_file}"

	(
		cd "${ROOT_DIR}"
		"$@"
	) 2>&1 | tee -a "${log_file}"
	local status=${PIPESTATUS[0]}

	printf '%s\n' "${status}" > "${RUN_DIR}/${name}.status"
	return "${status}"
}

collect_findings() {
	local source_name="$1"
	local log_file="$2"
	local findings_file="${RUN_DIR}/${source_name}.findings"

	if [[ ! -f "${log_file}" ]]; then
		return 1
	fi

	grep -nE '(^|[^A-Za-z])(warning|error|fatal error|note):|FAILED:|CMake Error|LeakSanitizer has encountered a fatal error|ninja: build stopped' \
		"${log_file}" > "${findings_file}" || true
	grep -vF 'ninja: warning: premature end of file; recovering' "${findings_file}" > "${findings_file}.tmp" || true
	mv "${findings_file}.tmp" "${findings_file}"
	if [[ -s "${findings_file}" ]]; then
		append_section "${source_name}" < "${findings_file}"
		return 0
	fi
	return 1
}

status_line() {
	local name="$1"
	local status="$2"
	printf '%-24s %s\n' "${name}" "${status}" >> "${SUMMARY_FILE}"
}

{
	printf 'Run directory: %s\n' "${RUN_DIR}"
	printf 'Repository: %s\n' "${ROOT_DIR}"
	printf '\n'
	printf 'Changed .cxx files:\n'
	if ((${#CHANGED_CXX[@]} == 0)); then
		printf '  (none)\n'
	else
		for path in "${CHANGED_CXX[@]}"; do
			printf '  %s\n' "${path}"
		done
	fi
	printf '\n'
	printf 'clang-tidy .cxx files:\n'
	if ((${#TIDY_CXX[@]} == 0)); then
		printf '  (none)\n'
	else
		for path in "${TIDY_CXX[@]}"; do
			printf '  %s\n' "${path}"
		done
	fi
	printf '\n'
	printf 'Step status:\n'
} >> "${SUMMARY_FILE}"

FAILED=0
TOTAL_FINDINGS=0

if ((${#CHANGED_CXX[@]} > 0)); then
	if run_logged clang-format clang-format -i "${CHANGED_CXX[@]}"; then
		status_line "clang-format" "ok"
	else
		status_line "clang-format" "failed"
		FAILED=1
	fi
else
	status_line "clang-format" "skipped (no changed .cxx files)"
fi

if run_logged configure-debug-gcc-stdcxx cmake --preset debug-gcc-stdcxx; then
	status_line "configure-debug-gcc" "ok"
else
	status_line "configure-debug-gcc" "failed"
	FAILED=1
fi

if run_logged build-debug-gcc-stdcxx cmake --build --preset debug-gcc-stdcxx; then
	status_line "build-debug-gcc" "ok"
else
	status_line "build-debug-gcc" "failed"
	FAILED=1
fi

if run_logged configure-debug-clang-libcxx cmake --preset debug-clang-libcxx; then
	status_line "configure-debug-clang" "ok"
else
	status_line "configure-debug-clang" "failed"
	FAILED=1
fi

if run_logged build-debug-clang-libcxx cmake --build --preset debug-clang-libcxx; then
	status_line "build-debug-clang" "ok"
else
	status_line "build-debug-clang" "failed"
	FAILED=1
fi

if ((${#TIDY_CXX[@]} > 0)); then
	if run_logged clang-tidy-fix clang-tidy -p "${CLANG_TIDY_BUILD_DIR}" --fix --format-style=file "${TIDY_CXX[@]}"; then
		status_line "clang-tidy --fix" "ok"
	else
		status_line "clang-tidy --fix" "failed"
		FAILED=1
	fi

	if run_logged clang-format-post-fix clang-format -i "${CHANGED_CXX[@]}"; then
		status_line "clang-format post-fix" "ok"
	else
		status_line "clang-format post-fix" "failed"
		FAILED=1
	fi

	if run_logged clang-tidy clang-tidy -p "${CLANG_TIDY_BUILD_DIR}" "${TIDY_CXX[@]}"; then
		status_line "clang-tidy" "ok"
	else
		status_line "clang-tidy" "failed"
		FAILED=1
	fi
else
	status_line "clang-tidy --fix" "skipped (no non-test changed .cxx files)"
	status_line "clang-format post-fix" "skipped (no non-test changed .cxx files)"
	status_line "clang-tidy" "skipped (no non-test changed .cxx files)"
fi

printf '\n' >> "${SUMMARY_FILE}"
printf 'Findings:\n\n' >> "${SUMMARY_FILE}"

for pair in \
	"gcc-build:${RUN_DIR}/build-debug-gcc-stdcxx.log" \
	"clang-build:${RUN_DIR}/build-debug-clang-libcxx.log" \
	"clang-tidy:${RUN_DIR}/clang-tidy.log"; do
	name="${pair%%:*}"
	log_file="${pair#*:}"
	if collect_findings "${name}" "${log_file}"; then
		TOTAL_FINDINGS=$((TOTAL_FINDINGS + $(wc -l < "${RUN_DIR}/${name}.findings")))
	fi
done

if ((TOTAL_FINDINGS == 0)); then
	append_section "result" <<'EOF'
No compiler or final clang-tidy findings detected.
EOF
else
	append_section "result" <<EOF
Total findings: ${TOTAL_FINDINGS}
EOF
fi

printf 'Report written to %s\n' "${SUMMARY_FILE}"

if ((FAILED != 0 || TOTAL_FINDINGS != 0)); then
	exit 1
fi

exit 0
