// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
module;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h> // NOLINT(modernize-deprecated-headers)
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
export module conflux.process;
import std;
import conflux.types;
import conflux.work;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ---------------------------------------------------------------------------
// Stdio — describes how stdin/stdout/stderr is connected in the child process.
// ---------------------------------------------------------------------------

export struct Stdio {
	// NOLINTNEXTLINE(performance-enum-size)
	enum class Kind : std::uint8_t {
		Inherit,
		Piped,
		Null,
		Fd,
	} kind{Kind::Inherit};
	int fd{-1};
	[[nodiscard]] static Stdio inherit() noexcept { return {.kind = Kind::Inherit}; }
	[[nodiscard]] static Stdio piped() noexcept { return {.kind = Kind::Piped}; }
	[[nodiscard]] static Stdio null() noexcept { return {.kind = Kind::Null}; }
	[[nodiscard]] static Stdio from_fd(
		int f) noexcept {
		return {.kind = Kind::Fd, .fd = f};
	}
};
// ---------------------------------------------------------------------------
// SpawnOptions
// ---------------------------------------------------------------------------

export struct SpawnOptions {
	std::filesystem::path working_dir{};
	std::vector<std::string> extra_env{}; // "KEY=VALUE" entries; add or override
	bool clear_env{false};
	Stdio stdin_{Stdio::inherit()};
	Stdio stdout_{Stdio::inherit()};
	Stdio stderr_{Stdio::inherit()};
	bool new_session{false}; // setsid() in child
	bool close_other_fds{true}; // close_range(3, ~0U, 0) after fd_map
	int dup3_flags{0}; // flags passed to dup3() (e.g. O_CLOEXEC)
	// Extra fd mappings: {parent_fd, child_fd}. Applied after stdio, before close_range.
	// close_range will close parent_fd originals if close_other_fds=true.
	std::vector<std::pair<int, int>> fd_map{};
	// Called in child after fd setup but before exec. Must use only async-signal-safe
	// functions (setrlimit, setsockopt, etc.). Not called if null.
	void (*pre_exec_fn)() = nullptr;
};
// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------

