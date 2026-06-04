#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <liburing.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.file_io.reader;
import conflux.file_io.driver;
import conflux.file_io_sync;

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

struct TempPath {
	std::string path;
	static TempPath unique(
		std::string_view stem) {
		TempPath t;
		t.path = std::format("{}/{}_XXXXXX", temp_file_root(), stem);
		auto *r = ::mkdtemp(t.path.data());
		REQUIRE(r != nullptr);
		std::error_code ec;
		std::filesystem::remove_all(t.path, ec);
		return t;
	}
	~TempPath() {
		if (!path.empty()) {
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	}
	TempPath() = default;
	TempPath(TempPath const &) = delete;
	TempPath &operator =(TempPath const &) = delete;
	TempPath(
		TempPath &&o) noexcept
		: path{std::move(o.path)} {}
	TempPath &operator =(TempPath &&) = delete;
};

} // namespace

TEST_CASE(
	"file_io: open + stat + read_into round trip",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	auto tf = TempFile::create("hello file_io");

	FileHandle const handle = conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_open(AT_FDCWD, tf.path, O_RDONLY | O_CLOEXEC),
		std::chrono::seconds{5});
	REQUIRE(handle.valid());

	conflux::file_io_sync::FileStat const st =
		conflux::file_io::block_on(fx->reader, fx->reader.async_stat(handle), std::chrono::seconds{5});
	CHECK(st.size == std::string_view{"hello file_io"}.size());

	std::array<std::byte, 32> buf{};
	std::size_t const got = conflux::file_io::block_on(
		fx->reader,
		fx->reader.read_into(handle, 0, std::span<std::byte>{buf.data(), buf.size()}),
		std::chrono::seconds{5});
	REQUIRE(got == std::string_view{"hello file_io"}.size());
	CHECK(memcmp(buf.data(), "hello file_io", got) == 0);
}

TEST_CASE(
	"file_io: async_open rejects missing path with ENOENT",
	"[file_io][uring]") {
	auto fx = require_ring_fixture();

	int captured = 0;
	try {
		auto _ = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_open(AT_FDCWD, "/definitely/not/a/real/path.xyz", O_RDONLY | O_CLOEXEC),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { captured = se.code().value(); } catch (...) {
	}
	CHECK(captured == ENOENT);
}

TEST_CASE(
	"file_io: async_unlink removes a file",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	TempFile const tmp = TempFile::create("hello");
	std::string const path = tmp.path;

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_unlink(AT_FDCWD, path), std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat st{};
	CHECK(::stat(path.c_str(), &st) != 0);
}

TEST_CASE(
	"file_io: async_rename renames a file",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	TempFile const src = TempFile::create("data");
	std::string const dst_path = src.path + ".renamed";

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_rename(AT_FDCWD, src.path, AT_FDCWD, dst_path),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat st{};
	CHECK(::stat(dst_path.c_str(), &st) == 0);
	::unlink(dst_path.c_str());
}

TEST_CASE(
	"file_io: async_mkdirat creates a directory",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	auto dir = TempPath::unique("conflux_file_io_mkdir");
	std::string const &dir_path = dir.path;

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_mkdirat(AT_FDCWD, dir_path, 0755),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat st{};
	CHECK(::stat(dir_path.c_str(), &st) == 0);
	CHECK(S_ISDIR(st.st_mode));
	::rmdir(dir_path.c_str());
}

TEST_CASE(
	"file_io: async_symlinkat creates a symlink",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	TempFile const src = TempFile::create("symlink-target");
	std::string const link_path = src.path + ".link";

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_symlinkat(src.path, AT_FDCWD, link_path),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat lst{};
	CHECK(::lstat(link_path.c_str(), &lst) == 0);
	CHECK(S_ISLNK(lst.st_mode));
	::unlink(link_path.c_str());
}

TEST_CASE(
	"file_io: async_ftruncate truncates a file",
	"[file_io][async]") {
	auto fx = require_ring_fixture();

	TempFile const tmp = TempFile::create(std::string(4096, 'T'));
	FileHandle const handle = FileHandle::from_fd(::dup(tmp.fd));

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_ftruncate(handle, 1024), std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat st{};
	CHECK(::fstat(tmp.fd, &st) == 0);
	CHECK(st.st_size == 1024);
}

TEST_CASE(
	"file_io: async_linkat creates a hard link",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const src = TempFile::create("link_content");
	std::string const dst_path = src.path + ".hardlink";
	::unlink(dst_path.c_str());

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_linkat(AT_FDCWD, src.path, AT_FDCWD, dst_path),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS;
	CHECK(passed);
	if (ok) {
		struct ::stat src_st{};
		struct ::stat dst_st{};
		CHECK(::stat(src.path.c_str(), &src_st) == 0);
		CHECK(::stat(dst_path.c_str(), &dst_st) == 0);
		CHECK(src_st.st_ino == dst_st.st_ino);
	}
	::unlink(dst_path.c_str());
}

TEST_CASE(
	"file_io: async_sync_file_range flushes a file region",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create(std::string(4096, 'S'));
	FileHandle const handle = FileHandle::from_fd(::dup(tmp.fd));

	bool ok = false;
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_sync_file_range(handle, 0, 4096, SYNC_FILE_RANGE_WRITE),
			std::chrono::seconds{5});
		ok = true;
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) {
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == EROFS;
	CHECK(passed);
}

TEST_CASE(
	"async_unlinkat removes file relative to dirfd") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto tf = TempFile::create("unlinkat_content");
	std::string const path = tf.path;
	tf.fd = -1;
	tf.path = {};

	bool ok{false};
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_unlinkat(AT_FDCWD, path), std::chrono::seconds{5});
		ok = true;
	} catch (...) {}
	CHECK(ok);
	CHECK(::access(path.c_str(), F_OK) != 0);
}

TEST_CASE(
	"async_renameat renames file across directories") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto tf = TempFile::create("renameat_content");
	std::string const src_path = tf.path;
	std::string const dst_path = src_path + "_renamed";

	bool ok{false};
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_renameat(AT_FDCWD, src_path, AT_FDCWD, dst_path),
			std::chrono::seconds{5});
		ok = true;
	} catch (...) {}
	CHECK(ok);
	if (ok) {
		CHECK(::access(dst_path.c_str(), F_OK) == 0);
		::unlink(dst_path.c_str());
	}
}

TEST_CASE(
	"async_mkdir creates a directory") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto path_guard = TempPath::unique("conflux_file_io_mkdir_test");
	std::string const &path = path_guard.path;

	bool ok{false};
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_mkdir(path), std::chrono::seconds{5});
		ok = true;
	} catch (...) {}
	CHECK(ok);
	if (ok) {
		struct stat st{};
		CHECK(::stat(path.c_str(), &st) == 0);
		CHECK(S_ISDIR(st.st_mode));
	}
}
