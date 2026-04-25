// External JSON conformance tests.
// Sources:
//   - nativejson-benchmark / miloyip: https://github.com/miloyip/nativejson-benchmark/tree/master/data/jsonchecker
//     (itself derived from json.org JSON_checker, distinct from nst/JSONTestSuite)
//   - RFC 8259 section-5 / section-6 hand-written edge cases
//   - Number regression cases from conflux.json fix history
//
// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

namespace json = conflux::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void check_valid(
	std::string_view s) {
	INFO("Expected valid: " << s);
	CHECK(json::parse(s).has_value());
}

static void check_invalid(
	std::string_view s) {
	INFO("Expected invalid: " << s);
	CHECK(!json::parse(s).has_value());
}

// ---------------------------------------------------------------------------
// nativejson-benchmark / miloyip jsonchecker — PASS cases
// (https://github.com/miloyip/nativejson-benchmark/tree/master/data/jsonchecker)
// ---------------------------------------------------------------------------

TEST_CASE(
	"jsonchecker/miloyip: pass01 — large mixed array",
	"[conformance][miloyip][pass]") {
	// pass01.json from miloyip/nativejson-benchmark
	check_valid(R"([
    "JSON Test Pattern pass1",
    {"object with 1 member":["array with 1 element"]},
    {},
    [],
    -42,
    true,
    false,
    null,
    {
        "integer": 1234567890,
        "real": -9876.543210,
        "e": 0.123456789e-12,
        "E": 1.234567890E+34,
        "":  23456789012E66,
        "zero": 0,
        "one": 1,
        "space": " ",
        "quote": "\"",
        "backslash": "\\",
        "controls": "\b\f\n\r\t",
        "slash": "/ & \/",
        "alpha": "abcdefghijklmnopqrstuvwyz",
        "ALPHA": "ABCDEFGHIJKLMNOPQRSTUVWYZ",
        "digit": "0123456789",
        "0123456789": "digit",
        "special": "`1~!@#$%^&*()_+-={':[,]}|;.</>?",
        "hex": "\u0123\u4567\u89AB\uCDEF\uabcd\uef4A",
        "true": true,
        "false": false,
        "null": null,
        "array":[  ],
        "object":{  },
        "address": "50 St. James Street",
        "url": "http://www.JSON.org/",
        "comment": "// /* <!-- --",
        "# -- --> */": " ",
        " s p a c e d " :[1,2,3,4,5,6,7],
        "compact":[1,2,3,4,5,6,7],
        "jsontext": "{\"object with 1 member\":[\"array with 1 element\"]}",
        "quotes": "&#34; \u0022 %22 0x22 034 &#x22;",
        "\/\\\"\uCAFE\uBABE\uAB98\uFCDE\ubcda\uef4A\b\f\n\r\t`1~!@#$%^&*()_+-=[]{}|;:',./<>?": "A key can be any string"
    },
    0.5,98.6,99.44,1066,1e1,0.1e1,1e-1,1e00,2e+00,2e-00,"rosebud"
])");
}

TEST_CASE(
	"jsonchecker/miloyip: pass02 — deeply nested array",
	"[conformance][miloyip][pass]") {
	// pass02.json — 19 levels of nesting
	check_valid(R"([[[[[[[[[[[[[[[[[[["Not too deep"]]]]]]]]]]]]]]]]]]])");
}

TEST_CASE(
	"jsonchecker/miloyip: pass03 — simple object",
	"[conformance][miloyip][pass]") {
	// pass03.json
	check_valid(R"({
    "JSON Test Pattern pass3": {
        "The outermost value": "must be an object or array.",
        "In this test": "It is an object."
    }
})");
}

// ---------------------------------------------------------------------------
// nativejson-benchmark / miloyip jsonchecker — FAIL cases
// ---------------------------------------------------------------------------

TEST_CASE(
	"jsonchecker/miloyip: fail cases — structural errors",
	"[conformance][miloyip][fail]") {
	// fail02: unclosed array
	check_invalid(R"(["Unclosed array")");

	// fail03: unquoted key
	check_invalid(R"({unquoted_key: "keys must be quoted"})");

	// fail04: extra trailing comma in array
	check_invalid(R"(["extra comma",])");

	// fail05: double extra comma
	check_invalid(R"(["double extra comma",,])");

	// fail06: missing value before comma
	check_invalid(R"([   , "<-- missing value"])");

	// fail07: comma after close bracket
	check_invalid(R"(["Comma after the close"],)");

	// fail08: extra close bracket
	check_invalid(R"(["Extra close"]])");

	// fail09: trailing comma in object
	check_invalid(R"({"Extra comma": true,})");

	// fail10: extra value after closing brace
	check_invalid(R"({"Extra value after close": true} "misplaced quoted value")");

	// fail32: comma instead of closing brace (unclosed object)
	check_invalid("{\"Comma instead if closing brace\": true,\n");

	// fail33: mismatched brackets
	check_invalid(R"(["mismatch"})");
}

