module;

#include <sys/wait.h>
#include <unistd.h>

export module conflux.tests.assert_probe_support;

export namespace conflux::tests {

[[nodiscard]] inline int run_assert_probe(
	char const *probe_bin,
	char const *probe) noexcept {
	pid_t const pid = ::fork();
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		char *args[] = {const_cast<char *>(probe_bin), const_cast<char *>(probe), nullptr};
		::execv(probe_bin, args);
		::_exit(3);
	}
	int status{};
	::waitpid(pid, &status, 0);
	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}
	return -1;
}

template<typename F>
struct ScopeExit {
	F fn;
	~ScopeExit() noexcept { fn(); }
};

template<typename F>
ScopeExit(F) -> ScopeExit<F>;

} // namespace conflux::tests
