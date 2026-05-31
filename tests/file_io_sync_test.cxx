#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.file_io_sync;
import conflux.file_map;
namespace {

struct TempDir {
	std::string path{};
	int fd{-1};
	TempDir() = default;
	TempDir(
		std::string p,
		int f) noexcept
		: path{std::move(p)}
		, fd{f} {}
	TempDir(TempDir const &) = delete;
	TempDir &operator =(TempDir const &) = delete;
	TempDir(
		TempDir &&o) noexcept
		: path{std::move(o.path)}
		, fd{std::exchange(o.fd, -1)} {}
	TempDir &operator =(TempDir &&) = delete;
	~TempDir() {
		if (fd >= 0) {
			::close(fd);
		}
		if (!path.empty()) {
			std::error_code ec;
			std::filesystem::remove_all(path, ec);
		}
	}
	static TempDir create() {
		std::string p = "/tmp/conflux_fio_sync_XXXXXX";
		auto *r = ::mkdtemp(p.data());
		REQUIRE(r != nullptr);
		int f = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		REQUIRE(f >= 0);
		return TempDir{std::move(p), f};
	}
	std::string read_file(
		std::string_view name) const {
		auto full = std::format("{}/{}", path, name);
		int f = ::open(full.c_str(), O_RDONLY);
		if (f < 0) {
			return {};
		}
		char buf[4096];
		auto n = ::read(f, buf, sizeof(buf));
		::close(f);
		if (n < 0) {
			return {};
		}
		return std::string{buf, static_cast<std::size_t>(n)};
	}
	bool file_exists(
		std::string_view name) const {
		auto full = std::format("{}/{}", path, name);
		struct stat st{};
		return ::stat(full.c_str(), &st) == 0;
	}
	void write_file(
		std::string_view name,
		std::string_view content) const {
		auto full = std::format("{}/{}", path, name);
		int f = ::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(f >= 0);
		auto _ = ::write(f, content.data(), content.size());
		::close(f);
	}
	void mkdir_sub(
		std::string_view name) const {
		auto full = std::format("{}/{}", path, name);
		auto _ = ::mkdir(full.c_str(), 0755);
	}
};

} // namespace
TEST_CASE(
	"file_io_sync: conflux::file_io_sync::blocking_open_tmpfile creates writable unnamed temp",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto result = conflux::file_io_sync::blocking_open_tmpfile(dir.fd);
	REQUIRE(result.has_value());
	CHECK(result->fd() >= 0);
	CHECK(result->unnamed());
}
TEST_CASE(
	"file_io_sync: conflux::file_io_sync::blocking_write_file_atomic_at creates new file",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	std::string_view text = "hello atomic world";
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(dir.fd, std::string_view{"newfile.txt"}, text);
	REQUIRE(r.has_value());
	CHECK(dir.read_file("newfile.txt") == text);
}
TEST_CASE(
	"file_io_sync: conflux::file_io_sync::blocking_write_file_atomic_at replaces existing file",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("target.txt", "old content");
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"target.txt"},
		std::string_view{"new content"});
	REQUIRE(r.has_value());
	CHECK(dir.read_file("target.txt") == "new content");
}
TEST_CASE(
	"file_io_sync: failed publish does not remove existing target",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("keep.txt", "original");
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"keep.txt"},
		std::string_view{"overwrite"},
		conflux::file_io_sync::TempFileOptions{},
		conflux::file_io_sync::TempPublishMode::create_new);
	CHECK(!r.has_value());
	CHECK(dir.read_file("keep.txt") == "original");
}
TEST_CASE(
	"file_io_sync: nested relative path stays below root",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.mkdir_sub("sub");
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"sub/nested.txt"},
		std::string_view{"deep"});
	REQUIRE(r.has_value());
	CHECK(dir.read_file("sub/nested.txt") == "deep");
}
TEST_CASE(
	"file_io_sync: absolute path rejected",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"/etc/passwd"},
		std::string_view{"nope"});
	CHECK(!r.has_value());
	CHECK(r.error().code().value() == EINVAL);
}
TEST_CASE(
	"file_io_sync: .. path rejected",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"../escape.txt"},
		std::string_view{"nope"});
	CHECK(!r.has_value());
	CHECK(r.error().code().value() == EINVAL);
}
TEST_CASE(
	"file_io_sync: named-temp fallback works when O_TMPFILE disabled",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"fallback.txt"},
		std::string_view{"via named"},
		conflux::file_io_sync::TempFileOptions{.prefer_otmpfile = false});
	REQUIRE(r.has_value());
	CHECK(dir.read_file("fallback.txt") == "via named");
}
TEST_CASE(
	"file_io_sync: file_and_directory durability path runs without error",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"durable.txt"},
		std::string_view{"synced"},
		conflux::file_io_sync::TempFileOptions{
			.durability = conflux::file_io_sync::TempDurability::file_and_directory});
	REQUIRE(r.has_value());
	CHECK(dir.read_file("durable.txt") == "synced");
}
TEST_CASE(
	"file_io_sync: create_new fails if target exists",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("exists.txt", "present");
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"exists.txt"},
		std::string_view{"replace"},
		conflux::file_io_sync::TempFileOptions{},
		conflux::file_io_sync::TempPublishMode::create_new);
	CHECK(!r.has_value());
	CHECK(dir.file_exists("exists.txt"));
	CHECK(dir.read_file("exists.txt") == "present");
}
TEST_CASE(
	"file_io_sync: create_new succeeds for new file",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"brand_new.txt"},
		std::string_view{"fresh"},
		conflux::file_io_sync::TempFileOptions{},
		conflux::file_io_sync::TempPublishMode::create_new);
	REQUIRE(r.has_value());
	CHECK(dir.read_file("brand_new.txt") == "fresh");
}
TEST_CASE(
	"file_io_sync: durability none skips fsync",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"fast.txt"},
		std::string_view{"no sync"},
		conflux::file_io_sync::TempFileOptions{.durability = conflux::file_io_sync::TempDurability::none});
	REQUIRE(r.has_value());
	CHECK(dir.read_file("fast.txt") == "no sync");
}
TEST_CASE(
	"file_io_sync: empty and dot paths rejected",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	CHECK(!conflux::file_io_sync::blocking_write_text_file_atomic_at(
			   dir.fd,
			   std::string_view{""},
			   std::string_view{"data"})
			   .has_value());
	CHECK(!conflux::file_io_sync::blocking_write_text_file_atomic_at(
			   dir.fd,
			   std::string_view{"."},
			   std::string_view{"data"})
			   .has_value());
	CHECK(!conflux::file_io_sync::blocking_write_text_file_atomic_at(
			   dir.fd,
			   std::string_view{".."},
			   std::string_view{"data"})
			   .has_value());
}
TEST_CASE(
	"file_io_sync: binary conflux::file_io_sync::blocking_write_file_atomic_at round-trips bytes",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	std::array<std::byte, 4> bytes{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
	auto r =
		conflux::file_io_sync::blocking_write_file_atomic_at(dir.fd, std::string_view{"binary.bin"}, std::span{bytes});
	REQUIRE(r.has_value());
	auto content = dir.read_file("binary.bin");
	REQUIRE(content.size() == 4);
	CHECK(static_cast<std::uint8_t>(content[0]) == 0xDE);
	CHECK(static_cast<std::uint8_t>(content[1]) == 0xAD);
	CHECK(static_cast<std::uint8_t>(content[2]) == 0xBE);
	CHECK(static_cast<std::uint8_t>(content[3]) == 0xEF);
}

TEST_CASE(
	"file_io_sync: conflux::file_io_sync::blocking_read_file_at reads contained relative files",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("data.txt", "hello read");
	auto content = conflux::file_io_sync::blocking_read_file_at(dir.fd, "data.txt");
	REQUIRE(content.has_value());
	CHECK(*content == "hello read");
}

TEST_CASE(
	"file_io_sync: conflux::file_io_sync::blocking_read_file_at enforces byte limit",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("data.txt", "hello read");
	auto content = conflux::file_io_sync::blocking_read_file_at(dir.fd, "data.txt", 4);
	REQUIRE_FALSE(content.has_value());
	CHECK(content.error().code().value() == EFBIG);
}

TEST_CASE(
	"file_io_sync: conflux::file_io_sync::blocking_openat_contained opens files below root only",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("safe.txt", "safe");
	auto fd = conflux::file_io_sync::blocking_openat_contained(dir.fd, "safe.txt", O_RDONLY);
	REQUIRE(fd.has_value());
	auto content = conflux::file_io_sync::blocking_read_all_fd(fd->fd());
	REQUIRE(content.has_value());
	CHECK(*content == "safe");

	auto escaped = conflux::file_io_sync::blocking_openat_contained(dir.fd, "../safe.txt", O_RDONLY);
	REQUIRE_FALSE(escaped.has_value());
	CHECK(escaped.error().code().value() == EINVAL);
}

TEST_CASE(
	"file_io_sync: conflux::file_io_sync::blocking_read_text_file reads absolute paths with limit",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("text.txt", "hello text");
	auto path = std::format("{}/{}", dir.path, "text.txt");
	auto content = conflux::file_io_sync::blocking_read_text_file(path);
	REQUIRE(content.has_value());
	CHECK(*content == "hello text");

	auto too_small = conflux::file_io_sync::blocking_read_text_file(path, 4);
	REQUIRE_FALSE(too_small.has_value());
	CHECK(too_small.error().code().value() == EFBIG);
	CHECK(
		conflux::file_io_sync::blocking_read_text_file_nothrow("/tmp/conflux_missing_no_std_streams_file").has_value()
		== false);
}

TEST_CASE(
	"file_io_sync: legacy sync spellings remain available",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	std::string_view text = "legacy sync aliases";
	auto tmp = conflux::file_io_sync::blocking_open_tmpfile(
		dir.fd,
		conflux::file_io_sync::TempFileOptions{.prefer_otmpfile = false});
	REQUIRE(tmp.has_value());

	auto wr = conflux::file_io_sync::write_all_fd(tmp->fd(), std::as_bytes(std::span{text.data(), text.size()}));
	REQUIRE(wr.has_value());
	auto pub = conflux::file_io_sync::blocking_publish_tmpfile(std::move(*tmp), dir.fd, std::string_view{"legacy.txt"});
	REQUIRE(pub.has_value());

	auto file = conflux::file_io_sync::blocking_openat_contained(dir.fd, "legacy.txt", O_RDONLY);
	REQUIRE(file.has_value());
	auto bytes = conflux::file_io_sync::read_all_fd(file->fd());
	REQUIRE(bytes.has_value());
	CHECK(*bytes == text);
}

TEST_CASE(
	"file_io_sync: low-level publish validates final basename",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	for (auto name: {std::string_view{""}, std::string_view{"."}, std::string_view{".."}, std::string_view{"a/b"}}) {
		auto tmp = conflux::file_io_sync::blocking_open_tmpfile(
			dir.fd,
			conflux::file_io_sync::TempFileOptions{.prefer_otmpfile = false});
		REQUIRE(tmp.has_value());
		auto pub = conflux::file_io_sync::blocking_publish_tmpfile(std::move(*tmp), dir.fd, name);
		REQUIRE_FALSE(pub.has_value());
		CHECK(pub.error().code().value() == EINVAL);
	}
}

TEST_CASE(
	"file_io_sync: blocking low-level aliases round-trip through contained file",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	std::string_view text = "blocking aliases";
	auto tmp = conflux::file_io_sync::blocking_open_tmpfile(
		dir.fd,
		conflux::file_io_sync::TempFileOptions{.prefer_otmpfile = false});
	REQUIRE(tmp.has_value());

	auto wr =
		conflux::file_io_sync::blocking_write_all_fd(tmp->fd(), std::as_bytes(std::span{text.data(), text.size()}));
	REQUIRE(wr.has_value());
	auto pub = conflux::file_io_sync::blocking_publish_tmpfile(std::move(*tmp), dir.fd, std::string_view{"alias.txt"});
	REQUIRE(pub.has_value());

	auto stat = conflux::file_io_sync::blocking_stat_at(dir.fd, std::string_view{"alias.txt"});
	REQUIRE(stat.has_value());
	CHECK(stat->size == text.size());

	auto content = conflux::file_io_sync::blocking_read_file_at(dir.fd, "alias.txt");
	REQUIRE(content.has_value());
	CHECK(*content == text);

	auto full = std::format("{}/{}", dir.path, "alias.txt");
	conflux::file_io_sync::UniqueFd fd{::open(full.c_str(), O_RDONLY | O_CLOEXEC)};
	REQUIRE(fd.valid());
	auto fd_stat = conflux::file_io_sync::blocking_fstat(fd.fd());
	REQUIRE(fd_stat.has_value());
	auto fd_content = conflux::file_io_sync::blocking_read_all_fd(fd.fd());
	REQUIRE(fd_content.has_value());
	CHECK(*fd_content == text);

	auto by_path = conflux::file_map::blocking_map_file_readonly(dir.fd, std::string_view{"alias.txt"});
	REQUIRE(by_path.has_value());
	CHECK(std::string_view{reinterpret_cast<char const *>(by_path->bytes().data()), by_path->bytes().size()} == text);

	auto by_fd = conflux::file_map::blocking_map_fd_readonly(fd.fd(), *fd_stat);
	REQUIRE(by_fd.has_value());
	CHECK(std::string_view{reinterpret_cast<char const *>(by_fd->bytes().data()), by_fd->bytes().size()} == text);
}

TEST_CASE(
	"file_io_sync: blocking atomic write aliases round-trip text and bytes",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto text = conflux::file_io_sync::blocking_write_text_file_atomic_at(
		dir.fd,
		std::string_view{"text.txt"},
		std::string_view{"new name"});
	REQUIRE(text.has_value());
	CHECK(dir.read_file("text.txt") == "new name");

	std::array<std::byte, 3> bytes{std::byte{0x41}, std::byte{0x42}, std::byte{0x43}};
	auto binary =
		conflux::file_io_sync::blocking_write_file_atomic_at(dir.fd, std::string_view{"bytes.bin"}, std::span{bytes});
	REQUIRE(binary.has_value());
	CHECK(dir.read_file("bytes.bin") == "ABC");
}
