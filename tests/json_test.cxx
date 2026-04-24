// Plain TU — not a module unit.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

using namespace std;
using namespace conflux::json;

// ---------------------------------------------------------------------------
// Parse — scalars
// ---------------------------------------------------------------------------

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
	CHECK(*u == numeric_limits<uint64_t>::max());
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
	"json: parse string",
	"[json]") {
	auto doc = parse(R"("hello world")");
	REQUIRE(doc.has_value());
	auto s = doc->root().as_string();
	REQUIRE(s.has_value());
	CHECK(*s == "hello world");
}

TEST_CASE(
	"json: parse string with escape sequences",
	"[json]") {
	auto doc = parse(R"("a\tb\nc")");
	REQUIRE(doc.has_value());
	auto s = doc->root().as_string();
	REQUIRE(s.has_value());
	CHECK(*s == "a\tb\nc");
}

TEST_CASE(
	"json: parse string with unicode escape",
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

// ---------------------------------------------------------------------------
// Parse — containers
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: parse empty array",
	"[json]") {
	auto doc = parse("[]");
	REQUIRE(doc.has_value());
	auto a = doc->root().as_array();
	REQUIRE(a.has_value());
	CHECK(a->size() == 0UZ);
}

TEST_CASE(
	"json: parse array",
	"[json]") {
	auto doc = parse("[1, 2, 3]");
	REQUIRE(doc.has_value());
	auto a = doc->root().as_array();
	REQUIRE(a.has_value());
	CHECK(a->size() == 3UZ);
	CHECK(*(*a->element(0)).as_number()->to_i64() == 1LL);
	CHECK(*(*a->element(1)).as_number()->to_i64() == 2LL);
	CHECK(*(*a->element(2)).as_number()->to_i64() == 3LL);
}

TEST_CASE(
	"json: array element range",
	"[json]") {
	auto doc = parse("[10, 20, 30]");
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	vector<int64_t> vals;
	for (NodeRef const elem: a.elements()) {
		vals.push_back(*elem.as_number()->to_i64());
	}
	REQUIRE(vals.size() == 3UZ);
	CHECK(vals[0] == 10LL);
	CHECK(vals[1] == 20LL);
	CHECK(vals[2] == 30LL);
}

TEST_CASE(
	"json: array out-of-range error",
	"[json]") {
	auto doc = parse("[1]");
	REQUIRE(doc.has_value());
	auto res = doc->root().as_array()->element(99);
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::index_out_of_range);
}

TEST_CASE(
	"json: parse empty object",
	"[json]") {
	auto doc = parse("{}");
	REQUIRE(doc.has_value());
	auto o = doc->root().as_object();
	REQUIRE(o.has_value());
	CHECK(o->size() == 0UZ);
}

TEST_CASE(
	"json: parse object",
	"[json]") {
	auto doc = parse(R"({"a": 1, "b": "two"})");
	REQUIRE(doc.has_value());
	auto o = doc->root().as_object();
	REQUIRE(o.has_value());
	auto a = o->member("a");
	REQUIRE(a.has_value());
	CHECK(*a->as_number()->to_i64() == 1LL);
	auto b = o->member("b");
	REQUIRE(b.has_value());
	CHECK(*b->as_string() == "two");
}

TEST_CASE(
	"json: object member range",
	"[json]") {
	auto doc = parse(R"({"x": 1, "y": 2})");
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	unordered_map<string, int64_t> seen;
	for (auto [name, val]: o.members()) {
		seen[string{name}] = *val.as_number()->to_i64();
	}
	CHECK(seen["x"] == 1LL);
	CHECK(seen["y"] == 2LL);
}

TEST_CASE(
	"json: object find_member",
	"[json]") {
	auto doc = parse(R"({"k": 42})");
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(o.find_member("k").has_value());
	CHECK_FALSE(o.find_member("missing").has_value());
}

TEST_CASE(
	"json: object missing member error",
	"[json]") {
	auto doc = parse(R"({"a": 1})");
	REQUIRE(doc.has_value());
	auto res = doc->root().as_object()->member("missing");
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::missing_member);
}

TEST_CASE(
	"json: reject duplicate object keys",
	"[json]") {
	auto doc = parse(R"({"k": 1, "k": 2})");
	CHECK_FALSE(doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::duplicate_member);
}

TEST_CASE(
	"json: nested object via path",
	"[json]") {
	auto doc = parse(R"({"x": {"y": 42}})");
	REQUIRE(doc.has_value());
	JsonPath p;
	p.push_member("x");
	p.push_member("y");
	auto node = doc->root().at(p);
	REQUIRE(node.has_value());
	CHECK(*node->as_number()->to_i64() == 42LL);
}

TEST_CASE(
	"json: nested array via path",
	"[json]") {
	auto doc = parse("[[1, 2], [3, 4]]");
	REQUIRE(doc.has_value());
	JsonPath p;
	p.push_index(1);
	p.push_index(0);
	auto node = doc->root().at(p);
	REQUIRE(node.has_value());
	CHECK(*node->as_number()->to_i64() == 3LL);
}

// ---------------------------------------------------------------------------
// Wrong-kind errors
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: wrong-kind errors",
	"[json]") {
	auto doc = parse("42");
	REQUIRE(doc.has_value());
	NodeRef const root = doc->root();
	CHECK_FALSE(root.as_bool().has_value());
	CHECK(root.as_bool().error().code == JsonIssueCode::wrong_kind);
	CHECK_FALSE(root.as_string().has_value());
	CHECK_FALSE(root.as_object().has_value());
	CHECK_FALSE(root.as_array().has_value());
}

// ---------------------------------------------------------------------------
// Parse errors
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: parse error — truncated",
	"[json]") {
	CHECK_FALSE(parse(R"({"a":)").has_value());
}

TEST_CASE(
	"json: parse error — invalid token",
	"[json]") {
	CHECK_FALSE(parse("xyz").has_value());
}

TEST_CASE(
	"json: parse error — trailing garbage",
	"[json]") {
	CHECK_FALSE(parse("42 garbage").has_value());
}

TEST_CASE(
	"json: whitespace is tolerated",
	"[json]") {
	auto doc = parse("  {  \"k\"  :  42  }  ");
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_object()->member("k")->as_number()->to_i64() == 42LL);
}

// ---------------------------------------------------------------------------
// Number conformance
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: reject leading zeros",
	"[json][conformance]") {
	CHECK_FALSE(parse("[012]").has_value());
	CHECK_FALSE(parse("[-01]").has_value());
	CHECK(parse("[0]").has_value());
	CHECK(parse("[-0]").has_value());
}

TEST_CASE(
	"json: reject trailing decimal point",
	"[json][conformance]") {
	CHECK_FALSE(parse("[-2.]").has_value());
	CHECK_FALSE(parse("[2.]").has_value());
	CHECK_FALSE(parse("[0.e1]").has_value());
}

TEST_CASE(
	"json: reject missing integer part",
	"[json][conformance]") {
	CHECK_FALSE(parse("[-.123]").has_value());
	CHECK_FALSE(parse("[.5]").has_value());
}

