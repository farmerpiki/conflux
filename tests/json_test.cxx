// Plain TU — not a module unit.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

using namespace std;

TEST_CASE(
	"json: parse null",
	"[json]") {
	auto doc = json::parse("null");
	REQUIRE(doc.has_value());
	CHECK(doc->is_null());
	CHECK(doc->type() == JsonType::Null);
}

TEST_CASE(
	"json: parse bool",
	"[json]") {
	auto t = json::parse("true");
	REQUIRE(t.has_value());
	CHECK(t->get<bool>() == true);

	auto f = json::parse("false");
	REQUIRE(f.has_value());
	CHECK(f->get<bool>() == false);
}

TEST_CASE(
	"json: parse integer",
	"[json]") {
	auto doc = json::parse("42");
	REQUIRE(doc.has_value());
	CHECK(doc->get<int64_t>() == 42LL);
}

TEST_CASE(
	"json: parse negative integer",
	"[json]") {
	auto doc = json::parse("-7");
	REQUIRE(doc.has_value());
	CHECK(doc->get<int64_t>() == -7LL);
}

TEST_CASE(
	"json: parse uint overflow",
	"[json]") {
	auto doc = json::parse("18446744073709551615"); // UINT64_MAX
	REQUIRE(doc.has_value());
	CHECK(doc->get<uint64_t>() == UINT64_MAX);
}

TEST_CASE(
	"json: parse float",
	"[json]") {
	auto doc = json::parse("3.14");
	REQUIRE(doc.has_value());
	auto v = doc->get<double>();
	REQUIRE(v.has_value());
	CHECK(*v == Catch::Approx(3.14));
}

TEST_CASE(
	"json: parse string",
	"[json]") {
	auto doc = json::parse(R"("hello world")");
	REQUIRE(doc.has_value());
	CHECK(doc->get<string_view>() == "hello world");
}

TEST_CASE(
	"json: parse string with escape",
	"[json]") {
	auto doc = json::parse(R"("a\tb\nc")");
	REQUIRE(doc.has_value());
	auto sv = doc->get<string_view>();
	REQUIRE(sv.has_value());
	CHECK(*sv == "a\tb\nc");
}

TEST_CASE(
	"json: parse string with unicode escape",
	"[json]") {
	auto doc = json::parse(R"("\u0041\u0042\u0043")"); // ABC
	REQUIRE(doc.has_value());
	CHECK(doc->get<string_view>() == "ABC");
}

TEST_CASE(
	"json: lone low surrogate is invalid unicode",
	"[json]") {
	auto doc = json::parse(R"("\uDC00")");
	REQUIRE(!doc.has_value());
	CHECK(doc.error().code == ParseError::Code::InvalidUnicode);
}

TEST_CASE(
	"json: parse empty string",
	"[json]") {
	auto doc = json::parse(R"("")");
	REQUIRE(doc.has_value());
	CHECK(doc->get<string_view>() == "");
}

TEST_CASE(
	"json: parse array",
	"[json]") {
	auto doc = json::parse("[1, 2, 3]");
	REQUIRE(doc.has_value());
	REQUIRE(doc->is_array());
	CHECK(doc->size() == 3UZ);
	CHECK((*doc)[0].get<int64_t>() == 1LL);
	CHECK((*doc)[1].get<int64_t>() == 2LL);
	CHECK((*doc)[2].get<int64_t>() == 3LL);
}

TEST_CASE(
	"json: parse nested array",
	"[json]") {
	auto doc = json::parse("[[1, 2], [3, 4]]");
	REQUIRE(doc.has_value());
	REQUIRE(doc->is_array());
	CHECK(doc->size() == 2UZ);
	CHECK((*doc)[0][0].get<int64_t>() == 1LL);
	CHECK((*doc)[1][1].get<int64_t>() == 4LL);
}

TEST_CASE(
	"json: parse object",
	"[json]") {
	auto doc = json::parse(R"({"a": 1, "b": "two"})");
	REQUIRE(doc.has_value());
	REQUIRE(doc->is_object());
	CHECK((*doc)["a"].get<int64_t>() == 1LL);
	CHECK((*doc)["b"].get<string_view>() == "two");
}

TEST_CASE(
	"json: parse nested object",
	"[json]") {
	auto doc = json::parse(R"({"x": {"y": 42}})");
	REQUIRE(doc.has_value());
	CHECK((*doc)["x"]["y"].get<int64_t>() == 42LL);
}

