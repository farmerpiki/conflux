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
	"json: decode<optional<int64_t>> — null",
	"[json][codec]") {
	auto doc = parse("null");
	REQUIRE(doc.has_value());
	auto r = decode<optional<int64_t>>(doc->root());
	REQUIRE(r.has_value());
	CHECK_FALSE(r->has_value());
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