// ---------------------------------------------------------------------------
// Nesting limit
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: deeply nested — within limit",
	"[json]") {
	string nested(100, '[');
	nested += "1";
	nested.append(100, ']');
	CHECK(parse(nested).has_value());
}

// ---------------------------------------------------------------------------
// JsonPath
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: JsonPath from_pointer roundtrip",
	"[json][path]") {
	string_view ptr = "/a/b/c";
	auto path = JsonPath::from_pointer(ptr);
	REQUIRE(path.has_value());
	CHECK(path->to_pointer() == ptr);
}

TEST_CASE(
	"json: JsonPath from_pointer with escapes",
	"[json][path]") {
	auto path = JsonPath::from_pointer("/a~0b/c~1d");
	REQUIRE(path.has_value());
	CHECK(path->to_pointer() == "/a~0b/c~1d");
}

TEST_CASE(
	"json: JsonPath empty pointer is root",
	"[json][path]") {
	auto path = JsonPath::from_pointer("");
	REQUIRE(path.has_value());
	CHECK(path->empty());
	CHECK(path->to_pointer().empty());
}

TEST_CASE(
	"json: JsonPath must start with /",
	"[json][path]") {
	auto path = JsonPath::from_pointer("noslash");
	CHECK_FALSE(path.has_value());
	CHECK(path.error().code == JsonIssueCode::invalid_pointer);
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: dump roundtrip",
	"[json][dump]") {
	auto doc = parse(R"({"k":[1,2,3],"n":null,"b":true})");
	REQUIRE(doc.has_value());
	auto dumped = doc->dump();
	REQUIRE(dumped.has_value());
	auto doc2 = parse(*dumped);
	REQUIRE(doc2.has_value());
	auto o = *doc2->root().as_object();
	CHECK(o.member("n")->is_null());
	CHECK(*o.member("b")->as_bool() == true);
}

TEST_CASE(
	"json: dump string escapes control chars",
	"[json][dump]") {
	auto doc = parse(R"("a\tb\nc")");
	REQUIRE(doc.has_value());
	auto d = doc->dump();
	REQUIRE(d.has_value());
	CHECK(d->find("\\t") != string::npos);
	CHECK(d->find("\\n") != string::npos);
}

TEST_CASE(
	"json: dump pretty",
	"[json][dump]") {
	auto doc = parse(R"({"k":1})");
	REQUIRE(doc.has_value());
	JsonDumpOptions opts;
	opts.pretty = true;
	auto d = doc->dump(opts);
	REQUIRE(d.has_value());
	CHECK(d->find('\n') != string::npos);
}

// ---------------------------------------------------------------------------
// Document is self-contained (parse copies input)
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: document is self-contained",
	"[json]") {
	string src{R"({"msg": "hello"})"};
	auto doc = parse(src);
	REQUIRE(doc.has_value());
	src.clear();
	src.shrink_to_fit();
	auto val = doc->root().as_object()->member("msg")->as_string();
	REQUIRE(val.has_value());
	CHECK(*val == "hello");
}

// ---------------------------------------------------------------------------
// Builder — scalar root
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: builder set_null",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_null().has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(doc->root().is_null());
}

TEST_CASE(
	"json: builder set_bool",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_bool(true).has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_bool() == true);
}

TEST_CASE(
	"json: builder set_string",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_string("hello").has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_string() == "hello");
}

TEST_CASE(
	"json: builder set_i64",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_i64(-99LL).has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_i64() == -99LL);
}

TEST_CASE(
	"json: builder set_u64",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_u64(numeric_limits<uint64_t>::max()).has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_u64() == numeric_limits<uint64_t>::max());
}

TEST_CASE(
	"json: builder set_f64",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_f64(1.5).has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_f64() == Catch::Approx(1.5));
}

TEST_CASE(
	"json: builder set_f64 rejects NaN",
	"[json][builder]") {
	auto b = value_builder();
	auto res = b.set_f64(numeric_limits<double>::quiet_NaN());
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::number_out_of_range);
}

TEST_CASE(
	"json: builder finish without set returns error",
	"[json][builder]") {
	auto b = value_builder();
	auto doc = move(b).finish();
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
	auto doc = move(b).finish();
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

// ---------------------------------------------------------------------------
// Builder — object root
// ---------------------------------------------------------------------------

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
	move(*ob).commit();
	auto doc = move(b).finish();
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

// ---------------------------------------------------------------------------
// Builder — array root
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: builder begin_array — flat array",
	"[json][builder]") {
	auto b = value_builder();
	auto ab = b.begin_array();
	REQUIRE(ab.has_value());
	REQUIRE(ab->append_i64(1LL).has_value());
	REQUIRE(ab->append_i64(2LL).has_value());
	REQUIRE(ab->append_i64(3LL).has_value());
	move(*ab).commit();
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	CHECK(a.size() == 3UZ);
	CHECK(*a.element(0)->as_number()->to_i64() == 1LL);
	CHECK(*a.element(2)->as_number()->to_i64() == 3LL);
}

TEST_CASE(
	"json: builder array with mixed types",
	"[json][builder]") {
	auto b = value_builder();
	auto ab = b.begin_array();
	REQUIRE(ab.has_value());
	REQUIRE(ab->append_null().has_value());
	REQUIRE(ab->append_bool(false).has_value());
	REQUIRE(ab->append_string("hi").has_value());
	REQUIRE(ab->append_f64(2.5).has_value());
	move(*ab).commit();
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	CHECK(a.size() == 4UZ);
	CHECK(a.element(0)->is_null());
	CHECK(*a.element(1)->as_bool() == false);
	CHECK(*a.element(2)->as_string() == "hi");
	CHECK(*a.element(3)->as_number()->to_f64() == Catch::Approx(2.5));
}

// ---------------------------------------------------------------------------
// Builder — number from lexeme
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: builder set_number valid lexeme",
	"[json][builder]") {
	auto b = value_builder();
	REQUIRE(b.set_number("1.5e2").has_value());
	auto doc = move(b).finish();
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

// ---------------------------------------------------------------------------
// Nullable<T>
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: Nullable<int64_t> — present value",
	"[json][nullable]") {
	auto doc = parse("42");
	REQUIRE(doc.has_value());
	auto res = decode<Nullable<int64_t>>(doc->root());
	REQUIRE(res.has_value());
	CHECK(res->has_value());
	CHECK(**res == 42LL);
}

TEST_CASE(
	"json: Nullable<int64_t> — null input",
	"[json][nullable]") {
	auto doc = parse("null");
	REQUIRE(doc.has_value());
	auto res = decode<Nullable<int64_t>>(doc->root());
	REQUIRE(res.has_value());
	CHECK(res->is_null());
}

// ---------------------------------------------------------------------------
// decode<T> — built-in codecs
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: decode<bool>",
	"[json][codec]") {
	auto doc = parse("true");
	REQUIRE(doc.has_value());
	auto r = decode<bool>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == true);
}

TEST_CASE(
	"json: decode<int64_t>",
	"[json][codec]") {
	auto doc = parse("-7");
	REQUIRE(doc.has_value());
	auto r = decode<int64_t>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == -7LL);
}

