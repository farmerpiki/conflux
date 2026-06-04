// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

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
	"json: dump std::string escapes control chars",
	"[json][dump]") {
	auto doc = parse(R"("a\tb\nc")");
	REQUIRE(doc.has_value());
	auto d = doc->dump();
	REQUIRE(d.has_value());
	CHECK(d->find("\\t") != std::string::npos);
	CHECK(d->find("\\n") != std::string::npos);
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
	CHECK(d->find('\n') != std::string::npos);
}

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
	CHECK(d->find("\\u") != std::string::npos);
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
	CHECK(d->find("\\ud83d") != std::string::npos);
	auto reparsed = parse(*d);
	REQUIRE(reparsed.has_value());
	CHECK(*reparsed->root().as_string() == "\xF0\x9F\x98\x80");
}

TEST_CASE(
	"json: dump ascii_only escapes high-byte boundaries",
	"[json][dump]") {
	auto doc = parse(R"(["aaaaaaaaaaaaaaaaé","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaé","oké"])");
	REQUIRE(doc.has_value());
	JsonDumpOptions opts;
	opts.ascii_only = true;
	auto d = doc->dump(opts);
	REQUIRE(d.has_value());
	CHECK(*d == R"(["aaaaaaaaaaaaaaaa\u00e9","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\u00e9","ok\u00e9"])");
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
	CHECK(d->find("    \"k\"") != std::string::npos);
}
