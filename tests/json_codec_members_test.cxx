// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

struct Point {
	std::int64_t x{};
	std::int64_t y{};
};

template<>
struct conflux::json::JsonMembers<Point> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("x", &Point::x),
			conflux::json::json_member("y", &Point::y),
		};
	}
	static constexpr std::string_view type_name() { return "Point"; }
};

TEST_CASE(
	"json: JsonMembers decode plain struct",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":3,"y":7})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->x == 3LL);
	CHECK(r->y == 7LL);
}

TEST_CASE(
	"json: decode_full_into updates existing JsonMembers struct",
	"[json][codec][members]") {
	Point point{.x = 1, .y = 2};
	auto r = decode_full_into(point, R"({"x":3,"y":7})");
	REQUIRE(r.has_value());
	CHECK(point.x == 3LL);
	CHECK(point.y == 7LL);
}

TEST_CASE(
	"json: JsonMembers decode missing required member yields missing_member",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":3})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::missing_member);
	CHECK(r.error().member_name == "y");
}

TEST_CASE(
	"json: JsonMembers decode unknown member yields invalid_value",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":1,"y":2,"z":3})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
	CHECK(r.error().member_name == "z");
}

TEST_CASE(
	"json: JsonMembers decode wrong kind for field propagates path",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":"bad","y":2})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	CHECK_FALSE(r.has_value());
	auto &err = r.error();
	CHECK(err.code == JsonIssueCode::wrong_kind);
	REQUIRE(err.path.size() == 1UZ);
	CHECK(get<JsonPathMember>(err.path.segment(0)).name == "x");
}

TEST_CASE(
	"json: has_json_codec true for JsonMembers type",
	"[json][codec][members]") {
	CHECK(has_json_codec<Point>);
}