TEST_CASE(
	"json: decode<uint64_t>",
	"[json][codec]") {
	auto doc = parse("42");
	REQUIRE(doc.has_value());
	auto r = decode<uint64_t>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == 42ULL);
}

TEST_CASE(
	"json: decode<double>",
	"[json][codec]") {
	auto doc = parse("3.14");
	REQUIRE(doc.has_value());
	auto r = decode<double>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == Catch::Approx(3.14));
}

TEST_CASE(
	"json: decode<string>",
	"[json][codec]") {
	auto doc = parse(R"("hello")");
	REQUIRE(doc.has_value());
	auto r = decode<string>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == "hello");
}

TEST_CASE(
	"json: decode<string_view>",
	"[json][codec]") {
	auto doc = parse(R"("world")");
	REQUIRE(doc.has_value());
	auto r = decode<string_view>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == "world");
}

TEST_CASE(
	"json: decode wrong kind returns error",
	"[json][codec]") {
	auto doc = parse("42");
	REQUIRE(doc.has_value());
	auto r = decode<bool>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::wrong_kind);
}

TEST_CASE(
	"json: decode<vector<int64_t>>",
	"[json][codec]") {
	auto doc = parse("[1,2,3]");
	REQUIRE(doc.has_value());
	auto r = decode<vector<int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 3UZ);
	CHECK((*r)[0] == 1LL);
	CHECK((*r)[1] == 2LL);
	CHECK((*r)[2] == 3LL);
}

TEST_CASE(
	"json: decode<optional<int64_t>> — present",
	"[json][codec]") {
	auto doc = parse("99");
	REQUIRE(doc.has_value());
	auto r = decode<optional<int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->has_value());
	CHECK(**r == 99LL);
}

TEST_CASE(
	"json: decode<optional<int64_t>> — null yields error",
	"[json][codec]") {
	auto doc = parse("null");
	REQUIRE(doc.has_value());
	auto r = decode<optional<int64_t>>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::wrong_kind);
}

// ---------------------------------------------------------------------------
// has_json_codec concept
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: has_json_codec detects built-in types",
	"[json][codec]") {
	CHECK(has_json_codec<bool>);
	CHECK(has_json_codec<int64_t>);
	CHECK(has_json_codec<uint64_t>);
	CHECK(has_json_codec<double>);
	CHECK(has_json_codec<string>);
	CHECK(has_json_codec<string_view>);
}

TEST_CASE(
	"json: has_json_codec false for non-codec types",
	"[json][codec]") {
	struct NoCodec {};
	CHECK_FALSE(has_json_codec<NoCodec>);
}

// ---------------------------------------------------------------------------
// Number model
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: number form — integer vs non_integer",
	"[json][number]") {
	auto check = [](string_view input, JsonNumberForm expected_form) {
		auto doc = parse(input);
		REQUIRE(doc.has_value());
		auto n = doc->root().as_number();
		REQUIRE(n.has_value());
		CHECK(n->form() == expected_form);
	};
	check("42", JsonNumberForm::integer);
	check("-1", JsonNumberForm::integer);
	check("0", JsonNumberForm::integer);
	check("1.0", JsonNumberForm::non_integer);
	check("1e2", JsonNumberForm::non_integer);
	check("1E0", JsonNumberForm::non_integer);
	check("0.5", JsonNumberForm::non_integer);
}

TEST_CASE(
	"json: to_i64 / to_u64 reject non_integer form",
	"[json][number]") {
	auto doc = parse("1.0");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	CHECK_FALSE(n->to_i64().has_value());
	CHECK_FALSE(n->to_u64().has_value());
	CHECK(n->to_i64().error().code == JsonIssueCode::invalid_number);
}

TEST_CASE(
	"json: to_u64 sign_mismatch on negative integer",
	"[json][number]") {
	auto doc = parse("-42");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto res = n->to_u64();
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::sign_mismatch);
}

TEST_CASE(
	"json: to_u64 sign_mismatch on negative zero",
	"[json][number]") {
	auto doc = parse("-0");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto res = n->to_u64();
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::sign_mismatch);
}

TEST_CASE(
	"json: to_u64 number_out_of_range on overflow",
	"[json][number]") {
	auto doc = parse("18446744073709551616");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto res = n->to_u64();
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::number_out_of_range);
}

TEST_CASE(
	"json: to_f64 number_out_of_range on overflow to infinity",
	"[json][number]") {
	auto doc = parse("1e999");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto res = n->to_f64();
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::number_out_of_range);
}

// ---------------------------------------------------------------------------
// Parse limits
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: max_depth exceeded",
	"[json][limits]") {
	JsonParseOptions opts;
	opts.max_depth = LimitOption::bound(3);
	string nested(4, '[');
	nested += "1";
	nested.append(4, ']');
	auto res = parse(nested, opts);
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::nesting_too_deep);
}

TEST_CASE(
	"json: max_depth not exceeded at boundary",
	"[json][limits]") {
	JsonParseOptions opts;
	opts.max_depth = LimitOption::bound(3);
	string nested(3, '[');
	nested += "1";
	nested.append(3, ']');
	CHECK(parse(nested, opts).has_value());
}

TEST_CASE(
	"json: max_input_size exceeded",
	"[json][limits]") {
	JsonParseOptions opts;
	opts.max_input_size = LimitOption::bound(4);
	auto res = parse("\"hello\"", opts);
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::input_too_large);
}

TEST_CASE(
	"json: max_string_size exceeded",
	"[json][limits]") {
	JsonParseOptions opts;
	opts.max_string_size = LimitOption::bound(2);
	auto res = parse("\"abc\"", opts);
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::string_too_large);
}

TEST_CASE(
	"json: max_depth zero rejects non-empty containers",
	"[json][limits]") {
	JsonParseOptions opts;
	opts.max_depth = LimitOption::bound(0);
	CHECK_FALSE(parse("[1]", opts).has_value());
	CHECK_FALSE(parse(R"({"a":1})", opts).has_value());
	CHECK(parse("[]", opts).has_value());
	CHECK(parse("{}", opts).has_value());
	CHECK(parse("null", opts).has_value());
}

// ---------------------------------------------------------------------------
// Input model — BOM and UTF-8
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: BOM at start is skipped",
	"[json][input]") {
	string_view bom_json = "\xEF\xBB\xBF\"hello\"";
	auto res = parse(bom_json);
	REQUIRE(res.has_value());
	CHECK(*res->root().as_string() == "hello");
}

TEST_CASE(
	"json: invalid UTF-8 is rejected",
	"[json][input]") {
	string_view bad = "\"\x80\"";
	auto res = parse(bad);
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::invalid_utf8);
}

// ---------------------------------------------------------------------------
// NodeRef identity and value equality
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: is_same_node — same document, same node",
	"[json][noderef]") {
	auto doc = parse("[1,2]");
	REQUIRE(doc.has_value());
	NodeRef const r = doc->root();
	CHECK(is_same_node(r, r));
}

