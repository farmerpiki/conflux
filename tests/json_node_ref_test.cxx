// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: default NodeRef is invalid",
	"[json][noderef]") {
	NodeRef node;
	CHECK_FALSE(node.is_valid());

	auto doc = parse("null");
	REQUIRE(doc.has_value());
	CHECK(doc->root().is_valid());
	CHECK(doc->root().is_null());
}

TEST_CASE(
	"json: is_same_node — same document, same node",
	"[json][noderef]") {
	auto doc = parse("[1,2]");
	REQUIRE(doc.has_value());
	NodeRef const r = doc->root();
	CHECK(is_same_node(r, r));
}

TEST_CASE(
	"json: is_same_node — different nodes in same document",
	"[json][noderef]") {
	auto doc = parse("[1,2]");
	REQUIRE(doc.has_value());
	auto a = doc->root().as_array();
	REQUIRE(a.has_value());
	auto n0 = a->element(0);
	auto n1 = a->element(1);
	REQUIRE(n0.has_value());
	REQUIRE(n1.has_value());
	CHECK_FALSE(is_same_node(*n0, *n1));
}

TEST_CASE(
	"json: is_value_equal — identical scalars",
	"[json][noderef]") {
	auto da = parse("42");
	auto db = parse("42");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK(is_value_equal(da->root(), db->root()));
}

TEST_CASE(
	"json: is_value_equal — 1 and 1.0 are equal (math value)",
	"[json][noderef]") {
	auto da = parse("1");
	auto db = parse("1.0");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK(is_value_equal(da->root(), db->root()));
}

TEST_CASE(
	"json: is_value_equal — different kinds",
	"[json][noderef]") {
	auto da = parse("1");
	auto db = parse("\"1\"");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK_FALSE(is_value_equal(da->root(), db->root()));
}

TEST_CASE(
	"json: is_value_equal — objects are order-insensitive",
	"[json][noderef]") {
	auto da = parse(R"({"a":1,"b":2})");
	auto db = parse(R"({"b":2,"a":1})");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK(is_value_equal(da->root(), db->root()));
}

TEST_CASE(
	"json: is_value_equal_exact — 1 and 1.0 are not equal",
	"[json][noderef]") {
	auto da = parse("1");
	auto db = parse("1.0");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK_FALSE(is_value_equal_exact(da->root(), db->root()));
}

TEST_CASE(
	"json: is_value_equal_exact — identical lexemes are equal",
	"[json][noderef]") {
	auto da = parse("1");
	auto db = parse("1");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK(is_value_equal_exact(da->root(), db->root()));
}
