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
	S path{};
	int fd{-1};
	TempDir() = default;
	TempDir(
		S p,
		int f) noexcept
		: path{move(p)}
		, fd{f} {}
	TempDir(TempDir const &) = delete;
	TempDir &operator =(TempDir const &) = delete;
	TempDir(
		TempDir &&o) noexcept
		: path{move(o.path)}
		, fd{exchange(o.fd, -1)} {}
	TempDir &operator =(TempDir &&) = delete;
	~TempDir() {
		if (fd >= 0) {
			::close(fd);
		}
		if (!path.empty()) {
			auto cmd = format("rm -rf {}", path);
			auto _ = ::system(cmd.c_str());
		}
	}
	static TempDir create() {
		S p = "/tmp/conflux_fio_sync_XXXXXX";
		auto *r = ::mkdtemp(p.data());
		REQUIRE(r != nullptr);
		int f = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		REQUIRE(f >= 0);
		return TempDir{move(p), f};
	}
	S read_file(
		SV name) const {
		auto full = format("{}/{}", path, name);
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
		return S{buf, static_cast<SZ>(n)};
	}
	bool file_exists(
		SV name) const {
		auto full = format("{}/{}", path, name);
		struct stat st{};
		return ::stat(full.c_str(), &st) == 0;
	}
	void write_file(
		SV name,
		SV content) const {
		auto full = format("{}/{}", path, name);
		int f = ::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(f >= 0);
		auto _ = ::write(f, content.data(), content.size());
		::close(f);
	}
	void mkdir_sub(
		SV name) const {
		auto full = format("{}/{}", path, name);
		auto _ = ::mkdir(full.c_str(), 0755);
	}
};

} // namespace
TEST_CASE(
	"file_io_sync: blocking_open_tmpfile creates writable unnamed temp",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto result = blocking_open_tmpfile(dir.fd);
	REQUIRE(result.has_value());
	CHECK(result->fd() >= 0);
	CHECK(result->unnamed());
}
TEST_CASE(
	"file_io_sync: blocking_write_file_atomic_at creates new file",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	SV text = "hello atomic world";
	auto r = blocking_write_text_file_atomic_at(dir.fd, SV{"newfile.txt"}, text);
	REQUIRE(r.has_value());
	CHECK(dir.read_file("newfile.txt") == text);
}
TEST_CASE(
	"file_io_sync: blocking_write_file_atomic_at replaces existing file",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("target.txt", "old content");
	auto r = blocking_write_text_file_atomic_at(dir.fd, SV{"target.txt"}, SV{"new content"});
	REQUIRE(r.has_value());
	CHECK(dir.read_file("target.txt") == "new content");
}
TEST_CASE(
	"file_io_sync: failed publish does not remove existing target",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("keep.txt", "original");
	auto r = blocking_write_text_file_atomic_at(
		dir.fd,
		SV{"keep.txt"},
		SV{"overwrite"},
		TempFileOptions{},
		TempPublishMode::create_new);
	CHECK(!r.has_value());
	CHECK(dir.read_file("keep.txt") == "original");
}
TEST_CASE(
	"file_io_sync: nested relative path stays below root",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.mkdir_sub("sub");
	auto r = blocking_write_text_file_atomic_at(dir.fd, SV{"sub/nested.txt"}, SV{"deep"});
	REQUIRE(r.has_value());
	CHECK(dir.read_file("sub/nested.txt") == "deep");
}
TEST_CASE(
	"file_io_sync: absolute path rejected",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = blocking_write_text_file_atomic_at(dir.fd, SV{"/etc/passwd"}, SV{"nope"});
	CHECK(!r.has_value());
	CHECK(r.error().code().value() == EINVAL);
}
TEST_CASE(
	"file_io_sync: .. path rejected",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = blocking_write_text_file_atomic_at(dir.fd, SV{"../escape.txt"}, SV{"nope"});
	CHECK(!r.has_value());
	CHECK(r.error().code().value() == EINVAL);
}
TEST_CASE(
	"file_io_sync: named-temp fallback works when O_TMPFILE disabled",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = blocking_write_text_file_atomic_at(
		dir.fd,
		SV{"fallback.txt"},
		SV{"via named"},
		TempFileOptions{.prefer_otmpfile = false});
	REQUIRE(r.has_value());
	CHECK(dir.read_file("fallback.txt") == "via named");
}
TEST_CASE(
	"file_io_sync: file_and_directory durability path runs without error",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = blocking_write_text_file_atomic_at(
		dir.fd,
		SV{"durable.txt"},
		SV{"synced"},
		TempFileOptions{.durability = TempDurability::file_and_directory});
	REQUIRE(r.has_value());
	CHECK(dir.read_file("durable.txt") == "synced");
}
TEST_CASE(
	"file_io_sync: create_new fails if target exists",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("exists.txt", "present");
	auto r = blocking_write_text_file_atomic_at(
		dir.fd,
		SV{"exists.txt"},
		SV{"replace"},
		TempFileOptions{},
		TempPublishMode::create_new);
	CHECK(!r.has_value());
	CHECK(dir.file_exists("exists.txt"));
	CHECK(dir.read_file("exists.txt") == "present");
}
TEST_CASE(
	"file_io_sync: create_new succeeds for new file",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = blocking_write_text_file_atomic_at(
		dir.fd,
		SV{"brand_new.txt"},
		SV{"fresh"},
		TempFileOptions{},
		TempPublishMode::create_new);
	REQUIRE(r.has_value());
	CHECK(dir.read_file("brand_new.txt") == "fresh");
}
TEST_CASE(
	"file_io_sync: durability none skips fsync",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto r = blocking_write_text_file_atomic_at(
		dir.fd,
		SV{"fast.txt"},
		SV{"no sync"},
		TempFileOptions{.durability = TempDurability::none});
	REQUIRE(r.has_value());
	CHECK(dir.read_file("fast.txt") == "no sync");
}
TEST_CASE(
	"file_io_sync: empty and dot paths rejected",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	CHECK(!blocking_write_text_file_atomic_at(dir.fd, SV{""}, SV{"data"}).has_value());
	CHECK(!blocking_write_text_file_atomic_at(dir.fd, SV{"."}, SV{"data"}).has_value());
	CHECK(!blocking_write_text_file_atomic_at(dir.fd, SV{".."}, SV{"data"}).has_value());
}
TEST_CASE(
	"file_io_sync: binary blocking_write_file_atomic_at round-trips bytes",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	A<byte, 4> bytes{byte{0xDE}, byte{0xAD}, byte{0xBE}, byte{0xEF}};
	auto r = blocking_write_file_atomic_at(dir.fd, SV{"binary.bin"}, span{bytes});
	REQUIRE(r.has_value());
	auto content = dir.read_file("binary.bin");
	REQUIRE(content.size() == 4);
	CHECK(static_cast<u8>(content[0]) == 0xDE);
	CHECK(static_cast<u8>(content[1]) == 0xAD);
	CHECK(static_cast<u8>(content[2]) == 0xBE);
	CHECK(static_cast<u8>(content[3]) == 0xEF);
}