export class Process {
	pid_t pid_{-1};
	int stdin_fd_{-1};
	int stdout_fd_{-1};
	int stderr_fd_{-1};
	void close_pipes() noexcept {
		if (stdin_fd_ >= 0) {
			::close(stdin_fd_);
			stdin_fd_ = -1;
		}
		if (stdout_fd_ >= 0) {
			::close(stdout_fd_);
			stdout_fd_ = -1;
		}
		if (stderr_fd_ >= 0) {
			::close(stderr_fd_);
			stderr_fd_ = -1;
		}
	}

public:
	Process() = default;
	Process(
		pid_t pid,
		int in_fd,
		int out_fd,
		int err_fd) noexcept
		: pid_{pid}
		, stdin_fd_{in_fd}
		, stdout_fd_{out_fd}
		, stderr_fd_{err_fd} {}
	Process(
		Process &&o) noexcept
		: pid_{std::exchange(o.pid_, -1)}
		, stdin_fd_{std::exchange(o.stdin_fd_, -1)}
		, stdout_fd_{std::exchange(o.stdout_fd_, -1)}
		, stderr_fd_{std::exchange(o.stderr_fd_, -1)} {}
	Process &operator =(
		Process &&o) noexcept {
		if (this != &o) {
			close_pipes();
			pid_ = std::exchange(o.pid_, -1);
			stdin_fd_ = std::exchange(o.stdin_fd_, -1);
			stdout_fd_ = std::exchange(o.stdout_fd_, -1);
			stderr_fd_ = std::exchange(o.stderr_fd_, -1);
		}
		return *this;
	}
	Process(Process const &) = delete;
	Process &operator =(Process const &) = delete;
	// Destructor closes pipe fds.  Does NOT wait() — zombie reaping is caller's job.
	~Process() { close_pipes(); }
	[[nodiscard]] pid_t pid() const noexcept { return pid_; }
	// Wait for process exit.  Returns: WEXITSTATUS on normal exit, -(signal) on signal kill.
	// Loops on EINTR.
	int wait() noexcept {
		if (pid_ < 0) {
			return -1;
		}
		int status = 0;
		pid_t r = 0;
		do {
			r = ::waitpid(pid_, &status, 0);
		} while (r < 0 && errno == EINTR);
		pid_ = -1;
		if (WIFEXITED(status)) {
			return WEXITSTATUS(status);
		}
		if (WIFSIGNALED(status)) {
			return -WTERMSIG(status);
		}
		return -1;
	}
	// Non-blocking wait.  Returns std::nullopt if still running.
	[[nodiscard]] std::optional<int> try_wait() noexcept {
		if (pid_ < 0) {
			return std::nullopt;
		}
		int status = 0;
		pid_t r = 0;
		do {
			r = ::waitpid(pid_, &status, WNOHANG);
		} while (r < 0 && errno == EINTR);
		if (r == 0) {
			return std::nullopt;
		}
		if (r < 0) {
			return std::nullopt;
		}
		pid_ = -1;
		if (WIFEXITED(status)) {
			return WEXITSTATUS(status);
		}
		if (WIFSIGNALED(status)) {
			return -WTERMSIG(status);
		}
		return -1;
	}
	void send_signal(
		int sig) const noexcept {
		if (pid_ > 0) {
			::kill(pid_, sig);
		}
	}
	void terminate() const noexcept { send_signal(SIGTERM); }
	void kill_process() const noexcept { send_signal(SIGKILL); }
	// Detach: caller takes responsibility for reaping the zombie.
	void detach() noexcept { pid_ = -1; }
	[[nodiscard]] int stdin_fd() const noexcept { return stdin_fd_; }
	[[nodiscard]] int stdout_fd() const noexcept { return stdout_fd_; }
	[[nodiscard]] int stderr_fd() const noexcept { return stderr_fd_; }
	// Transfer ownership — caller must ::close() the returned fd.
	int take_stdin_fd() noexcept { return std::exchange(stdin_fd_, -1); }
	int take_stdout_fd() noexcept { return std::exchange(stdout_fd_, -1); }
	int take_stderr_fd() noexcept { return std::exchange(stderr_fd_, -1); }
};
export struct RunResult {
	int exit_code{};
	std::string stdout_out{};
	std::string stderr_out{};
};
// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Build envp from environ (unless clear_env) + extra_env overrides.
std::vector<std::string> build_env(
	std::vector<std::string> const &extra_env,
	bool clear_env) {
	std::vector<std::string> env_strs;
	if (!clear_env) {
		// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		for (char *const *e = environ; *e != nullptr; ++e) {
			env_strs.emplace_back(*e);
		}
		// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
	}
	for (auto const &entry: extra_env) {
		// Strip any existing entry with the same KEY= prefix.
		auto const eq_pos = entry.find('=');
		if (eq_pos == std::string::npos) {
			continue;
		}
		std::string_view key{entry.data(), eq_pos + 1}; // includes '='
		auto it = remove_if(env_strs.begin(), env_strs.end(), [&](std::string const &s) {
			return std::string_view{s}.substr(0, key.size()) == key;
		});
		env_strs.erase(it, env_strs.end());
		env_strs.push_back(entry);
	}
	return env_strs;
}
// Prepare stdio fd for child: returns the fd to dup3 into 0/1/2, or -1 for inherit, or -2 for /dev/null.
// pipe_fds[2]: if kind==Piped, filled with pipe2(); returns pipe_fds[read_end] for stdin, [write_end] for out/err.
// parent_fd: set to the parent's end of the pipe.
// NOLINTBEGIN(modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-pointer-arithmetic)
int setup_stdio(
	Stdio const &s,
	bool is_stdin,
	int pipe_fds[2],
	int &parent_fd) {
	switch (s.kind) {
	case Stdio::Kind::Inherit: return -1;
	case Stdio::Kind::Null   : return -2;
	case Stdio::Kind::Fd     : return s.fd;
	case Stdio::Kind::Piped:
		if (::pipe2(pipe_fds, O_CLOEXEC) < 0) {
			return -3;
		}
		if (is_stdin) {
			parent_fd = pipe_fds[1];
			return pipe_fds[0];
		}
		parent_fd = pipe_fds[0];
		return pipe_fds[1];
	}
	return -1;
}
// NOLINTEND(modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-pointer-arithmetic)

