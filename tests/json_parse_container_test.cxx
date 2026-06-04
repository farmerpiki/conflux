// Plain TU - not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: parse empty A",
	"[json]") {
	auto doc = parse("[]");
	REQUIRE(doc.has_value());
	auto a = doc->root().as_array();
	REQUIRE(a.has_value());
	CHECK(a->size() == 0UZ);
}
TEST_CASE(
	"json: parse A",
	"[json]") {
	auto doc = parse("[1, 2, 3]");
	REQUIRE(doc.has_value());
	auto a = doc->root().as_array();
	REQUIRE(a.has_value());
	CHECK(a->size() == 3UZ);
	CHECK(*(*a->element(0)).as_number()->to_i64() == 1LL);
	CHECK(*(*a->element(1)).as_number()->to_i64() == 2LL);
	CHECK(*(*a->element(2)).as_number()->to_i64() == 3LL);
}
TEST_CASE(
	"json: A element range",
	"[json]") {
	auto doc = parse("[10, 20, 30]");
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	std::vector<std::int64_t> vals;
	for (NodeRef const elem: a.elements()) {
		vals.push_back(*elem.as_number()->to_i64());
	}
	REQUIRE(vals.size() == 3UZ);
	CHECK(vals[0] == 10LL);
	CHECK(vals[1] == 20LL);
	CHECK(vals[2] == 30LL);
}
TEST_CASE(
	"json: A out-of-range error",
	"[json]") {
	auto doc = parse("[1]");
	REQUIRE(doc.has_value());
	auto res = doc->root().as_array()->element(99);
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::index_out_of_range);
}
TEST_CASE(
	"json: parse empty object",
	"[json]") {
	auto doc = parse("{}");
	REQUIRE(doc.has_value());
	auto o = doc->root().as_object();
	REQUIRE(o.has_value());
	CHECK(o->size() == 0UZ);
}
TEST_CASE(
	"json: parse object",
	"[json]") {
	auto doc = parse(R"({"a": 1, "b": "two"})");
	REQUIRE(doc.has_value());
	auto o = doc->root().as_object();
	REQUIRE(o.has_value());
	auto a = o->member("a");
	REQUIRE(a.has_value());
	CHECK(*a->as_number()->to_i64() == 1LL);
	auto b = o->member("b");
	REQUIRE(b.has_value());
	CHECK(*b->as_string() == "two");
}
TEST_CASE(
	"json: object member range",
	"[json]") {
	auto doc = parse(R"({"x": 1, "y": 2})");
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	std::unordered_map<std::string, std::int64_t> seen;
	for (auto [name, val]: o.members()) {
		seen[std::string{name}] = *val.as_number()->to_i64();
	}
	CHECK(seen["x"] == 1LL);
	CHECK(seen["y"] == 2LL);
}
TEST_CASE(
	"json: object find_member",
	"[json]") {
	auto doc = parse(R"({"k": 42})");
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(o.find_member("k").has_value());
	CHECK_FALSE(o.find_member("missing").has_value());
}
TEST_CASE(
	"json: object missing member error",
	"[json]") {
	auto doc = parse(R"({"a": 1})");
	REQUIRE(doc.has_value());
	auto res = doc->root().as_object()->member("missing");
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::missing_member);
}
