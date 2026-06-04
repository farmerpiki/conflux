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
	"json: ValueBuilder::set<T> encodes via codec",
	"[json][builder][codec]") {
	auto b = value_builder();
	auto ok = b.set<std::int64_t>(42LL);
	REQUIRE(ok.has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<std::int64_t>(doc->root());
	REQUIRE(r.has_value());
	CHECK(*r == 42LL);
}

TEST_CASE(
	"json: encode std::vector<std::int64_t> via set<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	std::vector<std::int64_t> const v{1LL, 2LL, 3LL};
	auto ok = b.set<std::vector<std::int64_t>>(v);
	REQUIRE(ok.has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<std::vector<std::int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 3UZ);
	CHECK((*r)[0] == 1LL);
	CHECK((*r)[1] == 2LL);
	CHECK((*r)[2] == 3LL);
}

TEST_CASE(
	"json: encode std::pair<std::string,i64> via set<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	auto ok = b.set<std::pair<std::string, std::int64_t>>({"hello", 7LL});
	REQUIRE(ok.has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<std::pair<std::string, std::int64_t>>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->first == "hello");
	CHECK(r->second == 7LL);
}

TEST_CASE(
	"json: encode std::tuple<bool,i64> via set<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	auto ok = b.set<std::tuple<bool, std::int64_t>>(std::tuple{true, 99LL});
	REQUIRE(ok.has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<std::tuple<bool, std::int64_t>>(doc->root());
	REQUIRE(r.has_value());
	CHECK(get<0>(*r) == true);
	CHECK(get<1>(*r) == 99LL);
}

TEST_CASE(
	"json: encode std::map<std::string,i64> via set<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	std::map<std::string, std::int64_t> const m{
		{"a", 1LL},
		{"b", 2LL}
    };
	auto ok = b.set<std::map<std::string, std::int64_t>>(m);
	REQUIRE(ok.has_value());
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<std::map<std::string, std::int64_t>>(doc->root());
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
	std::move(obj).commit();
	auto doc = std::move(b).finish();
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
	"json: encode V via ArrayBuilder::append<T>",
	"[json][builder][codec]") {
	auto b = value_builder();
	auto arr_res = b.begin_array();
	REQUIRE(arr_res.has_value());
	auto &arr = *arr_res;
	REQUIRE(arr.append<std::int64_t>(10LL).has_value());
	REQUIRE(arr.append<std::int64_t>(20LL).has_value());
	std::move(arr).commit();
	auto doc = std::move(b).finish();
	REQUIRE(doc.has_value());
	auto r = decode<std::vector<std::int64_t>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 2UZ);
	CHECK((*r)[0] == 10LL);
	CHECK((*r)[1] == 20LL);
}
