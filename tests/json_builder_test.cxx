// Plain TU — not a module unit.
#include <catch2/catch_approx.hpp>
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
	"json: builder set_null",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_null().has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(doc->root().is_null());
}

TEST_CASE(
	"json: builder set_bool",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_bool(true).has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_bool() == true);
}

TEST_CASE(
	"json: builder set_string",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_string("hello").has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_string() == "hello");
}

TEST_CASE(
	"json: builder set_i64",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_i64(-99LL).has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_i64() == -99LL);
}

TEST_CASE(
	"json: builder set_u64",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_u64(std::numeric_limits<std::uint64_t>::max()).has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_u64() == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE(
	"json: builder set_f64",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_f64(1.5).has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_f64() == Catch::Approx(1.5));
}

TEST_CASE(
	"json: builder set_f64 rejects NaN",
	"[json][builder]") {
	auto b = value_builder();
	auto res = b.set_f64(std::numeric_limits<double>::quiet_NaN());
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::number_out_of_range);
}

TEST_CASE(
	"json: builder finish without set returns error",
	"[json][builder]") {
	auto b = value_builder();
	auto doc = std::move(b).finish();
	CHECK_FALSE(doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::constraint_violation);
}

TEST_CASE(
	"json: builder reset allows re-use",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_i64(1LL).has_value());
	b.reset();
	REQUIRE(b.set_i64(2LL).has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_i64() == 2LL);
}

TEST_CASE(
	"json: builder double-set returns error",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_i64(1LL).has_value());
	CHECK_FALSE(b.set_i64(2LL).has_value());
}

TEST_CASE(
	"json: builder begin_object — flat object",
	"[json][builder]") {
	auto b = value_builder();
	auto ob = b.begin_object();
	REQUIRE(ob.has_value());
	REQUIRE(ob->insert_i64("x", 10LL).has_value());
	REQUIRE(ob->insert_string("name", "alice").has_value());
	REQUIRE(ob->insert_bool("ok", true).has_value());
	REQUIRE(ob->insert_null("nothing").has_value());
	std::move(*ob).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("x")->as_number()->to_i64() == 10LL);
	CHECK(*o.member("name")->as_string() == "alice");
	CHECK(*o.member("ok")->as_bool() == true);
	CHECK(o.member("nothing")->is_null());
}

TEST_CASE(
	"json: builder object rejects duplicate keys",
	"[json][builder]") {
	auto b = value_builder();
	auto ob = b.begin_object();
	REQUIRE(ob.has_value());
	REQUIRE(ob->insert_i64("k", 1LL).has_value());
	auto dup = ob->insert_i64("k", 2LL);
	CHECK_FALSE(dup.has_value());
	CHECK(dup.error().code == JsonIssueCode::duplicate_member);
}

TEST_CASE(
	"json: builder begin_array — flat A",
	"[json][builder]") {
	auto b = value_builder();
	auto ab = b.begin_array();
	REQUIRE(ab.has_value());
	REQUIRE(ab->append_i64(1LL).has_value());
	REQUIRE(ab->append_i64(2LL).has_value());
	REQUIRE(ab->append_i64(3LL).has_value());
	std::move(*ab).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	CHECK(a.size() == 3UZ);
	CHECK(*a.element(0)->as_number()->to_i64() == 1LL);
	CHECK(*a.element(2)->as_number()->to_i64() == 3LL);
}

TEST_CASE(
	"json: builder A with mixed types",
	"[json][builder]") {
	auto b = value_builder();
	auto ab = b.begin_array();
	REQUIRE(ab.has_value());
	REQUIRE(ab->append_null().has_value());
	REQUIRE(ab->append_bool(false).has_value());
	REQUIRE(ab->append_string("hi").has_value());
	REQUIRE(ab->append_f64(2.5).has_value());
	std::move(*ab).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	CHECK(a.size() == 4UZ);
	CHECK(a.element(0)->is_null());
	CHECK(*a.element(1)->as_bool() == false);
	CHECK(*a.element(2)->as_string() == "hi");
	CHECK(*a.element(3)->as_number()->to_f64() == Catch::Approx(2.5));
}

