#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

import std;
import conflux.types;
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
			fx->reader.async_fgetxattr(handle, xattr_name, std::span<char>{buf.data(), buf.size()}),
			std::chrono::seconds{5});
	} catch (std::system_error const &se) { get_err = se.code().value(); } catch (...) {
	}

	CHECK(get_err == 0);
	REQUIRE(got == xattr_val.size());
	CHECK(std::string_view{buf.data(), got} == xattr_val);
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
