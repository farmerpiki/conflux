// JSONTestSuite conformance gate (nst/JSONTestSuite).
// y_* must parse successfully, n_* must fail, i_* tracked without gating.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

import std;
import conflux.types;
import conflux.json;

#ifndef JSONTESTSUITE_DIR
	#error "JSONTESTSUITE_DIR must be defined via target_compile_definitions"
#endif

using namespace conflux::json;
namespace fs = std::filesystem;

static constexpr JsonParseOptions conformance_opts{
	.duplicate_key = DuplicateKeyPolicy::last_wins,
};
static auto test_files(
	SV prefix) -> V<fs::path> {
	V<fs::path> out;
	fs::path dir{JSONTESTSUITE_DIR};
	if (!fs::is_directory(dir)) {
		return out;
	}
	for (auto const &entry: fs::directory_iterator{dir}) {
		S name = entry.path().filename().generic_string();
		if (name.starts_with(prefix) && name.ends_with(".json")) {
			out.push_back(entry.path());
		}
	}
	ranges::sort(out);
	return out;
}
static auto read_file(
	fs::path const &p) -> S {
	std::ifstream f{p, std::ios::binary};
	return S{std::istreambuf_iterator<char>{f}, {}};
}
TEST_CASE(
	"JSONTestSuite: y_* must accept",
	"[jsontestsuite][y]") {
	auto files = test_files("y_");
	REQUIRE(!files.empty());
	for (auto const &p: files) {
		DYNAMIC_SECTION(p.filename().generic_string()) {
			auto content = read_file(p);
			auto result = parse(content, conformance_opts);
			CHECK(result.has_value());
		}
	}
}
TEST_CASE(
	"JSONTestSuite: n_* must reject",
	"[jsontestsuite][n]") {
	auto files = test_files("n_");
	REQUIRE(!files.empty());
	for (auto const &p: files) {
		DYNAMIC_SECTION(p.filename().generic_string()) {
			auto content = read_file(p);
			auto result = parse(content, conformance_opts);
			CHECK_FALSE(result.has_value());
		}
	}
}
TEST_CASE(
	"JSONTestSuite: i_* implementation-defined (tracked)",
	"[jsontestsuite][i][!mayfail]") {
	auto files = test_files("i_");
	REQUIRE(!files.empty());
	SZ accepted{};
	SZ rejected{};
	for (auto const &p: files) {
		auto content = read_file(p);
		auto result = parse(content);
		if (result.has_value()) {
			++accepted;
		} else {
			++rejected;
		}
	}
	INFO("i_* files: " << files.size() << " total, " << accepted << " accepted, " << rejected << " rejected");
	CHECK(true);
}