TEST_CASE(
	"json: out of bounds array access returns null",
	"[json]") {
	auto doc = json::parse("[1]");
	REQUIRE(doc.has_value());
	// Non-const op[] on array throws out_of_range; const op[] returns null Value.
	CHECK(as_const(*doc)[99].is_null());
}

TEST_CASE(
	"json: missing key returns null",
	"[json]") {
	auto doc = json::parse(R"({"a": 1})");
	REQUIRE(doc.has_value());
	// Non-const op[] on object auto-inserts null on miss — use const for pure reads.
	CHECK(as_const(*doc)["missing"].is_null());
}

TEST_CASE(
	"json: get_or returns default on missing key",
	"[json]") {
	auto doc = json::parse(R"({"a": 1})");
	REQUIRE(doc.has_value());
	CHECK(doc->get_or<int64_t>("missing", 99LL) == 99LL);
	CHECK(doc->get_or<int64_t>("a", 99LL) == 1LL);
}

TEST_CASE(
	"json: as<> throws on type mismatch",
	"[json]") {
	auto doc = json::parse("42");
	REQUIRE(doc.has_value());
	CHECK_THROWS(doc->as<string_view>());
}

TEST_CASE(
	"json: dump roundtrip",
	"[json]") {
	string const src = R"({"k":[1,2,3],"n":null,"b":true})";
	auto doc = json::parse(src);
	REQUIRE(doc.has_value());
	auto dumped = doc->dump();
	auto doc2 = json::parse(dumped);
	REQUIRE(doc2.has_value());
	CHECK((*doc2)["k"][0].get<int64_t>() == 1LL);
	CHECK((*doc2)["n"].is_null());
	CHECK((*doc2)["b"].get<bool>() == true);
}

TEST_CASE(
	"json: parse(string_view) is self-contained",
	"[json]") {
	string src{R"({"msg": "hello"})"};
	auto doc = json::parse(src);
	REQUIRE(doc.has_value());
	src.clear(); // invalidate source
	CHECK((*doc)["msg"].get<string>().value_or("") == "hello");

	string src2{R"({"k": "owned"})"};
	auto doc2 = json::parse<string>(std::move(src2));
	REQUIRE(doc2.has_value());
	CHECK((*doc2)["k"].get<string>().value_or("") == "owned");
}

TEST_CASE(
	"json: parse_borrowed keeps document-managed backing alive",
	"[json]") {
	string src{R"({"msg": "borrowed"})"};
	auto doc = json::parse_borrowed(std::move(src));
	REQUIRE(doc.has_value());
	CHECK((*doc)["msg"].get<string_view>() == "borrowed");
}

TEST_CASE(
	"json: builder — object_set and array_push",
	"[json]") {
	Document doc;
	auto obj = json::object({});
	json::object_set(obj, "x", json::int_value(10));
	json::object_set(obj, "y", json::string_value(string_view{"hi"}));
	doc.set_root(std::move(obj));
	CHECK(doc["x"].get<int64_t>() == 10LL);
	CHECK(doc["y"].get<string_view>() == "hi");

	Document doc2;
	auto arr = json::array({});
	json::array_push(arr, json::int_value(1));
	json::array_push(arr, json::int_value(2));
	doc2.set_root(std::move(arr));
	CHECK(doc2[0].get<int64_t>() == 1LL);
	CHECK(doc2.size() == 2UZ);
}

TEST_CASE(
	"json: parse error on truncated input",
	"[json]") {
	auto doc = json::parse(R"({"a":)");
	CHECK(!doc.has_value());
}

TEST_CASE(
	"json: parse error on invalid token",
	"[json]") {
	auto doc = json::parse("xyz");
	CHECK(!doc.has_value());
}

TEST_CASE(
	"json: whitespace tolerance",
	"[json]") {
	auto doc = json::parse("  {  \"k\"  :  42  }  ");
	REQUIRE(doc.has_value());
	CHECK((*doc)["k"].get<int64_t>() == 42LL);
}

TEST_CASE(
	"json: empty array and object",
	"[json]") {
	auto arr = json::parse("[]");
	REQUIRE(arr.has_value());
	CHECK(arr->size() == 0UZ);
	CHECK(arr->empty());

	auto obj = json::parse("{}");
	REQUIRE(obj.has_value());
	CHECK(obj->size() == 0UZ);
}

