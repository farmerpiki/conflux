// Process example: explicit boundary between spawning errors and exit status.
//
// Demonstrates run(), SpawnOptions, captured stdout/stderr, extra environment,
// and non-zero process status without exceptions or shell-specific library APIs.
import conflux.process;
import conflux.types;
import std;

using std::println;

static void print_result(
	SV label,
	expected<RunResult, EC> const &result) {
	if (!result) {
		println("{}: spawn failed: {}", label, result.error().message());
		return;
	}

	println("{}: exit={}", label, result->exit_code);
	if (!result->stdout_out.empty()) {
		println("stdout:\n{}", result->stdout_out);
	}
	if (!result->stderr_out.empty()) {
		println("stderr:\n{}", result->stderr_out);
	}
}

int main() {
	SpawnOptions opts;
	opts.working_dir = "/tmp";
	opts.extra_env = V<S>{"CONFLUX_PROCESS_EXAMPLE=from-extra-env"};
	opts.clear_env = false;

	auto ok = run(
		fs::path{"/bin/sh"},
		V<SV>{
			"-c",
			"printf 'cwd=%s\\n' \"$PWD\"; "
			"printf 'env=%s\\n' \"$CONFLUX_PROCESS_EXAMPLE\"; "
			"printf 'diagnostic on stderr\\n' >&2",
		},
		opts);
	print_result("successful child", ok);

	// Non-zero exit is part of RunResult, not a spawn error.
	auto non_zero = run(fs::path{"/bin/sh"}, V<SV>{"-c", "printf 'no crash, just status\\n'; exit 7"});
	print_result("non-zero child", non_zero);

	// Missing executable is a spawn/exec boundary error.
	auto missing = run(fs::path{"/definitely/not/a/conflux/example/binary"}, V<SV>{});
	print_result("missing executable", missing);
}