// Child-side: set up one stdio fd (called between fork/exec; async-signal-safe only).
// Returns false on error.
bool child_setup_fd(
	int src_fd,
	int dst_fd,
	int flags) noexcept {
	if (src_fd == -1) {
		return true;
	} // inherit
	if (src_fd == -2) {
		int const null_fd =
			::open("/dev/null", O_RDWR | O_CLOEXEC); // NOLINT(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
		if (null_fd < 0) {
			return false;
		}
		if (null_fd != dst_fd) {
			if (::dup3(null_fd, dst_fd, flags) < 0) {
				::close(null_fd);
				return false;
			}
			::close(null_fd);
		}
		return true;
	}
	if (src_fd == dst_fd) {
		// Mirror dup3 flags for same-fd mappings.
		::fcntl(dst_fd, F_SETFD, (flags & O_CLOEXEC) != 0 ? FD_CLOEXEC : 0);
		return true;
	}
	return ::dup3(src_fd, dst_fd, flags) >= 0;
}

} // namespace
namespace {

constexpr int kExecFailed = 127;

} // namespace
// ---------------------------------------------------------------------------
// spawn_clone — core implementation
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
export std::expected<Process, std::error_code> spawn_clone(
	std::filesystem::path const &exe,
	std::vector<std::string_view> const &args,
	SpawnOptions const &opts,
	std::uint64_t clone_flags) {
	// Copy std::string_view args → std::vector<std::string> before fork (views may be into caller's stack).
	std::vector<std::string> arg_strs;
	arg_strs.reserve(args.size() + 1);
	arg_strs.emplace_back(exe.string());
	for (auto const &a: args) {
		arg_strs.emplace_back(a);
	}

	// Build argv and envp now (no alloc after fork in child).
	std::vector<char *> argv_ptrs;
	argv_ptrs.reserve(arg_strs.size() + 1);
	for (auto &s: arg_strs) {
		argv_ptrs.push_back(s.data());
	}
	argv_ptrs.push_back(nullptr);

	auto env_strs = build_env(opts.extra_env, opts.clear_env);
	std::vector<char *> envp_ptrs;
	envp_ptrs.reserve(env_strs.size() + 1);
	for (auto &s: env_strs) {
		envp_ptrs.push_back(s.data());
	}
	envp_ptrs.push_back(nullptr);

	// Set up stdio pipes.
	int in_pipe[2]{-1, -1}; // NOLINT(modernize-avoid-c-arrays)
	int out_pipe[2]{-1, -1}; // NOLINT(modernize-avoid-c-arrays)
	int err_pipe[2]{-1, -1}; // NOLINT(modernize-avoid-c-arrays)
	int parent_in = -1;
	int parent_out = -1;
	int parent_err = -1;

	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-A-to-pointer-decay,hicpp-no-A-decay)
	int const child_in = setup_stdio(opts.stdin_, true, in_pipe, parent_in);
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-A-to-pointer-decay,hicpp-no-A-decay)
	int const child_out = setup_stdio(opts.stdout_, false, out_pipe, parent_out);
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-A-to-pointer-decay,hicpp-no-A-decay)
	int const child_err = setup_stdio(opts.stderr_, false, err_pipe, parent_err);

	auto close_stdio_pipes = [&] {
		// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-A-index)
		for (auto *pipe: {&in_pipe, &out_pipe, &err_pipe}) {
			if ((*pipe)[0] >= 0) {
				::close((*pipe)[0]);
				::close((*pipe)[1]);
			}
		}
		// NOLINTEND(cppcoreguidelines-pro-bounds-constant-A-index)
	};

	if (child_in == -3 || child_out == -3 || child_err == -3) {
		close_stdio_pipes();
		return std::unexpected{
			std::error_code{errno, std::system_category()}
        };
	}

	// Error-reporting pipe: child writes errno if exec fails; parent detects success via EOF.
	int exec_err_pipe[2]{-1, -1}; // NOLINT(modernize-avoid-c-arrays)
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-A-to-pointer-decay,hicpp-no-A-decay)
	if (::pipe2(exec_err_pipe, O_CLOEXEC) < 0) {
		close_stdio_pipes();
		return std::unexpected{
			std::error_code{errno, std::system_category()}
        };
	}

	// Fork.
	// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise,google-runtime-int)
	auto const pid = static_cast<pid_t>(
		::syscall(SYS_clone, static_cast<long>(SIGCHLD | clone_flags), nullptr, nullptr, nullptr, 0L));
	// NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise,google-runtime-int)
	if (pid < 0) {
		int const err = errno;
		close_stdio_pipes();
		::close(exec_err_pipe[0]);
		::close(exec_err_pipe[1]);
		return std::unexpected{
			std::error_code{err, std::system_category()}
        };
	}

	if (pid == 0) {
		// ---- CHILD ----
		// Only async-signal-safe ops from here to exec.

		// Session.
		if (opts.new_session) {
			::setsid();
		}

		// Working directory.
		if (!opts.working_dir.empty()) {
			if (::chdir(opts.working_dir.c_str()) < 0) {
				int err = errno;
				[[maybe_unused]] auto wr = ::write(exec_err_pipe[1], reinterpret_cast<char const *>(&err), sizeof(int));
				::_exit(kExecFailed);
			}
		}

		// dup3 stdin/stdout/stderr.  All three before closing originals in case a pipe
		// fd happens to land on 0/1/2.
		if (!child_setup_fd(child_in, STDIN_FILENO, opts.dup3_flags)
			|| !child_setup_fd(child_out, STDOUT_FILENO, opts.dup3_flags)
			|| !child_setup_fd(child_err, STDERR_FILENO, opts.dup3_flags)) {
			int err = errno;
			[[maybe_unused]] auto wr = ::write(exec_err_pipe[1], reinterpret_cast<char const *>(&err), sizeof(int));
			::_exit(kExecFailed);
		}

		// fd_map: explicit fd mappings.
		for (auto const &[src, dst]: opts.fd_map) {
			if (src == dst) {
				if (::fcntl(dst, F_SETFD, (opts.dup3_flags & O_CLOEXEC) != 0 ? FD_CLOEXEC : 0) < 0) {
					int err = errno;
					[[maybe_unused]] auto wr =
						::write(exec_err_pipe[1], reinterpret_cast<char const *>(&err), sizeof(int));
					::_exit(kExecFailed);
				}
				continue;
			}
			if (::dup3(src, dst, opts.dup3_flags) < 0) {
				int err = errno;
				[[maybe_unused]] auto wr = ::write(exec_err_pipe[1], reinterpret_cast<char const *>(&err), sizeof(int));
				::_exit(kExecFailed);
			}
		}

		// Close all inherited fds >= 3, preserving exec_err_pipe[1] so exec failure can be reported.
		// exec_err_pipe[1] has O_CLOEXEC — it is closed automatically on successful exec.
		if (opts.close_other_fds) {
			auto const ep = static_cast<unsigned long>(exec_err_pipe[1]);
			// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
			if (ep > 3UL) {
				::syscall(SYS_close_range, 3UL, ep - 1UL, 0UL);
			}
			::syscall(SYS_close_range, ep + 1UL, ~0U, 0UL);
			// NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
		}

		// Pre-exec hook (async-signal-safe ops only: setrlimit, prctl, etc.).
		if (opts.pre_exec_fn != nullptr) {
			opts.pre_exec_fn();
		}

		// exec.
		::execvpe(exe.c_str(), argv_ptrs.data(), envp_ptrs.data());

		// exec failed — report errno.
		int err = errno;
		[[maybe_unused]] auto wr = ::write(exec_err_pipe[1], reinterpret_cast<char const *>(&err), sizeof(int));
		::_exit(kExecFailed);
	}

	// ---- PARENT ----

	// Close child-side pipe ends.
	if (in_pipe[0] >= 0) {
		::close(in_pipe[0]);
	}
	if (out_pipe[1] >= 0) {
		::close(out_pipe[1]);
	}
	if (err_pipe[1] >= 0) {
		::close(err_pipe[1]);
	}
	::close(exec_err_pipe[1]);

	// Check if exec succeeded.
	int child_errno = 0;
	ssize_t n = 0;
	do {
		n = ::read(exec_err_pipe[0], reinterpret_cast<char *>(&child_errno), sizeof(int));
	} while (n < 0 && errno == EINTR);
	::close(exec_err_pipe[0]);

	if (n == static_cast<ssize_t>(sizeof(int))) {
		// exec failed — reap the child and return the error.
		::waitpid(pid, nullptr, 0);
		if (parent_in >= 0) {
			::close(parent_in);
		}
		if (parent_out >= 0) {
			::close(parent_out);
		}
		if (parent_err >= 0) {
			::close(parent_err);
		}
		return std::unexpected{
			std::error_code{child_errno, std::system_category()}
        };
	}

	return Process{pid, parent_in, parent_out, parent_err};
}
// ---------------------------------------------------------------------------
// spawn — convenience wrapper with default clone flags
// ---------------------------------------------------------------------------