TEST_CASE(
	"json: is_same_node — different nodes in same document",
	"[json][noderef]") {
	auto doc = parse("[1,2]");
	REQUIRE(doc.has_value());
	auto a = doc->root().as_array();
	REQUIRE(a.has_value());
	auto n0 = a->element(0);
	auto n1 = a->element(1);
	REQUIRE(n0.has_value());
	REQUIRE(n1.has_value());
	CHECK_FALSE(is_same_node(*n0, *n1));
}

TEST_CASE(
	"json: is_value_equal — identical scalars",
	"[json][noderef]") {
	auto da = parse("42");
	auto db = parse("42");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK(is_value_equal(da->root(), db->root()));
}

TEST_CASE(
	"json: is_value_equal — 1 and 1.0 are equal (math value)",
	"[json][noderef]") {
	auto da = parse("1");
	auto db = parse("1.0");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK(is_value_equal(da->root(), db->root()));
}

TEST_CASE(
	"json: is_value_equal — different kinds",
	"[json][noderef]") {
	auto da = parse("1");
	auto db = parse("\"1\"");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK_FALSE(is_value_equal(da->root(), db->root()));
}

TEST_CASE(
	"json: is_value_equal — objects are order-insensitive",
	"[json][noderef]") {
	auto da = parse(R"({"a":1,"b":2})");
	auto db = parse(R"({"b":2,"a":1})");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK(is_value_equal(da->root(), db->root()));
}

TEST_CASE(
	"json: is_value_equal_exact — 1 and 1.0 are not equal",
	"[json][noderef]") {
	auto da = parse("1");
	auto db = parse("1.0");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK_FALSE(is_value_equal_exact(da->root(), db->root()));
}

TEST_CASE(
	"json: is_value_equal_exact — identical lexemes are equal",
	"[json][noderef]") {
	auto da = parse("1");
	auto db = parse("1");
	REQUIRE(da.has_value());
	REQUIRE(db.has_value());
	CHECK(is_value_equal_exact(da->root(), db->root()));
}

// ---------------------------------------------------------------------------
// Phase 2: JsonMembers<T> — plain struct decode
// ---------------------------------------------------------------------------

struct Point {
	int64_t x{};
	int64_t y{};
};

template<>
struct JsonMembers<Point> {
	static constexpr auto members() {
		return std::tuple{
			json_member("x", &Point::x),
			json_member("y", &Point::y),
		};
	}
	static constexpr string_view type_name() { return "Point"; }
};

TEST_CASE(
	"json: JsonMembers decode plain struct",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":3,"y":7})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->x == 3LL);
	CHECK(r->y == 7LL);
}

TEST_CASE(
	"json: JsonMembers decode missing required member yields missing_member",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":3})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::missing_member);
	CHECK(r.error().member_name == "y");
}

TEST_CASE(
	"json: JsonMembers decode unknown member yields invalid_value",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":1,"y":2,"z":3})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
	CHECK(r.error().member_name == "z");
}

TEST_CASE(
	"json: JsonMembers decode wrong kind for field propagates path",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":"bad","y":2})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	CHECK_FALSE(r.has_value());
	auto &err = r.error();
	CHECK(err.code == JsonIssueCode::wrong_kind);
	REQUIRE(err.path.size() == 1UZ);
	CHECK(get<JsonPathMember>(err.path.segment(0)).name == "x");
}

TEST_CASE(
	"json: has_json_codec true for JsonMembers type",
	"[json][codec][members]") {
	CHECK(has_json_codec<Point>);
}

// ---------------------------------------------------------------------------
// Phase 2: JsonCodec<T> — custom enum codec
// ---------------------------------------------------------------------------

enum class Color {
	red,
	green,
	blue,
};

template<>
struct JsonCodec<Color> {
	static expected<Color, JsonError> decode(
		NodeRef n) {
		auto s = n.as_string();
		if (!s) {
			return unexpected(move(s).error());
		}
		if (*s == "red") {
			return Color::red;
		}
		if (*s == "green") {
			return Color::green;
		}
		if (*s == "blue") {
			return Color::blue;
		}
		return unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::invalid_value,
				.target_type = string{type_name()},
				.message = format("unknown Color spelling: {}", *s)});
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		Color c) {
		switch (c) {
		case Color::red  : return b.set_string("red");
		case Color::green: return b.set_string("green");
		case Color::blue : return b.set_string("blue");
		}
		return unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::invalid_value,
				.target_type = string{type_name()},
				.message = "Color enum value outside declared range"});
	}
	static constexpr string_view type_name() { return "Color"; }
};

TEST_CASE(
	"json: JsonCodec custom enum decode",
	"[json][codec]") {
	auto doc = parse(R"("green")");
	REQUIRE(doc.has_value());
	auto r = decode<Color>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == Color::green);
}

TEST_CASE(
	"json: JsonCodec custom enum decode invalid spelling",
	"[json][codec]") {
	auto doc = parse(R"("purple")");
	REQUIRE(doc.has_value());
	auto r = decode<Color>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
}

TEST_CASE(
	"json: has_json_codec true for JsonCodec type",
	"[json][codec]") {
	CHECK(has_json_codec<Color>);
}

// ---------------------------------------------------------------------------
// Phase 2: optional<T> member in JsonMembers struct
// ---------------------------------------------------------------------------

struct Config {
	int64_t required_field{};
	optional<int64_t> optional_field{};
};

template<>
struct JsonMembers<Config> {
	static constexpr auto members() {
		return std::tuple{
			json_member("required_field", &Config::required_field),
			json_member("optional_field", &Config::optional_field),
		};
	}
	static constexpr string_view type_name() { return "Config"; }
};

TEST_CASE(
	"json: optional member present decodes value",
	"[json][codec][optional]") {
	auto doc = parse(R"({"required_field":1,"optional_field":42})");
	REQUIRE(doc.has_value());
	auto r = decode<Config>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->required_field == 1LL);
	REQUIRE(r->optional_field.has_value());
	CHECK(*r->optional_field == 42LL);
}

TEST_CASE(
	"json: optional member absent yields nullopt",
	"[json][codec][optional]") {
	auto doc = parse(R"({"required_field":1})");
	REQUIRE(doc.has_value());
	auto r = decode<Config>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->required_field == 1LL);
	CHECK_FALSE(r->optional_field.has_value());
}

// ---------------------------------------------------------------------------
// Phase 2: built-in targets — std::array<T,N>
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: decode<array<int64_t,3>>",
	"[json][codec][array]") {
	auto doc = parse("[10,20,30]");
	REQUIRE(doc.has_value());
	auto r = decode<array<int64_t, 3>>(doc->root());
	REQUIRE(r.has_value());
	CHECK((*r)[0] == 10LL);
	CHECK((*r)[1] == 20LL);
	CHECK((*r)[2] == 30LL);
}

TEST_CASE(
	"json: decode<array<int64_t,3>> wrong length yields invalid_value",
	"[json][codec][array]") {
	auto doc = parse("[1,2]");
	REQUIRE(doc.has_value());
	auto r = decode<array<int64_t, 3>>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
}

// ---------------------------------------------------------------------------
// Phase 2: built-in targets — std::pair<A,B>
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: decode<pair<string,int64_t>>",
	"[json][codec][pair]") {
	auto doc = parse(R"(["hello",42])");
	REQUIRE(doc.has_value());
	auto r = decode<pair<string, int64_t>>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->first == "hello");
	CHECK(r->second == 42LL);
}

