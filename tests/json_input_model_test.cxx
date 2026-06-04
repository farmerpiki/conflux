// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: BOM at start is skipped",
	"[json][input]") {
	std::string_view bom_json = "\xEF\xBB\xBF\"hello\"";
	auto res = parse(bom_json);
	REQUIRE(res.has_value());
	CHECK(*res->root().as_string() == "hello");
}

TEST_CASE(
	"json: invalid UTF-8 is rejected",
	"[json][input]") {
	std::string_view bad = "\"\x80\"";
	auto res = parse(bad);
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::invalid_utf8);
}
