// Plain TU - not a module unit. std::thread lambda -> module TU-local rule.
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.file_io.pipe_pool;
import conflux.file_io.reader;
import conflux.file_io.driver;

using conflux::uring::CompletionTable;
using conflux::uring::FileHandle;

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

std::unique_ptr<RingFixture> require_ring_fixture() {
	auto fx = RingFixture::make();
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

struct PipeGuard {
	int fds[4];
	~PipeGuard() {
		for (int const fd: fds) {
			if (fd >= 0) {
				::close(fd);
			}
		}
	}
};

} // namespace

TEST_CASE(
	"file_io: splice_to_fd streams file into external pipe",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	conflux::file_io::PipePool pipes{2};
	auto pipe = pipes.try_acquire();
	REQUIRE(pipe.has_value());

	std::string const content(8UL * 1024, 'S');
	auto tf = TempFile::create(content);

	int sink_pipe[2] = {-1, -1};
	REQUIRE(::pipe2(sink_pipe, O_CLOEXEC) == 0);
	::fcntl(sink_pipe[1], F_SETPIPE_SZ, 1 << 20);

	FileHandle const handle = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_open(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC),
		std::chrono::seconds{5});
	REQUIRE(handle.valid());

	std::size_t const delivered = conflux::file_io::block_on(
		fx->reader,
		fx->reader.splice_to_fd(handle, 0, content.size(), sink_pipe[1], std::move(*pipe)),
		std::chrono::seconds{5});
	CHECK(delivered == content.size());

	std::string drained(content.size(), '\0');
	std::size_t off = 0;
	while (off < drained.size()) {
		ssize_t const n = ::read(sink_pipe[0], drained.data() + off, drained.size() - off);
		if (n <= 0) {
			break;
		}
		off += static_cast<std::size_t>(n);
	}
	::close(sink_pipe[0]);
	::close(sink_pipe[1]);
	CHECK(off == content.size());
	CHECK(drained == content);
}
TEST_CASE(
	"file_io: PipePool acquire/release recycles pairs",
	"[file_io][unit]") {
	conflux::file_io::PipePool pool{3};
	CHECK(pool.capacity() == 3);
	CHECK(pool.available() == 3);

	auto a = pool.try_acquire();
	auto b = pool.try_acquire();
	auto c = pool.try_acquire();
	auto d = pool.try_acquire();
	REQUIRE(a.has_value());
	REQUIRE(b.has_value());
	REQUIRE(c.has_value());
	CHECK_FALSE(d.has_value());
	CHECK(a->read_fd() >= 0);
	CHECK(a->write_fd() >= 0);
	CHECK(a->capacity() > 0);

	a.reset();
	CHECK(pool.available() == 1);
}
TEST_CASE(
	"file_io: async_tee copies data between pipes",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	int src_pipe[2] = {-1, -1};
	int dst_pipe[2] = {-1, -1};
	if (::pipe2(src_pipe, O_CLOEXEC) < 0 || ::pipe2(dst_pipe, O_CLOEXEC) < 0) {
		if (src_pipe[0] >= 0) {
			::close(src_pipe[0]);
			::close(src_pipe[1]);
		}
		SKIP("pipe2 failed");
	}
	PipeGuard const guard{src_pipe[0], src_pipe[1], dst_pipe[0], dst_pipe[1]};
	std::string const payload(64, 'T');
	ssize_t const written = ::write(src_pipe[1], payload.data(), payload.size());
	REQUIRE(written == static_cast<ssize_t>(payload.size()));

	std::size_t got = 0;
	bool ok = false;
	int err = 0;
	try {
		got = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_tee(src_pipe[0], dst_pipe[1], payload.size()),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	if (ok) {
		CHECK(got == payload.size());
		std::string dst_buf(payload.size(), '\0');
		ssize_t const n = ::read(dst_pipe[0], dst_buf.data(), dst_buf.size());
		CHECK(n == static_cast<ssize_t>(payload.size()));
		CHECK(dst_buf == payload);
	}
}
TEST_CASE(
	"file_io: async_pipe creates a functional pipe",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	std::pair<int, int> fds{-1, -1};
	bool ok = false;
	int err = 0;
	try {
#if defined(__clang__) || defined(__GNUC__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
		fds = conflux::file_io::block_on(fx->reader, fx->reader.async_pipe(O_CLOEXEC), std::chrono::seconds{5});
#if defined(__clang__) || defined(__GNUC__)
	#pragma GCC diagnostic pop
#endif
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	if (ok) {
		CHECK(fds.first >= 0);
		CHECK(fds.second >= 0);
		std::string const msg = "ping";
		ssize_t const w = ::write(fds.second, msg.data(), msg.size());
		CHECK(w == static_cast<ssize_t>(msg.size()));
		std::array<char, 8> buf{};
		ssize_t const n = ::read(fds.first, buf.data(), buf.size());
		CHECK(n == static_cast<ssize_t>(msg.size()));
		CHECK(std::string_view{buf.data(), static_cast<std::size_t>(n)} == msg);
		::close(fds.first);
		::close(fds.second);
	}
}