TEST_CASE(
	"file_io_sync: blocking_read_file_at reads contained relative files",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("data.txt", "hello read");
	auto content = blocking_read_file_at(dir.fd, "data.txt");
	REQUIRE(content.has_value());
	CHECK(*content == "hello read");
}

TEST_CASE(
	"file_io_sync: blocking_read_file_at enforces byte limit",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("data.txt", "hello read");
	auto content = blocking_read_file_at(dir.fd, "data.txt", 4);
	REQUIRE_FALSE(content.has_value());
	CHECK(content.error().code().value() == EFBIG);
}

TEST_CASE(
	"file_io_sync: blocking_openat_contained opens files below root only",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("safe.txt", "safe");
	auto fd = blocking_openat_contained(dir.fd, "safe.txt", O_RDONLY);
	REQUIRE(fd.has_value());
	auto content = blocking_read_all_fd(fd->fd());
	REQUIRE(content.has_value());
	CHECK(*content == "safe");

	auto escaped = blocking_openat_contained(dir.fd, "../safe.txt", O_RDONLY);
	REQUIRE_FALSE(escaped.has_value());
	CHECK(escaped.error().code().value() == EINVAL);
}

TEST_CASE(
	"file_io_sync: blocking_read_text_file reads absolute paths with limit",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	dir.write_file("text.txt", "hello text");
	auto path = format("{}/{}", dir.path, "text.txt");
	auto content = blocking_read_text_file(path);
	REQUIRE(content.has_value());
	CHECK(*content == "hello text");

	auto too_small = blocking_read_text_file(path, 4);
	REQUIRE_FALSE(too_small.has_value());
	CHECK(too_small.error().code().value() == EFBIG);
	CHECK(blocking_read_text_file_nothrow("/tmp/conflux_missing_no_std_streams_file").has_value() == false);
}