export std::expected<Process, std::error_code> spawn(
	std::filesystem::path const &exe,
	std::vector<std::string_view> const &args,
	SpawnOptions const &opts = {}) {
	return spawn_clone(exe, args, opts, 0);
}
export template<typename Target>
[[nodiscard]] auto async_spawn_in(
	Target &target,
	std::filesystem::path exe,
	std::vector<std::string> args,
	SpawnOptions opts = {}) -> conflux::work::root::Task<std::expected<Process, std::error_code>> {
	return async_run_on(target, [exe = std::move(exe), args = std::move(args), opts = std::move(opts)]() mutable {
		std::vector<std::string_view> views;
		views.reserve(args.size());
		for (auto const &a: args) {
			views.push_back(a);
		}
		return spawn(exe, views, opts);
	});
}

export template<typename Target>
[[nodiscard]] auto spawn_async_in(
	Target &target,
	std::filesystem::path exe,
	std::vector<std::string> args,
	SpawnOptions opts = {}) -> conflux::work::root::Task<std::expected<Process, std::error_code>> {
	return async_spawn_in(target, std::move(exe), std::move(args), std::move(opts));
}
// ---------------------------------------------------------------------------
// run — spawn + drain stdout/stderr + wait
// ---------------------------------------------------------------------------

export std::expected<RunResult, std::error_code> run(
	std::filesystem::path const &exe,
	std::vector<std::string_view> const &args,
	SpawnOptions opts = {}) {
	opts.stdout_ = Stdio::piped();
	opts.stderr_ = Stdio::piped();

	auto proc = spawn(exe, args, opts);
	if (!proc) {
		return std::unexpected{proc.error()};
	}

	int const out_fd = proc->take_stdout_fd();
	int const err_fd = proc->take_stderr_fd();

	RunResult result;

	// Poll loop: drain stdout and stderr concurrently to prevent deadlock.
	std::array<pollfd, 2> pfds{};
	pfds[0] = {.fd = out_fd, .events = POLLIN, .revents = 0};
	pfds[1] = {.fd = err_fd, .events = POLLIN, .revents = 0};

	while (pfds[0].fd >= 0 || pfds[1].fd >= 0) {
		// Build active pollfd A (skip closed fds).
		std::array<pollfd, 2> active{};
		int n_active = 0; // NOLINT(misc-const-correctness) — incremented in loop
		for (auto const &pfd: pfds) {
			if (pfd.fd >= 0) {
				active[static_cast<std::size_t>(n_active++)] =
					pfd; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			}
		}

		int const r = ::poll(active.data(), static_cast<nfds_t>(n_active), -1);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r < 0) {
			int const poll_err = errno;
			if (pfds[0].fd >= 0) {
				::close(pfds[0].fd);
				pfds[0].fd = -1;
			}
			if (pfds[1].fd >= 0) {
				::close(pfds[1].fd);
				pfds[1].fd = -1;
			}
			return std::unexpected{
				std::error_code{poll_err, std::system_category()}
            };
		}

		for (auto const &a: active) {
			if (a.revents == 0) {
				continue;
			}
			std::string &buf = (a.fd == out_fd) ? result.stdout_out : result.stderr_out;
			if ((a.revents & POLLIN) != 0) {
				char tmp[4096]; // NOLINT(modernize-avoid-c-arrays,readability-magic-numbers)
				ssize_t nr = 0;
				do {
					// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-A-to-pointer-decay,hicpp-no-A-decay)
					nr = ::read(a.fd, tmp, sizeof(tmp));
				} while (nr < 0 && errno == EINTR);
				if (nr > 0) {
					// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-A-to-pointer-decay,hicpp-no-A-decay)
					buf.append(tmp, static_cast<std::size_t>(nr));
				}
			}
			if ((a.revents & (POLLHUP | POLLERR)) != 0) {
				// Close this fd in the pfds tracking A.
				if (a.fd == pfds[0].fd) {
					::close(pfds[0].fd);
					pfds[0].fd = -1;
				} else {
					::close(pfds[1].fd);
					pfds[1].fd = -1;
				}
			}
		}
	}

	result.exit_code = proc->wait();
	return result;
}
export template<typename Target>
[[nodiscard]] auto async_run_in(
	Target &target,
	std::filesystem::path exe,
	std::vector<std::string> args,
	SpawnOptions opts = {}) -> conflux::work::root::Task<std::expected<RunResult, std::error_code>> {
	return async_run_on(target, [exe = std::move(exe), args = std::move(args), opts = std::move(opts)]() mutable {
		std::vector<std::string_view> views;
		views.reserve(args.size());
		for (auto const &a: args) {
			views.push_back(a);
		}
		return run(exe, views, std::move(opts));
	});
}

export template<typename Target>
[[nodiscard]] auto run_async_in(
	Target &target,
	std::filesystem::path exe,
	std::vector<std::string> args,
	SpawnOptions opts = {}) -> conflux::work::root::Task<std::expected<RunResult, std::error_code>> {
	return async_run_in(target, std::move(exe), std::move(args), std::move(opts));
}

export template<typename Target>
[[nodiscard]] auto async_wait_in(
	Target &target,
	Process proc) -> conflux::work::root::Task<int> {
	return async_run_on(target, [proc = std::move(proc)]() mutable { return proc.wait(); });
}

export template<typename Target>
[[nodiscard]] auto wait_async_in(
	Target &target,
	Process proc) -> conflux::work::root::Task<int> {
	return async_wait_in(target, std::move(proc));
}