TEST_CASE(
	"json: deeply nested structure",
	"[json]") {
	// Build deep nesting via parse to exercise iterative parser stack.
	string nested = "[";
	for (int i = 0; i < 50; ++i) {
		nested += "[";
	}
	nested += "1";
	for (int i = 0; i < 50; ++i) {
		nested += "]";
	}
	nested += "]";
	auto doc = json::parse(nested);
	REQUIRE(doc.has_value());
}

// RFC 8259 §6 number grammar — cases found via JSONTestSuite conformance run.
TEST_CASE(
	"json: reject leading zeros",
	"[json][conformance]") {
	CHECK(!json::parse("[012]").has_value());
	CHECK(!json::parse("[-01]").has_value());
	CHECK(json::parse("[0]").has_value()); // lone zero is fine
	CHECK(json::parse("[-0]").has_value()); // negative zero is fine
}

TEST_CASE(
	"json: reject trailing decimal point",
	"[json][conformance]") {
	CHECK(!json::parse("[-2.]").has_value());
	CHECK(!json::parse("[2.]").has_value());
	CHECK(!json::parse("[0.e1]").has_value());
	CHECK(!json::parse("[2.e+3]").has_value());
	CHECK(!json::parse("[2.e-3]").has_value());
	CHECK(!json::parse("[2.e3]").has_value());
}

TEST_CASE(
	"json: reject no integer part before decimal",
	"[json][conformance]") {
	CHECK(!json::parse("[-.123]").has_value());
	CHECK(!json::parse("[.5]").has_value());
}

// ---------------------------------------------------------------------------
// Modernized builder API — implicit scalar ctors, mutating op[], push_back,
// set, contains, clone.
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: implicit scalar conversion",
	"[json][builder]") {
	Value v_int = 42;
	Value v_uint = 42U;
	Value v_float = 3.14;
	Value v_bool = true;
	Value v_cstr = "hello";
	Value v_null = nullptr;
	CHECK(v_int.get<int64_t>() == 42LL);
	CHECK(v_uint.get<uint64_t>() == 42ULL);
	CHECK(*v_float.get<double>() == Catch::Approx(3.14));
	CHECK(v_bool.get<bool>() == true);
	CHECK(v_cstr.get<string_view>() == "hello");
	CHECK(v_null.is_null());
}

TEST_CASE(
	"json: bulk array/object construction",
	"[json][builder]") {
	// Implicit ctors make bulk init clean.
	Value arr = json::array({1, "two", true, nullptr, 3.14});
	CHECK(arr.size() == 5UZ);
	CHECK(arr[0].get<int64_t>() == 1LL);
	CHECK(arr[1].get<string_view>() == "two");
	CHECK(arr[2].get<bool>() == true);
	CHECK(arr[3].is_null());

	Value obj = json::object({
		{  "name", "alice"},
		{   "age",      30},
		{"active",    true},
	});
	CHECK(obj["name"].get<string_view>() == "alice");
	CHECK(obj["age"].get<int64_t>() == 30LL);
	CHECK(obj["active"].get<bool>() == true);
}

TEST_CASE(
	"json: mutating op[] on object",
	"[json][builder]") {
	Value obj = json::object({});
	obj["x"] = 10;
	obj["y"] = "hi";
	obj["nested"] = json::object({
		{"inner", 1}
    });
	CHECK(obj["x"].get<int64_t>() == 10LL);
	CHECK(obj["y"].get<string_view>() == "hi");
	CHECK(obj["nested"]["inner"].get<int64_t>() == 1LL);

	// Null upgrades to object on first write.
	Value v;
	v["k"] = 99;
	CHECK(v.is_object());
	CHECK(v["k"].get<int64_t>() == 99LL);
}

TEST_CASE(
	"json: push_back and set",
	"[json][builder]") {
	Value arr = json::array({});
	arr.push_back(1);
	arr.push_back("two");
	arr.push_back(nullptr);
	CHECK(arr.size() == 3UZ);
	CHECK(arr[0].get<int64_t>() == 1LL);

	// push_back on null upgrades to array.
	Value v2;
	v2.push_back(42);
	CHECK(v2.is_array());
	CHECK(v2.size() == 1UZ);

	Value obj;
	obj.set("k", 1);
	obj.set("k", 2); // replace
	CHECK(obj["k"].get<int64_t>() == 2LL);
}

