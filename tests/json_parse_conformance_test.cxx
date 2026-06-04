// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: whitespace is tolerated",
	"[json]") {
	auto doc = parse("  {  \"k\"  :  42  }  ");
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_object()->member("k")->as_number()->to_i64() == 42LL);
}

TEST_CASE(
	"json: reject leading zeros",
	"[json][conformance]") {
	CHECK_FALSE(parse("[012]").has_value());
	CHECK_FALSE(parse("[-01]").has_value());
	CHECK(parse("[0]").has_value());
	CHECK(parse("[-0]").has_value());
}

TEST_CASE(
	"json: reject trailing decimal point",
	"[json][conformance]") {
	CHECK_FALSE(parse("[-2.]").has_value());
	CHECK_FALSE(parse("[2.]").has_value());
	CHECK_FALSE(parse("[0.e1]").has_value());
}

TEST_CASE(
	"json: reject missing integer part",
	"[json][conformance]") {
	CHECK_FALSE(parse("[-.123]").has_value());
	CHECK_FALSE(parse("[.5]").has_value());
}

TEST_CASE(
	"json: deeply nested — within limit",
	"[json]") {
	std::string nested(100, '[');
	nested += "1";
	nested.append(100, ']');
	CHECK(parse(nested).has_value());
}
