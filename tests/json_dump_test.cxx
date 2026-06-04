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
