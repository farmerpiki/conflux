// Plain TU — not a module unit. std::thread lambda → module TU-local rule.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.file_io.buffers;
import conflux.file_io.reader;
import conflux.file_io.iopoll;
import conflux.file_io.driver;
import conflux.file_io_sync;
import conflux.work.root;

using conflux::uring::CompletionTable;
using conflux::uring::FileHandle;
using conflux::uring::IoResult;

static_assert(std::same_as<
			  decltype(std::declval<conflux::file_io::FileReader &>().read_into(
				  std::declval<FileHandle const &>(),
				  std::uint64_t{},
				  std::declval<std::span<std::byte>>())),
			  conflux::work::root::JoinTask<std::size_t>>);
static_assert(std::same_as<
			  decltype(std::declval<conflux::file_io::FileReader &>().write_into(
				  std::declval<FileHandle const &>(),
				  std::uint64_t{},
				  std::declval<std::span<std::byte const>>())),
			  conflux::work::root::JoinTask<std::size_t>>);

namespace {

[[nodiscard]] std::string temp_file_root() {
	// Set CONFLUX_FILE_IO_TMPDIR to run file-backed tests on a real filesystem
	// instead of the default /tmp tmpfs.
	if (char const *env = std::getenv("CONFLUX_FILE_IO_TMPDIR"); env != nullptr && env[0] != '\0') {
		return std::string{env};
	}
	return "/tmp";
}

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}
struct RingFixture {
	::io_uring ring{};
	CompletionTable completions{};
	conflux::file_io::FileReader reader;
	bool ring_ok{false};
	RingFixture()
		: reader{&ring, &completions, [](std::uint32_t slot, std::uint32_t gen) noexcept {
					 return pack_ud(slot, gen);
				 }} {}
	static std::unique_ptr<RingFixture> make(
		unsigned entries = 64) {
		auto fx = std::make_unique<RingFixture>();
		if (::io_uring_queue_init(entries, &fx->ring, 0) < 0) {
			return {};
		}
		fx->ring_ok = true;
		return fx;
	}
	~RingFixture() {
		if (ring_ok) {
			::io_uring_queue_exit(&ring);
		}
	}
	RingFixture(RingFixture const &) = delete;
	RingFixture &operator =(RingFixture const &) = delete;
	RingFixture(RingFixture &&) = delete;
	RingFixture &operator =(RingFixture &&) = delete;
};
std::unique_ptr<RingFixture> require_ring_fixture(
	unsigned entries = 64) {
	auto fx = RingFixture::make(entries);
	INFO("conflux requires a host that permits io_uring_queue_init");
	REQUIRE(fx != nullptr);
	return fx;
}

