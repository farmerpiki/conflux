// Plain TU — not a module unit. std::thread lambda → module TU-local rule.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fcntl.h>
#include <liburing.h>
#include <linux/futex.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io.buffers;
import conflux.file_io.reader;
import conflux.file_io.iopoll;
import conflux.file_io.driver;
import conflux.file_io_sync;

namespace root = conflux::work::root;
using conflux::uring::CompletionTable;
using conflux::uring::FileHandle;
using conflux::uring::IoResult;

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

struct TempDir {
	std::string path;
	int fd{-1};
	static TempDir create() {
		TempDir t;
		t.path = std::format("{}/conflux_file_io_dir_XXXXXX", temp_file_root());
		auto *r = ::mkdtemp(t.path.data());
		REQUIRE(r != nullptr);
		t.fd = ::open(t.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		REQUIRE(t.fd >= 0);
		return t;
	}
	~TempDir() {
		if (fd >= 0) {
			::close(fd);
		}
		if (!path.empty()) {
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	}
	TempDir() = default;
	TempDir(TempDir const &) = delete;
	TempDir &operator =(TempDir const &) = delete;
	TempDir(
		TempDir &&o) noexcept
		: path{std::move(o.path)}
		, fd{std::exchange(o.fd, -1)} {}
	TempDir &operator =(TempDir &&) = delete;
	void mkdir_sub(
		std::string_view name) const {
		auto full = std::format("{}/{}", path, name);
		REQUIRE(::mkdir(full.c_str(), 0755) == 0);
	}
	void write_file(
		std::string_view name,
		std::string_view content) const {
		auto full = std::format("{}/{}", path, name);
		int const f = ::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		REQUIRE(f >= 0);
		auto const w = ::write(f, content.data(), content.size());
		::close(f);
		REQUIRE(w == static_cast<ssize_t>(content.size()));
	}
	[[nodiscard]] std::string read_file(
		std::string_view name) const {
		auto full = std::format("{}/{}", path, name);
		int const f = ::open(full.c_str(), O_RDONLY | O_CLOEXEC);
		REQUIRE(f >= 0);
		std::string out(4096, '\0');
		auto const n = ::read(f, out.data(), out.size());
		::close(f);
		REQUIRE(n >= 0);
		out.resize(static_cast<std::size_t>(n));
		return out;
	}
	[[nodiscard]] bool has_staging_files(
		std::string_view subdir = {}) const {
		std::filesystem::path p{path};
		if (!subdir.empty()) {
			p /= std::string{subdir};
		}
		for (auto const &entry: std::filesystem::directory_iterator{p}) {
			auto const name = entry.path().filename().string();
			if (name.starts_with(".conflux.tmp.")) {
				return true;
			}
		}
		return false;
	}
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
		(void)conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_open(AT_FDCWD, "/definitely/not/a/real/path.xyz", O_RDONLY | O_CLOEXEC),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) {
		captured = se.code().value();
	} catch (...) { // NOLINT(bugprone-empty-catch) — test swallows other exceptions
	}
	CHECK(captured == ENOENT);
}
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
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
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
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat st{};
	CHECK(::stat(dst_path.c_str(), &st) == 0);
	::unlink(dst_path.c_str());
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
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
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
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
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
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
	CHECK(err == 0);
	struct ::stat st{};
	CHECK(::fstat(tmp.fd, &st) == 0);
	CHECK(st.st_size == 1024);
}
TEST_CASE(
	"file_io: async_fsetxattr + async_fgetxattr round-trips an xattr",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create("xattr test");
	FileHandle const handle = FileHandle::from_fd(::dup(tmp.fd));

	bool set_ok = false;
	int set_err = 0;
	std::string const xattr_name = "user.test_key";
	std::string const xattr_val = "hello_xattr";
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_fsetxattr(handle, xattr_name, xattr_val),
			std::chrono::seconds{5});
		set_ok = true;
	} catch (std::system_error const &se) { set_err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const set_passed =
		set_ok || set_err == EOPNOTSUPP || set_err == ENOTSUP || set_err == EINVAL || set_err == ENOSYS;
	CHECK(set_passed);
	if (!set_ok) {
		return;
	}

	std::array<char, 64> buf{};
	std::size_t got = 0;
	int get_err = 0;
	try {
		got = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_fgetxattr(handle, xattr_name, std::span<char>{buf.data(), buf.size()}),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { get_err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(get_err == 0);
	REQUIRE(got == xattr_val.size());
	CHECK(std::string_view{buf.data(), got} == xattr_val);
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
		(void)
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
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
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
	} catch (std::system_error const &se) { err = se.code().value(); } catch (...) { // NOLINT(bugprone-empty-catch)
	}

	bool const passed = ok || err == EINVAL || err == ENOSYS || err == EROFS;
	CHECK(passed);
}
TEST_CASE(
	"file_io: async_cancel on non-existent user_data succeeds (ENOENT → ok)",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	bool ok = false;
	int err = 0;
	// user_data 0xDEADBEEF has no pending op — should resolve (ENOENT → ok path).
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
	// val=0 but *futex=1 — condition already met, returns immediately.
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
	"file_io: async_setxattr + async_getxattr round-trips path-based xattr",
	"[file_io][async]") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring init failed");
	}

	TempFile const tmp = TempFile::create("xattr path test");

	bool set_ok = false;
	int set_err = 0;
	std::string const xattr_val = "path_xattr_val";
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_setxattr(tmp.path, "user.path_test_key", xattr_val),
			std::chrono::seconds{5});
		set_ok = true;
	} catch (std::system_error const &se) { set_err = se.code().value(); } catch (...) {
	}

	bool const set_passed =
		set_ok || set_err == EOPNOTSUPP || set_err == ENOTSUP || set_err == EINVAL || set_err == ENOSYS;
	CHECK(set_passed);
	if (!set_ok) {
		return;
	}

	std::array<char, 64> buf{};
	std::size_t got = 0;
	int get_err = 0;
	try {
		got = conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_getxattr(tmp.path, "user.path_test_key", std::span<char>{buf.data(), buf.size()}),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { get_err = se.code().value(); } catch (...) {
	}

	CHECK(get_err == 0);
	REQUIRE(got == xattr_val.size());
	CHECK(std::string_view{buf.data(), got} == xattr_val);
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
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}

	CHECK(ok);
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
	// uaddr cast to std::uint64_t as expected by futex_waitv
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
	// Remove a timeout tag that was never armed — should resolve (ENOENT→ok).
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
	"async_poll_remove cancels pending poll") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	bool remove_ok{false};
	int err{0};

	// Open a socket that is never written to (poll will block indefinitely).
	int sv[2];
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) == 0);

	// Submit poll_add (won't fire because nothing writes to sv[0]).
	auto poll_flow = fx->reader.async_poll_add(sv[0], POLLIN);
	io_uring_submit(fx->reader.ring());

	// Now cancel it: we need the user_data of the poll SQE.
	// Our fixture encodes ud as pack_ud(slot, gen). We know the poll_add
	// reserved slot 0 gen 1 (first reservation after construction).
	// Use async_cancel_fd instead — simpler to test.
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
	"async_unlinkat removes file relative to dirfd") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto tf = TempFile::create("unlinkat_content");
	std::string const path = tf.path;
	tf.fd = -1; // don't let TempFile close (will unlink)
	tf.path = {}; // don't let TempFile unlink

	bool ok{false};
	try {
		conflux::file_io::block_on(fx->reader, fx->reader.async_unlinkat(AT_FDCWD, path), std::chrono::seconds{5});
		ok = true;
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
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
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
	CHECK(ok);
	if (ok) {
		CHECK(::access(dst_path.c_str(), F_OK) == 0);
		::unlink(dst_path.c_str());
	}
}
TEST_CASE(
	"async_atomic_write stages in nested parent and publishes atomically") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto dir = TempDir::create();
	dir.mkdir_sub("sub");

	std::string const payload = "async atomic nested content";
	conflux::file_io::block_on(
		fx->reader,
		fx->reader.async_atomic_write(dir.fd, std::string{"sub/out.txt"}, std::as_bytes(std::span{payload})),
		std::chrono::seconds{5});

	CHECK(dir.read_file("sub/out.txt") == payload);
	CHECK_FALSE(dir.has_staging_files());
	CHECK_FALSE(dir.has_staging_files("sub"));
}
TEST_CASE(
	"async_atomic_write create_new preserves existing target and removes staging") {
	auto fx = RingFixture::make();
	if (!fx) {
		SKIP("io_uring_queue_init failed");
	}
	auto dir = TempDir::create();
	dir.write_file("target.txt", "original");

	std::string const replacement = "replacement";
	int err = 0;
	try {
		conflux::file_io::block_on(
			fx->reader,
			fx->reader.async_atomic_write(
				dir.fd,
				std::string{"target.txt"},
				std::as_bytes(std::span{replacement}),
				0644,
				conflux::file_io_sync::TempPublishMode::create_new),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { err = se.code().value(); }

	CHECK(err == EEXIST);
	CHECK(dir.read_file("target.txt") == "original");
	CHECK_FALSE(dir.has_staging_files());
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
	} catch (...) { // NOLINT(bugprone-empty-catch)
	}
	CHECK(ok);
	if (ok) {
		struct stat st{};
		CHECK(::stat(path.c_str(), &st) == 0);
		CHECK(S_ISDIR(st.st_mode));
	}
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