TEST_CASE(
	"jsonchecker/miloyip: fail cases — bad values and keywords",
	"[conformance][miloyip][fail]") {
	// fail11: illegal expression
	check_invalid(R"({"Illegal expression": 1 + 2})");

	// fail12: illegal invocation
	check_invalid(R"({"Illegal invocation": alert()})");

	// fail23: bare word truth (not true)
	check_invalid(R"(["Bad value", truth])");

	// fail19: missing colon
	check_invalid(R"({"Missing colon" null})");

	// fail20: double colon
	check_invalid(R"({"Double colon":: null})");

	// fail21: comma instead of colon
	check_invalid(R"({"Comma instead of colon", null})");

	// fail22: colon instead of comma
	check_invalid(R"(["Colon instead of comma": false])");
}

TEST_CASE(
	"jsonchecker/miloyip: fail cases — invalid numbers",
	"[conformance][miloyip][fail]") {
	// fail13: leading zero
	check_invalid(R"({"Numbers cannot have leading zeroes": 013})");

	// fail14: hex literal
	check_invalid(R"({"Numbers cannot be hex": 0x14})");

	// fail29: bare exponent
	check_invalid("[0e]");

	// fail30: exponent with sign but no digits
	check_invalid("[0e+]");

	// fail31: double exponent sign
	check_invalid("[0e+-1]");
}

TEST_CASE(
	"jsonchecker/miloyip: fail cases — invalid strings and escapes",
	"[conformance][miloyip][fail]") {
	// fail15: illegal \x escape
	check_invalid("[\"Illegal backslash escape: \\x15\"]");

	// fail16: bare backslash before identifier
	check_invalid("[\\naked]");

	// fail17: octal escape \017
	check_invalid("[\"Illegal backslash escape: \\017\"]");

	// fail24: single-quoted string
	check_invalid("['single quote']");

	// fail26: backslash-space (bad continuation)
	check_invalid("[\"tab\\   character\\   in\\  string\\  \"]");

	// fail28: escaped newline (not a valid JSON escape)
	check_invalid("[\"line\\\nbreak\"]");
}

TEST_CASE(
	"jsonchecker/miloyip: fail cases — literal control characters in strings",
	"[conformance][miloyip][fail]") {
	// fail25: literal tab character inside string
	check_invalid("[\"literal\ttab\"]");

	// fail27: literal newline inside string
	check_invalid("[\"line\nbreak\"]");
}

// ---------------------------------------------------------------------------
// RFC 8259 — section 5 (objects) and section 6 (numbers) hand-written cases
// ---------------------------------------------------------------------------

TEST_CASE(
	"RFC 8259 §5: object structure",
	"[conformance][rfc8259][pass]") {
	check_valid("{}");
	check_valid("{\"a\":1}");
	check_valid("{\"a\":1,\"b\":2}");
	check_valid("{\"key\":null}");
	check_valid("{\"key\":true}");
	check_valid("{\"key\":false}");
	check_valid("{\"key\":[1,2,3]}");
	check_valid("{\"key\":{\"nested\":\"value\"}}");
	check_valid("{\"\":\"empty key is valid\"}");
}

TEST_CASE(
	"RFC 8259 §4: array structure",
	"[conformance][rfc8259][pass]") {
	check_valid("[]");
	check_valid("[1]");
	check_valid("[1,2,3]");
	check_valid("[null,true,false]");
	check_valid("[[],[]]");
	check_valid("[{\"a\":1},{\"b\":2}]");
	check_valid("[\"hello\",\"world\"]");
}

TEST_CASE(
	"RFC 8259 §6: number literals",
	"[conformance][rfc8259][pass]") {
	check_valid("0");
	check_valid("-0");
	check_valid("1");
	check_valid("-1");
	check_valid("1.0");
	check_valid("-1.0");
	check_valid("1e1");
	check_valid("1E1");
	check_valid("1e+1");
	check_valid("1e-1");
	check_valid("1.5e10");
	check_valid("1.5E-10");
	check_valid("0.0");
	check_valid("-0.0");
	check_valid("1234567890");
	check_valid("-9876543210");
	check_valid("1.23456789e100");
	check_valid("[0]");
	check_valid("[1]");
	check_valid("[-1]");
	check_valid("[0.1]");
	check_valid("[0e1]");
	check_valid("[0E1]");
	check_valid("[0.1e1]");
	check_valid("[1e100]");
	check_valid("[1E100]");
}

TEST_CASE(
	"RFC 8259 §6: invalid number forms",
	"[conformance][rfc8259][fail]") {
	// Leading plus sign
	check_invalid("[+1]");
	// Bare decimal point
	check_invalid("[.]");
	// Infinity (not in RFC 8259)
	check_invalid("[Infinity]");
	check_invalid("[-Infinity]");
	// NaN (not in RFC 8259)
	check_invalid("[NaN]");
	// Bare number with leading zero
	check_invalid("[01]");
}

TEST_CASE(
	"RFC 8259 §7: string unicode escapes",
	"[conformance][rfc8259][pass]") {
	check_valid("\"\"");
	check_valid("\"hello\"");
	check_valid("\"\\u0000\"");
	check_valid("\"\\uFFFF\"");
	check_valid("\"\\u0041\""); // 'A'
	check_valid("\"\\uD83D\\uDE00\""); // surrogate pair: emoji
	check_valid("[\"\\u4E2D\\u6587\"]"); // Chinese characters
}