TEST_CASE(
	"json: builder lambda sugar builds nested documents",
	"[json][builder]") {
	auto doc = conflux::json::object([](auto &obj) {
		REQUIRE(obj("id", 42).has_value());
		REQUIRE(obj("name", "alice").has_value());
		REQUIRE(obj.array(
					   "tags",
					   [](auto &arr) {
						   REQUIRE(arr("x").has_value());
						   REQUIRE(arr("y").has_value());
					   })
					.has_value());
	});
	REQUIRE(doc.has_value());

	auto root = doc->root().as_object();
	REQUIRE(root.has_value());
	CHECK(*root->required<std::int64_t>("id") == 42);
	CHECK(*root->required<std::string>("name") == "alice");

	auto tags = root->member("tags")->as_array();
	REQUIRE(tags.has_value());
	CHECK(*tags->element(0)->as_string() == "x");
	CHECK(*tags->element(1)->as_string() == "y");
}

TEST_CASE(
	"json: builder set_number valid lexeme",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_number("1.5e2").has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_f64() == Catch::Approx(150.0));
}

TEST_CASE(
	"json: builder set_number invalid lexeme",
	"[json][builder]") {
	auto b = value_builder();
	auto res = b.set_number("not-a-number");
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::invalid_number);
}

TEST_CASE(
	"json: example — builder construction of arbitrary JSON number lexemes",
	"[json][examples]") {
	{
		auto b = value_builder();
		REQUIRE(b.set_number("1e100").has_value());
		auto doc = std::move(b).finish();
		REQUIRE(doc.has_value());
		auto n = doc->root().as_number();
		REQUIRE(n.has_value());
		CHECK(n->lexeme() == "1e100");
		CHECK(n->form() == JsonNumberForm::non_integer);
		auto dumped = doc->dump();
		REQUIRE(dumped.has_value());
		CHECK(*dumped == "1e100");
	}
	{
		auto b = value_builder();
		REQUIRE(b.set_number("99999999999999999999999999999999").has_value());
		auto doc = std::move(b).finish();
		REQUIRE(doc.has_value());
		auto n = doc->root().as_number();
		REQUIRE(n.has_value());
		CHECK(n->lexeme() == "99999999999999999999999999999999");
	}
	{
		auto b = value_builder();
		REQUIRE(b.set_number("1.0").has_value());
		auto doc = std::move(b).finish();
		REQUIRE(doc.has_value());
		auto dumped = doc->dump();
		REQUIRE(dumped.has_value());
		CHECK(*dumped == "1.0");
	}
	{
		auto b = value_builder();
		CHECK_FALSE(b.set_number("+1").has_value());
		CHECK_FALSE(b.set_number("01").has_value());
		CHECK_FALSE(b.set_number("1.").has_value());
		CHECK_FALSE(b.set_number("NaN").has_value());
		CHECK_FALSE(b.set_number("Infinity").has_value());
	}
}

TEST_CASE(
	"json: example — commit-on-success abort-on-error child builder patterns",
	"[json][examples]") {
	auto build_doc = [](bool inject_error) -> std::expected<Document, JsonError> {
		auto vb = value_builder();
		auto root_res = vb.begin_object();
		if (!root_res) {
			return std::unexpected(std::move(root_res).error());
		}
		auto &root = *root_res;

		{
			auto child_res = root.insert_object("user");
			if (!child_res) {
				return std::unexpected(std::move(child_res).error());
			}
			auto &child = *child_res;
			auto id_res = child.insert_i64("id", inject_error ? -1LL : 1LL);
			if (inject_error) {
				auto dup_res = child.insert_i64("id", 99LL);
				if (!dup_res) {
					return std::unexpected(std::move(dup_res).error());
				}
			}
			if (!id_res) {
				return std::unexpected(std::move(id_res).error());
			}
			REQUIRE(child.insert_string("name", "bob").has_value());
			std::move(child).commit();
		}

		std::move(root).commit();
		return std::move(vb).finish();
	};

	{
		auto doc = build_doc(false);
		REQUIRE(doc.has_value());
		auto obj = doc->root().as_object();
		REQUIRE(obj.has_value());
		auto user = obj->member("user");
		REQUIRE(user.has_value());
		auto user_obj = user->as_object();
		REQUIRE(user_obj.has_value());
		CHECK(*user_obj->member("name")->as_string() == "bob");
	}

	{
		auto doc = build_doc(true);
		CHECK_FALSE(doc.has_value());
		CHECK(doc.error().code == JsonIssueCode::duplicate_member);
	}
}

TEST_CASE(
	"json: example — child abort leaves parent unchanged",
	"[json][examples]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	{
		auto child_res = root.insert_object("discarded");
		REQUIRE(child_res.has_value());
		auto &child = *child_res;
		REQUIRE(child.insert_i64("x", 7LL).has_value());
	}

	REQUIRE(root.insert_string("kept", "yes").has_value());
	std::move(root).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	CHECK_FALSE(obj->find_member("discarded").has_value());
	REQUIRE(obj->find_member("kept").has_value());
	CHECK(*obj->member("kept")->as_string() == "yes");
}

