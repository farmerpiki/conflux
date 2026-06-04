// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: decode<std::array<std::int64_t,3>>",
	"[json][codec][A]") {
	auto doc = parse("[10,20,30]");
	REQUIRE(doc.has_value());
	auto r = decode<std::array<std::int64_t, 3>>(doc->root());
	REQUIRE(r.has_value());
	CHECK((*r)[0] == 10LL);
	CHECK((*r)[1] == 20LL);
	CHECK((*r)[2] == 30LL);
}

TEST_CASE(
	"json: decode<std::array<std::int64_t,3>> wrong length yields invalid_value",
	"[json][codec][A]") {
	auto doc = parse("[1,2]");
	REQUIRE(doc.has_value());
	auto r = decode<std::array<std::int64_t, 3>>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
}
