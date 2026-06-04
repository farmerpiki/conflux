// Plain TU — not a module unit.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

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
	"json: decode<std::int64_t>",
	"[json][codec]") {
	auto doc = parse("-7");
	REQUIRE(doc.has_value());
	auto r = decode<std::int64_t>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == -7LL);
}

TEST_CASE(
	"json: decode<std::uint64_t>",
	"[json][codec]") {
	auto doc = parse("42");
	REQUIRE(doc.has_value());
	auto r = decode<std::uint64_t>(doc->root());
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
	"json: decode<std::string>",
	"[json][codec]") {
	auto doc = parse(R"("hello")");
	REQUIRE(doc.has_value());
	auto r = decode<std::string>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == "hello");
}

TEST_CASE(
	"json: decode<std::string_view>",
	"[json][codec]") {
	auto doc = parse(R"("world")");
	REQUIRE(doc.has_value());
	auto r = decode<std::string_view>(doc->root());
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
	"json: decode<std::vector<std::int64_t>>",
	"[json][codec]") {
	auto doc = parse("[1,2,3]");
	REQUIRE(doc.has_value());
	auto r = decode<std::vector<std::int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 3UZ);
	CHECK((*r)[0] == 1LL);
	CHECK((*r)[1] == 2LL);
	CHECK((*r)[2] == 3LL);
}

TEST_CASE(
	"json: decode<std::optional<std::int64_t>> — present",
	"[json][codec]") {
	auto doc = parse("99");
	REQUIRE(doc.has_value());
	auto r = decode<std::optional<std::int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->has_value());
	CHECK(**r == 99LL);
}

TEST_CASE(
	"json: decode<std::optional<std::int64_t>> — null yields error",
	"[json][codec]") {
	auto doc = parse("null");
	REQUIRE(doc.has_value());
	auto r = decode<std::optional<std::int64_t>>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::wrong_kind);
}

TEST_CASE(
	"json: has_json_codec detects built-in types",
	"[json][codec]") {
	CHECK(has_json_codec<bool>);
	CHECK(has_json_codec<std::int64_t>);
	CHECK(has_json_codec<std::uint64_t>);
	CHECK(has_json_codec<double>);
	CHECK(has_json_codec<std::string>);
	CHECK(has_json_codec<std::string_view>);
}

TEST_CASE(
	"json: has_json_codec false for non-codec types",
	"[json][codec]") {
	struct NoCodec {};
	CHECK_FALSE(has_json_codec<NoCodec>);
}