TEST_CASE(
	"json: example — partial-build abandonment via reset()",
	"[json][examples]") {
	auto b = value_builder();

	REQUIRE(b.set_i64(100LL).has_value());

	b.reset();

	REQUIRE(b.set_string("after-reset").has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_string() == "after-reset");
}

TEST_CASE(
	"json: example — reset then build complex document",
	"[json][examples]") {
	auto b = value_builder();

	REQUIRE(b.set_bool(false).has_value());
	b.reset();

	auto obj_res = b.begin_object();
	REQUIRE(obj_res.has_value());
	auto &obj = *obj_res;
	REQUIRE(obj.insert_i64("v", 42LL).has_value());
	std::move(obj).commit();

	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto o = doc->root().as_object();
	REQUIRE(o.has_value());
	CHECK(*o->member("v")->as_number()->to_i64() == 42LL);
}

TEST_CASE(
	"json: ObjectBuilder::insert_object — nested object committed",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	auto user_res = root.insert_object("user");
	REQUIRE(user_res.has_value());
	auto &user = *user_res;
	REQUIRE(user.insert_i64("id", 42LL).has_value());
	REQUIRE(user.insert_string("name", "alice").has_value());
	std::move(user).commit();

	std::move(root).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	auto user_node = obj->member("user");
	REQUIRE(user_node.has_value());
	auto user_obj = user_node->as_object();
	REQUIRE(user_obj.has_value());
	auto id = user_obj->member("id");
	REQUIRE(id.has_value());
	CHECK(*id->as_number()->to_i64() == 42LL);
	auto name = user_obj->member("name");
	REQUIRE(name.has_value());
	CHECK(*name->as_string() == "alice");
}

TEST_CASE(
	"json: ObjectBuilder::insert_array — nested A committed",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	auto tags_res = root.insert_array("tags");
	REQUIRE(tags_res.has_value());
	auto &tags = *tags_res;
	REQUIRE(tags.append_string("x").has_value());
	REQUIRE(tags.append_string("y").has_value());
	std::move(tags).commit();

	std::move(root).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	auto tags_node = obj->member("tags");
	REQUIRE(tags_node.has_value());
	auto arr = tags_node->as_array();
	REQUIRE(arr.has_value());
	REQUIRE(arr->size() == 2UZ);
	CHECK(*arr->element(0)->as_string() == "x");
	CHECK(*arr->element(1)->as_string() == "y");
}

TEST_CASE(
	"json: ArrayBuilder::append_object — nested object committed",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_array();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	auto item_res = root.append_object();
	REQUIRE(item_res.has_value());
	auto &item = *item_res;
	REQUIRE(item.insert_string("k", "v").has_value());
	std::move(item).commit();

	std::move(root).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	auto arr = doc->root().as_array();
	REQUIRE(arr.has_value());
	REQUIRE(arr->size() == 1UZ);
	auto obj = arr->element(0)->as_object();
	REQUIRE(obj.has_value());
	CHECK(*obj->member("k")->as_string() == "v");
}

TEST_CASE(
	"json: ArrayBuilder::append_array — nested A committed",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_array();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	auto inner_res = root.append_array();
	REQUIRE(inner_res.has_value());
	auto &inner = *inner_res;
	REQUIRE(inner.append_i64(1LL).has_value());
	REQUIRE(inner.append_i64(2LL).has_value());
	std::move(inner).commit();

	std::move(root).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	auto outer = doc->root().as_array();
	REQUIRE(outer.has_value());
	REQUIRE(outer->size() == 1UZ);
	auto inner_arr = outer->element(0)->as_array();
	REQUIRE(inner_arr.has_value());
	REQUIRE(inner_arr->size() == 2UZ);
	CHECK(*inner_arr->element(0)->as_number()->to_i64() == 1LL);
	CHECK(*inner_arr->element(1)->as_number()->to_i64() == 2LL);
}