TEST_CASE(
	"json: decode<pair<string,int64_t>> wrong length yields invalid_value",
	"[json][codec][pair]") {
	auto doc = parse("[1]");
	REQUIRE(doc.has_value());
	auto r = decode<pair<string, int64_t>>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
}

// ---------------------------------------------------------------------------
// Phase 2: built-in targets — std::tuple<Ts...>
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: decode<tuple<bool,int64_t,string>>",
	"[json][codec][tuple]") {
	auto doc = parse(R"([true,99,"hi"])");
	REQUIRE(doc.has_value());
	auto r = decode<tuple<bool, int64_t, string>>(doc->root());
	REQUIRE(r.has_value());
	CHECK(get<0>(*r) == true);
	CHECK(get<1>(*r) == 99LL);
	CHECK(get<2>(*r) == "hi");
}

TEST_CASE(
	"json: decode<tuple<int64_t,int64_t>> wrong length yields invalid_value",
	"[json][codec][tuple]") {
	auto doc = parse("[1,2,3]");
	REQUIRE(doc.has_value());
	auto r = decode<tuple<int64_t, int64_t>>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
}

// ---------------------------------------------------------------------------
// Phase 2: built-in targets — std::map<string, T>
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: decode<map<string,int64_t>>",
	"[json][codec][map]") {
	auto doc = parse(R"({"a":1,"b":2,"c":3})");
	REQUIRE(doc.has_value());
	auto r = decode<map<string, int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 3UZ);
	CHECK((*r)["a"] == 1LL);
	CHECK((*r)["b"] == 2LL);
	CHECK((*r)["c"] == 3LL);
}

TEST_CASE(
	"json: decode<map<string,int64_t>> wrong kind yields wrong_kind",
	"[json][codec][map]") {
	auto doc = parse("[1,2,3]");
	REQUIRE(doc.has_value());
	auto r = decode<map<string, int64_t>>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::wrong_kind);
}

// ---------------------------------------------------------------------------
// Phase 2: built-in targets — std::unordered_map<string, T>
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: decode<unordered_map<string,int64_t>>",
	"[json][codec][map]") {
	auto doc = parse(R"({"x":10,"y":20})");
	REQUIRE(doc.has_value());
	auto r = decode<unordered_map<string, int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 2UZ);
	CHECK((*r)["x"] == 10LL);
	CHECK((*r)["y"] == 20LL);
}

// ---------------------------------------------------------------------------
// Phase 2: Nullable<T> full surface
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: Nullable<T> default-constructs to null",
	"[json][nullable]") {
	Nullable<int64_t> n;
	CHECK(n.is_null());
	CHECK_FALSE(n.has_value());
	CHECK_FALSE(static_cast<bool>(n));
}

TEST_CASE(
	"json: Nullable<T> value state",
	"[json][nullable]") {
	Nullable<int64_t> n{42LL};
	CHECK_FALSE(n.is_null());
	CHECK(n.has_value());
	CHECK(static_cast<bool>(n));
	CHECK(*n == 42LL);
	CHECK(n.value() == 42LL);
}

TEST_CASE(
	"json: Nullable<T> value_or",
	"[json][nullable]") {
	Nullable<int64_t> n_null;
	Nullable<int64_t> n_val{7LL};
	CHECK(n_null.value_or(99LL) == 99LL);
	CHECK(n_val.value_or(99LL) == 7LL);
}

TEST_CASE(
	"json: Nullable<T> equality",
	"[json][nullable]") {
	Nullable<int64_t> a{1LL};
	Nullable<int64_t> b{1LL};
	Nullable<int64_t> c{2LL};
	Nullable<int64_t> n;
	CHECK(a == b);
	CHECK_FALSE(a == c);
	CHECK_FALSE(a == n);
}

TEST_CASE(
	"json: Nullable<T> operator->",
	"[json][nullable]") {
	Nullable<string> n{"hello"};
	CHECK(n->size() == 5UZ);
}

TEST_CASE(
	"json: decode<Nullable<string>> — non-null",
	"[json][nullable][codec]") {
	auto doc = parse(R"("world")");
	REQUIRE(doc.has_value());
	auto r = decode<Nullable<string>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->has_value());
	CHECK(**r == "world");
}

// ---------------------------------------------------------------------------
// Phase 2: JsonError::with_prefix
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Phase 2: encode for new built-in targets via ValueBuilder::set<T>
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: ValueBuilder::set<T> encodes via codec",
	"[json][builder][codec]") {
	auto b = value_builder();
	auto ok = b.set<int64_t>(42LL);
	REQUIRE(ok.has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<int64_t>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == 42LL);
}

TEST_CASE(
	"json: encode vector<int64_t> via set<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	vector<int64_t> const v{1LL, 2LL, 3LL};
	auto ok = b.set<vector<int64_t>>(v);
	REQUIRE(ok.has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<vector<int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 3UZ);
	CHECK((*r)[0] == 1LL);
	CHECK((*r)[1] == 2LL);
	CHECK((*r)[2] == 3LL);
}

TEST_CASE(
	"json: encode pair<string,int64_t> via set<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	auto ok = b.set<pair<string, int64_t>>({"hello", 7LL});
	REQUIRE(ok.has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<pair<string, int64_t>>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->first == "hello");
	CHECK(r->second == 7LL);
}

TEST_CASE(
	"json: encode tuple<bool,int64_t> via set<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	auto ok = b.set<tuple<bool, int64_t>>(tuple{true, 99LL});
	REQUIRE(ok.has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<tuple<bool, int64_t>>(doc->root());
	REQUIRE(r.has_value());
	CHECK(get<0>(*r) == true);
	CHECK(get<1>(*r) == 99LL);
}

TEST_CASE(
	"json: encode map<string,int64_t> via set<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	map<string, int64_t> const m{
		{"a", 1LL},
		{"b", 2LL}
    };
	auto ok = b.set<map<string, int64_t>>(m);
	REQUIRE(ok.has_value());
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<map<string, int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 2UZ);
	CHECK((*r)["a"] == 1LL);
	CHECK((*r)["b"] == 2LL);
}

TEST_CASE(
	"json: encode Color via ObjectBuilder::insert<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	auto obj_res = b.begin_object();
	REQUIRE(obj_res.has_value());
	auto &obj = *obj_res;
	auto ok = obj.insert<Color>("color", Color::blue);
	REQUIRE(ok.has_value());
	move(obj).commit();
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto r = doc->root().as_object();
	REQUIRE(r.has_value());
	auto m = r->member("color");
	REQUIRE(m.has_value());
	auto s = m->as_string();
	REQUIRE(s.has_value());
	CHECK(*s == "blue");
}

TEST_CASE(
	"json: encode vector via ArrayBuilder::append<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	auto arr_res = b.begin_array();
	REQUIRE(arr_res.has_value());
	auto &arr = *arr_res;
	REQUIRE(arr.append<int64_t>(10LL).has_value());
	REQUIRE(arr.append<int64_t>(20LL).has_value());
	move(arr).commit();
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<vector<int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 2UZ);
	CHECK((*r)[0] == 10LL);
	CHECK((*r)[1] == 20LL);
}

