#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
base="${TMPDIR:-/tmp}/$(basename "$root")-provider-policy-$$"
empty_pc_dir="$base/empty-pkgconfig"
no_argon2_bin="$base/no-argon2-bin"
mkdir -p "$empty_pc_dir"
trap 'rm -rf "$base"' EXIT

run_configure() {
	local name="$1"
	shift
	local build_dir="$base/$name"
	printf 'provider-policy: configure %s\n' "$name" >&2
	cmake -S "$root" -B "$build_dir" -G Ninja "$@" >&2
	printf '%s\n' "$build_dir"
}

run_configure_no_system_pc() {
	local name="$1"
	shift
	local build_dir="$base/$name"
	printf 'provider-policy: configure %s (empty pkg-config path)\n' "$name" >&2
	PKG_CONFIG_LIBDIR="$empty_pc_dir" PKG_CONFIG_PATH= \
		cmake -S "$root" -B "$build_dir" -G Ninja "$@" >&2
	printf '%s\n' "$build_dir"
}

run_configure_no_argon2_pc() {
	local name="$1"
	shift
	local build_dir="$base/$name"
	mkdir -p "$no_argon2_bin"
	local wrapper="$no_argon2_bin/pkg-config"
	if [[ ! -x "$wrapper" ]]; then
		printf '%s\n' \
			'#!/usr/bin/env bash' \
			'for arg in "$@"; do' \
			'	if [[ "$arg" == "libargon2" ]]; then' \
			'		exit 1' \
			'	fi' \
			'done' \
			'exec "${CONFLUX_REAL_PKG_CONFIG:?}" "$@"' >"$wrapper"
		chmod +x "$wrapper"
	fi
	printf 'provider-policy: configure %s (pkg-config without libargon2)\n' "$name" >&2
	CONFLUX_REAL_PKG_CONFIG="$(command -v pkg-config)" PATH="$no_argon2_bin:$PATH" \
		cmake -S "$root" -B "$build_dir" -G Ninja "$@" >&2
	printf '%s\n' "$build_dir"
}

module_probe_dir="$base/module-interface-probe"
module_probe_log="$base/module-interface-probe.log"
if ! cmake -S "$root" -B "$module_probe_dir" -G Ninja \
	-DCONFLUX_FEATURE_SET=core \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF >"$module_probe_log" 2>&1; then
	if grep -q "requires CMake-discoverable C++23 import std support" "$module_probe_log"; then
		printf 'provider-policy: skip; MODULE_INTERFACE import std unsupported by this toolchain\n'
		exit 0
	fi
	cat "$module_probe_log" >&2
	exit 1
fi
rm -rf "$module_probe_dir" "$module_probe_log"

run_build() {
	local build_dir="$1"
	shift
	printf 'provider-policy: build %s %s\n' "$(basename "$build_dir")" "$*"
	cmake --build "$build_dir" "$@"
}

cleanup_build() {
	local build_dir="$1"
	printf 'provider-policy: clean %s\n' "$(basename "$build_dir")"
	rm -rf "$build_dir"
}

core_dir="$(run_configure core \
	-DCONFLUX_FEATURE_SET=core \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF)"
run_build "$core_dir"
if grep -q '^PkgConfig_DIR:' "$core_dir/CMakeCache.txt"; then
	printf 'provider-policy: core unexpectedly resolved pkg-config\n' >&2
	exit 1
fi
cleanup_build "$core_dir"

json_internal_dir="$(run_configure json-internal \
	-DCONFLUX_FEATURE_SET=json \
	-DCONFLUX_JSON_HASH_PROVIDER=INTERNAL \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF)"
run_build "$json_internal_dir" --target conflux_json
if grep -q '^PkgConfig_DIR:' "$json_internal_dir/CMakeCache.txt"; then
	printf 'provider-policy: json INTERNAL unexpectedly resolved pkg-config\n' >&2
	exit 1
fi
cleanup_build "$json_internal_dir"

json_dir="$(run_configure json-auto \
	-DCONFLUX_FEATURE_SET=json \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF)"
run_build "$json_dir" --target conflux_json
cleanup_build "$json_dir"

json_no_xxhash_dir="$(run_configure_no_system_pc json-auto-no-xxhash \
	-DCONFLUX_FEATURE_SET=json \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF)"