TEST_CASE(
	"json: contains",
	"[json][builder]") {
	auto obj = json::object({
		{"a", 1},
		{"b", 2}
    });
	CHECK(obj.contains("a"));
	CHECK(obj.contains("b"));
	CHECK(!obj.contains("c"));
	// Non-object value returns false, does not throw.
	Value v = 42;
	CHECK(!v.contains("anything"));
}

TEST_CASE(
	"json: op[] throws on wrong type",
	"[json][builder]") {
	Value v = 42;
	CHECK_THROWS(v["key"] = 1);
	Value arr = json::array({1, 2});
	CHECK_THROWS(arr[99] = 0); // OOB
}

TEST_CASE(
	"json: clone produces independent copy",
	"[json][builder]") {
	auto orig = json::object({
		{ "arr", json::array({1, 2, 3})},
		{"name",					"x"},
	});
	auto copy = orig.clone();
	copy["name"] = "changed";
	copy["arr"].push_back(4);

	CHECK(orig["name"].get<string_view>() == "x");
	CHECK(orig["arr"].size() == 3UZ);
	CHECK(copy["name"].get<string_view>() == "changed");
	CHECK(copy["arr"].size() == 4UZ);
}

TEST_CASE(
	"json: dump roundtrip with modern builder",
	"[json][builder]") {
	auto v = json::object({
		{"items", json::array({1, 2, 3})},
		{   "ok",				   true},
		{ "name",				 "test"},
	});
	auto dumped = v.dump();
	auto parsed = json::parse(dumped);
	REQUIRE(parsed.has_value());
	CHECK((*parsed)["ok"].get<bool>() == true);
	CHECK((*parsed)["items"][1].get<int64_t>() == 2LL);
}

TEST_CASE(
	"json: push_back on non-array non-null throws",
	"[json][builder]") {
	Value v = 42;
	CHECK_THROWS_AS(v.push_back(1), std::invalid_argument);

	Value s = "hello";
	CHECK_THROWS_AS(s.push_back(1), std::invalid_argument);
}

TEST_CASE(
	"json: set on non-object non-null throws",
	"[json][builder]") {
	Value v = 42;
	CHECK_THROWS_AS(v.set("k", 1), std::invalid_argument);

	Value arr = json::array({1, 2});
	CHECK_THROWS_AS(arr.set("k", 1), std::invalid_argument);
}

TEST_CASE(
	"json: dump string with control characters",
	"[json]") {
	Value v = "a\tb\nc\rd";
	auto dumped = v.dump();
	// Control chars must be escaped.
	CHECK(dumped == R"("a\tb\nc\rd")");
}

TEST_CASE(
	"json: dump string with backslash and quote",
	"[json]") {
	Value v = "say \"hi\" \\path";
	auto dumped = v.dump();
	CHECK(dumped == R"("say \"hi\" \\path")");
}

TEST_CASE(
	"json: parse surrogate pair produces correct UTF-8",
	"[json]") {
	// U+1F600 via JSON surrogate pair escape; UTF-8 is F0 9F 98 80
	auto doc = json::parse(R"("\uD83D\uDE00")");
	REQUIRE(doc.has_value());
	CHECK(doc->get<string_view>() == "\xF0\x9F\x98\x80");
}

TEST_CASE(
	"json: lone high surrogate without following \\u is rejected",
	"[json]") {
	auto doc = json::parse(R"("\uD83D")");
	CHECK_FALSE(doc.has_value());
}

TEST_CASE(
	"json: high surrogate followed by non-surrogate low is rejected",
	"[json]") {
	// \uD83D is a high surrogate; A (A) is not a low surrogate
	auto doc = json::parse(R"("\uD83DA")");
	CHECK_FALSE(doc.has_value());
}

TEST_CASE(
	"json: lone low surrogate is rejected",
	"[json]") {
	// \uDC00 is a bare low surrogate — not valid without a preceding high surrogate
	auto doc = json::parse(R"("\uDC00")");
	CHECK_FALSE(doc.has_value());
}

TEST_CASE(
	"json: dump NaN produces null",
	"[json]") {
	Value v{numeric_limits<double>::quiet_NaN()};
	CHECK(v.dump() == "null");
}

TEST_CASE(
	"json: dump Infinity produces null",
	"[json]") {
	Value v{numeric_limits<double>::infinity()};
	CHECK(v.dump() == "null");
}

TEST_CASE(
	"json: dump negative Infinity produces null",
	"[json]") {
	Value v{-numeric_limits<double>::infinity()};
	CHECK(v.dump() == "null");
}