// ---------------------------------------------------------------------------
// Phase 3: Nested ObjectBuilder / ArrayBuilder
// ---------------------------------------------------------------------------

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
	move(user).commit();

	move(root).commit();
	auto doc = move(b).finish();
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
	"json: ObjectBuilder::insert_array — nested array committed",
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
	move(tags).commit();

	move(root).commit();
	auto doc = move(b).finish();
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
	move(item).commit();

	move(root).commit();
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());

	auto arr = doc->root().as_array();
	REQUIRE(arr.has_value());
	REQUIRE(arr->size() == 1UZ);
	auto obj = arr->element(0)->as_object();
	REQUIRE(obj.has_value());
	CHECK(*obj->member("k")->as_string() == "v");
}

TEST_CASE(
	"json: ArrayBuilder::append_array — nested array committed",
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
	move(inner).commit();

	move(root).commit();
	auto doc = move(b).finish();
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
	move(user).commit();

	auto tags_res = root.insert_array("tags");
	REQUIRE(tags_res.has_value());
	auto &tags = *tags_res;
	REQUIRE(tags.append_string("x").has_value());
	REQUIRE(tags.append_string("y").has_value());
	move(tags).commit();

	move(root).commit();
	auto doc = move(vb).finish();
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
		// child falls off scope without commit — should abort
	}

	// After abort, root should be active again and "inner" should not be present.
	REQUIRE(root.insert_i64("kept", 1LL).has_value());
	move(root).commit();

	auto doc = move(b).finish();
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
		// child falls off scope without commit — should abort
	}

	REQUIRE(root.insert_string("kept", "yes").has_value());
	move(root).commit();

	auto doc = move(b).finish();
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
		// obj falls off scope without commit — aborts, root_set should be cleared
	}
	// After abort, we should be able to set root again.
	REQUIRE(b.set_null().has_value());
	auto doc = move(b).finish();
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
	// Insert<T> duplicate check must fire before encode dispatch.
	auto dup = root.insert<int64_t>("key", 2LL);
	REQUIRE(!dup.has_value());
	CHECK(dup.error().code == JsonIssueCode::duplicate_member);
}

TEST_CASE(
	"json: set_number stores lexeme verbatim",
	"[json][builder][phase3]") {
	auto b = value_builder();
	REQUIRE(b.set_number("1.0").has_value());
	auto doc = move(b).finish();
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
	move(arr).commit();
	auto doc = move(b).finish();
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
	move(root).commit();
	auto doc = move(b).finish();
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
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_string() == "fresh");
}

TEST_CASE(
	"json: ValueBuilder::discard() — prevents finish",
	"[json][builder][phase3]") {
	auto b = value_builder();
	REQUIRE(b.set_null().has_value());
	move(b).discard();
}

TEST_CASE(
	"json: finish() fails when root never set",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto doc = move(b).finish();
	REQUIRE(!doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::constraint_violation);
}

TEST_CASE(
	"json: finish() fails when child builder still active",
	"[json][builder][phase3]") {
	auto b = value_builder();
	auto obj_res = b.begin_object();
	REQUIRE(obj_res.has_value());
	auto doc = move(b).finish();
	REQUIRE(!doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::constraint_violation);
	// Cleanup: let obj fall off scope to abort cleanly
}

TEST_CASE(
	"json: set<T> with JsonMembers type encodes as object",
	"[json][builder][phase3][members]") {
	auto b = value_builder();
	Point const p{.x = 3LL, .y = 7LL};
	REQUIRE(b.set<Point>(p).has_value());
	auto doc = move(b).finish();
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
	move(root).commit();
	auto doc = move(b).finish();
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
	move(arr).commit();
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<vector<Point>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 2UZ);
	CHECK((*r)[0].x == 10LL);
	CHECK((*r)[0].y == 20LL);
	CHECK((*r)[1].x == 30LL);
	CHECK((*r)[1].y == 40LL);
}

TEST_CASE(
	"json: deeply nested builder — object in array in object",
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
	move(elem).commit();

	move(items).commit();
	move(root).commit();

	auto doc = move(b).finish();
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
	move(*a_res).commit();

	REQUIRE(root.insert_i64("b", 2LL).has_value());
	move(root).commit();

	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	CHECK(obj->find_member("a").has_value());
	CHECK(obj->find_member("b").has_value());
}

// ---------------------------------------------------------------------------
// Phase 4: dump options
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: dump sort_object_keys produces stable key order",
	"[json][dump][examples]") {
	auto doc = parse(R"({"z":3,"a":1,"m":2})");
	REQUIRE(doc.has_value());
	JsonDumpOptions opts;
	opts.sort_object_keys = true;
	auto d = doc->dump(opts);
	REQUIRE(d.has_value());
	CHECK(*d == R"({"a":1,"m":2,"z":3})");
}

TEST_CASE(
	"json: dump ascii_only escapes non-ASCII code points",
	"[json][dump][examples]") {
	auto doc = parse(R"("café")");
	REQUIRE(doc.has_value());
	JsonDumpOptions opts;
	opts.ascii_only = true;
	auto d = doc->dump(opts);
	REQUIRE(d.has_value());
	CHECK(d->find("\\u") != string::npos);
	auto reparsed = parse(*d);
	REQUIRE(reparsed.has_value());
	CHECK(*reparsed->root().as_string() == "café");
}

TEST_CASE(
	"json: dump ascii_only escapes surrogate-pair code point",
	"[json][dump][examples]") {
	auto doc = parse(R"("😀")");
	REQUIRE(doc.has_value());
	JsonDumpOptions opts;
	opts.ascii_only = true;
	auto d = doc->dump(opts);
	REQUIRE(d.has_value());
	CHECK(d->find("\\ud83d") != string::npos);
	auto reparsed = parse(*d);
	REQUIRE(reparsed.has_value());
	CHECK(*reparsed->root().as_string() == "\xF0\x9F\x98\x80");
}

TEST_CASE(
	"json: dump pretty with custom indent width",
	"[json][dump][examples]") {
	auto doc = parse(R"({"k":1})");
	REQUIRE(doc.has_value());
	JsonDumpOptions opts;
	opts.pretty = true;
	opts.indent = 4;
	auto d = doc->dump(opts);
	REQUIRE(d.has_value());
	CHECK(d->find("    \"k\"") != string::npos);
}

// ---------------------------------------------------------------------------
// Phase 4 examples: generic vs typed view access
// ---------------------------------------------------------------------------

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

	auto decoded = decode<map<string, int64_t>>(*score_node);
	CHECK_FALSE(decoded.has_value());
	CHECK(decoded.error().code == JsonIssueCode::wrong_kind);
}

