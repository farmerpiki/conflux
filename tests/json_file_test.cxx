#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.file_io_sync;
import conflux.json;
import conflux.json.file;

using namespace conflux::json;

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
		std::string p = "/tmp/conflux_json_file_XXXXXX";
		auto *r = ::mkdtemp(p.data());
		REQUIRE(r != nullptr);
		int f = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		REQUIRE(f >= 0);
		return TempDir{std::move(p), f};
	}
};

} // namespace

TEST_CASE(
	"json_file: blocking_parse_file_at reads and parses via explicit component",
	"[json_file][unit]") {
	auto dir = TempDir::create();
	auto wr = blocking_write_text_file_atomic_at(dir.fd, "config.json", R"({"answer":42,"name":"ok"})");
	REQUIRE(wr.has_value());

	auto doc = blocking_parse_file_at(dir.fd, "config.json");
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	CHECK(*obj->member("answer")->as_number()->to_i64() == 42LL);
	CHECK(*obj->member("name")->as_string() == "ok");
}

TEST_CASE(
	"json_file: blocking_parse_file_at reports parse failures separately from read failures",
	"[json_file][unit]") {
	auto dir = TempDir::create();
	auto wr = blocking_write_text_file_atomic_at(dir.fd, "bad.json", R"({"broken": )");
	REQUIRE(wr.has_value());

	auto doc = blocking_parse_file_at(dir.fd, "bad.json");
	REQUIRE_FALSE(doc.has_value());
	CHECK(doc.error().is_parse_error());
	REQUIRE(doc.error().json.has_value());
	CHECK(doc.error().json->stage == JsonStage::parse);
}

TEST_CASE(
	"json_file: blocking_parse_file_at uses parse max_input_size as read limit",
	"[json_file][unit]") {
	auto dir = TempDir::create();
	auto wr = blocking_write_text_file_atomic_at(dir.fd, "config.json", R"({"answer":42})");
	REQUIRE(wr.has_value());

	JsonParseOptions opts{};
	opts.max_input_size = LimitOption::bound(4);
	auto doc = blocking_parse_file_at(dir.fd, "config.json", opts);
	REQUIRE_FALSE(doc.has_value());
	CHECK(doc.error().is_file_error());
	CHECK(doc.error().file_errno == EFBIG);
}

TEST_CASE(
	"json_file: blocking parse aliases read and parse via explicit component",
	"[json_file][unit]") {
	auto dir = TempDir::create();
	auto wr = blocking_write_text_file_atomic_at(dir.fd, "config.json", R"({"answer":43})");
	REQUIRE(wr.has_value());

	auto doc = blocking_parse_file_at(dir.fd, "config.json");
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	CHECK(*obj->member("answer")->as_number()->to_i64() == 43LL);
}
