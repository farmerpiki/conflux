// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: decode<std::map<std::string,i64>>",
	"[json][codec][map]") {
	auto doc = parse(R"({"a":1,"b":2,"c":3})");
	REQUIRE(doc.has_value());
	auto r = decode<std::map<std::string, std::int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 3UZ);
	CHECK((*r)["a"] == 1LL);
	CHECK((*r)["b"] == 2LL);
	CHECK((*r)["c"] == 3LL);
}

TEST_CASE(
	"json: decode<std::map<std::string,i64>> wrong kind yields wrong_kind",
	"[json][codec][map]") {
	auto doc = parse("[1,2,3]");
	REQUIRE(doc.has_value());
	auto r = decode<std::map<std::string, std::int64_t>>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::wrong_kind);
}

TEST_CASE(
	"json: decode<std::unordered_map<std::string,i64>>",
	"[json][codec][map]") {
	auto doc = parse(R"({"x":10,"y":20})");
	REQUIRE(doc.has_value());
	auto r = decode<std::unordered_map<std::string, std::int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 2UZ);
	CHECK((*r)["x"] == 10LL);
	CHECK((*r)["y"] == 20LL);
}