TEST_CASE(
	"json: builder typical-pattern — full spec example",
	"[json][builder][phase3]") {
	auto vb = value_builder();
	auto root_res = vb.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	auto user_res = root.insert_object("user");
	REQUIRE(user_res.has_value());
	auto &user = *user_res;
	REQUIRE(user.insert_i64("id", 42LL).has_value());
	REQUIRE(user.insert_string("name", "alice").has_value());
	std::move(user).commit();

	auto tags_res = root.insert_array("tags");
	REQUIRE(tags_res.has_value());
	auto &tags = *tags_res;
	REQUIRE(tags.append_string("x").has_value());
	REQUIRE(tags.append_string("y").has_value());
	std::move(tags).commit();

	std::move(root).commit();
	auto doc = std::move(vb).finish();
	REQUIRE(doc.has_value());

	auto dumped = doc->dump();
	REQUIRE(dumped.has_value());
	auto reparsed = parse(*dumped);
	REQUIRE(reparsed.has_value());

	auto obj = reparsed->root().as_object();
	REQUIRE(obj.has_value());
	REQUIRE(obj->member("user").has_value());
	REQUIRE(obj->member("tags").has_value());
}

TEST_CASE(
	"json: child ObjectBuilder destructor aborts — parent reactivated",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	{
		auto child_res = root.insert_object("inner");
		REQUIRE(child_res.has_value());
		auto &child = *child_res;
		REQUIRE(child.insert_i64("x", 99LL).has_value());
	}

	REQUIRE(root.insert_i64("kept", 1LL).has_value());
	std::move(root).commit();

	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	CHECK(!obj->find_member("inner").has_value());
	CHECK(obj->find_member("kept").has_value());
}

TEST_CASE(
	"json: child ArrayBuilder destructor aborts — parent reactivated",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	{
		auto child_res = root.insert_array("items");
		REQUIRE(child_res.has_value());
		auto &child = *child_res;
		REQUIRE(child.append_string("x").has_value());
	}

	REQUIRE(root.insert_string("kept", "yes").has_value());
	std::move(root).commit();

	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	CHECK(!obj->find_member("items").has_value());
	CHECK(obj->find_member("kept").has_value());
}

