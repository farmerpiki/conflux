// Plain TU — not a module unit.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

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
