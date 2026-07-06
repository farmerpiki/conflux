#!/usr/bin/env bash
set -euo pipefail

usage() {
	printf 'usage: %s [NAME=VALUE ...] <configured-preset-build-root>/<preset>/{tests,benchmarks,examples,fuzz}/<exe> [args...]\n' "$0" >&2
	printf '       %s [NAME=VALUE ...] ./build/<preset>/{tests,benchmarks,examples,fuzz}/<exe> [args...]\n' "$0" >&2
	printf '       %s [NAME=VALUE ...] /tmp/conflux/<preset>/{tests,benchmarks,examples,fuzz}/<exe> [args...]\n' "$0" >&2
	printf '       %s [NAME=VALUE ...] <configured-preset-build-root>/<preset>/conflux_<example> [args...]\n' "$0" >&2
	printf '       defaults: PG_TEST_CONNINFO=postgresql:///postgres?user=postgres, PG_CONNINFO=postgresql:///conflux_bench?user=postgres\n' >&2
}

valid_profile() {
	python3 scripts/cmake-preset-build-dir.py "$PWD" "$1" >/dev/null 2>&1
}

valid_root_example() {
	case "$1" in
		conflux_coroutines|conflux_crypto_sealing_example|conflux_custom_json_provider_example|\
			conflux_db_basic|conflux_db_pool|\
			conflux_dual|conflux_file_io_example|conflux_forms|conflux_gzip|conflux_h3_probe|\
			conflux_h3_server|conflux_hello|conflux_http_client|conflux_http_client_builder_example|\
			conflux_http_observability_example|conflux_http_policy_stack_example|\
			conflux_production_showcase_example|\
			conflux_json_config_example|conflux_json_diagnostics_example|conflux_json_example|\
			conflux_http_client_json_example|conflux_http_explicit_offload_example|\
			conflux_json_stream_ingest_example|conflux_json_transform_example|\
			conflux_advanced_postgres_json|conflux_middleware|conflux_process_run_example|conflux_quickstart_hello|\
			conflux_quickstart_json_crud|conflux_quickstart_middleware|\
			conflux_quickstart_openapi|conflux_quickstart_sse|conflux_quickstart_static_files|\
			conflux_quickstart_websocket|conflux_sse|conflux_static|\
			conflux_template_pages_example|conflux_vhost_openapi_example|conflux_work_join_all_example|\
			conflux_api_typed_json_example)
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

build_root="$PWD/build"
artifact_abs=$artifact
if [[ $artifact != /* ]]; then
	artifact_abs="$PWD/${artifact#./}"
fi
case "$artifact" in
	*/tests/*|*/benchmarks/*|*/examples/*|*/fuzz/*) ;;
	*/conflux_*)
		if ! valid_root_example "$(basename "$artifact")"; then
			printf 'refusing to run non-example root build artifact: %s\n' "$artifact" >&2
			exit 126
		fi
		;;
	*)
		case "$artifact_abs" in
			"$build_root"/*/tests/*|"$build_root"/*/benchmarks/*|"$build_root"/*/examples/*|"$build_root"/*/fuzz/*) ;;
			"$build_root"/*/conflux_*)
				if ! valid_root_example "$(basename "$artifact_abs")"; then
					printf 'refusing to run non-example root build artifact: %s\n' "$artifact" >&2
					exit 126
				fi
				;;
			*)
				printf 'refusing to run non-preset test/benchmark/example artifact: %s\n' "$artifact" >&2
				exit 126
				;;
		esac
		;;
esac

if [[ $artifact_abs == "$build_root"/* ]]; then
	profile=${artifact_abs#"$build_root"/}
elif [[ $artifact_abs == */tests/* ]]; then
	profile=${artifact_abs%/tests/*}
	profile=${profile##*/}
elif [[ $artifact_abs == */benchmarks/* ]]; then
	profile=${artifact_abs%/benchmarks/*}
	profile=${profile##*/}
elif [[ $artifact_abs == */examples/* ]]; then
	profile=${artifact_abs%/examples/*}
	profile=${profile##*/}
elif [[ $artifact_abs == */fuzz/* ]]; then
	profile=${artifact_abs%/fuzz/*}
	profile=${profile##*/}
else
	profile=$(basename "$(dirname "$artifact_abs")")
fi
profile=${profile%%/*}
if ! valid_profile "$profile"; then
	printf 'refusing artifact from unsupported build profile: %s\n' "$profile" >&2
	exit 126
fi

expected_dir="$(python3 scripts/cmake-preset-build-dir.py "$PWD" "$profile")"
tmp_build_dir="/tmp/conflux/$profile"
if [[ $artifact_abs != "$build_root/$profile/"* && $artifact_abs != "$expected_dir/"* && $artifact_abs != "$tmp_build_dir/"* ]]; then
	printf 'refusing artifact outside configured build dir for %s: %s\n' "$profile" "$artifact" >&2
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

# Keep both values present by default for libpq-based codepaths.
# Tests and benchmarks intentionally default to different DBs. Put explicit
# NAME=VALUE arguments last so one-off overrides win over these defaults.
exec env \
	PG_TEST_CONNINFO="${PG_TEST_CONNINFO:-postgresql:///conflux_test?user=postgres}" \
	PG_CONNINFO="${PG_CONNINFO:-postgresql:///conflux_bench?user=postgres}" \
	"${env_args[@]}" \
	"$artifact" "$@"
