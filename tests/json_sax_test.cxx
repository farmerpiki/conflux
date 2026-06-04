// Plain TU — not a module unit.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

using namespace conflux::json;

// ─── Phase 3 — SAX / Event Interface ────────────────────────────────────────

TEST_CASE(
	"phase3: parse_sax null literal",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		bool got_null = false;
		std::expected<void, JsonError> on_null() {
			got_null = true;
			return {};
		}
	} h;
	auto r = parse_sax("null", h);
	REQUIRE(r.has_value());
	CHECK(h.got_null);
}
TEST_CASE(
	"phase3: parse_sax bool true/false",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		std::vector<bool> vals;
		std::expected<void, JsonError> on_bool(
			bool b) {
			vals.push_back(b);
			return {};
		}
	} h;
	auto r = parse_sax("[true,false]", h);
	REQUIRE(r.has_value());
	REQUIRE(h.vals.size() == 2UZ);
	CHECK(h.vals[0] == true);
	CHECK(h.vals[1] == false);
}
TEST_CASE(
	"phase3: parse_sax string value decoded",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		std::string got;
		std::expected<void, JsonError> on_string(
			std::string_view sv) {
			got = sv;
			return {};
		}
	} h;
	auto r = parse_sax(R"("hello")", h);
	REQUIRE(r.has_value());
	CHECK(h.got == "hello");
}
TEST_CASE(
	"phase3: parse_sax string with escapes",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		std::string got;
		std::expected<void, JsonError> on_string(
			std::string_view sv) {
			got = sv;
			return {};
		}
	} h;
	auto r = parse_sax(R"("a\nb")", h);
	REQUIRE(r.has_value());
	CHECK(h.got == "a\nb");
}
TEST_CASE(
	"phase3: parse_sax typed number dispatch i64",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		int64_t val{};
		std::expected<void, JsonError> on_i64(
			int64_t v) {
			val = v;
			return {};
		}
	} h;
	auto r = parse_sax("-42", h);
	REQUIRE(r.has_value());
	CHECK(h.val == -42LL);
}
TEST_CASE(
	"phase3: parse_sax typed number dispatch u64",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		uint64_t val{};
		std::expected<void, JsonError> on_u64(
			uint64_t v) {
			val = v;
			return {};
		}
	} h;
	// large positive beyond std::int64_t max
	auto r = parse_sax("18446744073709551615", h);
	REQUIRE(r.has_value());
	CHECK(h.val == 18446744073709551615ULL);
}
TEST_CASE(
	"phase3: parse_sax typed number dispatch double",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		double val{};
		std::expected<void, JsonError> on_double(
			double v) {
			val = v;
			return {};
		}
	} h;
	auto r = parse_sax("3.14", h);
	REQUIRE(r.has_value());
	CHECK(h.val == Catch::Approx(3.14));
}
TEST_CASE(
	"phase3: parse_sax on_number_raw opt-in",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		std::string raw;
		std::expected<void, JsonError> on_number_raw(
			std::string_view sv) {
			raw = sv;
			return {};
		}
	} h;
	auto r = parse_sax("3.14", h);
	REQUIRE(r.has_value());
	CHECK(h.raw == "3.14");
}
TEST_CASE(
	"phase3: parse_sax object events and keys",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		int begin_obj{}, end_obj{};
		std::vector<std::string> keys;
		std::vector<std::string> strings;
		std::expected<void, JsonError> on_begin_object() {
			++begin_obj;
			return {};
		}
		std::expected<void, JsonError> on_end_object() {
			++end_obj;
			return {};
		}
		std::expected<void, JsonError> on_key(
			std::string_view k) {
			keys.emplace_back(k);
			return {};
		}
		std::expected<void, JsonError> on_string(
			std::string_view v) {
			strings.emplace_back(v);
			return {};
		}
	} h;
	auto r = parse_sax(R"({"a":"x","b":"y"})", h);
	REQUIRE(r.has_value());
	CHECK(h.begin_obj == 1);
	CHECK(h.end_obj == 1);
	REQUIRE(h.keys.size() == 2UZ);
	CHECK(h.keys[0] == "a");
	CHECK(h.keys[1] == "b");
	CHECK(h.strings[0] == "x");
	CHECK(h.strings[1] == "y");
}
TEST_CASE(
	"phase3: parse_sax array events",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		int begin_arr{}, end_arr{};
		std::vector<int64_t> nums;
		std::expected<void, JsonError> on_begin_array() {
			++begin_arr;
			return {};
		}
		std::expected<void, JsonError> on_end_array() {
			++end_arr;
			return {};
		}
		std::expected<void, JsonError> on_i64(
			int64_t v) {
			nums.push_back(v);
			return {};
		}
	} h;
	auto r = parse_sax("[1,2,3]", h);
	REQUIRE(r.has_value());
	CHECK(h.begin_arr == 1);
	CHECK(h.end_arr == 1);
	REQUIRE(h.nums.size() == 3UZ);
	CHECK(h.nums[0] == 1LL);
	CHECK(h.nums[2] == 3LL);
}
TEST_CASE(
	"phase3: parse_sax handler abort via error return",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		int count{};
		std::expected<void, JsonError> on_i64(
			int64_t) {
			++count;
			if (count >= 2) {
				return std::unexpected(JsonError{.code = JsonIssueCode::invalid_value, .message = "stop"});
			}
			return {};
		}
	} h;
	auto r = parse_sax("[1,2,3]", h);
	CHECK_FALSE(r.has_value());
	CHECK(h.count == 2);
}
TEST_CASE(
	"phase3: parse_sax void handler compiles and runs",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		int nulls{};
		void on_null() { ++nulls; }
	} h;
	auto r = parse_sax("[null,null]", h);
	REQUIRE(r.has_value());
	CHECK(h.nulls == 2);
}
TEST_CASE(
	"phase3: parse_sax malformed input returns error",
	"[phase3]") {
	struct H : JsonDefaultHandler {
	} h;
	auto r = parse_sax("{bad}", h);
	CHECK_FALSE(r.has_value());
}
TEST_CASE(
	"phase3: parse_sax nested object structure",
	"[phase3]") {
	struct H : JsonDefaultHandler {
		int depth_max{};
		int current{};
		std::expected<void, JsonError> on_begin_object() {
			++current;
			depth_max = std::max(depth_max, current);
			return {};
		}
		std::expected<void, JsonError> on_end_object() {
			--current;
			return {};
		}
	} h;
	auto r = parse_sax(R"({"a":{"b":{"c":1}}})", h);
	REQUIRE(r.has_value());
	CHECK(h.depth_max == 3);
	CHECK(h.current == 0);
}