// ---------------------------------------------------------------------------
// Phase 4 examples: strict parse failure on duplicates
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Phase 4 examples: strict number handling including scientific-notation
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Phase 4 examples: strict typed decode
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: example — strict typed decode",
	"[json][examples]") {
	{
		auto doc = parse("\"hello\"");
		REQUIRE(doc.has_value());
		auto r = decode<int64_t>(doc->root());
		CHECK_FALSE(r.has_value());
		CHECK(r.error().code == JsonIssueCode::wrong_kind);
	}
	{
		auto doc = parse("1.5");
		REQUIRE(doc.has_value());
		auto r = decode<int64_t>(doc->root());
		CHECK_FALSE(r.has_value());
		CHECK(r.error().code == JsonIssueCode::invalid_number);
	}
	{
		auto doc = parse("-1");
		REQUIRE(doc.has_value());
		auto r = decode<uint64_t>(doc->root());
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

// ---------------------------------------------------------------------------
// Phase 4 examples: missing vs null vs optional field modeling
// ---------------------------------------------------------------------------

struct ThreeFieldModel {
	int64_t required_val{};
	optional<int64_t> optional_val{};
	Nullable<int64_t> nullable_val{};
	optional<Nullable<int64_t>> opt_nullable_val{};
};

template<>
struct JsonMembers<ThreeFieldModel> {
	static constexpr auto members() {
		return std::tuple{
			json_member("required_val", &ThreeFieldModel::required_val),
			json_member("optional_val", &ThreeFieldModel::optional_val),
			json_member("nullable_val", &ThreeFieldModel::nullable_val),
			json_member("opt_nullable_val", &ThreeFieldModel::opt_nullable_val),
		};
	}
	static constexpr string_view type_name() { return "ThreeFieldModel"; }
};

TEST_CASE(
	"json: example — missing vs null vs optional field modeling",
	"[json][examples]") {
	{
		auto doc = parse(R"({
			"required_val": 1,
			"nullable_val": null,
			"opt_nullable_val": 42
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
			"required_val": 5,
			"nullable_val": 99
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
			"required_val": 7,
			"nullable_val": null,
			"opt_nullable_val": null
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
		CHECK_FALSE(r.has_value());
		CHECK(r.error().code == JsonIssueCode::wrong_kind);
	}
}

// ---------------------------------------------------------------------------
// Phase 4 examples: builder construction of arbitrary JSON number lexemes
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: example — builder construction of arbitrary JSON number lexemes",
	"[json][examples]") {
	{
		auto b = value_builder();
		REQUIRE(b.set_number("1e100").has_value());
		auto doc = move(b).finish();
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
		auto doc = move(b).finish();
		REQUIRE(doc.has_value());
		auto n = doc->root().as_number();
		REQUIRE(n.has_value());
		CHECK(n->lexeme() == "99999999999999999999999999999999");
	}
	{
		auto b = value_builder();
		REQUIRE(b.set_number("1.0").has_value());
		auto doc = move(b).finish();
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

// ---------------------------------------------------------------------------
// Phase 4 examples: commit-on-success / abort-on-error child builder patterns
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: example — commit-on-success abort-on-error child builder patterns",
	"[json][examples]") {
	auto build_doc = [](bool inject_error) -> expected<Document, JsonError> {
		auto vb = value_builder();
		auto root_res = vb.begin_object();
		if (!root_res) {
			return unexpected(move(root_res).error());
		}
		auto &root = *root_res;

		{
			auto child_res = root.insert_object("user");
			if (!child_res) {
				return unexpected(move(child_res).error());
			}
			auto &child = *child_res;
			auto id_res = child.insert_i64("id", inject_error ? -1LL : 1LL);
			if (inject_error) {
				auto dup_res = child.insert_i64("id", 99LL);
				if (!dup_res) {
					return unexpected(move(dup_res).error());
				}
			}
			if (!id_res) {
				return unexpected(move(id_res).error());
			}
			REQUIRE(child.insert_string("name", "bob").has_value());
			move(child).commit();
		}

		move(root).commit();
		return move(vb).finish();
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
	move(root).commit();
	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	CHECK_FALSE(obj->find_member("discarded").has_value());
	REQUIRE(obj->find_member("kept").has_value());
	CHECK(*obj->member("kept")->as_string() == "yes");
}

// ---------------------------------------------------------------------------
// Phase 4 examples: partial-build abandonment via reset()
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: example — partial-build abandonment via reset()",
	"[json][examples]") {
	auto b = value_builder();

	REQUIRE(b.set_i64(100LL).has_value());

	b.reset();

	REQUIRE(b.set_string("after-reset").has_value());
	auto doc = move(b).finish();
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
	move(obj).commit();

	auto doc = move(b).finish();
	REQUIRE(doc.has_value());
	auto o = doc->root().as_object();
	REQUIRE(o.has_value());
	CHECK(*o->member("v")->as_number()->to_i64() == 42LL);
}

// ---------------------------------------------------------------------------
// Phase 4 examples: RFC 6901 round-trip via to_pointer / from_pointer
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: example — RFC 6901 round-trip via to_pointer / from_pointer",
	"[json][examples]") {
	{
		JsonPath path;
		path.push_member("users");
		path.push_index(0);
		path.push_member("name");

		auto ptr = path.to_pointer();
		CHECK(ptr == "/users/0/name");

		auto reparsed = JsonPath::from_pointer(ptr);
		REQUIRE(reparsed.has_value());
		CHECK(reparsed->to_pointer() == ptr);
	}

	{
		auto path = JsonPath::from_pointer("/a~0b/c~1d");
		REQUIRE(path.has_value());
		CHECK(path->size() == 2UZ);
		CHECK(get<JsonPathMember>(path->segment(0)).name == "a~b");
		CHECK(get<JsonPathMember>(path->segment(1)).name == "c/d");
		CHECK(path->to_pointer() == "/a~0b/c~1d");
	}

	{
		auto path = JsonPath::from_pointer("");
		REQUIRE(path.has_value());
		CHECK(path->empty());
		CHECK(path->to_pointer().empty());
	}

	{
		auto path = JsonPath::from_pointer("noslash");
		CHECK_FALSE(path.has_value());
		CHECK(path.error().code == JsonIssueCode::invalid_pointer);
		CHECK(path.error().stage == JsonStage::parse);
	}

	{
		auto doc = parse(R"({"a": {"b": [1, 2, 3]}})");
		REQUIRE(doc.has_value());
		auto path = JsonPath::from_pointer("/a/b");
		REQUIRE(path.has_value());
		auto node = doc->root().at(*path);
		REQUIRE(node.has_value());
		CHECK(node->kind() == JsonKind::array);
	}

	{
		JsonPath idx_path;
		idx_path.push_index(2);
		auto ptr = idx_path.to_pointer();
		CHECK(ptr == "/2");
		auto reparsed = JsonPath::from_pointer(ptr);
		REQUIRE(reparsed.has_value());
		CHECK(reparsed->size() == 1UZ);
		CHECK(holds_alternative<JsonPathMember>(reparsed->segment(0)));
		CHECK(get<JsonPathMember>(reparsed->segment(0)).name == "2");

		auto doc = parse("[10,20,30]");
		REQUIRE(doc.has_value());
		auto by_member = doc->root().at(*reparsed);
		CHECK_FALSE(by_member.has_value());
		CHECK(by_member.error().code == JsonIssueCode::wrong_kind);

		auto by_index = doc->root().at(idx_path);
		REQUIRE(by_index.has_value());
		CHECK(*by_index->as_number()->to_i64() == 30LL);
	}
}

// ---------------------------------------------------------------------------
// Phase 4 examples: nested-codec error propagation via with_prefix
// ---------------------------------------------------------------------------

struct InnerData {
	int64_t value{};
};

template<>
struct JsonMembers<InnerData> {
	static constexpr auto members() {
		return std::tuple{
			json_member("value", &InnerData::value),
		};
	}
	static constexpr string_view type_name() { return "InnerData"; }
};

struct OuterWithPrefix {
	InnerData inner{};
};

template<>
struct JsonCodec<OuterWithPrefix> {
	static expected<OuterWithPrefix, JsonError> decode(
		NodeRef n) {
		auto obj = n.as_object();
		if (!obj) {
			return unexpected(move(obj).error());
		}

		JsonPath prefix;
		prefix.push_member("inner");

		auto inner_node = n.at(prefix);
		if (!inner_node) {
			return unexpected(move(inner_node).error().with_prefix(prefix));
		}

		auto inner = ::decode<InnerData>(*inner_node);
		if (!inner) {
			return unexpected(move(inner).error().with_prefix(prefix));
		}

		return OuterWithPrefix{.inner = move(*inner)};
	}
	static expected<void, JsonError> encode(
		ValueBuilder &b,
		OuterWithPrefix const &v) {
		auto obj_res = b.begin_object();
		if (!obj_res) {
			return unexpected(move(obj_res).error());
		}
		auto &obj = *obj_res;
		auto inner_res = obj.insert<InnerData>("inner", v.inner);
		if (!inner_res) {
			return unexpected(move(inner_res).error());
		}
		move(obj).commit();
		return {};
	}
	static constexpr string_view type_name() { return "OuterWithPrefix"; }
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
		auto doc = move(b).finish();
		REQUIRE(doc.has_value());
		auto r = decode<OuterWithPrefix>(doc->root());
		REQUIRE(r.has_value());
		CHECK(r->inner.value == 42LL);
	}
}

// ---------------------------------------------------------------------------
// Pathological limit-matrix tests
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: limits — max_depth matrix",
	"[json][limits][pathological]") {
	// depth-0: empty containers pass, non-empty fail
	{
		JsonParseOptions opts;
		opts.max_depth = LimitOption::bound(0);
		CHECK(parse("null", opts).has_value());
		CHECK(parse("42", opts).has_value());
		CHECK(parse("{}", opts).has_value());
		CHECK(parse("[]", opts).has_value());
		CHECK_FALSE(parse("[1]", opts).has_value());
		CHECK_FALSE(parse(R"({"a":1})", opts).has_value());
	}
	// depth-1: one level of children allowed
	{
		JsonParseOptions opts;
		opts.max_depth = LimitOption::bound(1);
		CHECK(parse("[1,2,3]", opts).has_value());
		CHECK(parse(R"({"a":1})", opts).has_value());
		CHECK_FALSE(parse("[[1]]", opts).has_value());
		CHECK_FALSE(parse(R"({"a":{"b":1}})", opts).has_value());
	}
	// boundary: depth exactly N passes, N+1 fails
	{
		JsonParseOptions opts;
		opts.max_depth = LimitOption::bound(3);
		string at_boundary(3, '[');
		at_boundary += "1";
		at_boundary.append(3, ']');
		CHECK(parse(at_boundary, opts).has_value());
		string over_boundary(4, '[');
		over_boundary += "1";
		over_boundary.append(4, ']');
		CHECK_FALSE(parse(over_boundary, opts).has_value());
	}
	// no_limit: very deep input passes
	{
		JsonParseOptions opts;
		opts.max_depth = no_limit;
		string deep(200, '[');
		deep += "1";
		deep.append(200, ']');
		CHECK(parse(deep, opts).has_value());
	}
}

