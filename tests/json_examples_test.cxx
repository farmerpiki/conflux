// Plain TU — not a module unit.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: example — generic vs typed view access",
	"[json][examples]") {
	auto doc = parse(R"({"name":"alice","score":42,"active":true})");
	REQUIRE(doc.has_value());

	NodeRef root = doc->root();
	CHECK(root.kind() == JsonKind::object);

	auto obj = root.as_object();
	REQUIRE(obj.has_value());
	CHECK(obj->size() == 3UZ);

	auto name_node = obj->member("name");
	REQUIRE(name_node.has_value());
	CHECK(name_node->kind() == JsonKind::string);
	CHECK(*name_node->as_string() == "alice");

	auto score_node = obj->member("score");
	REQUIRE(score_node.has_value());
	CHECK(score_node->kind() == JsonKind::number);
	CHECK(*score_node->as_number()->to_i64() == 42LL);

	auto active_node = obj->member("active");
	REQUIRE(active_node.has_value());
	CHECK(active_node->kind() == JsonKind::boolean);
	CHECK(*active_node->as_bool() == true);

	auto decoded = decode<std::map<std::string, std::int64_t>>(*score_node);
	CHECK_FALSE(decoded.has_value());
	CHECK(decoded.error().code == JsonIssueCode::wrong_kind);
}

TEST_CASE(
	"json: example — strict parse failure on duplicates",
	"[json][examples]") {
	auto res = parse(R"({"key":1,"key":2})");
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::duplicate_member);
	CHECK(res.error().stage == JsonStage::parse);

	auto ok = parse(R"({"key":1,"other":2})");
	CHECK(ok.has_value());
}

TEST_CASE(
	"json: example — strict number handling and scientific-notation rejection",
	"[json][examples]") {
	{
		auto doc = parse("1e2");
		REQUIRE(doc.has_value());
		auto n = doc->root().as_number();
		REQUIRE(n.has_value());
		CHECK(n->form() == JsonNumberForm::non_integer);

		auto as_i64 = n->to_i64();
		CHECK_FALSE(as_i64.has_value());
		CHECK(as_i64.error().code == JsonIssueCode::invalid_number);

		auto as_u64 = n->to_u64();
		CHECK_FALSE(as_u64.has_value());
		CHECK(as_u64.error().code == JsonIssueCode::invalid_number);

		auto as_f64 = n->to_f64();
		REQUIRE(as_f64.has_value());
		CHECK(*as_f64 == Catch::Approx(100.0));
	}
	{
		auto doc = parse("1.0");
		REQUIRE(doc.has_value());
		auto n = doc->root().as_number();
		REQUIRE(n.has_value());
		CHECK(n->form() == JsonNumberForm::non_integer);
		CHECK_FALSE(n->to_i64().has_value());
	}
	{
		auto doc = parse("42");
		REQUIRE(doc.has_value());
		auto n = doc->root().as_number();
		REQUIRE(n.has_value());
		CHECK(n->form() == JsonNumberForm::integer);
		REQUIRE(n->to_i64().has_value());
		CHECK(*n->to_i64() == 42LL);
	}
}

TEST_CASE(
	"json: example — strict typed decode",
	"[json][examples]") {
	{
		auto doc = parse("\"hello\"");
		REQUIRE(doc.has_value());
		auto r = decode<std::int64_t>(doc->root());
		CHECK_FALSE(r.has_value());
		CHECK(r.error().code == JsonIssueCode::wrong_kind);
	}
	{
		auto doc = parse("1.5");
		REQUIRE(doc.has_value());
		auto r = decode<std::int64_t>(doc->root());
		CHECK_FALSE(r.has_value());
		CHECK(r.error().code == JsonIssueCode::invalid_number);
	}
	{
		auto doc = parse("-1");
		REQUIRE(doc.has_value());
		auto r = decode<std::uint64_t>(doc->root());
		CHECK_FALSE(r.has_value());
		CHECK(r.error().code == JsonIssueCode::sign_mismatch);
	}
	{
		auto doc = parse("true");
		REQUIRE(doc.has_value());
		auto r = decode<bool>(doc->root());
		REQUIRE(r.has_value());
		CHECK(*r == true);
	}
}

struct ThreeFieldModel {
	std::int64_t required_val{};
	std::optional<std::int64_t> optional_val{};
	Nullable<std::int64_t> nullable_val{};
	std::optional<Nullable<std::int64_t>> opt_nullable_val{};
};

template<>
struct conflux::json::JsonMembers<ThreeFieldModel> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("required_val", &ThreeFieldModel::required_val),
			conflux::json::json_member("optional_val", &ThreeFieldModel::optional_val),
			conflux::json::json_member("nullable_val", &ThreeFieldModel::nullable_val),
			conflux::json::json_member("opt_nullable_val", &ThreeFieldModel::opt_nullable_val),
		};
	}
	static constexpr std::string_view type_name() { return "ThreeFieldModel"; }
};

TEST_CASE(
	"json: example — missing vs null vs Opt field modeling",
	"[json][examples]") {
	{
		auto doc = parse(R"({
"required_val":1,
"nullable_val":null,
"opt_nullable_val":42
})");
		REQUIRE(doc.has_value());
		auto r = decode<ThreeFieldModel>(doc->root());
		REQUIRE(r.has_value());
		CHECK(r->required_val == 1LL);
		CHECK_FALSE(r->optional_val.has_value());
		CHECK(r->nullable_val.is_null());
		REQUIRE(r->opt_nullable_val.has_value());
		REQUIRE(r->opt_nullable_val->has_value());
		CHECK(*(*r->opt_nullable_val) == 42LL);
	}
	{
		auto doc = parse(R"({
"required_val":5,
"nullable_val":99
})");
		REQUIRE(doc.has_value());
		auto r = decode<ThreeFieldModel>(doc->root());
		REQUIRE(r.has_value());
		CHECK(r->required_val == 5LL);
		CHECK_FALSE(r->optional_val.has_value());
		REQUIRE(r->nullable_val.has_value());
		CHECK(*r->nullable_val == 99LL);
		CHECK_FALSE(r->opt_nullable_val.has_value());
	}
	{
		auto doc = parse(R"({
"required_val":7,
"nullable_val":null,
"opt_nullable_val":null
})");
		REQUIRE(doc.has_value());
		auto r = decode<ThreeFieldModel>(doc->root());
		REQUIRE(r.has_value());
		REQUIRE(r->opt_nullable_val.has_value());
		CHECK(r->opt_nullable_val->is_null());
	}
	{
		auto doc = parse(R"({"nullable_val": null})");
		REQUIRE(doc.has_value());
		auto r = decode<ThreeFieldModel>(doc->root());
		CHECK_FALSE(r.has_value());
		CHECK(r.error().code == JsonIssueCode::missing_member);
		CHECK(r.error().member_name == "required_val");
	}
	{
		auto doc = parse(R"({"required_val": 1, "optional_val": null, "nullable_val": null})");
		REQUIRE(doc.has_value());
		auto r = decode<ThreeFieldModel>(doc->root());
		REQUIRE(r.has_value());
		CHECK_FALSE(r->optional_val.has_value());
	}
}
