// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.process;
import conflux.work;

TEST_CASE(
	"process: run echo",
	"[process]") {
	auto result = run("/bin/echo", {"hello"});
	REQUIRE(result.has_value());
	CHECK(result->exit_code == 0);
	CHECK(result->stdout_out == "hello\n");
	CHECK(result->stderr_out.empty());
}

TEST_CASE(
	"process: run exit code",
	"[process]") {
	auto result = run("/bin/sh", {"-c", "exit 42"});
	REQUIRE(result.has_value());
	CHECK(result->exit_code == 42);
}

TEST_CASE(
	"process: run captures stderr",
	"[process]") {
	auto result = run("/bin/sh", {"-c", "echo err >&2"});
	REQUIRE(result.has_value());
	CHECK(result->exit_code == 0);
	CHECK(result->stdout_out.empty());
	CHECK(result->stderr_out == "err\n");
}

TEST_CASE(
	"process: run captures large output",
	"[process]") {
	// Output > pipe buffer (64 KB) to verify poll() deadlock prevention.
	auto result = run("/bin/sh", {"-c", "dd if=/dev/zero bs=1024 count=128 2>/dev/null | tr '\\0' 'x'"});
	REQUIRE(result.has_value());
	CHECK(result->exit_code == 0);
	CHECK(result->stdout_out.size() >= 65536UZ);
}

TEST_CASE(
	"process: spawn and wait",
	"[process]") {
	SpawnOptions opts;
	auto proc = spawn("/bin/sleep", {"0"}, opts);
	REQUIRE(proc.has_value());
	CHECK(proc->pid() > 0);
	int const code = proc->wait();
	CHECK(code == 0);
}

TEST_CASE(
	"process: spawn non-existent exe fails",
	"[process]") {
	auto proc = spawn("/nonexistent/binary_xyz", {});
	CHECK(!proc.has_value());
}

TEST_CASE(
	"process: working_dir",
	"[process]") {
	SpawnOptions opts;
	opts.working_dir = "/tmp";
	auto result = run("/bin/pwd", {}, opts);
	REQUIRE(result.has_value());
	CHECK(result->exit_code == 0);
	// /tmp might be a symlink; just check it ends with "tmp\n".
	CHECK(result->stdout_out.ends_with("tmp\n"));
}

TEST_CASE(
	"process: env override",
	"[process]") {
	SpawnOptions opts;
	opts.extra_env = {"MY_TEST_VAR=hello"};
	auto result = run("/bin/sh", {"-c", "echo $MY_TEST_VAR"}, opts);
	REQUIRE(result.has_value());
	CHECK(result->stdout_out == "hello\n");
}

TEST_CASE(
	"process: clear_env",
	"[process]") {
	SpawnOptions opts;
	opts.clear_env = true;
	opts.extra_env = {"PATH=/bin:/usr/bin"};
	auto result = run("/bin/sh", {"-c", "echo ${HOME:-unset}"}, opts);
	REQUIRE(result.has_value());
	CHECK(result->stdout_out == "unset\n");
}

TEST_CASE(
	"process: stdin piped",
	"[process]") {
	SpawnOptions opts;
	opts.stdin_ = Stdio::piped();
	auto proc = spawn("/bin/cat", {}, opts);
	REQUIRE(proc.has_value());

	int const in_fd = proc->take_stdin_fd();
	S const msg = "test input\n";
	[[maybe_unused]] auto wr1 = ::write(in_fd, msg.data(), msg.size());
	::close(in_fd);

	int const code = proc->wait();
	CHECK(code == 0);
}

