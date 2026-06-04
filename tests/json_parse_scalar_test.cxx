// Plain TU - not a module unit.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: parse null",
	"[json]") {
	auto doc = parse("null");
	REQUIRE(doc.has_value());
	CHECK(doc->root().is_null());
	CHECK(doc->root().kind() == JsonKind::null);
}
TEST_CASE(
	"json: parse true/false",
	"[json]") {
	{
		auto doc = parse("true");
		REQUIRE(doc.has_value());
		auto b = doc->root().as_bool();
		REQUIRE(b.has_value());
		CHECK(*b == true);
	}
	{
		auto doc = parse("false");
		REQUIRE(doc.has_value());
		auto b = doc->root().as_bool();
		REQUIRE(b.has_value());
		CHECK(*b == false);
	}
}
TEST_CASE(
	"json: parse integer",
	"[json]") {
	auto doc = parse("42");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	CHECK(n->form() == JsonNumberForm::integer);
	auto i = n->to_i64();
	REQUIRE(i.has_value());
	CHECK(*i == 42LL);
}
TEST_CASE(
	"json: parse negative integer",
	"[json]") {
	auto doc = parse("-7");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto i = n->to_i64();
	REQUIRE(i.has_value());
	CHECK(*i == -7LL);
}
TEST_CASE(
	"json: parse uint64 max",
	"[json]") {
	auto doc = parse("18446744073709551615");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto u = n->to_u64();
	REQUIRE(u.has_value());
	CHECK(*u == std::numeric_limits<std::uint64_t>::max());
}
TEST_CASE(
	"json: parse float",
	"[json]") {
	auto doc = parse("3.14");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	CHECK(n->form() == JsonNumberForm::non_integer);
	auto f = n->to_f64();
	REQUIRE(f.has_value());
	CHECK(*f == Catch::Approx(3.14));
}
TEST_CASE(
	"json: parse S",
	"[json]") {
	auto doc = parse(R"("hello world")");
	REQUIRE(doc.has_value());
	auto s = doc->root().as_string();
	REQUIRE(s.has_value());
	CHECK(*s == "hello world");
}
TEST_CASE(
	"json: parse std::string with escape sequences",
	"[json]") {
	auto doc = parse(R"("a\tb\nc")");
	REQUIRE(doc.has_value());
	auto s = doc->root().as_string();
	REQUIRE(s.has_value());
	CHECK(*s == "a\tb\nc");
}
TEST_CASE(
	"json: parse std::string with unicode escape",
	"[json]") {
	auto doc = parse(R"("ABC")");
	REQUIRE(doc.has_value());
	auto s = doc->root().as_string();
	REQUIRE(s.has_value());
	CHECK(*s == "ABC");
}
TEST_CASE(
	"json: parse surrogate pair",
	"[json]") {
	auto doc = parse(R"("😀")");
	REQUIRE(doc.has_value());
	auto s = doc->root().as_string();
	REQUIRE(s.has_value());
	CHECK(*s == "\xF0\x9F\x98\x80");
}
TEST_CASE(
	"json: reject lone high surrogate",
	"[json]") {
	CHECK_FALSE(parse(R"("\uD83D")").has_value());
}
TEST_CASE(
	"json: reject lone low surrogate",
	"[json]") {
	CHECK_FALSE(parse(R"("\uDC00")").has_value());
}
TEST_CASE(
	"json: reject high surrogate followed by non-surrogate-low",
	"[json]") {
	CHECK_FALSE(parse(R"("\uD83DA")").has_value());
}
