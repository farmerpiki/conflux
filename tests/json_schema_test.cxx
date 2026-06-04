// Plain TU - not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

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

struct Config {
	std::int64_t required_field{};
	std::optional<std::int64_t> optional_field{};
};

template<>
struct conflux::json::JsonMembers<Config> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("required_field", &Config::required_field),
			conflux::json::json_member("optional_field", &Config::optional_field),
		};
	}
	static constexpr std::string_view type_name() { return "Config"; }
};

TEST_CASE(
	"phase8.3: schema_for generates correct schema for Point",
	"[phase8.3]") {
	auto doc_r = schema_for<Point>();
	REQUIRE(doc_r.has_value());
	auto root = *doc_r->root().as_object();
	CHECK(*root.find_member("type")->as_string() == "object");

	auto props = *root.find_member("properties")->as_object();
	auto x_obj = *props.find_member("x")->as_object();
	CHECK(*x_obj.find_member("type")->as_string() == "integer");
	auto y_obj = *props.find_member("y")->as_object();
	CHECK(*y_obj.find_member("type")->as_string() == "integer");

	auto req = *root.find_member("required")->as_array();
	CHECK(req.size() == 2);
	CHECK(*req.element(0)->as_string() == "x");
	CHECK(*req.element(1)->as_string() == "y");
}
TEST_CASE(
	"phase8.3: schema_for with optional field",
	"[phase8.3]") {
	auto doc_r = schema_for<Config>();
	REQUIRE(doc_r.has_value());
	auto root = *doc_r->root().as_object();

	auto props = *root.find_member("properties")->as_object();
	auto rf_obj = *props.find_member("required_field")->as_object();
	CHECK(*rf_obj.find_member("type")->as_string() == "integer");
	auto of_obj = *props.find_member("optional_field")->as_object();
	CHECK(*of_obj.find_member("type")->as_string() == "integer");

	auto req = *root.find_member("required")->as_array();
	CHECK(req.size() == 1);
	CHECK(*req.element(0)->as_string() == "required_field");
}
TEST_CASE(
	"phase8.3: validate passes valid object",
	"[phase8.3]") {
	auto schema_r = schema_for<Point>();
	REQUIRE(schema_r.has_value());
	auto doc_r = parse(R"({"x": 1, "y": 2})");
	REQUIRE(doc_r.has_value());
	auto result = validate(doc_r->root(), schema_r->root());
	CHECK(result.has_value());
}
TEST_CASE(
	"phase8.3: validate detects wrong type",
	"[phase8.3]") {
	auto schema_r = schema_for<Point>();
	REQUIRE(schema_r.has_value());
	auto doc_r = parse(R"([1, 2])");
	REQUIRE(doc_r.has_value());
	auto result = validate(doc_r->root(), schema_r->root());
	REQUIRE(!result.has_value());
	CHECK(result.error().code == JsonIssueCode::wrong_kind);
}
TEST_CASE(
	"phase8.3: validate detects missing required member",
	"[phase8.3]") {
	auto schema_r = schema_for<Point>();
	REQUIRE(schema_r.has_value());
	auto doc_r = parse(R"({"x": 1})");
	REQUIRE(doc_r.has_value());
	auto result = validate(doc_r->root(), schema_r->root());
	REQUIRE(!result.has_value());
	CHECK(result.error().code == JsonIssueCode::missing_member);
	CHECK(result.error().member_name == "y");
}
TEST_CASE(
	"phase8.3: validate allows missing optional field",
	"[phase8.3]") {
	auto schema_r = schema_for<Config>();
	REQUIRE(schema_r.has_value());
	auto doc_r = parse(R"({"required_field": 42})");
	REQUIRE(doc_r.has_value());
	auto result = validate(doc_r->root(), schema_r->root());
	CHECK(result.has_value());
}
TEST_CASE(
	"phase8.3: validate detects field type mismatch",
	"[phase8.3]") {
	auto schema_r = schema_for<Point>();
	REQUIRE(schema_r.has_value());
	auto doc_r = parse(R"({"x": "bad", "y": 2})");
	REQUIRE(doc_r.has_value());
	auto result = validate(doc_r->root(), schema_r->root());
	REQUIRE(!result.has_value());
	CHECK(result.error().code == JsonIssueCode::wrong_kind);
	CHECK(result.error().member_name == "x");
}