TEST_CASE(
	"process: fd_map",
	"[process]") {
	// Create a pipe, map read-end into child at fd 3, read it back via echo.
	int pipefd[2]{};
	REQUIRE(::pipe2(pipefd, O_CLOEXEC) == 0);

	S const msg = "fd_map_test";
	[[maybe_unused]] auto wr2 = ::write(pipefd[1], msg.data(), msg.size());
	::close(pipefd[1]);

	SpawnOptions opts;
	opts.fd_map = {
		{pipefd[0], 3}
    };
	opts.close_other_fds = false; // keep fd 3 open
	opts.stdout_ = Stdio::piped();
	auto proc = spawn("/bin/sh", {"-c", "cat /proc/self/fd/3"}, opts);
	::close(pipefd[0]); // parent no longer needs read end
	REQUIRE(proc.has_value());

	int const out_fd = proc->take_stdout_fd();
	S out;
	char buf[256]{};
	for (;;) {
		auto n = ::read(out_fd, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		out.append(buf, static_cast<SZ>(n));
	}
	::close(out_fd);
	CHECK(proc->wait() == 0);
	CHECK(out == msg);
}

TEST_CASE(
	"process: fd_map preserves same fd across exec",
	"[process]") {
	int pipefd[2]{};
	REQUIRE(::pipe2(pipefd, O_CLOEXEC) == 0);

	int const child_fd = ::fcntl(pipefd[0], F_DUPFD_CLOEXEC, 10);
	REQUIRE(child_fd >= 10);
	::close(pipefd[0]);

	S const msg = "same_fd_map_test";
	[[maybe_unused]] auto wr = ::write(pipefd[1], msg.data(), msg.size());
	::close(pipefd[1]);

	SpawnOptions opts;
	opts.fd_map = {
		{child_fd, child_fd}
    };
	opts.close_other_fds = false;
	opts.stdout_ = Stdio::piped();
	auto proc = spawn("/bin/sh", {"-c", format("cat /proc/self/fd/{}", child_fd)}, opts);
	::close(child_fd);
	REQUIRE(proc.has_value());

	int const out_fd = proc->take_stdout_fd();
	S out;
	char buf[256]{};
	for (;;) {
		auto n = ::read(out_fd, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		out.append(buf, static_cast<SZ>(n));
	}
	::close(out_fd);
	CHECK(proc->wait() == 0);
	CHECK(out == msg);
}

TEST_CASE(
	"process: try_wait returns std::nullopt while running",
	"[process]") {
	auto proc = spawn("/bin/sleep", {"5"});
	REQUIRE(proc.has_value());
	auto r = proc->try_wait();
	CHECK(!r.has_value()); // still running
	proc->terminate();
	proc->wait();
}

TEST_CASE(
	"process: new_session",
	"[process]") {
	SpawnOptions opts;
	opts.new_session = true;
	auto result = run("/bin/sh", {"-c", "echo $SHLVL"}, opts);
	REQUIRE(result.has_value());
	CHECK(result->exit_code == 0);
}

TEST_CASE(
	"process: run_in uses work pool",
	"[process]") {
	WorkPool pool;
	auto result = wait(run_in(pool, "/bin/echo", {"hello from pool"}));
	REQUIRE(result.has_value());
	CHECK(result->exit_code == 0);
	CHECK(result->stdout_out == "hello from pool\n");
	CHECK(result->stderr_out.empty());
}

TEST_CASE(
	"process: spawn_in and wait_in use work pool",
	"[process]") {
	WorkPool pool;
	auto proc = wait(spawn_in(pool, "/bin/sleep", {"0"}));
	REQUIRE(proc.has_value());
	CHECK(proc->pid() > 0);
	CHECK(wait(wait_in(pool, move(*proc))) == 0);
}

TEST_CASE(
	"process: pid returns positive for live process",
	"[process]") {
	auto proc = spawn("/bin/sleep", {"10"});
	REQUIRE(proc.has_value());
	CHECK(proc->pid() > 0);
	proc->kill_process();
	proc->wait();
}

TEST_CASE(
	"process: terminate sends SIGTERM, wait returns negative",
	"[process]") {
	auto proc = spawn("/bin/sleep", {"10"});
	REQUIRE(proc.has_value());
	proc->terminate();
	int const exit_code = proc->wait();
	CHECK(exit_code == -SIGTERM);
}

TEST_CASE(
	"process: kill_process sends SIGKILL, wait returns negative",
	"[process]") {
	auto proc = spawn("/bin/sleep", {"10"});
	REQUIRE(proc.has_value());
	proc->kill_process();
	int const exit_code = proc->wait();
	CHECK(exit_code == -SIGKILL);
}

TEST_CASE(
	"process: stdin piped writes reach child stdout",
	"[process]") {
	SpawnOptions opts;
	opts.stdin_ = Stdio::piped();
	opts.stdout_ = Stdio::piped();
	auto proc = spawn("/bin/cat", {}, opts);
	REQUIRE(proc.has_value());

	int const in_fd = proc->take_stdin_fd();
	int const out_fd = proc->take_stdout_fd();

	S const msg = "hello from parent";
	[[maybe_unused]] auto wr = ::write(in_fd, msg.data(), msg.size());
	::close(in_fd);

	S out;
	char buf[256]{};
	for (;;) {
		auto n = ::read(out_fd, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		out.append(buf, static_cast<SZ>(n));
	}
	::close(out_fd);
	CHECK(proc->wait() == 0);
	CHECK(out == msg);
}

TEST_CASE(
	"process: detach leaves zombie reaped by OS",
	"[process]") {
	auto proc = spawn("/bin/true", {});
	REQUIRE(proc.has_value());
	pid_t const pid = proc->pid();
	CHECK(pid > 0);
	proc->detach();
	CHECK(proc->pid() == -1);
	// Reap the now-detached zombie so the process table is clean.
	::waitpid(pid, nullptr, 0);
}
