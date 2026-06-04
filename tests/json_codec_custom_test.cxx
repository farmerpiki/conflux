// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

enum class Color {
	red,
	green,
	blue,
};

template<>
struct conflux::json::JsonCodec<Color> {
	static std::expected<Color, JsonError> decode(
		NodeRef n) {
		auto s = n.as_string();
		if (!s) {
			return std::unexpected(std::move(s).error());
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
		return std::unexpected(
			JsonError{
				.stage = JsonStage::decode,
				.code = JsonIssueCode::invalid_value,
				.target_type = std::string{type_name()},
				.message = std::format("unknown Color spelling: {}", *s)});
	}

	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		Color c) {
		switch (c) {
		case Color::red  : return b.set_string("red");
		case Color::green: return b.set_string("green");
		case Color::blue : return b.set_string("blue");
		}
		return std::unexpected(
			JsonError{
				.stage = JsonStage::build,
				.code = JsonIssueCode::invalid_value,
				.target_type = std::string{type_name()},
				.message = "Color enum value outside declared range"});
	}

	static constexpr std::string_view type_name() { return "Color"; }
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