TEST_CASE(
	"json: limits — max_input_size matrix",
	"[json][limits][pathological]") {
	// size-0: any non-empty input fails
	{
		JsonParseOptions opts;
		opts.max_input_size = LimitOption::bound(0);
		auto res = parse("1", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::input_too_large);
	}
	// size-1: single byte passes only for "1", "0", etc.
	{
		JsonParseOptions opts;
		opts.max_input_size = LimitOption::bound(1);
		CHECK(parse("1", opts).has_value());
		auto res = parse("12", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::input_too_large);
	}
	// boundary: exactly N bytes passes, N+1 fails
	{
		string const s = R"("hello")"; // 7 bytes
		JsonParseOptions opts;
		opts.max_input_size = LimitOption::bound(s.size());
		CHECK(parse(s, opts).has_value());
		opts.max_input_size = LimitOption::bound(s.size() - 1);
		CHECK_FALSE(parse(s, opts).has_value());
	}
	// no_limit: large input passes
	{
		JsonParseOptions opts;
		opts.max_input_size = no_limit;
		string big = "[";
		for (int i = 0; i < 1000; ++i) {
			if (i > 0) {
				big += ',';
			}
			big += to_string(i);
		}
		big += ']';
		CHECK(parse(big, opts).has_value());
	}
}

TEST_CASE(
	"json: limits — max_string_size matrix",
	"[json][limits][pathological]") {
	// size-0: any non-empty string fails
	{
		JsonParseOptions opts;
		opts.max_string_size = LimitOption::bound(0);
		auto res = parse(R"("x")", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::string_too_large);
	}
	// size-0 empty string passes
	{
		JsonParseOptions opts;
		opts.max_string_size = LimitOption::bound(0);
		CHECK(parse(R"("")", opts).has_value());
	}
	// boundary: exactly N chars passes, N+1 fails
	{
		JsonParseOptions opts;
		opts.max_string_size = LimitOption::bound(3);
		CHECK(parse(R"("abc")", opts).has_value());
		auto res = parse(R"("abcd")", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::string_too_large);
	}
	// no_limit: large string passes
	{
		JsonParseOptions opts;
		opts.max_string_size = no_limit;
		string big_str = "\"";
		big_str.append(100000, 'x');
		big_str += '"';
		CHECK(parse(big_str, opts).has_value());
	}
}

TEST_CASE(
	"json: limits — two limits interact, only one violated",
	"[json][limits][pathological]") {
	// max_depth=1 ok, max_string_size=2 fails
	{
		JsonParseOptions opts;
		opts.max_depth = LimitOption::bound(1);
		opts.max_string_size = LimitOption::bound(2);
		auto res = parse(R"(["abc"])", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::string_too_large);
	}
	// max_input_size ok, max_depth fails
	{
		JsonParseOptions opts;
		opts.max_input_size = LimitOption::bound(20);
		opts.max_depth = LimitOption::bound(1);
		auto res = parse("[[1]]", opts); // 5 bytes, depth 2
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::nesting_too_deep);
	}
	// both violated: input_too_large checked first
	{
		JsonParseOptions opts;
		opts.max_input_size = LimitOption::bound(2);
		opts.max_depth = LimitOption::bound(0);
		auto res = parse("[1]", opts);
		REQUIRE_FALSE(res.has_value());
		CHECK(res.error().code == JsonIssueCode::input_too_large);
	}
}