TEST_CASE(
	"RFC 8259 §7: invalid string escapes",
	"[conformance][rfc8259][fail]") {
	// Incomplete unicode escape
	check_invalid("[\"\\u123\"]");
	// Bad unicode escape (non-hex)
	check_invalid("[\"\\uGHIJ\"]");
	// Unescaped control character U+0001 — RFC 8259 §7 forbids U+0000..U+001F unescaped
	check_invalid("[\"\x01\"]");
	// Unescaped BEL (U+0007)
	check_invalid("[\"\x07\"]");
	// Unescaped vertical tab (U+000B)
	check_invalid("[\"\x0b\"]");
	// Unescaped form-feed (U+000C)
	check_invalid("[\"\x0c\"]");
}

// ---------------------------------------------------------------------------
// Number regression — 8 cases from conflux.json fix history
// ---------------------------------------------------------------------------

TEST_CASE(
	"number regression: leading zeros and malformed decimals",
	"[conformance][regression]") {
	// Leading zero on negative integer
	check_invalid("[-01]");

	// Leading zero on positive integer
	check_invalid("[012]");

	// Trailing decimal point, negative
	check_invalid("[-2.]");

	// Trailing decimal point, positive
	check_invalid("[2.]");

	// Dot followed immediately by exponent (no fractional digits)
	check_invalid("[0.e1]");
	check_invalid("[2.e+3]");
	check_invalid("[2.e-3]");

	// Minus sign with no integer part before decimal
	check_invalid("[-.123]");
}

// ---------------------------------------------------------------------------
// Additional edge cases — whitespace, nesting, unicode beyond BMP
// ---------------------------------------------------------------------------

TEST_CASE(
	"edge cases: whitespace handling",
	"[conformance][edge]") {
	// RFC allows whitespace (space, tab, LF, CR) before/after structural chars
	check_valid("  [  1  ,  2  ]  ");
	check_valid("\t{\t\"k\"\t:\t1\t}\t");
	check_valid("\r\n[\r\n1\r\n,\r\n2\r\n]\r\n");
	check_valid("   null   ");
	check_valid("   true   ");
	check_valid("   false  ");
	check_valid("   42     ");
	check_valid("   \"hi\"  ");
}

TEST_CASE(
	"edge cases: nesting limits",
	"[conformance][edge]") {
	// Moderately deep nesting — should be valid
	check_valid("[[[[[[[[[1]]]]]]]]]");
	check_valid("{\"a\":{\"b\":{\"c\":{\"d\":{}}}}}");
}

TEST_CASE(
	"edge cases: special string content",
	"[conformance][edge][pass]") {
	// Solidus (/) may be unescaped
	check_valid("[\"http:\\/\\/example.com\"]");
	check_valid("[\"http://example.com\"]");
	// All basic escape sequences
	check_valid("[\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"]");
	// Unicode null
	check_valid("[\"\\u0000\"]");
	// All-whitespace string
	check_valid("[\"   \"]");
	// Very long string
	check_valid("[\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"]");
}

TEST_CASE(
	"edge cases: top-level scalars (RFC 8259 allows)",
	"[conformance][edge]") {
	// RFC 8259 (2017) explicitly permits any value as top-level
	check_valid("null");
	check_valid("true");
	check_valid("false");
	check_valid("42");
	check_valid("-3.14");
	check_valid("\"hello world\"");
}

TEST_CASE(
	"edge cases: empty and minimal",
	"[conformance][edge]") {
	// Minimal valid documents
	check_valid("{}");
	check_valid("[]");
	check_valid("0");

	// Completely empty input is NOT valid JSON
	check_invalid("");

	// Only whitespace is NOT valid JSON
	check_invalid("   ");
	check_invalid("\t\n");
}

// ---------------------------------------------------------------------------
// UUU — Escaped-key regression tests (v15 Phase 6 mandatory, FFF/GGG coverage)
// Tests 1–4 apply before the hash index (Phase 6).
// Tests 5–6 (hash_fallback_linear_on_escaped, build_probe_cap_adversarial_hash)
// require warm_member_index and are added when Phase 6 lands.
// ---------------------------------------------------------------------------

TEST_CASE(
	"UUU: find_member with escape-decoded key (\\n in key)",
	"[conformance][escaped-key]") {
	// JSON key "a\nb" decodes to a, 0x0A, b (3 bytes).
	auto doc = json::parse(R"({"a\nb": 1})");
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	std::string const key{"a\nb"};
	auto node = o.find_member(key);
	REQUIRE(node.has_value());
	CHECK(*node->as_number()->to_i64() == 1LL);
}

TEST_CASE(
	"UUU: find_member with null byte in key (\\u0000 escape)",
	"[conformance][escaped-key]") {
	// JSON: {"a\u0000b": 42} — \u0000 decodes to 0x00; stored key is a,NUL,b (3 bytes).
	auto doc = json::parse(R"({"a\u0000b": 42})");
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	// C++ string literal with embedded null: 'a', '\0' (0x00), 'b'
	std::string_view const key{"a\0b", 3};
	auto node = o.find_member(key);
	REQUIRE(node.has_value());
	CHECK(*node->as_number()->to_i64() == 42LL);
}

