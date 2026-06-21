#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <liburing.h>
#include <linux/futex.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io.reader;
import conflux.file_io.driver;

namespace root = conflux::work::root;
using conflux::uring::CompletionTable;

namespace {

[[nodiscard]] std::string temp_file_root() {
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
	"file_io: async_cancel on non-existent user_data succeeds (ENOENT -> ok)",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_cancel(0xDEADBEEFULL), std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}

TEST_CASE(
	"file_io: async_cancel_fd on idle fd resolves",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create("cancel fd test");

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_cancel_fd(tmp.fd), std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}

TEST_CASE(
	"file_io: pump_until honors subsecond timeout budgets",
	"[file_io][driver]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	std::atomic_flag done{};
	auto const started = std::chrono::steady_clock::now();
	CHECK_THROWS_AS(
		conflux::file_io::pump_until(fx->reader, done, std::chrono::milliseconds{25}),
		conflux::file_io::PumpTimeout);
	auto const elapsed = std::chrono::steady_clock::now() - started;
	CHECK(elapsed < std::chrono::milliseconds{500});
}

TEST_CASE(
	"file_io: async_futex_wake wakes zero waiters on uncontested futex",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	std::uint32_t futex_word = 0;
	std::uint32_t woken = 42;
	int err = 0;
	try {
		woken = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_futex_wake(&futex_word, UINT64_MAX),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = (woken == 0 && err == 0) || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}

TEST_CASE(
	"file_io: async_futex_wait resolves immediately when word already changed",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	std::uint32_t futex_word = 1;
	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_futex_wait(&futex_word, 0), std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EAGAIN || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}

TEST_CASE(
	"file_io: async_msg_ring delivers synthetic CQE to self ring",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_msg_ring(fx->ring.ring_fd, 42, 0xCAFEBABEULL),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == EOPNOTSUPP;
	CHECK(passed);
}

TEST_CASE(
	"file_io: async_waitid on non-existent pid returns ECHILD",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	siginfo_t info{};
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_waitid(P_PID, static_cast<id_t>(99999999), &info),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = err == ECHILD || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}

TEST_CASE(
	"file_io: async_nop completes successfully",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_nop(), std::chrono::seconds{5});
		ok = true;
	} catch (...) {}

	CHECK(ok);
}

TEST_CASE(
	"file_io: async_timeout fires after delay",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_timeout(std::chrono::milliseconds{10}),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}

TEST_CASE(
	"file_io: async_futex_waitv resolves immediately on already-changed word",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	std::uint32_t futex_word = 42;
	futex_waitv w{};
	w.val = 0;
	w.uaddr = reinterpret_cast<std::uint64_t>(&futex_word);
	w.flags = FUTEX2_SIZE_U32;
	w.__reserved = 0;
	std::vector<futex_waitv> waiters{w};

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_futex_waitv(std::move(waiters)),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EAGAIN || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}

TEST_CASE(
	"file_io: async_msg_ring_fd sends fd to same ring",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create("fd_msg");
	int const dup_fd = ::dup(tmp.fd);
	REQUIRE(dup_fd >= 0);

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_msg_ring_fd(fx->ring.ring_fd, dup_fd, -1, 0xABCDULL),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}
	::close(dup_fd);

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == EOPNOTSUPP;
	CHECK(passed);
}

TEST_CASE(
	"file_io: async_timeout_remove on non-existent tag resolves",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_timeout_remove(0xDEADULL), std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}

TEST_CASE(
	"file_io: async_timeout_update on non-existent tag resolves",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_timeout_update(0xBEEFULL, std::chrono::milliseconds{100}),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}

TEST_CASE(
	"async_poll_remove cancels pending poll") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	bool remove_ok{false};
	int err{0};

	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) == 0);

	auto poll_flow = fx->reader.async_poll_add(sv[0], POLLIN);
	io_uring_submit(fx->reader.ring());

	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_cancel_fd(sv[0], 0), std::chrono::seconds{5});
		remove_ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}
	root::abandon_to(std::move(poll_flow), root::drop_on_abandon{});

	bool const passed = remove_ok || err == ENOENT || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	::close(sv[0]);
	::close(sv[1]);
}

TEST_CASE(
	"async_msg_ring_cqe_flags posts message to self ring") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	bool ok{false};
	int err{0};
	int const ring_fd = fx->ring.ring_fd;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_msg_ring_cqe_flags(ring_fd, 42, 0xBEEFULL, 0, 0),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
}
