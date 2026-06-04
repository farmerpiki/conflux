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
import conflux.file_io_sync;

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

} // namespace

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
