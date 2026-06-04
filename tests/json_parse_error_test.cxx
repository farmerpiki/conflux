// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: wrong-kind errors",
	"[json]") {
	auto doc = parse("42");
	REQUIRE(doc.has_value());
	NodeRef const root = doc->root();
	CHECK_FALSE(root.as_bool().has_value());
	CHECK(root.as_bool().error().code == JsonIssueCode::wrong_kind);
	CHECK_FALSE(root.as_string().has_value());
	CHECK_FALSE(root.as_object().has_value());
	CHECK_FALSE(root.as_array().has_value());
}

TEST_CASE(
	"json: parse error — truncated",
	"[json]") {
	CHECK_FALSE(parse(R"({"a":)").has_value());
}

TEST_CASE(
	"json: parse error — invalid token",
	"[json]") {
	CHECK_FALSE(parse("xyz").has_value());
}

TEST_CASE(
	"json: parse error — trailing garbage",
	"[json]") {
	CHECK_FALSE(parse("42 garbage").has_value());
}
