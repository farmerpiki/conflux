// Plain TU - not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

using namespace conflux::json;

TEST_CASE(
	"json: at_pointer parses and resolves in one call",
	"[json][pointer]") {
	auto doc = parse(R"({"users":[{"name":"Ada"}]})");
	REQUIRE(doc.has_value());

	auto name = doc->root().at_pointer("/users/0/name");
	REQUIRE(name.has_value());
	CHECK(*name->as_string() == "Ada");

	auto bad = doc->root().at_pointer("users/0/name");
	REQUIRE(!bad.has_value());
	CHECK(bad.error().code == JsonIssueCode::invalid_pointer);
}

TEST_CASE(
	"json: merge_patch applies RFC 7396 object changes",
	"[json][merge_patch]") {
	auto target = parse(R"({
		"title": "Goodbye!",
		"author": {"givenName": "John", "familyName": "Doe"},
		"tags": ["example", "sample"],
		"content": "This will be unchanged"
	})");
	auto patch = parse(R"({
		"title": "Hello!",
		"phoneNumber": "+01-123-456-7890",
		"author": {"familyName": null},
		"tags": ["example"]
	})");
	REQUIRE(target.has_value());
	REQUIRE(patch.has_value());

	auto merged = merge_patch(*target, *patch);
	REQUIRE(merged.has_value());

	CHECK(*merged->root().at_pointer("/title")->as_string() == "Hello!");
	CHECK(*merged->root().at_pointer("/content")->as_string() == "This will be unchanged");
	CHECK(*merged->root().at_pointer("/phoneNumber")->as_string() == "+01-123-456-7890");
	CHECK(*merged->root().at_pointer("/author/givenName")->as_string() == "John");
	CHECK_FALSE(merged->root().at_pointer("/author/familyName").has_value());
	auto tags = merged->root().at_pointer("/tags")->as_array();
	REQUIRE(tags.has_value());
	REQUIRE(tags->size() == 1UZ);
	CHECK(*tags->element(0)->as_string() == "example");

	CHECK(*target->root().at_pointer("/author/familyName")->as_string() == "Doe");
}

TEST_CASE(
	"json: merge_patch replaces root with non-object patch",
	"[json][merge_patch]") {
	auto target = parse(R"({"a":1})");
	auto patch = parse(R"([1,2,3])");
	REQUIRE(target.has_value());
	REQUIRE(patch.has_value());

	auto merged = merge_patch(target->root(), patch->root());
	REQUIRE(merged.has_value());
	auto arr = merged->root().as_array();
	REQUIRE(arr.has_value());
	CHECK(arr->size() == 3UZ);
	CHECK(*arr->element(2)->as_i64() == 3);
}

TEST_CASE(
	"json: merge_patch null patch replaces root but deletes object members inside objects",
	"[json][merge_patch]") {
	{
		auto target = parse(R"({"a":1})");
		auto patch = parse("null");
		REQUIRE(target.has_value());
		REQUIRE(patch.has_value());
		auto merged = merge_patch(*target, *patch);
		REQUIRE(merged.has_value());
		CHECK(merged->root().is_null());
	}
	{
		auto target = parse(R"({"a":1,"b":2})");
		auto patch = parse(R"({"a":null})");
		REQUIRE(target.has_value());
		REQUIRE(patch.has_value());
		auto merged = merge_patch(*target, *patch);
		REQUIRE(merged.has_value());
		CHECK_FALSE(merged->root().at_pointer("/a").has_value());
		CHECK(*merged->root().at_pointer("/b")->as_i64() == 2);
	}
}

TEST_CASE(
	"json: merge_patch applies nested null deletion inside new objects",
	"[json][merge_patch]") {
	auto target = parse(R"({})");
	auto patch = parse(R"({
		"profile": {
			"isAdmin": null,
			"prefs": {"dark": null, "lang": "en"},
			"roles": [{"name": null}]
		}
	})");
	REQUIRE(target.has_value());
	REQUIRE(patch.has_value());

	auto merged = merge_patch(*target, *patch);
	REQUIRE(merged.has_value());

	CHECK_FALSE(merged->root().at_pointer("/profile/isAdmin").has_value());
	CHECK_FALSE(merged->root().at_pointer("/profile/prefs/dark").has_value());
	CHECK(*merged->root().at_pointer("/profile/prefs/lang")->as_string() == "en");
	auto array_null = merged->root().at_pointer("/profile/roles/0/name");
	REQUIRE(array_null.has_value());
	CHECK(array_null->is_null());
}

TEST_CASE(
	"json: merge_patch applies object patches against non-object targets as empty objects",
	"[json][merge_patch]") {
	auto target = parse(R"({"profile": 0})");
	auto patch = parse(R"({"profile": {"isAdmin": null, "name": "Ada"}})");
	REQUIRE(target.has_value());
	REQUIRE(patch.has_value());

	auto merged = merge_patch(*target, *patch);
	REQUIRE(merged.has_value());

	CHECK_FALSE(merged->root().at_pointer("/profile/isAdmin").has_value());
	CHECK(*merged->root().at_pointer("/profile/name")->as_string() == "Ada");
}

