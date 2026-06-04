// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: decode<std::pair<std::string,i64>>",
	"[json][codec][P]") {
	auto doc = parse(R"(["hello",42])");
	REQUIRE(doc.has_value());
	auto r = decode<std::pair<std::string, std::int64_t>>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->first == "hello");
	CHECK(r->second == 42LL);
}

TEST_CASE(
	"json: decode<std::pair<std::string,i64>> wrong length yields invalid_value",
	"[json][codec][P]") {
	auto doc = parse("[1]");
	REQUIRE(doc.has_value());
	auto r = decode<std::pair<std::string, std::int64_t>>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
}

TEST_CASE(
	"json: decode<std::tuple<bool,std::int64_t,S>>",
	"[json][codec][Tup]") {
	auto doc = parse(R"([true,99,"hi"])");
	REQUIRE(doc.has_value());
	auto r = decode<std::tuple<bool, std::int64_t, std::string>>(doc->root());
	REQUIRE(r.has_value());
	CHECK(get<0>(*r) == true);
	CHECK(get<1>(*r) == 99LL);
	CHECK(get<2>(*r) == "hi");
}

TEST_CASE(
	"json: decode<std::tuple<std::int64_t,i64>> wrong length yields invalid_value",
	"[json][codec][Tup]") {
	auto doc = parse("[1,2,3]");
	REQUIRE(doc.has_value());
	auto r = decode<std::tuple<std::int64_t, std::int64_t>>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
}