TEST_CASE(
	"json: ValueBuilder::begin_object abort — root_set is cleared",
	"[json][builder][phase3]") {
	auto b = value_builder();
	{
		auto obj_res = b.begin_object();
		REQUIRE(obj_res.has_value());
	}
	REQUIRE(b.set_null().has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(doc->root().is_null());
}

TEST_CASE(
	"json: duplicate member via insert_object fails before child opens",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	REQUIRE(root.insert_i64("a", 1LL).has_value());
	auto dup = root.insert_object("a");
	REQUIRE(!dup.has_value());
	CHECK(dup.error().code == JsonIssueCode::duplicate_member);
}

TEST_CASE(
	"json: duplicate member via insert_array fails before child opens",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	REQUIRE(root.insert_string("a", "v").has_value());
	auto dup = root.insert_array("a");
	REQUIRE(!dup.has_value());
	CHECK(dup.error().code == JsonIssueCode::duplicate_member);
}

TEST_CASE(
	"json: insert<T> pre-checks duplicate before encoding",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	REQUIRE(root.insert_i64("key", 1LL).has_value());
	auto dup = root.insert<std::int64_t>("key", 2LL);
	REQUIRE(!dup.has_value());
	CHECK(dup.error().code == JsonIssueCode::duplicate_member);
}

TEST_CASE(
	"json: set_number stores lexeme verbatim",
	"[json][builder][phase3]") {
	auto b = value_builder();
	REQUIRE(b.set_number("1.0").has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto dumped = doc->dump();
	REQUIRE(dumped.has_value());
	CHECK(*dumped == "1.0");
}

TEST_CASE(
	"json: set_number rejects invalid lexemes",
	"[json][builder][phase3]") {
	{
		auto b = value_builder();
		CHECK(!b.set_number("+1").has_value());
	}
	{
		auto b = value_builder();
		CHECK(!b.set_number("01").has_value());
	}
	{
		auto b = value_builder();
		CHECK(!b.set_number("1.").has_value());
	}
	{
		auto b = value_builder();
		CHECK(!b.set_number("1e").has_value());
	}
	{
		auto b = value_builder();
		CHECK(!b.set_number("-").has_value());
	}
	{
		auto b = value_builder();
		CHECK(!b.set_number("NaN").has_value());
	}
}

TEST_CASE(
	"json: append_number stores lexeme verbatim",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto arr_res = b.begin_array();
	REQUIRE(arr_res.has_value());
	auto &arr = *arr_res;
	REQUIRE(arr.append_number("3.14").has_value());
	REQUIRE(arr.append_number("1e10").has_value());
	std::move(arr).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto dumped = doc->dump();
	REQUIRE(dumped.has_value());
	CHECK(*dumped == "[3.14,1e10]");
}

TEST_CASE(
	"json: insert_number stores lexeme verbatim",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;
	REQUIRE(root.insert_number("pi", "3.14159").has_value());
	std::move(root).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	auto pi = obj->member("pi");
	REQUIRE(pi.has_value());
	auto num = pi->as_number();
	REQUIRE(num.has_value());
	CHECK(num->lexeme() == "3.14159");
}

TEST_CASE(
	"json: ValueBuilder::reset() — clears accumulated state",
	"[json][builder][phase3]") {
	auto b = value_builder();
	REQUIRE(b.set_i64(42LL).has_value());
	b.reset();
	REQUIRE(b.set_string("fresh").has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_string() == "fresh");
}

TEST_CASE(
	"json: ValueBuilder::reset() clears borrowed string storage",
	"[json][builder][lifetime]") {
	auto b = value_builder();
	std::string borrowed = "borrowed";
	auto obj = b.begin_object();
	REQUIRE(obj.has_value());
	REQUIRE(obj->insert_string_borrowed("name", borrowed).has_value());
	std::move(*obj).commit();
	b.reset();
	borrowed = "mutated";
	REQUIRE(b.set_string("owned").has_value());

	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto value = doc->root().as_string();
	REQUIRE(value.has_value());
	CHECK(*value == "owned");
}

TEST_CASE(
	"json: ValueBuilder::discard() — prevents finish",
	"[json][builder][phase3]") {
	auto b = value_builder();
	REQUIRE(b.set_null().has_value());
	std::move(b).discard();
}

TEST_CASE(
	"json: finish() fails when root never set",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto doc = std::move(b).finish();
	REQUIRE(!doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::constraint_violation);
}

TEST_CASE(
	"json: finish() fails when child builder still active",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto obj_res = b.begin_object();
	REQUIRE(obj_res.has_value());
	auto doc = std::move(b).finish();
	REQUIRE(!doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::constraint_violation);
}

TEST_CASE(
	"json: set<T> with JsonMembers type encodes as object",
	"[json][builder][phase3][members]") {
	auto b = value_builder();
	Point const p{.x = 3LL, .y = 7LL};
	REQUIRE(b.set<Point>(p).has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->x == 3LL);
	CHECK(r->y == 7LL);
}

TEST_CASE(
	"json: ObjectBuilder::insert<T> with JsonMembers type",
	"[json][builder][phase3][members]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;
	Point const p{.x = 1LL, .y = 2LL};
	REQUIRE(root.insert<Point>("pt", p).has_value());
	std::move(root).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	auto pt = obj->member("pt");
	REQUIRE(pt.has_value());
	auto r = decode<Point>(*pt);
	REQUIRE(r.has_value());
	CHECK(r->x == 1LL);
	CHECK(r->y == 2LL);
}

TEST_CASE(
	"json: ArrayBuilder::append<T> with JsonMembers type",
	"[json][builder][phase3][members]") {
	auto b = value_builder();
	auto arr_res = b.begin_array();
	REQUIRE(arr_res.has_value());
	auto &arr = *arr_res;
	REQUIRE(arr.append<Point>({.x = 10LL, .y = 20LL}).has_value());
	REQUIRE(arr.append<Point>({.x = 30LL, .y = 40LL}).has_value());
	std::move(arr).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<std::vector<Point>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 2UZ);
	CHECK((*r)[0].x == 10LL);
	CHECK((*r)[0].y == 20LL);
	CHECK((*r)[1].x == 30LL);
	CHECK((*r)[1].y == 40LL);
}

TEST_CASE(
	"json: deeply nested builder — object in A in object",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	auto items_res = root.insert_array("items");
	REQUIRE(items_res.has_value());
	auto &items = *items_res;

	auto elem_res = items.append_object();
	REQUIRE(elem_res.has_value());
	auto &elem = *elem_res;
	REQUIRE(elem.insert_bool("active", true).has_value());
	std::move(elem).commit();

	std::move(items).commit();
	std::move(root).commit();

	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	JsonPath path;
	path.push_member("items");
	path.push_index(0);
	path.push_member("active");

	auto node = doc->root().at(path);
	REQUIRE(node.has_value());
	CHECK(*node->as_bool() == true);
}

TEST_CASE(
	"json: child commit then parent can add more members",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto root_res = b.begin_object();
	REQUIRE(root_res.has_value());
	auto &root = *root_res;

	auto a_res = root.insert_object("a");
	REQUIRE(a_res.has_value());
	std::move(*a_res).commit();

	REQUIRE(root.insert_i64("b", 2LL).has_value());
	std::move(root).commit();

	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	CHECK(obj->find_member("a").has_value());
	CHECK(obj->find_member("b").has_value());
}