TEST_CASE(
	"UUU: find_member with unicode-escaped ASCII key (\\u0061 = 'a')",
	"[conformance][escaped-key]") {
	// JSON: {"\u0061": 99} — \u0061 decodes to 'a'; stored key is a (1 byte).
	auto doc = json::parse(R"({"\u0061": 99})");
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	auto node = o.find_member("a");
	REQUIRE(node.has_value());
	CHECK(*node->as_number()->to_i64() == 99LL);
}

TEST_CASE(
	"UUU: duplicate member detected across literal and unicode-escaped form",
	"[conformance][escaped-key]") {
	// JSON: {"a": 1, "\u0061": 2} — "a" and "\u0061" both decode to "a".
	auto doc = json::parse(R"({"a": 1, "\u0061": 2})");
	CHECK_FALSE(doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::duplicate_member);
}

TEST_CASE(
	"UUU: hash_fallback_linear_on_escaped -- 9-member object with warm_member_index",
	"[conformance][escaped-key][phase6]") {
	// Object with 9 members (>= kHashThreshold=8) including plain keys.
	// warm_member_index pre-builds the hash table; find_member must still return
	// correct result for all keys.
	auto doc = json::parse(R"({
		"k0": 0, "k1": 1, "k2": 2, "k3": 3, "k4": 4,
		"k5": 5, "k6": 6, "k7": 7, "k8": 8
	})");
	REQUIRE(doc.has_value());
	auto warm = doc->warm_member_index(doc->root());
	REQUIRE(warm.has_value());
	auto o = *doc->root().as_object();
	CHECK(o.size() == 9UZ);
	auto found = o.find_member("k8");
	REQUIRE(found.has_value());
	CHECK(*found->as_number()->to_i64() == 8LL);
	CHECK(*o.find_member("k0")->as_number()->to_i64() == 0LL);
	CHECK(*o.find_member("k7")->as_number()->to_i64() == 7LL);
	CHECK_FALSE(o.find_member("k9").has_value());
}

TEST_CASE(
	"UUU: build_probe_cap_adversarial_hash -- 65 colliding-hash members",
	"[conformance][escaped-key][phase6]") {
	// Generate 65 keys whose std::hash<string_view>, truncated to 32 bits and
	// masked to the low 8 bits (mask=cap-1 with cap=256 for member_count in
	// [65,128]), all map to the same slot. build_table iterates kProbeChainMax
	// (64) probes before giving up; the 65th insertion must hit the cap and
	// abandon the table.
	constexpr std::uint32_t target_low = 0x37;
	std::vector<std::string> keys;
	keys.reserve(65);
	for (std::uint64_t i = 0; keys.size() < 65 && i < (1ULL << 24); ++i) {
		std::string candidate = "k_" + std::to_string(i);
		auto const h = static_cast<std::uint32_t>(std::hash<std::string_view>{}(std::string_view{candidate}));
		if ((h & 0xFFu) == target_low) {
			keys.push_back(std::move(candidate));
		}
	}
	REQUIRE(keys.size() == 65);

	std::string body;
	body.reserve(64 * keys.size());
	body += '{';
	for (std::size_t i = 0; i < keys.size(); ++i) {
		if (i != 0) {
			body += ',';
		}
		body += '"';
		body += keys[i];
		body += "\":";
		body += std::to_string(i);
	}
	body += '}';

	auto doc = json::parse(body);
	REQUIRE(doc.has_value());

	// warm hits build_table probe-cap (RRR).
	auto warm = doc->warm_member_index(doc->root());
	REQUIRE_FALSE(warm.has_value());
	CHECK(warm.error().code == JsonIssueCode::resource_exhausted);

	// find_member must still return correct value via linear-scan fallback.
	auto o = *doc->root().as_object();
	CHECK(o.size() == keys.size());
	auto first = o.find_member(keys.front());
	REQUIRE(first.has_value());
	CHECK(*first->as_number()->to_i64() == 0LL);
	auto mid = o.find_member(keys[32]);
	REQUIRE(mid.has_value());
	CHECK(*mid->as_number()->to_i64() == 32LL);
	auto last = o.find_member(keys.back());
	REQUIRE(last.has_value());
	CHECK(*last->as_number()->to_i64() == 64LL);
	CHECK_FALSE(o.find_member("definitely_not_present").has_value());
}

TEST_CASE(
	"UUU: warm_member_indices -- all objects in document get hash index",
	"[conformance][escaped-key][phase6]") {
	// Document with nested objects each exceeding the hash threshold.
	auto doc = json::parse(R"({
		"outer0":0,"outer1":1,"outer2":2,"outer3":3,"outer4":4,
		"outer5":5,"outer6":6,"outer7":7,"outer8":8,
		"inner": {
			"i0":0,"i1":1,"i2":2,"i3":3,"i4":4,
			"i5":5,"i6":6,"i7":7,"i8":8
		}
	})");
	REQUIRE(doc.has_value());
	auto warm = doc->warm_member_indices();
	REQUIRE(warm.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.find_member("outer8")->as_number()->to_i64() == 8LL);
	auto inner = *o.find_member("inner")->as_object();
	CHECK(*inner.find_member("i8")->as_number()->to_i64() == 8LL);
	CHECK_FALSE(inner.find_member("missing").has_value());
	// warm_member_indices is idempotent.
	REQUIRE(doc->warm_member_indices().has_value());
}

