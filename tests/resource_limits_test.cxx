// Plain TU — not a module unit.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <dirent.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.socket_io;
namespace {

struct RingGuard {
	io_uring ring{};
	explicit RingGuard(
		unsigned entries) {
		if (io_uring_queue_init(entries, &ring, 0) < 0) {
			throw std::runtime_error{"io_uring_queue_init"};
		}
	}
	~RingGuard() { io_uring_queue_exit(&ring); }
	RingGuard(RingGuard const &) = delete;
	RingGuard &operator =(RingGuard const &) = delete;
	[[nodiscard]] io_uring *get() noexcept { return &ring; }
};
struct FdGuard {
	int fd = -1;
	explicit FdGuard(
		int f) noexcept
		: fd{f} {}
	~FdGuard() {
		if (fd >= 0) {
			::close(fd);
		}
	}
	FdGuard(FdGuard const &) = delete;
	FdGuard &operator =(FdGuard const &) = delete;
};
// Count open fds via /proc/self/fd.
// opendir itself opens a fd that appears in the listing, so we subtract it.
static int count_open_fds() noexcept {
	int n = 0;
	auto *d = ::opendir("/proc/self/fd");
	if (!d) {
		return -1;
	}
	while (::readdir(d)) {
		++n;
	}
	::closedir(d);
	// n = . + .. + fds before opendir + dirfd itself
	// actual pre-opendir fd count = n - 3
	return n - 3;
}
// Run fn() in a child process. Returns child exit code (0 = pass).
// Child exits 0 on success, non-zero on any failure or exception.
static int fork_run(
	auto fn) {
	pid_t const pid = ::fork();
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		try {
			fn();
		} catch (...) { ::_exit(1); }
		::_exit(0);
	}
	int status = 0;
	::waitpid(pid, &status, 0);
	if (!WIFEXITED(status)) {
		return -1;
	}
	return WEXITSTATUS(status);
}
// Set rlimit so no new fds can be opened: limit = count of currently open fds.
static bool seal_fds() noexcept {
	rlimit rl{};
	if (::getrlimit(RLIMIT_NOFILE, &rl) != 0) {
		return false;
	}
	int n = 0;
	auto *d = ::opendir("/proc/self/fd");
	if (!d) {
		return false;
	}
	while (::readdir(d)) {
		++n;
	}
	::closedir(d);
	// n-3 = fds open before opendir. After closedir, that is the current count.
	rl.rlim_cur = static_cast<rlim_t>(n - 3);
	return ::setrlimit(RLIMIT_NOFILE, &rl) == 0;
}

} // namespace
// ─── io_uring ring lifecycle fd leak ────────────────────────────────────────

TEST_CASE(
	"resource: io_uring ring create/destroy does not leak fds",
	"[resource]") {
	int const fd_before = count_open_fds();
	REQUIRE(fd_before > 0);
	constexpr int kIters = 50;
	for (int i = 0; i < kIters; ++i) {
		io_uring ring{};
		if (::io_uring_queue_init(64, &ring, 0) < 0) {
			SKIP("io_uring_queue_init failed");
		}
		::io_uring_queue_exit(&ring);
	}
	CHECK(count_open_fds() == fd_before);
}
// ─── FD leak regression ──────────────────────────────────────────────────────

