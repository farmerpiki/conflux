// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

using namespace conflux::json;

// ---------------------------------------------------------------------------
// Phase 8.1 — JSON5 relaxed parse mode
// ---------------------------------------------------------------------------

static constexpr JsonParseOptions json5_opts{.mode = ParseMode::json5};
TEST_CASE(
	"phase8: json5 line comment",
	"[phase8]") {
	auto doc = parse("// comment\n42", json5_opts);
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_i64() == 42);
}
TEST_CASE(
	"phase8: json5 block comment",
	"[phase8]") {
	auto doc = parse("/* block */42", json5_opts);
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_i64() == 42);
}
TEST_CASE(
	"phase8: json5 block comment with newlines",
	"[phase8]") {
	auto doc = parse("/*\n multi\n line\n*/42", json5_opts);
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_i64() == 42);
}
TEST_CASE(
	"phase8: json5 inline comment after value",
	"[phase8]") {
	auto doc = parse("[1, /* mid */ 2]", json5_opts);
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	CHECK(*a.element(0)->as_number()->to_i64() == 1);
	CHECK(*a.element(1)->as_number()->to_i64() == 2);
}
TEST_CASE(
	"phase8: json5 unterminated block comment",
	"[phase8]") {
	auto doc = parse("/* never closed", json5_opts);
	CHECK_FALSE(doc.has_value());
	CHECK(doc.error().message == "unterminated block comment");
	REQUIRE(doc.error().source.has_value());
	CHECK(doc.error().source->offset == 0);
}
TEST_CASE(
	"phase8: json5 SAX unterminated block comment",
	"[phase8]") {
	JsonReader r{"[1, /* never closed", json5_opts};
	REQUIRE(r.next().has_value());
	REQUIRE(r.next().has_value());
	auto ev = r.next();
	CHECK_FALSE(ev.has_value());
	CHECK(ev.error().message == "unterminated block comment");
}
TEST_CASE(
	"phase8: json5 trailing comma in array",
	"[phase8]") {
	auto doc = parse("[1, 2, 3,]", json5_opts);
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	CHECK(a.size() == 3);
	CHECK(*a.element(2)->as_number()->to_i64() == 3);
}
TEST_CASE(
	"phase8: json5 trailing comma in object",
	"[phase8]") {
	auto doc = parse(R"({"a": 1, "b": 2,})", json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("a")->as_number()->to_i64() == 1);
	CHECK(*o.member("b")->as_number()->to_i64() == 2);
}
TEST_CASE(
	"phase8: strict mode rejects trailing comma",
	"[phase8]") {
	CHECK_FALSE(parse("[1,]").has_value());
	CHECK_FALSE(parse(R"({"a":1,})").has_value());
}
TEST_CASE(
	"phase8: json5 single-quoted string value",
	"[phase8]") {
	auto doc = parse("'hello'", json5_opts);
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_string() == "hello");
}
TEST_CASE(
	"phase8: json5 single-quoted string with escapes",
	"[phase8]") {
	auto doc = parse(R"('it\'s \"fine\"')", json5_opts);
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_string() == "it's \"fine\"");
}
TEST_CASE(
	"phase8: json5 reader rejects unsupported string escapes",
	"[phase8]") {
	JsonReader double_quoted{R"("bad\q")", json5_opts};
	auto ev = double_quoted.next();
	REQUIRE_FALSE(ev.has_value());
	CHECK(ev.error().message == "invalid escape");

	JsonReader single_quoted{R"('bad\q')", json5_opts};
	ev = single_quoted.next();
	REQUIRE_FALSE(ev.has_value());
	CHECK(ev.error().message == "invalid escape");
}
TEST_CASE(
	"phase8: json5 single-quoted string in array",
	"[phase8]") {
	auto doc = parse("['a', 'b']", json5_opts);
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	CHECK(*a.element(0)->as_string() == "a");
	CHECK(*a.element(1)->as_string() == "b");
}
TEST_CASE(
	"phase8: json5 unquoted key",
	"[phase8]") {
	auto doc = parse("{foo: 1, bar: 2}", json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("foo")->as_number()->to_i64() == 1);
	CHECK(*o.member("bar")->as_number()->to_i64() == 2);
}
TEST_CASE(
	"phase8: json5 unquoted key with underscore and dollar",
	"[phase8]") {
	auto doc = parse("{_priv: 1, $ref: 2, a1b2: 3}", json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("_priv")->as_number()->to_i64() == 1);
	CHECK(*o.member("$ref")->as_number()->to_i64() == 2);
	CHECK(*o.member("a1b2")->as_number()->to_i64() == 3);
}
TEST_CASE(
	"phase8: json5 mixed quoted and unquoted keys dedup",
	"[phase8]") {
	JsonParseOptions opts5{.duplicate_key = DuplicateKeyPolicy::reject, .mode = ParseMode::json5};
	auto doc = parse(R"({"a": 1, a: 2})", opts5);
	CHECK_FALSE(doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::duplicate_member);
}
TEST_CASE(
	"phase8: json5 single-quoted key",
	"[phase8]") {
	auto doc = parse("{'key': 42}", json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("key")->as_number()->to_i64() == 42);
}
TEST_CASE(
	"phase8: json5 combined features",
	"[phase8]") {
	auto doc = parse(
		R"({
// line comment
name:'Alice',
age:30,
/* block comment */
tags:['a',"b",],
})",
		json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("name")->as_string() == "Alice");
	CHECK(*o.member("age")->as_number()->to_i64() == 30);
	auto tags = *o.member("tags")->as_array();
	CHECK(tags.size() == 2);
	CHECK(*tags.element(0)->as_string() == "a");
	CHECK(*tags.element(1)->as_string() == "b");
}
TEST_CASE(
	"phase8: strict mode rejects json5 features",
	"[phase8]") {
	CHECK_FALSE(parse("// comment\n42").has_value());
	CHECK_FALSE(parse("{foo: 1}").has_value());
	CHECK_FALSE(parse("'hello'").has_value());
}
TEST_CASE(
	"phase8: json5 empty trailing comma array",
	"[phase8]") {
	auto doc = parse("[,]", json5_opts);
	CHECK_FALSE(doc.has_value());
}
TEST_CASE(
	"phase8: json5 comment between key and colon",
	"[phase8]") {
	auto doc = parse("{foo /* c */ : 1}", json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("foo")->as_number()->to_i64() == 1);
}
TEST_CASE(
	"phase8: json5 SAX reader with comments and trailing comma",
	"[phase8]") {
	JsonReader r{"[1, /* x */ 2,]", json5_opts};
	using Ev = JsonReader::Event;
	auto e1 = r.next();
	REQUIRE(e1.has_value());
	CHECK(**e1 == Ev::begin_array);
	auto e2 = r.next();
	REQUIRE(e2.has_value());
	CHECK(**e2 == Ev::number_value);
	auto e3 = r.next();
	REQUIRE(e3.has_value());
	CHECK(**e3 == Ev::number_value);
	auto e4 = r.next();
	REQUIRE(e4.has_value());
	CHECK(**e4 == Ev::end_array);
	auto e5 = r.next();
	REQUIRE(e5.has_value());
	CHECK_FALSE(e5->has_value());
}
TEST_CASE(
	"phase8: json5 SAX reader unquoted key",
	"[phase8]") {
	JsonReader r{"{foo: 'bar',}", json5_opts};
	using Ev = JsonReader::Event;
	auto e1 = r.next();
	REQUIRE(e1.has_value());
	CHECK(**e1 == Ev::begin_object);
	auto e2 = r.next();
	REQUIRE(e2.has_value());
	CHECK(**e2 == Ev::key);
	std::string key_str;
	REQUIRE(r.key_token().append_decoded_to(key_str).has_value());
	CHECK(key_str == "foo");
	auto e3 = r.next();
	REQUIRE(e3.has_value());
	CHECK(**e3 == Ev::string_value);
	std::string val_str;
	REQUIRE(r.string_token().append_decoded_to(val_str).has_value());
	CHECK(val_str == "bar");
	auto e4 = r.next();
	REQUIRE(e4.has_value());
	CHECK(**e4 == Ev::end_object);
}