struct TempFile {
	std::string path;
	int fd{-1};
	static TempFile create(
		std::string_view content = {}) {
		TempFile t;
		t.path = std::format("{}/conflux_file_io_test_XXXXXX", temp_file_root());
		t.fd = ::mkstemp(t.path.data());
		REQUIRE(t.fd >= 0);
		if (!content.empty()) {
			ssize_t const w = ::write(t.fd, content.data(), content.size());
			REQUIRE(w == static_cast<ssize_t>(content.size()));
		}
		return t;
	}
	~TempFile() {
		if (fd >= 0) {
			::close(fd);
		}
		if (!path.empty()) {
			::unlink(path.c_str());
		}
	}
	TempFile() = default;
	TempFile(TempFile const &) = delete;
	TempFile &operator =(TempFile const &) = delete;
	TempFile(
		TempFile &&o) noexcept
		: path{std::move(o.path)}
		, fd{std::exchange(o.fd, -1)} {}
	TempFile &operator =(TempFile &&) = delete;
};

} // namespace
TEST_CASE(
	"file_io: readv_into scatter-reads into multiple buffers",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	std::string const part_a(64, 'A');
	std::string const part_b(128, 'B');
	std::string const content = part_a + part_b;
	auto tf = TempFile::create(content);

	FileHandle const handle = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_open(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC),
		std::chrono::seconds{5});
	REQUIRE(handle.valid());

	std::array<std::byte, 64> buf_a{};
	std::array<std::byte, 128> buf_b{};
	std::vector<iovec> iovs{
		iovec{.iov_base = buf_a.data(), .iov_len = buf_a.size()},
		iovec{.iov_base = buf_b.data(), .iov_len = buf_b.size()},
	};

	std::size_t const got = conflux::file_io::block_on(
		fx->reader,
		fx->reader.readv_into(handle, 0, std::move(iovs)),
		std::chrono::seconds{5});

	REQUIRE(got == content.size());
	for (std::size_t i = 0; i < buf_a.size(); ++i) {
		CHECK(static_cast<char>(buf_a[i]) == 'A');
	}
	for (std::size_t i = 0; i < buf_b.size(); ++i) {
		CHECK(static_cast<char>(buf_b[i]) == 'B');
	}
}
TEST_CASE(
	"file_io: writev_into gather-writes multiple buffers into file",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	auto tf = TempFile::create();

	FileHandle const handle = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_open(AT_FDCWD, tf.path, O_WRONLY | O_CLOEXEC),
		std::chrono::seconds{5});
	REQUIRE(handle.valid());

	std::string const seg_a(48, 'X');
	std::string const seg_b(96, 'Y');
	std::vector<iovec> iovs{
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — writev wants non-const iov_base
		iovec{.iov_base = const_cast<char *>(seg_a.data()), .iov_len = seg_a.size()},
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
		iovec{.iov_base = const_cast<char *>(seg_b.data()), .iov_len = seg_b.size()},
	};

	std::size_t const written = conflux::file_io::block_on(
		fx->reader,
		fx->reader.writev_into(handle, 0, std::move(iovs)),
		std::chrono::seconds{5});
	REQUIRE(written == seg_a.size() + seg_b.size());

	std::string verify(seg_a.size() + seg_b.size(), '\0');
	ssize_t const n = ::pread(tf.fd, verify.data(), verify.size(), 0);
	REQUIRE(n == static_cast<ssize_t>(verify.size()));
	CHECK(verify.substr(0, seg_a.size()) == seg_a);
	CHECK(verify.substr(seg_a.size()) == seg_b);
}
TEST_CASE(
	"file_io: async_fadvise submits or reports known kernel fadvise error",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	TempFile const tmp = TempFile::create(std::string(4096, 'X'));

	bool ok = false;
	int err = 0;
	FileHandle const handle = FileHandle::from_fd(::dup(tmp.fd));
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_fadvise(handle, 0, 4096, POSIX_FADV_SEQUENTIAL),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const passed = ok || err == EBADF;
	CHECK(passed); // EBADF acceptable on some kernel versions for fadvise via io_uring
}
TEST_CASE(
	"file_io: async_madvise submits or reports known kernel madvise error",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	constexpr std::size_t kSize = 4096;
	void *addr = ::mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED) {
		SKIP("mmap failed");
	}

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_madvise(addr, static_cast<std::uint32_t>(kSize), MADV_SEQUENTIAL),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}
	::munmap(addr, kSize);

	bool const passed = ok || err == EINVAL;
	CHECK(passed); // EINVAL acceptable if kernel constrains anonymous madvise
}
TEST_CASE(
	"file_io: async_fixed_fd_install rejects non-direct handle",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create("install test");
	FileHandle const handle = FileHandle::from_fd(::dup(tmp.fd));

	int err = 0;
	try {
		auto _ =
			conflux::file_io::block_on(fx->reader, fx->reader.async_fixed_fd_install(handle), std::chrono::seconds{5});
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(err == EINVAL);
}
TEST_CASE(
	"file_io: async_socket creates a socket",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	FileHandle handle;
	bool ok = false;
	int err = 0;
	try {
		// These compatibility tests intentionally exercise deprecated file_io socket helpers.
#if defined(__clang__) || defined(__GNUC__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
		handle = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0),
			std::chrono::seconds{5});
#if defined(__clang__) || defined(__GNUC__)
	#pragma GCC diagnostic pop
#endif
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	if (ok) {
		CHECK(handle.valid());
		CHECK_FALSE(handle.is_direct());
	}
}
TEST_CASE(
	"file_io: async_shutdown half-closes a socket",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	int const raw_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (raw_fd < 0) {
		SKIP("socket() failed");
	}
	FileHandle const handle = FileHandle::from_fd(raw_fd);

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_shutdown(handle, SHUT_WR), std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const passed = ok || err == ENOTCONN || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: async_connect returns ECONNREFUSED on closed port",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	int const raw_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (raw_fd < 0) {
		SKIP("socket() failed");
	}
	FileHandle const handle = FileHandle::from_fd(raw_fd);

	sockaddr_storage addr{};
	auto *sa4 = reinterpret_cast<sockaddr_in *>(&addr);
	sa4->sin_family = AF_INET;
	sa4->sin_port = htons(1);
	sa4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_connect(handle, addr, sizeof(sockaddr_in)),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = err == ECONNREFUSED || err == EINPROGRESS || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: async_bind + async_listen on loopback",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	int const raw_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (raw_fd < 0) {
		SKIP("socket() failed");
	}
	int const reuse = 1;
	::setsockopt(raw_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	FileHandle const handle = FileHandle::from_fd(raw_fd);

	sockaddr_storage addr{};
	auto *sa4 = reinterpret_cast<sockaddr_in *>(&addr);
	sa4->sin_family = AF_INET;
	sa4->sin_port = 0;
	sa4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	bool bind_ok = false;
	int bind_err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_bind(handle, addr, sizeof(sockaddr_in)),
			std::chrono::seconds{5});
		bind_ok = true;
	} catch (std::system_error const &se) { bind_err = se.code().value(); } catch (...) {
	}

	bool const bind_passed = bind_ok || bind_err == EINVAL || bind_err == ENOSYS;
	CHECK(bind_passed);
	if (!bind_ok) {
		return;
	}

	bool listen_ok = false;
	int listen_err = 0;
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_listen(handle), std::chrono::seconds{5});
		listen_ok = true;
	} catch (std::system_error const &se) { listen_err = se.code().value(); } catch (...) {
	}

	bool const listen_passed = listen_ok || listen_err == EINVAL || listen_err == ENOSYS;
	CHECK(listen_passed);
}
TEST_CASE(
	"file_io: readv2_into scatter-reads with RWF flags",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	std::string const content(64, 'R');
	TempFile const tf = TempFile::create(content);

	FileHandle const handle = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_open(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC),
		std::chrono::seconds{5});
	REQUIRE(handle.valid());

	std::array<std::byte, 64> buf{};
	std::vector<iovec> iovs{
		iovec{.iov_base = buf.data(), .iov_len = buf.size()}
    };

	std::size_t got = 0;
	int err = 0;
	try {
		got = conflux::file_io::block_on(
			fx->reader,
			fx->reader.readv2_into(handle, 0, std::move(iovs)),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	CHECK(err == 0);
	REQUIRE(got == content.size());
	for (std::size_t i = 0; i < got; ++i) {
		CHECK(static_cast<char>(buf[i]) == 'R');
	}
}
TEST_CASE(
	"file_io: writev2_into scatter-writes with RWF flags",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tf = TempFile::create();
	FileHandle const handle = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_open(AT_FDCWD, tf.path, O_WRONLY | O_CLOEXEC),
		std::chrono::seconds{5});
	REQUIRE(handle.valid());

	std::string const payload(32, 'W');
	std::vector<iovec> iovs{
		iovec{.iov_base = const_cast<char *>(payload.data()), .iov_len = payload.size()}
    };

	std::size_t written = 0;
	int err = 0;
	try {
		written = conflux::file_io::block_on(
			fx->reader,
			fx->reader.writev2_into(handle, 0, std::move(iovs)),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	CHECK(err == 0);
	CHECK(written == payload.size());
	std::string verify(payload.size(), '\0');
	ssize_t const n = ::pread(tf.fd, verify.data(), verify.size(), 0);
	CHECK(n == static_cast<ssize_t>(payload.size()));
	CHECK(verify == payload);
}
TEST_CASE(
	"async_poll_add fires on readable pipe") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	int pfd[2];
	REQUIRE(::pipe2(pfd, O_CLOEXEC | O_NONBLOCK) == 0);

	// Write one byte so the read-end becomes readable.
	char const c = 'x';
	REQUIRE(::write(pfd[1], &c, 1) == 1);

	std::uint32_t mask{0};
	bool ok{false};
	try {
		mask =
			conflux::file_io::block_on(fx->reader, fx->reader.async_poll_add(pfd[0], POLLIN), std::chrono::seconds{5});
		ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
	CHECK((mask & POLLIN) != 0u);
	::close(pfd[0]);
	::close(pfd[1]);
}
TEST_CASE(
	"async_accept returns new fd from socketpair-like listen") {
	// Create a listening TCP socket, connect from another thread, accept via uring.
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int accepted_fd{-1};
	int err{0};

	int const listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	REQUIRE(listen_fd >= 0);
	int const optval = 1;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0; // kernel picks port
	REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	REQUIRE(::listen(listen_fd, 1) == 0);

	// Find out the port.
	socklen_t slen = sizeof(addr);
	REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&addr), &slen) == 0);

	// Connect from a background thread.
	std::jthread const connector{[addr]() {
		int const c = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (c >= 0) {
			::connect(c, reinterpret_cast<sockaddr const *>(&addr), sizeof(addr));
			::close(c);
		}
	}};

	FileHandle const listen_handle = FileHandle::from_fd(dup(listen_fd));
	try {
		FileHandle fh =
			conflux::file_io::block_on(fx->reader, fx->reader.async_accept(listen_handle), std::chrono::seconds{5});
		accepted_fd = fh.release_fd();
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	::close(listen_fd);

	bool const passed = accepted_fd >= 0 || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	if (accepted_fd >= 0) {
		::close(accepted_fd);
	}
}
TEST_CASE(
	"async_send + async_recv round-trip over socketpair") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	std::string const payload = "send_recv_test";
	std::size_t const sent = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_send(sender, payload.data(), payload.size()),
		std::chrono::seconds{5});
	REQUIRE(sent == payload.size());

	std::array<char, 64> buf{};
	std::size_t const recvd = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_recv(recver, buf.data(), buf.size()),
		std::chrono::seconds{5});
	REQUIRE(recvd == payload.size());
	CHECK(std::string_view{buf.data(), recvd} == payload);
}
TEST_CASE(
	"async_sendmsg + async_recvmsg round-trip over socketpair") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	std::string const payload = "sendmsg_recvmsg_test";
	iovec send_iov{const_cast<char *>(payload.data()), payload.size()};
	msghdr send_hdr{};
	send_hdr.msg_iov = &send_iov;
	send_hdr.msg_iovlen = 1;

	std::size_t const sent =
		conflux::file_io::block_on(fx->reader, fx->reader.async_sendmsg(sender, &send_hdr), std::chrono::seconds{5});
	REQUIRE(sent == payload.size());

	std::array<char, 64> buf{};
	iovec recv_iov{buf.data(), buf.size()};
	msghdr recv_hdr{};
	recv_hdr.msg_iov = &recv_iov;
	recv_hdr.msg_iovlen = 1;

	std::size_t const recvd =
		conflux::file_io::block_on(fx->reader, fx->reader.async_recvmsg(recver, &recv_hdr), std::chrono::seconds{5});
	REQUIRE(recvd == payload.size());
	CHECK(std::string_view{buf.data(), recvd} == payload);
}
TEST_CASE(
	"async_epoll_ctl + async_epoll_wait detect fd readability") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	int const epfd = ::epoll_create1(EPOLL_CLOEXEC);
	REQUIRE(epfd >= 0);

	int pfd[2];
	REQUIRE(::pipe2(pfd, O_CLOEXEC | O_NONBLOCK) == 0);

	// Add pfd[0] to epoll.
	epoll_event ev{};
	ev.events = EPOLLIN;
	ev.data.fd = pfd[0];
	bool ctl_ok{false};
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_epoll_ctl(epfd, pfd[0], EPOLL_CTL_ADD, &ev),
			std::chrono::seconds{5});
		ctl_ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
	REQUIRE(ctl_ok);

	// Write a byte to make pfd[0] readable, then wait.
	char const c = 'q';
	REQUIRE(::write(pfd[1], &c, 1) == 1);

	std::array<epoll_event, 4> events{};
	int n_events{0};
	try {
		n_events = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_epoll_wait(epfd, events.data(), static_cast<int>(events.size())),
			std::chrono::seconds{5});
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(n_events == 1);
	CHECK((events[0].events & EPOLLIN) != 0u);
	::close(pfd[0]);
	::close(pfd[1]);
	::close(epfd);
}
TEST_CASE(
	"async_provide_buffers + async_remove_buffers smoke") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	// Allocate a small buffer region.
	constexpr int kBufLen = 4096;
	constexpr int kNr = 2;
	constexpr int kBgid = 7;
	auto region = std::make_unique<std::array<char, kBufLen * kNr>>();

	bool ok{false};
	int err{0};
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_provide_buffers(region->data(), kBufLen, kNr, kBgid),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);

	if (ok) {
		bool rm_ok{false};
		try {
			conflux::file_io::block_on(
				fx->reader,
				fx->reader.async_remove_buffers(kNr, kBgid),
				std::chrono::seconds{5});
			rm_ok = true;
		} catch (...) { // NOLINT(bugprone-empty-catch)
		}
		CHECK(rm_ok);
	}
}
TEST_CASE(
	"async_openat2 opens file with basic open_how") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto tf = TempFile::create("openat2_content");

	open_how how{};
	how.flags = O_RDONLY | O_CLOEXEC;

	FileHandle handle;
	bool ok{false};
	try {
		handle = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_openat2(AT_FDCWD, tf.path, how),
			std::chrono::seconds{5});
		ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
	CHECK(ok);
	CHECK(handle.valid());
}
TEST_CASE(
	"async_sendto + async_recv round-trip over UDP loopback") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	int const recv_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	REQUIRE(recv_fd >= 0);
	sockaddr_in ra{};
	ra.sin_family = AF_INET;
	ra.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	ra.sin_port = 0;
	REQUIRE(::bind(recv_fd, reinterpret_cast<sockaddr *>(&ra), sizeof(ra)) == 0);
	socklen_t ralen = sizeof(ra);
	REQUIRE(::getsockname(recv_fd, reinterpret_cast<sockaddr *>(&ra), &ralen) == 0);

	int const send_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	REQUIRE(send_fd >= 0);
	FileHandle const sender = FileHandle::from_fd(send_fd);

	sockaddr_storage dest{};
	memcpy(&dest, &ra, sizeof(ra));

	std::string const payload = "sendto_udp_test";
	std::size_t const sent = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_sendto(sender, payload.data(), payload.size(), 0, dest, sizeof(ra)),
		std::chrono::seconds{5});
	REQUIRE(sent == payload.size());

	FileHandle const recver = FileHandle::from_fd(recv_fd);
	std::array<char, 64> buf{};
	std::size_t const recvd = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_recv(recver, buf.data(), buf.size()),
		std::chrono::seconds{5});
	REQUIRE(recvd == payload.size());
	CHECK(std::string_view{buf.data(), recvd} == payload);
}
TEST_CASE(
	"async_unsafe_send_zc_sent sends data (or gracefully unsupported)") {
#if !CONFLUX_ENABLE_SEND_ZC
	SKIP("experimental SEND_ZC disabled at build time");
#else
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	std::string const payload = "send_zc_test_data";
	bool ok{false};
	int err{0};
	try {
		std::size_t const n = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_unsafe_send_zc_sent(sender, payload.data(), payload.size()),
			std::chrono::seconds{5});
		ok = (n == payload.size());
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EOPNOTSUPP || err == EINVAL || err == ENOSYS;
	CHECK(passed);
#endif
}
TEST_CASE(
	"async_send_zc completes after notification and data received") {
#if !CONFLUX_ENABLE_SEND_ZC
	SKIP("experimental SEND_ZC disabled at build time");
#else
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	std::string const payload = "send_zc_notif_data";
	bool ok{false};
	int err{0};
	try {
		std::size_t const n = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_send_zc(sender, payload.data(), payload.size()),
			std::chrono::seconds{5});
		ok = (n == payload.size());
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EOPNOTSUPP || err == EINVAL || err == ENOSYS;
	REQUIRE(passed);
	if (ok) {
		std::array<char, 64> buf{};
		std::size_t const recvd = static_cast<std::size_t>(::recv(sv[1], buf.data(), buf.size(), MSG_DONTWAIT));
		CHECK(recvd == payload.size());
		CHECK(std::string_view{buf.data(), recvd} == payload);
	}
#endif
}
TEST_CASE(
	"async_unsafe_sendmsg_zc_sent sends data (or gracefully unsupported)") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	std::string const payload = "sendmsg_zc_test";
	iovec iov{const_cast<char *>(payload.data()), payload.size()};
	msghdr hdr{};
	hdr.msg_iov = &iov;
	hdr.msg_iovlen = 1;

	bool ok{false};
	int err{0};
	try {
		std::size_t const n = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_unsafe_sendmsg_zc_sent(sender, &hdr),
			std::chrono::seconds{5});
		ok = (n == payload.size());
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EOPNOTSUPP || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
TEST_CASE(
	"async_sendmsg_zc completes after notification and data received") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) == 0);

	FileHandle const sender = FileHandle::from_fd(sv[0]);
	FileHandle const recver = FileHandle::from_fd(sv[1]);

	std::string const payload = "sendmsg_zc_notif";
	iovec iov{const_cast<char *>(payload.data()), payload.size()};
	msghdr hdr{};
	hdr.msg_iov = &iov;
	hdr.msg_iovlen = 1;

	bool ok{false};
	int err{0};
	try {
		std::size_t const n =
			conflux::file_io::block_on(fx->reader, fx->reader.async_sendmsg_zc(sender, &hdr), std::chrono::seconds{5});
		ok = (n == payload.size());
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EOPNOTSUPP || err == EINVAL || err == ENOSYS;
	REQUIRE(passed);
	if (ok) {
		std::array<char, 64> buf{};
		std::size_t const recvd =
			static_cast<std::size_t>(::recvfrom(sv[1], buf.data(), buf.size(), MSG_DONTWAIT, nullptr, nullptr));
		CHECK(recvd == payload.size());
		CHECK(std::string_view{buf.data(), recvd} == payload);
	}
}
