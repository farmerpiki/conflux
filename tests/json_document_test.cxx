// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: document is self-contained",
	"[json]") {
	std::string src{R"({"msg": "hello"})"};
	auto doc = parse_copy(src);
	REQUIRE(doc.has_value());
	src.clear();
	src.shrink_to_fit();
	auto val = doc->root().as_object()->member("msg")->as_string();
	REQUIRE(val.has_value());
	CHECK(*val == "hello");
}