TEST_CASE(
	"json: apply_patch supports RFC 6902 operations without mutating input",
	"[json][patch]") {
	auto target = parse(R"({"foo":"bar","numbers":[1,2,3],"obj":{"a":1}})");
	auto patch = parse(R"([
		{"op":"add","path":"/baz","value":"qux"},
		{"op":"add","path":"/numbers/-","value":4},
		{"op":"remove","path":"/foo"},
		{"op":"replace","path":"/obj/a","value":2},
		{"op":"copy","from":"/baz","path":"/copied"},
		{"op":"move","from":"/copied","path":"/moved"},
		{"op":"test","path":"/moved","value":"qux"}
	])");
	REQUIRE(target.has_value());
	REQUIRE(patch.has_value());

	auto result = conflux::json::apply_patch(*target, *patch);
	REQUIRE(result.has_value());
	CHECK_FALSE(result->root().at_pointer("/foo").has_value());
	CHECK(*result->root().at_pointer("/baz")->as_string() == "qux");
	CHECK(*result->root().at_pointer("/moved")->as_string() == "qux");
	CHECK(*result->root().at_pointer("/obj/a")->as_i64() == 2);
	auto numbers = result->root().at_pointer("/numbers")->as_array();
	REQUIRE(numbers.has_value());
	CHECK(numbers->size() == 4UZ);
	CHECK(*numbers->element(3)->as_i64() == 4);

	CHECK(*target->root().at_pointer("/foo")->as_string() == "bar");
	CHECK_FALSE(target->root().at_pointer("/baz").has_value());
}

TEST_CASE(
	"json: apply_patch reports stable RFC 6902 error codes",
	"[json][patch]") {
	auto target = parse(R"({"a":{"b":1},"items":[1]})");
	REQUIRE(target.has_value());
	{
		auto patch = parse(R"({"op":"add","path":"/x","value":1})");
		REQUIRE(patch.has_value());
		auto result = conflux::json::validate_patch(patch->root());
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::invalid_patch);
	}
	{
		auto patch = parse(R"([{"path":"/x","value":1}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::validate_patch(patch->root());
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_op_missing);
		CHECK(result.error().operation_index == 0UZ);
	}
	{
		auto patch = parse(R"([{"op":"bogus","path":"/x"}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::validate_patch(patch->root());
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_op_unknown);
	}
	{
		auto patch = parse(R"([{"op":"remove","path":"/items/2"}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::apply_patch(*target, *patch);
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_array_index_out_of_range);
		CHECK(result.error().pointer == "/items/2");
	}
	{
		auto patch = parse(R"([{"op":"move","from":"/a","path":"/a/b/c"}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::apply_patch(*target, *patch);
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_move_into_child);
	}
	{
		auto patch = parse(R"([{"op":"test","path":"/a/b","value":2}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::apply_patch(*target, *patch);
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_test_failed);
		CHECK(*target->root().at_pointer("/a/b")->as_i64() == 1);
	}
}

TEST_CASE(
	"json: apply_patch enforces limits and remove policy",
	"[json][patch]") {
	auto target = parse(R"({"a":1})");
	REQUIRE(target.has_value());
	{
		auto patch = parse(R"([{"op":"remove","path":""}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::apply_patch(*target, *patch);
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_remove_document_root);
	}
	{
		auto patch = parse(R"([{"op":"remove","path":"/missing"}])");
		REQUIRE(patch.has_value());
		auto result =
			conflux::json::apply_patch(*target, *patch, conflux::json::JsonPatchOptions{.allow_missing_remove = true});
		REQUIRE(result.has_value());
		CHECK(*result->root().at_pointer("/a")->as_i64() == 1);
	}
	{
		auto patch = parse(R"([{"op":"add","path":"/a/b","value":1}])");
		REQUIRE(patch.has_value());
		auto result =
			conflux::json::validate_patch(patch->root(), conflux::json::JsonPatchOptions{.max_pointer_depth = 1});
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_pointer_too_deep);
	}
	{
		auto patch = parse(R"([{"op":"test","path":"/a","value":1},{"op":"test","path":"/a","value":1}])");
		REQUIRE(patch.has_value());
		auto result =
			conflux::json::validate_patch(patch->root(), conflux::json::JsonPatchOptions{.max_operations = 1});
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_too_many_operations);
	}
	{
		auto patch = parse(R"([{"op":"copy","from":"","path":"/copy1"},{"op":"copy","from":"","path":"/copy2"}])");
		REQUIRE(patch.has_value());
		auto result =
			conflux::json::apply_patch(*target, *patch, conflux::json::JsonPatchOptions{.max_result_nodes = 5});
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::output_too_large);
	}
}
