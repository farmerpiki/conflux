// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: Nullable<std::int64_t> — present value",
	"[json][nullable]") {
	auto doc = parse("42");
	REQUIRE(doc.has_value());
	auto res = decode<Nullable<std::int64_t>>(doc->root());
	REQUIRE(res.has_value());
	CHECK(res->has_value());
	CHECK(**res == 42LL);
}

TEST_CASE(
	"json: Nullable<std::int64_t> — null input",
	"[json][nullable]") {
	auto doc = parse("null");
	REQUIRE(doc.has_value());
	auto res = decode<Nullable<std::int64_t>>(doc->root());
	REQUIRE(res.has_value());
	CHECK(res->is_null());
}
