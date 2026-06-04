// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

using namespace conflux::json;

TEST_CASE(
	"json: JsonPath from_pointer roundtrip",
	"[json][path]") {
	std::string_view ptr = "/a/b/c";
	auto path = JsonPath::from_pointer(ptr);
	REQUIRE(path.has_value());
	CHECK(path->to_pointer() == ptr);
}

TEST_CASE(
	"json: JsonPath from_pointer with escapes",
	"[json][path]") {
	auto path = JsonPath::from_pointer("/a~0b/c~1d");
	REQUIRE(path.has_value());
	CHECK(path->to_pointer() == "/a~0b/c~1d");
}

TEST_CASE(
	"json: JsonPath empty pointer is root",
	"[json][path]") {
	auto path = JsonPath::from_pointer("");
	REQUIRE(path.has_value());
	CHECK(path->empty());
	CHECK(path->to_pointer().empty());
}

TEST_CASE(
	"json: JsonPath must start with /",
	"[json][path]") {
	auto path = JsonPath::from_pointer("noslash");
	CHECK_FALSE(path.has_value());
	CHECK(path.error().code == JsonIssueCode::invalid_pointer);
}
