// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: max_depth exceeded",
	"[json][limits]") {
	JsonParseOptions opts;
	opts.max_depth = LimitOption::bound(3);
	std::string nested(4, '[');
	nested += "1";
	nested.append(4, ']');
	auto res = parse(nested, opts);
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::nesting_too_deep);
}

TEST_CASE(
	"json: max_depth not exceeded at boundary",
	"[json][limits]") {
	JsonParseOptions opts;
	opts.max_depth = LimitOption::bound(3);
	std::string nested(3, '[');
	nested += "1";
	nested.append(3, ']');
	CHECK(parse(nested, opts).has_value());
}

TEST_CASE(
	"json: max_input_size exceeded",
	"[json][limits]") {
	JsonParseOptions opts;
	opts.max_input_size = LimitOption::bound(4);
	auto res = parse("\"hello\"", opts);
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::input_too_large);
}

TEST_CASE(
	"json: max_string_size exceeded",
	"[json][limits]") {
	JsonParseOptions opts;
	opts.max_string_size = LimitOption::bound(2);
	auto res = parse("\"abc\"", opts);
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::string_too_large);
}

TEST_CASE(
	"json: max_depth zero rejects non-empty containers",
	"[json][limits]") {
	JsonParseOptions opts;
	opts.max_depth = LimitOption::bound(0);
	CHECK_FALSE(parse("[1]", opts).has_value());
	CHECK_FALSE(parse(R"({"a":1})", opts).has_value());
	CHECK(parse("[]", opts).has_value());
	CHECK(parse("{}", opts).has_value());
	CHECK(parse("null", opts).has_value());
}
