// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: JsonPath from_pointer roundtrip",
	"[json][path]") {
	std::string_view ptr = "/a/b/c";
	auto path = JsonPath::from_pointer(ptr);
	REQUIRE(path.has_value());
	CHECK(path->to_pointer() == ptr);
}

TEST_CASE(
	"json: JsonPath from_pointer with escapes",
	"[json][path]") {
	auto path = JsonPath::from_pointer("/a~0b/c~1d");
	REQUIRE(path.has_value());
	CHECK(path->to_pointer() == "/a~0b/c~1d");
}

TEST_CASE(
	"json: JsonPath empty pointer is root",
	"[json][path]") {
	auto path = JsonPath::from_pointer("");
	REQUIRE(path.has_value());
	CHECK(path->empty());
	CHECK(path->to_pointer().empty());
}

TEST_CASE(
	"json: JsonPath must start with /",
	"[json][path]") {
	auto path = JsonPath::from_pointer("noslash");
	CHECK_FALSE(path.has_value());
	CHECK(path.error().code == JsonIssueCode::invalid_pointer);
}

TEST_CASE(
	"json: example — RFC 6901 round-trip via to_pointer / from_pointer",
	"[json][examples]") {
	{
		JsonPath path;
		path.push_member("users");
		path.push_index(0);
		path.push_member("name");

		auto ptr = path.to_pointer();
		CHECK(ptr == "/users/0/name");

		auto reparsed = JsonPath::from_pointer(ptr);
		REQUIRE(reparsed.has_value());
		CHECK(reparsed->to_pointer() == ptr);
	}

	{
		auto path = JsonPath::from_pointer("/a~0b/c~1d");
		REQUIRE(path.has_value());
		CHECK(path->size() == 2UZ);
		CHECK(std::get<JsonPathMember>(path->segment(0)).name == "a~b");
		CHECK(std::get<JsonPathMember>(path->segment(1)).name == "c/d");
		CHECK(path->to_pointer() == "/a~0b/c~1d");
	}

	{
		auto path = JsonPath::from_pointer("");
		REQUIRE(path.has_value());
		CHECK(path->empty());
		CHECK(path->to_pointer().empty());
	}

	{
		auto path = JsonPath::from_pointer("noslash");
		CHECK_FALSE(path.has_value());
		CHECK(path.error().code == JsonIssueCode::invalid_pointer);
		CHECK(path.error().stage == JsonStage::parse);
	}

	{
		auto doc = parse(R"({"a": {"b": [1, 2, 3]}})");
		REQUIRE(doc.has_value());
		auto path = JsonPath::from_pointer("/a/b");
		REQUIRE(path.has_value());
		auto node = doc->root().at(*path);
		REQUIRE(node.has_value());
		CHECK(node->kind() == JsonKind::array);
	}

	{
		JsonPath idx_path;
		idx_path.push_index(2);
		auto ptr = idx_path.to_pointer();
		CHECK(ptr == "/2");
		auto reparsed = JsonPath::from_pointer(ptr);
		REQUIRE(reparsed.has_value());
		CHECK(reparsed->size() == 1UZ);
		CHECK(std::holds_alternative<JsonPathMember>(reparsed->segment(0)));
		CHECK(std::get<JsonPathMember>(reparsed->segment(0)).name == "2");

		auto doc = parse("[10,20,30]");
		REQUIRE(doc.has_value());
		auto by_member = doc->root().at(*reparsed);
		REQUIRE(by_member.has_value());
		CHECK(*by_member->as_number()->to_i64() == 30LL);

		auto by_index = doc->root().at(idx_path);
		REQUIRE(by_index.has_value());
		CHECK(*by_index->as_number()->to_i64() == 30LL);
	}
}