// ---------------------------------------------------------------------------
// NNN — Move-stability matrix (v15): every borrowed-handle type must remain
// valid across move-construct and move-assign of the source Document.
// ---------------------------------------------------------------------------

TEST_CASE(
	"NNN: NodeRef survives Document move-construct",
	"[conformance][move-stability]") {
	auto doc1 = json::parse(R"({"k": 42})");
	REQUIRE(doc1.has_value());
	auto root_before = doc1->root();
	Document doc2 = std::move(*doc1);
	auto obj = root_before.as_object();
	REQUIRE(obj.has_value());
	auto k = obj->find_member("k");
	REQUIRE(k.has_value());
	CHECK(*k->as_number()->to_i64() == 42LL);
}

TEST_CASE(
	"NNN: NodeRef survives Document move-assign",
	"[conformance][move-stability]") {
	auto doc1 = json::parse(R"({"k": 42})");
	REQUIRE(doc1.has_value());
	auto root_before = doc1->root();
	Document doc2;
	doc2 = std::move(*doc1);
	auto obj = root_before.as_object();
	REQUIRE(obj.has_value());
	CHECK(*obj->find_member("k")->as_number()->to_i64() == 42LL);
}

TEST_CASE(
	"NNN: ObjectView survives Document move",
	"[conformance][move-stability]") {
	auto doc1 = json::parse(R"({"a": 1, "b": 2, "c": 3})");
	REQUIRE(doc1.has_value());
	auto obj_before = *doc1->root().as_object();
	CHECK(obj_before.size() == 3UZ);
	Document doc2 = std::move(*doc1);
	CHECK(obj_before.size() == 3UZ);
	CHECK(*obj_before.find_member("b")->as_number()->to_i64() == 2LL);
}

TEST_CASE(
	"NNN: ArrayView survives Document move",
	"[conformance][move-stability]") {
	auto doc1 = json::parse(R"([10, 20, 30])");
	REQUIRE(doc1.has_value());
	auto arr_before = *doc1->root().as_array();
	Document doc2 = std::move(*doc1);
	CHECK(arr_before.size() == 3UZ);
	CHECK(*arr_before.element(1)->as_number()->to_i64() == 20LL);
}

TEST_CASE(
	"NNN: JsonNumberView survives Document move",
	"[conformance][move-stability]") {
	auto doc1 = json::parse(R"(3.14)");
	REQUIRE(doc1.has_value());
	auto num_before = *doc1->root().as_number();
	Document doc2 = std::move(*doc1);
	auto v = num_before.to_f64();
	REQUIRE(v.has_value());
	CHECK(std::abs(*v - 3.14) < 1e-12);
	CHECK(num_before.lexeme() == "3.14");
}

TEST_CASE(
	"NNN: warm_member_index pointer stable across Document move",
	"[conformance][move-stability]") {
	// Build object large enough to use hash index (size >= kHashThreshold=8).
	auto doc1 = json::parse(R"({
		"k0":0,"k1":1,"k2":2,"k3":3,"k4":4,
		"k5":5,"k6":6,"k7":7,"k8":8,"k9":9
	})");
	REQUIRE(doc1.has_value());
	REQUIRE(doc1->warm_member_index(doc1->root()).has_value());
	auto obj_before = *doc1->root().as_object();
	CHECK(*obj_before.find_member("k5")->as_number()->to_i64() == 5LL);
	Document doc2 = std::move(*doc1);
	// After move, the hash index pointer in the moved-from storage is now in
	// doc2's storage; obj_before still holds the same DocumentStorage*, so
	// hash lookups must continue to work.
	CHECK(*obj_before.find_member("k5")->as_number()->to_i64() == 5LL);
	CHECK(*obj_before.find_member("k9")->as_number()->to_i64() == 9LL);
}

// -1e-400 sign-bit regression gate (CC). Strtod_l fallback path must preserve
// the negative sign on subnormal/underflow values.
TEST_CASE(
	"NNN: -1e-400 underflow preserves sign bit via strtod_l",
	"[conformance][numbers][regression]") {
	auto doc = json::parse("-1e-400");
	REQUIRE(doc.has_value());
	auto v = doc->root().as_number()->to_f64();
	REQUIRE(v.has_value());
	CHECK(*v == 0.0);
	CHECK(std::signbit(*v) == true);

	auto doc_pos = json::parse("1e-400");
	REQUIRE(doc_pos.has_value());
	auto vp = doc_pos->root().as_number()->to_f64();
	REQUIRE(vp.has_value());
	CHECK(*vp == 0.0);
	CHECK(std::signbit(*vp) == false);
}

