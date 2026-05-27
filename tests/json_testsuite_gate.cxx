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

static constexpr JsonParseOptions conformance_opts{
	.duplicate_key = DuplicateKeyPolicy::last_wins,
};
static auto test_files(
	std::string_view prefix) -> std::vector<std::filesystem::path> {
	std::vector<std::filesystem::path> out;
	std::filesystem::path dir{JSONTESTSUITE_DIR};
	if (!std::filesystem::is_directory(dir)) {
		return out;
	}
	for (auto const &entry: std::filesystem::directory_iterator{dir}) {
		std::string name = entry.path().filename().generic_string();
		if (name.starts_with(prefix) && name.ends_with(".json")) {
			out.push_back(entry.path());
		}
	}
	std::ranges::sort(out);
	return out;
}
static auto read_file(
	std::filesystem::path const &p) -> std::string {
	std::ifstream f{p, std::ios::binary};
	return std::string{std::istreambuf_iterator<char>{f}, {}};
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
