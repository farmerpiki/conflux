// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

struct Config {
	std::int64_t required_field{};
	std::optional<std::int64_t> optional_field{};
};

template<>
struct conflux::json::JsonMembers<Config> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("required_field", &Config::required_field),
			conflux::json::json_member("optional_field", &Config::optional_field),
		};
	}
	static constexpr std::string_view type_name() { return "Config"; }
};

TEST_CASE(
	"json: Opt member present decodes value",
	"[json][codec][Opt]") {
	auto doc = parse(R"({"required_field":1,"optional_field":42})");
	REQUIRE(doc.has_value());
	auto r = decode<Config>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->required_field == 1LL);
	REQUIRE(r->optional_field.has_value());
	CHECK(*r->optional_field == 42LL);
}

TEST_CASE(
	"json: Opt member absent yields std::nullopt",
	"[json][codec][Opt]") {
	auto doc = parse(R"({"required_field":1})");
	REQUIRE(doc.has_value());
	auto r = decode<Config>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->required_field == 1LL);
	CHECK_FALSE(r->optional_field.has_value());
}