// ---------------------------------------------------------------------------
// v11 Phase 0 — layout invariants exercised end-to-end.
// (Correction K: pre-parsed numbers, deferred f64-overflow.
//  Correction Q: BOM offset reporting.)
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase0: huge integer lexeme parses, to_i64/u64/f64 follow Correction K",
	"[conformance][phase0][numbers]") {
	// 30-digit integer — outside i64 and u64; f64 conversion produces a
	// finite (rounded) value.
	auto doc = json::parse("123456789012345678901234567890");
	REQUIRE(doc.has_value());
	auto num = *doc->root().as_number();
	CHECK(num.form() == JsonNumberForm::integer);
	CHECK_FALSE(num.to_i64().has_value());
	CHECK(num.to_i64().error().code == JsonIssueCode::number_out_of_range);
	CHECK_FALSE(num.to_u64().has_value());
	CHECK(num.to_u64().error().code == JsonIssueCode::number_out_of_range);
	auto f = num.to_f64();
	REQUIRE(f.has_value());
	CHECK(*f > 1e29);
	// dump preserves the lexeme bytes.
	auto out = doc->dump();
	REQUIRE(out.has_value());
	CHECK(*out == "123456789012345678901234567890");
}

TEST_CASE(
	"phase0: f64-overflow node — parse succeeds, to_f64 fails, dump preserves",
	"[conformance][phase0][numbers]") {
	auto doc = json::parse("1e10000");
	REQUIRE(doc.has_value());
	auto num = *doc->root().as_number();
	CHECK(num.form() == JsonNumberForm::non_integer);
	CHECK_FALSE(num.to_f64().has_value());
	CHECK(num.to_f64().error().code == JsonIssueCode::number_out_of_range);
	auto out = doc->dump();
	REQUIRE(out.has_value());
	CHECK(*out == "1e10000");
}

TEST_CASE(
	"phase0: BOM offset reporting — error offset includes 3 BOM bytes",
	"[conformance][phase0][bom]") {
	// UTF-8 BOM (EF BB BF) followed by an ill-formed token at byte index 4
	// (relative to raw input including BOM): the error source.offset must
	// be reported in raw input coordinates.
	std::string input;
	input += '\xEF';
	input += '\xBB';
	input += '\xBF';
	input += "[1,]"; // trailing comma at offset (3 + 3) = 6 raw bytes in
	auto doc = json::parse(input);
	REQUIRE_FALSE(doc.has_value());
	// The offset reported should be raw-byte offset including BOM.
	REQUIRE(doc.error().source.has_value());
	CHECK(doc.error().source->offset >= 3);
}

// ---------------------------------------------------------------------------
// v11 Phase 1 — owned input buffer + zero-copy number lexemes.
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase1: parse(string_view) copies input — original buffer can be freed",
	"[conformance][phase1][input]") {
	std::string transient = "[1, 2.5, 3]";
	auto doc = json::parse(transient);
	REQUIRE(doc.has_value());
	transient.clear();
	transient.shrink_to_fit();
	auto arr = *doc->root().as_array();
	CHECK(arr.size() == 3UZ);
	CHECK(*arr.element(0)->as_number()->to_i64() == 1LL);
	auto v1 = arr.element(1)->as_number()->to_f64();
	REQUIRE(v1.has_value());
	CHECK(std::abs(*v1 - 2.5) < 1e-12);
	CHECK(*arr.element(2)->as_number()->to_i64() == 3LL);
}

TEST_CASE(
	"phase1: parse(string&&) moves input",
	"[conformance][phase1][input]") {
	// Long string forces heap allocation, ensuring move actually transfers
	// rather than relying on SSO.
	std::string s(256, 'x');
	for (auto &c: s) {
		c = '1';
	}
	s = "[" + s + "]";
	auto doc = json::parse(std::move(s));
	REQUIRE(doc.has_value());
	auto arr = *doc->root().as_array();
	CHECK(arr.size() == 1UZ);
}

TEST_CASE(
	"phase1: parse_borrowed references caller bytes — number lexeme is zero-copy",
	"[conformance][phase1][input]") {
	// Verify number lexeme bytes point into the caller's buffer (no copy
	// into string_arena for numbers).
	std::string buf = "12345";
	auto doc = json::parse_borrowed(buf);
	REQUIRE(doc.has_value());
	auto num = *doc->root().as_number();
	CHECK(num.lexeme().data() == buf.data());
	CHECK(num.lexeme() == "12345");
}

TEST_CASE(
	"phase1: parse_borrowed BOM is stripped from input_view but not from caller buffer",
	"[conformance][phase1][input]") {
	std::string buf =
		"\xEF\xBB\xBF"
		"42";
	auto doc = json::parse_borrowed(buf);
	REQUIRE(doc.has_value());
	auto num = *doc->root().as_number();
	// Lexeme starts after the BOM (3 bytes in).
	CHECK(num.lexeme().data() == buf.data() + 3);
	CHECK(num.lexeme() == "42");
}