TEST_CASE(
	"file_io_sync: legacy sync spellings remain available",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	SV text = "legacy sync aliases";
	auto tmp = open_tmpfile_sync(dir.fd, TempFileOptions{.prefer_otmpfile = false});
	REQUIRE(tmp.has_value());

	auto wr = write_all_fd(tmp->fd(), as_bytes(span{text.data(), text.size()}));
	REQUIRE(wr.has_value());
	auto pub = publish_tmpfile_sync(move(*tmp), dir.fd, SV{"legacy.txt"});
	REQUIRE(pub.has_value());

	auto file = openat_contained_sync(dir.fd, "legacy.txt", O_RDONLY);
	REQUIRE(file.has_value());
	auto bytes = read_all_fd(file->fd());
	REQUIRE(bytes.has_value());
	CHECK(*bytes == text);
}

TEST_CASE(
	"file_io_sync: blocking low-level aliases round-trip through contained file",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	SV text = "blocking aliases";
	auto tmp = blocking_open_tmpfile(dir.fd, TempFileOptions{.prefer_otmpfile = false});
	REQUIRE(tmp.has_value());

	auto wr = blocking_write_all_fd(tmp->fd(), as_bytes(span{text.data(), text.size()}));
	REQUIRE(wr.has_value());
	auto pub = blocking_publish_tmpfile(move(*tmp), dir.fd, SV{"alias.txt"});
	REQUIRE(pub.has_value());

	auto stat = blocking_stat_at(dir.fd, SV{"alias.txt"});
	REQUIRE(stat.has_value());
	CHECK(stat->size == text.size());

	auto content = blocking_read_file_at(dir.fd, "alias.txt");
	REQUIRE(content.has_value());
	CHECK(*content == text);

	auto full = format("{}/{}", dir.path, "alias.txt");
	UniqueFd fd{::open(full.c_str(), O_RDONLY | O_CLOEXEC)};
	REQUIRE(fd.valid());
	auto fd_stat = blocking_fstat(fd.fd());
	REQUIRE(fd_stat.has_value());
	auto fd_content = blocking_read_all_fd(fd.fd());
	REQUIRE(fd_content.has_value());
	CHECK(*fd_content == text);

	auto by_path = blocking_map_file_readonly(dir.fd, SV{"alias.txt"});
	REQUIRE(by_path.has_value());
	CHECK(SV{reinterpret_cast<char const *>(by_path->bytes().data()), by_path->bytes().size()} == text);

	auto by_fd = blocking_map_fd_readonly(fd.fd(), *fd_stat);
	REQUIRE(by_fd.has_value());
	CHECK(SV{reinterpret_cast<char const *>(by_fd->bytes().data()), by_fd->bytes().size()} == text);
}

TEST_CASE(
	"file_io_sync: blocking atomic write aliases round-trip text and bytes",
	"[file_io_sync][unit]") {
	auto dir = TempDir::create();
	auto text = blocking_write_text_file_atomic_at(dir.fd, SV{"text.txt"}, SV{"new name"});
	REQUIRE(text.has_value());
	CHECK(dir.read_file("text.txt") == "new name");

	A<byte, 3> bytes{byte{0x41}, byte{0x42}, byte{0x43}};
	auto binary = blocking_write_file_atomic_at(dir.fd, SV{"bytes.bin"}, span{bytes});
	REQUIRE(binary.has_value());
	CHECK(dir.read_file("bytes.bin") == "ABC");
}
