#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.json;
import conflux.json.native_provider;

namespace cj = conflux::json;
namespace jb = conflux::json::boundary;

TEST_CASE(
	"json boundary: native provider dumps documents without HTTP depending on Document directly",
	"[json][boundary]") {
	static_assert(jb::JsonDocumentProvider<jb::NativeJsonProvider>);
	static_assert(jb::JsonDumpProvider<jb::NativeJsonProvider, ::Document>);
	static_assert(jb::JsonDecodeProvider<jb::NativeJsonProvider, i64>);

	auto doc = cj::parse_copy(SV{R"({"ok":true,"n":42})"});
	REQUIRE(doc.has_value());

	auto body = jb::dump_with<jb::NativeJsonProvider>(*doc);
	REQUIRE(body.has_value());
	CHECK(*body == R"({"ok":true,"n":42})");
}

TEST_CASE(
	"json boundary: native provider decodes through provider-neutral errors",
	"[json][boundary]") {
	auto value = jb::decode_with<jb::NativeJsonProvider, i64>("42", {.copy_input = false});
	REQUIRE(value.has_value());
	CHECK(*value == 42);

	auto failed = jb::decode_with<jb::NativeJsonProvider, i64>(R"("not an int")");
	REQUIRE_FALSE(failed.has_value());
	CHECK(failed.error().stage == jb::ErrorStage::lookup);
	CHECK(failed.error().code == jb::ErrorCode::wrong_kind);
}

TEST_CASE(
	"json boundary: native provider can encode codec-backed values",
	"[json][boundary]") {
	auto body = jb::dump_native<i64>(42);
	REQUIRE(body.has_value());
	CHECK(*body == "42");
}