// parse_borrowed must reject an std::string rvalue at compile time.
template<class T, class = void>
constexpr bool kCanCallParseBorrowedRvalue = false;
template<class T>
constexpr bool kCanCallParseBorrowedRvalue<T, std::void_t<decltype(json::parse_borrowed(std::declval<T>()))>> = true;
static_assert(kCanCallParseBorrowedRvalue<std::string_view>);
static_assert(kCanCallParseBorrowedRvalue<std::string &>);
static_assert(!kCanCallParseBorrowedRvalue<std::string>);

// ---------------------------------------------------------------------------
// Builder-API conformance — exercise the ValueBuilder/ObjectBuilder/ArrayBuilder
// API against real-world documents, verifying build/parse → dump round-trips.
// ---------------------------------------------------------------------------

TEST_CASE(
	"builder: parsed document dump/reparse preserves structure",
	"[conformance][builder]") {
	constexpr std::string_view src = R"({
        "name": "conformance",
        "count": 42,
        "ratio": -3.14,
        "flag": true,
        "empty": null,
        "nested": {"arr": [1, 2, 3], "inner": {"k": "v"}},
        "mixed": [null, true, "s", 1, 2.5]
    })";
	auto doc = json::parse(src);
	REQUIRE(doc.has_value());

	auto dumped = doc->dump();
	REQUIRE(dumped.has_value());
	auto doc2 = json::parse(*dumped);
	REQUIRE(doc2.has_value());

	auto o = *doc2->root().as_object();
	CHECK(*o.member("name")->as_string() == "conformance");
	CHECK(*o.member("count")->as_number()->to_i64() == 42LL);
	CHECK(*o.member("flag")->as_bool() == true);
	CHECK(o.member("empty")->is_null());
	auto nested = *o.member("nested")->as_object();
	auto arr = *nested.member("arr")->as_array();
	CHECK(arr.size() == 3UZ);
	auto mixed = *o.member("mixed")->as_array();
	CHECK(mixed.size() == 5UZ);
}

TEST_CASE(
	"builder: reconstruct object via builder API",
	"[conformance][builder]") {
	auto b = value_builder();
	auto ob = b.begin_object();
	REQUIRE(ob.has_value());
	REQUIRE(ob->insert_string("name", "alice").has_value());
	REQUIRE(ob->insert_i64("age", 30LL).has_value());
	auto tags = ob->insert_array("tags");
	REQUIRE(tags.has_value());
	REQUIRE(tags->append_string("admin").has_value());
	REQUIRE(tags->append_string("user").has_value());
	std::move(*tags).commit();
	auto meta = ob->insert_object("meta");
	REQUIRE(meta.has_value());
	REQUIRE(meta->insert_bool("active", true).has_value());
	REQUIRE(meta->insert_f64("score", 1.5).has_value());
	std::move(*meta).commit();
	std::move(*ob).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	auto dumped = doc->dump();
	REQUIRE(dumped.has_value());
	auto doc2 = json::parse(*dumped);
	REQUIRE(doc2.has_value());
	auto o = *doc2->root().as_object();
	CHECK(*o.member("name")->as_string() == "alice");
	CHECK(*o.member("age")->as_number()->to_i64() == 30LL);
	auto tags_v = *o.member("tags")->as_array();
	CHECK(tags_v.size() == 2UZ);
	CHECK(*tags_v.element(0)->as_string() == "admin");
	CHECK(*tags_v.element(1)->as_string() == "user");
	auto meta_v = *o.member("meta")->as_object();
	CHECK(*meta_v.member("active")->as_bool() == true);
}

TEST_CASE(
	"builder: build document then dump and reparse",
	"[conformance][builder]") {
	auto b = value_builder();
	auto ob = b.begin_object();
	REQUIRE(ob.has_value());
	auto items = ob->insert_array("items");
	REQUIRE(items.has_value());
	REQUIRE(items->append_i64(1LL).has_value());
	REQUIRE(items->append_i64(2LL).has_value());
	REQUIRE(items->append_i64(3LL).has_value());
	REQUIRE(items->append_i64(4LL).has_value());
	std::move(*items).commit();
	auto mb = ob->insert_object("meta");
	REQUIRE(mb.has_value());
	REQUIRE(mb->insert_string("created", "2026-04-17").has_value());
	std::move(*mb).commit();
	REQUIRE(ob->insert_bool("flag", true).has_value());
	std::move(*ob).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	auto dumped = doc->dump();
	REQUIRE(dumped.has_value());
	auto doc2 = json::parse(*dumped);
	REQUIRE(doc2.has_value());
	auto o = *doc2->root().as_object();
	auto items_v = *o.member("items")->as_array();
	CHECK(items_v.size() == 4UZ);
	CHECK(*items_v.element(3)->as_number()->to_i64() == 4LL);
	CHECK(*o.member("meta")->as_object()->member("created")->as_string() == "2026-04-17");
	CHECK(*o.member("flag")->as_bool() == true);
}

TEST_CASE(
	"builder: parsed document is self-contained after input corruption",
	"[conformance][builder]") {
	std::string src{R"(["alpha","beta","gamma"])"};
	auto doc = json::parse(src);
	REQUIRE(doc.has_value());
	src.assign(src.size(), 'X');

	auto a = *doc->root().as_array();
	CHECK(*a.element(0)->as_string() == "alpha");
	CHECK(*a.element(1)->as_string() == "beta");
	CHECK(*a.element(2)->as_string() == "gamma");
}

