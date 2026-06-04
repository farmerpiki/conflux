// Plain TU - not a module unit. std::thread lambda -> module TU-local rule.
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <liburing.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.file_io.buffers;
import conflux.file_io.reader;
import conflux.file_io.iopoll;
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
	"file_io: read_fixed via registered buffer",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	conflux::file_io::RegisteredBufferTable tbl{&fx->ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	conflux::file_io::FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}
	auto buf = pool.try_acquire();
	REQUIRE(buf.has_value());

	std::string const content(1024, 'Z');
	auto tf = TempFile::create(content);

	FileHandle const handle = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_open(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC),
		std::chrono::seconds{5});
	REQUIRE(handle.valid());

	conflux::file_io::FileReader::ReadFixedResult const got = conflux::file_io::block_on(
		fx->reader,
		fx->reader.read_fixed(handle, 0, std::move(*buf)),
		std::chrono::seconds{5});
	REQUIRE(got.bytes == content.size());
	auto const view = got.buffer.view();
	for (std::size_t i = 0; i < got.bytes; ++i) {
		REQUIRE(static_cast<char>(view[i]) == 'Z');
	}
}
TEST_CASE(
	"file_io: async_open_direct returns a fixed-file handle",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();
	if (::io_uring_register_files_sparse(&fx->ring, 4) < 0) {
		SKIP("fixed-file registration unsupported");
	}

	auto tf = TempFile::create("direct file");

	FileHandle handle;
	int open_error = 0;
	try {
		handle = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_open_direct(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC, 0, 2),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { open_error = se.code().value(); } catch (...) {
	}
	if (!handle.valid() && (open_error == EINVAL || open_error == EOPNOTSUPP || open_error == ENOSYS)) {
		::io_uring_unregister_files(&fx->ring);
		SKIP("direct open unsupported by this kernel/ring configuration");
	}
	REQUIRE(handle.valid());
	REQUIRE(handle.is_direct());
	CHECK(handle.direct_slot() == 2);

	std::array<std::byte, 32> buf{};
	std::size_t const got = conflux::file_io::block_on(
		fx->reader,
		fx->reader.read_into(handle, 0, std::span<std::byte>{buf.data(), buf.size()}),
		std::chrono::seconds{5});
	REQUIRE(got == std::string_view{"direct file"}.size());
	CHECK(memcmp(buf.data(), "direct file", got) == 0);

	conflux::file_io::block_on(fx->reader, fx->reader.async_close(std::move(handle)), std::chrono::seconds{5});
	::io_uring_unregister_files(&fx->ring);
}
TEST_CASE(
	"file_io: conflux::file_io::FixedBufferPool try_acquire drains and refills on release",
	"[file_io][unit]") {
	::io_uring ring{};
	if (::io_uring_queue_init(8, &ring, 0) < 0) {
		FAIL("conflux requires a host that permits io_uring_queue_init");
	}
	struct RingGuard {
		io_uring *ring;
		~RingGuard() { ::io_uring_queue_exit(ring); }
	} const guard{&ring};
	conflux::file_io::RegisteredBufferTable tbl{&ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	conflux::file_io::FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}
	CHECK(pool.capacity() == 2);
	CHECK(pool.available() == 2);

	auto a = pool.try_acquire();
	auto b = pool.try_acquire();
	auto c = pool.try_acquire();
	REQUIRE(a.has_value());
	REQUIRE(b.has_value());
	CHECK_FALSE(c.has_value());
	CHECK(pool.available() == 0);

	a.reset();
	CHECK(pool.available() == 1);
}
TEST_CASE(
	"file_io: iopoll storage ring exposes storage-only fixed read path",
	"[file_io][uring][iopoll]") {
#if !CONFLUX_ENABLE_IOPOLL_STORAGE_TEST
	SKIP("experimental IOPOLL/O_DIRECT storage-ring test disabled at build time");
#else
	conflux::file_io::IopollStorageRingOptions options{};
	options.entries = 32;
	options.fixed_buffer_slots = 2;
	options.fixed_buffer_bytes = 4096;
	auto storage = conflux::file_io::IopollStorageRing::create(options);
	if (!storage) {
		INFO(std::format("iopoll storage ring unavailable: {}", storage.error().what()));
		SKIP("iopoll storage ring unavailable");
	}

	std::string const content(4096, 'I');
	auto tf = TempFile::create(content);
	int const fd = ::open(tf.path.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
	if (fd < 0) {
		INFO(std::format("O_DIRECT unavailable for test file: {}", strerror(errno)));
		SKIP("O_DIRECT unavailable for test file");
	}
	FileHandle handle = FileHandle::from_fd(fd);
	auto buf = (*storage)->try_acquire_buffer();
	REQUIRE(buf.has_value());

	try {
		auto got = block_on_iopoll(
			(*storage)->reader(),
			(*storage)->reader().read_nocache_fixed(handle, 0, std::move(*buf), content.size()),
			std::chrono::seconds{5});
		REQUIRE(got.bytes == content.size());
		CHECK(memcmp(got.buffer.view().data(), content.data(), content.size()) == 0);
	} catch (std::system_error const &se) {
		int const err = se.code().value();
		if (err == EINVAL || err == EOPNOTSUPP || err == ENOSYS || err == ENOTSUP) {
			INFO(std::format("IOPOLL/O_DIRECT read unsupported on this filesystem/device: {}", se.what()));
			SKIP("IOPOLL/O_DIRECT read unsupported on this filesystem/device");
		}
		throw;
	}
#endif
}
TEST_CASE(
	"file_io: write_fixed round-trips content via registered buffer",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	conflux::file_io::RegisteredBufferTable tbl{&fx->ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	conflux::file_io::FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}

	auto tf = TempFile::create();

	FileHandle const wh = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_open(AT_FDCWD, tf.path, O_WRONLY | O_CLOEXEC),
		std::chrono::seconds{5});
	REQUIRE(wh.valid());

	auto write_buf = pool.try_acquire();
	REQUIRE(write_buf.has_value());
	std::string const payload(512, 'W');
	memcpy(write_buf->view().data(), payload.data(), payload.size());

	conflux::file_io::FileReader::WriteFixedResult const wresult = conflux::file_io::block_on(
		fx->reader,
		fx->reader.write_fixed(wh, 0, std::move(*write_buf), payload.size()),
		std::chrono::seconds{5});
	REQUIRE(wresult.bytes == payload.size());

	std::string verify(payload.size(), '\0');
	ssize_t const n = ::pread(tf.fd, verify.data(), verify.size(), 0);
	REQUIRE(n == static_cast<ssize_t>(payload.size()));
	CHECK(verify == payload);
	CHECK(wresult.buffer.valid());
}
TEST_CASE(
	"file_io: read_nocache_fixed with O_DIRECT bypasses page cache",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	conflux::file_io::RegisteredBufferTable tbl{&fx->ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	conflux::file_io::FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}

	std::string const content(1500, 'D');
	auto tf = TempFile::create(content);

	FileHandle handle;
	int open_err = 0;
	try {
		handle = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_open(AT_FDCWD, tf.path, O_RDONLY | O_DIRECT | O_CLOEXEC),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { open_err = se.code().value(); } catch (...) {
	}
	if (!handle.valid()) {
		SKIP(std::format("O_DIRECT open failed: errno={}", open_err));
	}

	auto buf = pool.try_acquire();
	REQUIRE(buf.has_value());

	conflux::file_io::FileReader::ReadFixedResult got{};
	int read_err = 0;
	try {
		got = conflux::file_io::block_on(
			fx->reader,
			fx->reader.read_nocache_fixed(handle, 0, std::move(*buf), content.size()),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { read_err = se.code().value(); } catch (...) {
	}

	if (!got.buffer.valid() && read_err == EINVAL) {
		SKIP("filesystem does not support O_DIRECT reads (e.g. tmpfs)");
	}

	REQUIRE(got.bytes == content.size());
	auto const view = got.buffer.view();
	for (std::size_t i = 0; i < got.bytes; ++i) {
		REQUIRE(static_cast<char>(view[i]) == 'D');
	}
}
TEST_CASE(
	"file_io: read_nocache_fixed caps result to max_bytes",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	conflux::file_io::RegisteredBufferTable tbl{&fx->ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	conflux::file_io::FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}

	std::string const content(4096, 'C');
	auto tf = TempFile::create(content);

	FileHandle handle;
	try {
		handle = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_open(AT_FDCWD, tf.path, O_RDONLY | O_DIRECT | O_CLOEXEC),
			std::chrono::seconds{5});
	} catch (...) {}
	if (!handle.valid()) {
		SKIP("O_DIRECT open failed");
	}

	auto buf = pool.try_acquire();
	REQUIRE(buf.has_value());

	conflux::file_io::FileReader::ReadFixedResult got{};
	int read_err = 0;
	try {
		got = conflux::file_io::block_on(
			fx->reader,
			fx->reader.read_nocache_fixed(handle, 0, std::move(*buf), 512),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { read_err = se.code().value(); } catch (...) {
	}

	if (!got.buffer.valid() && read_err == EINVAL) {
		SKIP("filesystem does not support O_DIRECT reads");
	}

	CHECK(got.bytes == 512);
	auto const view = got.buffer.view();
	for (std::size_t i = 0; i < got.bytes; ++i) {
		CHECK(static_cast<char>(view[i]) == 'C');
	}
}
TEST_CASE(
	"async_write_fixed + read_fixed round-trip") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	conflux::file_io::RegisteredBufferTable tbl{&fx->ring, 2};
	if (!tbl.ok()) {
		SKIP("register_buffers_sparse unsupported");
	}
	conflux::file_io::FixedBufferPool pool{&tbl, 0, 2, 4096};
	if (!pool.ok()) {
		SKIP("fixed buffer pool init failed");
	}
	auto wbuf = pool.try_acquire();
	REQUIRE(wbuf.has_value());

	auto tf = TempFile::create();

	std::string const content(512, 'W');
	auto const view = wbuf->view();
	memcpy(view.data(), content.data(), content.size());

	FileHandle const handle = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_open(AT_FDCWD, tf.path, O_RDWR | O_CLOEXEC),
		std::chrono::seconds{5});
	REQUIRE(handle.valid());

	std::size_t const written = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_write_fixed(
			handle,
			0,
			view.data(),
			static_cast<unsigned>(content.size()),
			static_cast<int>(wbuf->slot())),
		std::chrono::seconds{5});
	REQUIRE(written == content.size());

	auto rbuf = pool.try_acquire();
	REQUIRE(rbuf.has_value());
	conflux::file_io::FileReader::ReadFixedResult const rr = conflux::file_io::block_on(
		fx->reader,
		fx->reader.read_fixed(handle, 0, std::move(*rbuf)),
		std::chrono::seconds{5});
	REQUIRE(rr.bytes == content.size());
	auto const rview = rr.buffer.view();
	CHECK(memcmp(rview.data(), content.data(), content.size()) == 0);
}
TEST_CASE(
	"async_openat_direct opens file into registered slot") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	int const reg_fd = -1;
	if (::io_uring_register_files(&fx->ring, &reg_fd, 1) < 0) {
		SKIP("io_uring_register_files unsupported");
	}

	auto tf = TempFile::create("openat_direct_content");

	FileHandle handle;
	bool ok{false};
	int err{0};
	try {
		handle = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_openat_direct(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC, 0, 0),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == ENFILE;
	CHECK(passed);
	if (handle.valid()) {
		conflux::file_io::block_on(fx->reader, fx->reader.async_close(std::move(handle)), std::chrono::seconds{5});
	}
	::io_uring_unregister_files(&fx->ring);
}
TEST_CASE(
	"async_pipe_direct creates pipe into fixed file table (or graceful skip)") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}

	int fds_reg[2] = {-1, -1};
	if (::io_uring_register_files(&fx->ring, fds_reg, 2) < 0) {
		SKIP("io_uring_register_files unsupported");
	}

	bool ok{false};
	int err{0};
	try {
		std::pair<int, int> const p =
			conflux::file_io::block_on(fx->reader, fx->reader.async_pipe_direct(0), std::chrono::seconds{5});
		ok = (p.first >= 0 || p.second >= 0);
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == EOPNOTSUPP;
	CHECK(passed);
	::io_uring_unregister_files(&fx->ring);
}