run_build "$json_no_xxhash_dir" --target conflux_json
if ! grep -q '^CONFLUX_JSON_HASH_PROVIDER_UPPER:INTERNAL=INTERNAL' "$json_no_xxhash_dir/CMakeCache.txt"; then
	printf 'provider-policy: JSON AUTO did not fall back to internal hash without xxhash\n' >&2
	exit 1
fi
cleanup_build "$json_no_xxhash_dir"

http_api_no_compression_dir="$(run_configure http-api-no-compression \
	-DCONFLUX_FEATURE_SET=http-api \
	-DCONFLUX_GZIP_PROVIDER=OFF \
	-DCONFLUX_BROTLI_PROVIDER=OFF \
	-DCONFLUX_ZSTD_PROVIDER=OFF \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF)"
run_build "$http_api_no_compression_dir" --target conflux_http_auth
if cmake --build "$http_api_no_compression_dir" --target help | grep -q '^... conflux_http_compression:'; then
	printf 'provider-policy: http-api no-compression scenario exposed compression target\n' >&2
	exit 1
fi
cleanup_build "$http_api_no_compression_dir"

web_dir="$(run_configure web-auto \
	-DCONFLUX_FEATURE_SET=web-server \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF)"
run_build "$web_dir" --target conflux_http_compression
cleanup_build "$web_dir"

web_all_dir="$(run_configure web-gzip-all \
	-DCONFLUX_FEATURE_SET=web-server \
	-DCONFLUX_GZIP_PROVIDER=ALL \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF)"
run_build "$web_all_dir" --target conflux_http_compression
cleanup_build "$web_all_dir"

http3_off_dir="$(run_configure http3-off \
	-DCONFLUX_FEATURE_SET=http-server-complete \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF)"
if cmake --build "$http3_off_dir" --target help | grep -q '^... conflux_http3:'; then
	printf 'provider-policy: HTTP/3 target exists without experimental gate\n' >&2
	exit 1
fi
cleanup_build "$http3_off_dir"

auth_runtime_dir="$(run_configure auth-runtime \
	-DCONFLUX_FEATURE_SET=http-api \
	-DCONFLUX_PASSWORD_HASH_ARGON2_PROVIDER=RUNTIME \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF)"
run_build "$auth_runtime_dir" --target conflux_http_auth
cleanup_build "$auth_runtime_dir"

auth_auto_no_argon2_dir="$(run_configure_no_argon2_pc auth-auto-no-argon2 \
	-DCONFLUX_FEATURE_SET=http-api \
	-DCONFLUX_PASSWORD_HASH_ARGON2_PROVIDER=AUTO \
	-DCONFLUX_BUILD_TESTS=OFF \
	-DCONFLUX_BUILD_EXAMPLES=OFF \
	-DCONFLUX_BUILD_BENCHMARKS=OFF \
	-DCONFLUX_FETCH_TEST_DEPS=OFF)"
run_build "$auth_auto_no_argon2_dir" --target conflux_http_auth
cleanup_build "$auth_auto_no_argon2_dir"

if pkg-config --exists libargon2; then
	auth_auto_system_dir="$(run_configure auth-auto-system-argon2 \
		-DCONFLUX_FEATURE_SET=http-api \
		-DCONFLUX_PASSWORD_HASH_ARGON2_PROVIDER=AUTO \
		-DCONFLUX_BUILD_TESTS=OFF \
		-DCONFLUX_BUILD_EXAMPLES=OFF \
		-DCONFLUX_BUILD_BENCHMARKS=OFF \
		-DCONFLUX_FETCH_TEST_DEPS=OFF)"
	run_build "$auth_auto_system_dir" --target conflux_http_auth
	cleanup_build "$auth_auto_system_dir"
fi

if pkg-config --exists libngtcp2 libngtcp2_crypto_ossl libnghttp3; then
	dev_exp_h3_dir="$(run_configure dev-exp-http3 \
		-DCONFLUX_FEATURE_SET=dev-exp-all \
		-DCONFLUX_BUILD_TESTS=OFF \
		-DCONFLUX_BUILD_EXAMPLES=OFF \
		-DCONFLUX_BUILD_BENCHMARKS=OFF \
		-DCONFLUX_FETCH_TEST_DEPS=OFF)"
	run_build "$dev_exp_h3_dir" --target conflux_http3
	cleanup_build "$dev_exp_h3_dir"
else
	printf 'provider-policy: skip dev-exp HTTP/3 build; HTTP/3 pkg-config deps missing\n'
fi

printf 'provider-policy: ok\n'
