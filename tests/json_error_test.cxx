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

struct InnerData {
	std::int64_t value{};
};

template<>
struct conflux::json::JsonMembers<InnerData> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("value", &InnerData::value),
		};
	}
	static constexpr std::string_view type_name() { return "InnerData"; }
};

struct OuterWithPrefix {
	InnerData inner{};
};

template<>
struct conflux::json::JsonCodec<OuterWithPrefix> {
	static std::expected<OuterWithPrefix, JsonError> decode(
		NodeRef n) {
		auto obj = n.as_object();
		if (!obj) {
			return std::unexpected(std::move(obj).error());
		}

		JsonPath prefix;
		prefix.push_member("inner");

		auto inner_node = n.at(prefix);
		if (!inner_node) {
			return std::unexpected(std::move(inner_node).error().with_prefix(prefix));
		}

		auto inner = ::decode<InnerData>(*inner_node);
		if (!inner) {
			return std::unexpected(std::move(inner).error().with_prefix(prefix));
		}

		return OuterWithPrefix{.inner = std::move(*inner)};
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		OuterWithPrefix const &v) {
		auto obj_res = b.begin_object();
		if (!obj_res) {
			return std::unexpected(std::move(obj_res).error());
		}
		auto &obj = *obj_res;
		auto inner_res = obj.insert<InnerData>("inner", v.inner);
		if (!inner_res) {
			return std::unexpected(std::move(inner_res).error());
		}
		std::move(obj).commit();
		return {};
	}
	static constexpr std::string_view type_name() { return "OuterWithPrefix"; }
};

TEST_CASE(
	"json: example — nested-codec error propagation via with_prefix",
	"[json][examples]") {
	{
		auto doc = parse(R"({"inner": {"value": 7}})");
		REQUIRE(doc.has_value());
		auto r = decode<OuterWithPrefix>(doc->root());
		REQUIRE(r.has_value());
		CHECK(r->inner.value == 7LL);
	}

	{
		auto doc = parse(R"({"inner": {"value": "bad"}})");
		REQUIRE(doc.has_value());
		auto r = decode<OuterWithPrefix>(doc->root());
		CHECK_FALSE(r.has_value());
		CHECK(r.error().code == JsonIssueCode::wrong_kind);
		REQUIRE(r.error().path.size() >= 2UZ);
		CHECK(get<JsonPathMember>(r.error().path.segment(0)).name == "inner");
		CHECK(get<JsonPathMember>(r.error().path.segment(1)).name == "value");
	}

	{
		auto doc = parse(R"({"other": 1})");
		REQUIRE(doc.has_value());
		auto r = decode<OuterWithPrefix>(doc->root());
		CHECK_FALSE(r.has_value());
		CHECK(r.error().code == JsonIssueCode::missing_member);
	}

	{
		auto b = value_builder();
		OuterWithPrefix const v{.inner = InnerData{.value = 42LL}};
		REQUIRE(b.set<OuterWithPrefix>(v).has_value());
		auto doc = std::move(b).finish();
		REQUIRE(doc.has_value());
		auto r = decode<OuterWithPrefix>(doc->root());
		REQUIRE(r.has_value());
		CHECK(r->inner.value == 42LL);
	}
}
