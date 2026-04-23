#!/usr/bin/env bash
set -euo pipefail

usage() {
	printf 'usage: %s [NAME=VALUE ...] ./build/{debug,release,tsan}-{clang-libcxx,gcc-stdcxx}/{tests,benchmarks,examples}/<exe> [args...]\n' "$0" >&2
	printf '       %s [NAME=VALUE ...] ./build/{debug,release,tsan}-{clang-libcxx,gcc-stdcxx}/conflux_<example> [args...]\n' "$0" >&2
}

valid_profile() {
	case "$1" in
		debug-clang-libcxx|debug-gcc-stdcxx|release-clang-libcxx|release-gcc-stdcxx|tsan-clang-libcxx|tsan-gcc-stdcxx)
			return 0
			;;
		*)
			return 1
			;;
	esac
}

valid_root_example() {
	case "$1" in
		conflux_coroutines|conflux_db_basic|conflux_db_pool|conflux_dual|conflux_file_io_example|\
			conflux_forms|conflux_gzip|conflux_h3_probe|conflux_h3_server|conflux_hello|\
			conflux_http_client|conflux_middleware|conflux_sse|conflux_static)
			return 0
			;;
		*)
			return 1
			;;
	esac
}

env_args=()
while (($# > 0)); do
	case "$1" in
		*=*)
			name=${1%%=*}
			if [[ ! $name =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
				usage
				exit 2
			fi
			env_args+=("$1")
			shift
			;;
		*)
			break
			;;
	esac
done

if (($# < 1)); then
	usage
	exit 2
fi

artifact=$1
shift

case "$artifact" in
	./build/*/tests/*|./build/*/benchmarks/*|./build/*/examples/*) ;;
	build/*/tests/*|build/*/benchmarks/*|build/*/examples/*) artifact="./$artifact" ;;
	./build/*/conflux_*|build/*/conflux_*)
		artifact=${artifact#./}
		if ! valid_root_example "$(basename "$artifact")"; then
			printf 'refusing to run non-example root build artifact: %s\n' "$artifact" >&2
			exit 126
		fi
		artifact="./$artifact"
		;;
	*)
		printf 'refusing to run non-build test/benchmark/example artifact: %s\n' "$artifact" >&2
		exit 126
		;;
esac

profile=${artifact#./build/}
profile=${profile%%/*}
if ! valid_profile "$profile"; then
	printf 'refusing artifact from unsupported build profile: %s\n' "$profile" >&2
	exit 126
fi

if [[ "$artifact" == *"/.."* || "$artifact" == *"//"* ]]; then
	printf 'refusing suspicious artifact path: %s\n' "$artifact" >&2
	exit 126
fi

if [[ ! -x "$artifact" || -d "$artifact" ]]; then
	printf 'artifact is not an executable file: %s\n' "$artifact" >&2
	exit 126
fi

exec env "${env_args[@]}" "$artifact" "$@"