TEST_CASE(
	"builder: deep nesting round-trip via nested builders",
	"[conformance][builder]") {
	auto b = value_builder();
	auto ob_a = b.begin_object();
	REQUIRE(ob_a.has_value());
	auto ob_b = ob_a->insert_object("a");
	REQUIRE(ob_b.has_value());
	auto ob_c = ob_b->insert_object("b");
	REQUIRE(ob_c.has_value());
	auto ob_d = ob_c->insert_object("c");
	REQUIRE(ob_d.has_value());
	REQUIRE(ob_d->insert_i64("d", 42LL).has_value());
	auto list = ob_d->insert_array("list");
	REQUIRE(list.has_value());
	REQUIRE(list->append_i64(1LL).has_value());
	REQUIRE(list->append_i64(2LL).has_value());
	REQUIRE(list->append_i64(3LL).has_value());
	std::move(*list).commit();
	std::move(*ob_d).commit();
	std::move(*ob_c).commit();
	std::move(*ob_b).commit();
	std::move(*ob_a).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	auto dumped = doc->dump();
	REQUIRE(dumped.has_value());
	auto doc2 = json::parse(*dumped);
	REQUIRE(doc2.has_value());

	JsonPath p_d;
	p_d.push_member("a");
	p_d.push_member("b");
	p_d.push_member("c");
	p_d.push_member("d");
	auto d_node = doc2->root().at(p_d);
	REQUIRE(d_node.has_value());
	CHECK(*d_node->as_number()->to_i64() == 42LL);

	JsonPath p_list2;
	p_list2.push_member("a");
	p_list2.push_member("b");
	p_list2.push_member("c");
	p_list2.push_member("list");
	p_list2.push_index(2);
	auto list2_node = doc2->root().at(p_list2);
	REQUIRE(list2_node.has_value());
	CHECK(*list2_node->as_number()->to_i64() == 3LL);
}

TEST_CASE(
	"builder: find_member and missing member lookup",
	"[conformance][builder]") {
	auto doc = json::parse(R"({"k":1,"other":"x"})");
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();

	CHECK(o.find_member("k").has_value());
	CHECK(o.find_member("other").has_value());
	CHECK_FALSE(o.find_member("missing").has_value());
	CHECK(o.size() == 2UZ);
	CHECK(*o.member("k")->as_number()->to_i64() == 1LL);
	CHECK(*o.member("other")->as_string() == "x");
}

TEST_CASE(
	"builder: large mixed document round-trip via builder",
	"[conformance][builder]") {
	auto b = value_builder();
	auto ab = b.begin_array();
	REQUIRE(ab.has_value());
	REQUIRE(ab->append_string("builder test pass").has_value());
	auto inner_ob = ab->append_object();
	REQUIRE(inner_ob.has_value());
	auto inner_arr = inner_ob->insert_array("object with 1 member");
	REQUIRE(inner_arr.has_value());
	REQUIRE(inner_arr->append_string("array with 1 element").has_value());
	std::move(*inner_arr).commit();
	std::move(*inner_ob).commit();
	auto empty_ob = ab->append_object();
	REQUIRE(empty_ob.has_value());
	std::move(*empty_ob).commit();
	auto empty_arr = ab->append_array();
	REQUIRE(empty_arr.has_value());
	std::move(*empty_arr).commit();
	REQUIRE(ab->append_i64(-42LL).has_value());
	REQUIRE(ab->append_bool(true).has_value());
	REQUIRE(ab->append_bool(false).has_value());
	REQUIRE(ab->append_null().has_value());
	auto stats_ob = ab->append_object();
	REQUIRE(stats_ob.has_value());
	REQUIRE(stats_ob->insert_i64("integer", 1234567890LL).has_value());
	REQUIRE(stats_ob->insert_f64("real", -9876.543210).has_value());
	REQUIRE(stats_ob->insert_i64("zero", 0LL).has_value());
	REQUIRE(stats_ob->insert_i64("one", 1LL).has_value());
	REQUIRE(stats_ob->insert_string("empty_string", "").has_value());
	REQUIRE(stats_ob->insert_string("space", " ").has_value());
	auto arr2 = stats_ob->insert_array("array");
	REQUIRE(arr2.has_value());
	std::move(*arr2).commit();
	auto ob2 = stats_ob->insert_object("object");
	REQUIRE(ob2.has_value());
	std::move(*ob2).commit();
	std::move(*stats_ob).commit();
	std::move(*ab).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());

	auto dumped = doc->dump();
	REQUIRE(dumped.has_value());
	auto doc2 = json::parse(*dumped);
	REQUIRE(doc2.has_value());
	auto a = *doc2->root().as_array();
	CHECK(*a.element(0)->as_string() == "builder test pass");
	CHECK(*a.element(4)->as_number()->to_i64() == -42LL);
	CHECK(*a.element(5)->as_bool() == true);
	CHECK(a.element(7)->is_null());
	auto stats = *a.element(8)->as_object();
	CHECK(*stats.member("integer")->as_number()->to_i64() == 1234567890LL);
}