TEST_CASE(
	"resource: submit_fixed_fd_install does not leak OS fds",
	"[resource]") {
	// Regression: IORING_OP_FIXED_FD_INSTALL returns a new OS fd as cqe->res.
	// If the caller never closes it, fd count grows until EMFILE crashes later tests.
	// Run 1000 install ops and verify the fd count is stable.
	int ls_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(ls_fd >= 0);
	FdGuard lsg{ls_fd};
	{
		int one = 1;
		(void)::setsockopt(ls_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		REQUIRE(::bind(ls_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
		REQUIRE(::listen(ls_fd, 16) == 0);
	}
	sockaddr_in addr{};
	socklen_t len = sizeof(addr);
	REQUIRE(::getsockname(ls_fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0);
	std::uint16_t const port = ntohs(addr.sin_port);

	RingGuard rg{256};
	SocketRawRing raw{rg.get()};
	DirectFdTable dft{rg.get(), 64};
	REQUIRE(dft.registered());

	int cli = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(cli >= 0);
	FdGuard cg{cli};
	addr.sin_port = htons(port);
	REQUIRE(::connect(cli, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	int srv = ::accept4(ls_fd, nullptr, nullptr, SOCK_CLOEXEC);
	REQUIRE(srv >= 0);
	FdGuard sg{srv};
	REQUIRE(dft.install(0, srv));

	int const fd_before = count_open_fds();
	REQUIRE(fd_before > 0);

	constexpr int kIters = 1000;
	for (int i = 0; i < kIters; ++i) {
		submit_fixed_fd_install(raw, 0, static_cast<std::uint64_t>(i));
		REQUIRE(raw.submit() >= 0);
		io_uring_cqe *cqe{};
		REQUIRE(::io_uring_wait_cqe(rg.get(), &cqe) == 0);
		int const installed = cqe->res;
		::io_uring_cqe_seen(rg.get(), cqe);
		REQUIRE(installed >= 0);
		::close(installed);
	}

	CHECK(count_open_fds() == fd_before);
}
// ─── FD exhaustion — must return errors, not hang ────────────────────────────

TEST_CASE(
	"resource: socket() returns EMFILE under fd exhaustion",
	"[resource]") {
	int const rc = fork_run([] {
		if (!seal_fds()) {
			::_exit(1);
		}
		errno = 0;
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (fd >= 0) {
			::close(fd);
			::_exit(2);
		} // unexpectedly got a fd
		if (errno != EMFILE && errno != ENFILE) {
			::_exit(3); // wrong error
		}
	});
	REQUIRE(rc == 0);
}
TEST_CASE(
	"resource: io_uring_queue_init fails under fd exhaustion",
	"[resource]") {
	int const rc = fork_run([] {
		if (!seal_fds()) {
			::_exit(1);
		}
		io_uring ring{};
		int const err = ::io_uring_queue_init(64, &ring, 0);
		if (err == 0) {
			::io_uring_queue_exit(&ring);
			::_exit(2);
		} // unexpectedly succeeded
		// err < 0 is correct — ring init failed without hanging
	});
	REQUIRE(rc == 0);
}
TEST_CASE(
	"resource: DirectFdTable skips registration under fd exhaustion",
	"[resource]") {
	// DirectFdTable constructor calls io_uring_register_files_sparse.
	// If the ring itself can't be created, we verify no hang occurs.
	int const rc = fork_run([] {
		// Create ring before sealing (ring needs an fd).
		io_uring ring{};
		if (::io_uring_queue_init(64, &ring, 0) != 0) {
			::_exit(0); // ring failed, skip
		}
		if (!seal_fds()) {
			::io_uring_queue_exit(&ring);
			::_exit(1);
		}
		{
			// registered_ will be false if register_files_sparse fails — no hang.
			DirectFdTable dft{&ring, 64};
			// Either registered or not — both are acceptable, no hang is the requirement.
			(void)dft.registered();
		}
		::io_uring_queue_exit(&ring);
	});
	REQUIRE(rc == 0);
}
TEST_CASE(
	"resource: BufferRing fails gracefully under low RLIMIT_MEMLOCK",
	"[resource]") {
	// Buffer ring uses io_uring registered buffers which consume locked memory.
	// Under a tight memlock limit, allocation should fail or succeed — not hang.
	int const rc = fork_run([] {
		// 64 KiB memlock — too small for 256 × 4 KiB = 1 MiB buffer ring.
		rlimit const rl{.rlim_cur = 65536, .rlim_max = 65536};
		if (::setrlimit(RLIMIT_MEMLOCK, &rl) != 0) {
			::_exit(1);
		}
		io_uring ring{};
		if (::io_uring_queue_init(256, &ring, 0) != 0) {
			::_exit(0); // ring failed → skip
		}
		SocketRawRing raw{&ring};
		try {
			auto const caps = conflux::uring::detect_caps(conflux::uring::RingRef{&ring});
			BufferRing bufs{
				&ring,
				{.count = 256, .buf_size = 4096, .group_id = 0, .huge_pages = false},
				caps
            };
			(void)bufs; // success also fine — memlock may not apply to registered bufs
		} catch (...) {
			// Exception on failure is acceptable.
		}
		::io_uring_queue_exit(&ring);
		// If we reach here without hanging, the test passes.
	});
	REQUIRE(rc == 0);
}
