#!/usr/bin/env bash
set -euo pipefail

usage() {
	printf 'usage: %s [NAME=VALUE ...] --test-dir build/{debug,release,tsan}-{clang-libcxx,gcc-stdcxx} [ctest args...]\n' "$0" >&2
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

if (($# < 2)) || [[ "$1" != "--test-dir" ]]; then
	usage
	exit 2
fi

test_dir=$2
shift 2

case "$test_dir" in
	./build/*|build/*) ;;
	*)
		printf 'refusing non-build test dir: %s\n' "$test_dir" >&2
		exit 126
		;;
esac

test_dir=${test_dir#./}
profile=${test_dir#build/}
profile=${profile%%/*}

if [[ "$test_dir" != "build/$profile" ]]; then
	printf 'refusing nested test dir: %s\n' "$test_dir" >&2
	exit 126
fi

if ! valid_profile "$profile"; then
	printf 'refusing unsupported build profile: %s\n' "$profile" >&2
	exit 126
fi

if [[ ! -d "$test_dir" ]]; then
	printf 'test dir does not exist: %s\n' "$test_dir" >&2
	exit 126
fi

# Keep both values present by default for libpq-based codepaths.
# Tests and benchmarks intentionally default to different DBs.
: "${PG_TEST_CONNINFO:=postgresql:///postgres?user=postgres}"
: "${PG_CONNINFO:=postgresql:///conflux_bench?user=postgres}"

exec env "${env_args[@]}" \
	PG_TEST_CONNINFO="$PG_TEST_CONNINFO" \
	PG_CONNINFO="$PG_CONNINFO" \
	ctest --test-dir "$test_dir" "$@"
