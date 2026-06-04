// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: JsonError::with_prefix prepends path segments",
	"[json][error]") {
	JsonError err{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "test error"};
	err.path.push_index(1);

	JsonPath prefix;
	prefix.push_member("items");

	auto prefixed = err.with_prefix(prefix);
	REQUIRE(prefixed.path.size() == 2UZ);
	CHECK(get<JsonPathMember>(prefixed.path.segment(0)).name == "items");
	CHECK(get<JsonPathIndex>(prefixed.path.segment(1)).index == 1UZ);
}

TEST_CASE(
	"json: JsonError::with_prefix on empty path",
	"[json][error]") {
	JsonError err{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "test error"};

	JsonPath prefix;
	prefix.push_member("field");

	auto prefixed = err.with_prefix(prefix);
	REQUIRE(prefixed.path.size() == 1UZ);
	CHECK(get<JsonPathMember>(prefixed.path.segment(0)).name == "field");
}
