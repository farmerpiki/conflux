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

TEST_CASE(
	"json: limits — max_depth matrix",
	"[json][limits][pathological]") {
	{
		JsonParseOptions opts;
		opts.max_depth = LimitOption::bound(0);
		CHECK(parse("null", opts).has_value());
		CHECK(parse("42", opts).has_value());
		CHECK(parse("{}", opts).has_value());
		CHECK(parse("[]", opts).has_value());
		CHECK_FALSE(parse("[1]", opts).has_value());
		CHECK_FALSE(parse(R"({"a":1})", opts).has_value());
	}
	{
		JsonParseOptions opts;
		opts.max_depth = LimitOption::bound(1);
		CHECK(parse("[1,2,3]", opts).has_value());
		CHECK(parse(R"({"a":1})", opts).has_value());
		CHECK_FALSE(parse("[[1]]", opts).has_value());
		CHECK_FALSE(parse(R"({"a":{"b":1}})", opts).has_value());
	}
	{
		JsonParseOptions opts;
		opts.max_depth = LimitOption::bound(3);
		std::string at_boundary(3, '[');
		at_boundary += "1";
		at_boundary.append(3, ']');
		CHECK(parse(at_boundary, opts).has_value());
		std::string over_boundary(4, '[');
		over_boundary += "1";
		over_boundary.append(4, ']');
		CHECK_FALSE(parse(over_boundary, opts).has_value());
	}
	{
		JsonParseOptions opts;
		opts.max_depth = no_limit;
		std::string deep(200, '[');
		deep += "1";
		deep.append(200, ']');
		CHECK(parse(deep, opts).has_value());
	}
}

TEST_CASE(
	"json: limits — max_input_size matrix",
	"[json][limits][pathological]") {
	{
		JsonParseOptions opts;
		opts.max_input_size = LimitOption::bound(0);
		auto res = parse("1", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::input_too_large);
	}
	{
		JsonParseOptions opts;
		opts.max_input_size = LimitOption::bound(1);
		CHECK(parse("1", opts).has_value());
		auto res = parse("12", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::input_too_large);
	}
	{
		std::string const s = R"("hello")";
		JsonParseOptions opts;
		opts.max_input_size = LimitOption::bound(s.size());
		CHECK(parse(s, opts).has_value());
		opts.max_input_size = LimitOption::bound(s.size() - 1);
		CHECK_FALSE(parse(s, opts).has_value());
	}
	{
		JsonParseOptions opts;
		opts.max_input_size = no_limit;
		std::string big = "[";
		for (int i = 0; i < 1000; ++i) {
			if (i > 0) {
				big += ',';
			}
			big += std::to_string(i);
		}
		big += ']';
		CHECK(parse(big, opts).has_value());
	}
}

TEST_CASE(
	"json: limits — max_string_size matrix",
	"[json][limits][pathological]") {
	{
		JsonParseOptions opts;
		opts.max_string_size = LimitOption::bound(0);
		auto res = parse(R"("x")", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::string_too_large);
	}
	{
		JsonParseOptions opts;
		opts.max_string_size = LimitOption::bound(0);
		CHECK(parse(R"("")", opts).has_value());
	}
	{
		JsonParseOptions opts;
		opts.max_string_size = LimitOption::bound(3);
		CHECK(parse(R"("abc")", opts).has_value());
		auto res = parse(R"("abcd")", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::string_too_large);
	}
	{
		JsonParseOptions opts;
		opts.max_string_size = no_limit;
		std::string big_str = "\"";
		big_str.append(100000, 'x');
		big_str += '"';
		CHECK(parse(big_str, opts).has_value());
	}
}

TEST_CASE(
	"json: limits — two limits interact, only one violated",
	"[json][limits][pathological]") {
	{
		JsonParseOptions opts;
		opts.max_depth = LimitOption::bound(1);
		opts.max_string_size = LimitOption::bound(2);
		auto res = parse(R"(["abc"])", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::string_too_large);
	}
	{
		JsonParseOptions opts;
		opts.max_input_size = LimitOption::bound(20);
		opts.max_depth = LimitOption::bound(1);
		auto res = parse("[[1]]", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::nesting_too_deep);
	}
	{
		JsonParseOptions opts;
		opts.max_input_size = LimitOption::bound(2);
		opts.max_depth = LimitOption::bound(0);
		auto res = parse("[1]", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::input_too_large);
	}
}

TEST_CASE(
	"json: limits — number lexeme length",
	"[json][limits][pathological]") {
	{
		std::string at_limit = "0.";
		at_limit.append(1022, '1');
		CHECK(parse(at_limit).has_value());
	}
	{
		std::string over_limit = "0.";
		over_limit.append(1023, '1');
		auto res = parse(over_limit);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::invalid_number);
	}
}
