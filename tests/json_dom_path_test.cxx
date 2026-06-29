// Plain TU - not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: DOM convenience getters decode typed values",
	"[json][codec]") {
	auto doc = parse(R"({"user":{"id":42,"name":"alice"}})");
	REQUIRE(doc.has_value());

	auto id = doc->get<std::uint64_t>("/user/id");
	REQUIRE(id.has_value());
	CHECK(*id == 42UZ);

	auto user_node = doc->root().as_object()->member("user");
	REQUIRE(user_node.has_value());
	auto obj = user_node->as_object();
	REQUIRE(obj.has_value());
	auto name = obj->required<std::string_view>("name");
	REQUIRE(name.has_value());
	CHECK(*name == "alice");

	auto missing = obj->optional<std::uint64_t>("missing");
	REQUIRE(missing.has_value());
	CHECK_FALSE(missing->has_value());

	auto doc_with_null = parse(R"({"count": null})");
	REQUIRE(doc_with_null.has_value());
	auto obj_with_null = doc_with_null->root().as_object();
	REQUIRE(obj_with_null.has_value());
	auto null_count = obj_with_null->optional<std::uint64_t>("count");
	REQUIRE(null_count.has_value());
	CHECK_FALSE(null_count->has_value());

	auto node = obj->member("id");
	REQUIRE(node.has_value());
	auto direct = node->as<std::uint64_t>();
	REQUIRE(direct.has_value());
	CHECK(*direct == 42UZ);
}

TEST_CASE(
	"json: reject duplicate object keys",
	"[json]") {
	auto doc = parse(R"({"k": 1, "k": 2})");
	CHECK_FALSE(doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::duplicate_member);
}
TEST_CASE(
	"json: nested object via path",
	"[json]") {
	auto doc = parse(R"({"x": {"y": 42}})");
	REQUIRE(doc.has_value());
	JsonPath p;
	p.push_member("x");
	p.push_member("y");
	auto node = doc->root().at(p);
	REQUIRE(node.has_value());
	CHECK(*node->as_number()->to_i64() == 42LL);
}
TEST_CASE(
	"json: nested A via path",
	"[json]") {
	auto doc = parse("[[1, 2], [3, 4]]");
	REQUIRE(doc.has_value());
	JsonPath p;
	p.push_index(1);
	p.push_index(0);
	auto node = doc->root().at(p);
	REQUIRE(node.has_value());
	CHECK(*node->as_number()->to_i64() == 3LL);
}
