#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.http.types;

TEST_CASE(
	"conflux::http::HttpFields::set replaces all duplicate entries with a single one") {
	conflux::http::HttpFields f{true};
	f.emplace_back("Set-Cookie", "a=1");
	f.emplace_back("Set-Cookie", "b=2");
	f.emplace_back("Set-Cookie", "c=3");
	REQUIRE(f.size() == 3);

	f.set("set-cookie", "z=9");
	REQUIRE(f.size() == 1);
	REQUIRE(f.get("set-cookie") == "z=9");
	REQUIRE(f.values("set-cookie").size() == 1);
	REQUIRE(f.contains("set-cookie"));
}

TEST_CASE(
	"conflux::http::HttpFields::set inserts when key absent") {
	conflux::http::HttpFields f{true};
	f.set("Content-Type", "text/plain");
	REQUIRE(f.size() == 1);
	REQUIRE(f.get("content-type") == "text/plain");
}

TEST_CASE(
	"conflux::http::HttpFields::set preserves positions of other keys") {
	conflux::http::HttpFields f{true};
	f.emplace_back("A", "1");
	f.emplace_back("Dup", "x");
	f.emplace_back("B", "2");
	f.emplace_back("Dup", "y");
	f.emplace_back("C", "3");

	f.set("dup", "merged");
	REQUIRE(f.size() == 4);
	std::vector<std::string> keys;
	for (auto const &[k, _v]: f) {
		keys.push_back(k);
	}
	REQUIRE(keys == (std::vector<std::string>{"A", "Dup", "B", "C"}));
	REQUIRE(f.get("dup") == "merged");
	REQUIRE(f.get("a") == "1");
	REQUIRE(f.get("b") == "2");
	REQUIRE(f.get("c") == "3");
}

TEST_CASE(
	"conflux::http::HttpFields::erase removes all matches and returns count") {
	conflux::http::HttpFields f{true};
	f.emplace_back("Cookie", "a=1");
	f.emplace_back("Cookie", "b=2");
	f.emplace_back("Host", "example.com");

	auto removed = f.erase("cookie");
	REQUIRE(removed == 2);
	REQUIRE(f.size() == 1);
	REQUIRE(!f.contains("cookie"));
	REQUIRE(f.get("host") == "example.com");
}

TEST_CASE(
	"conflux::http::HttpFields::erase returns 0 when key absent") {
	conflux::http::HttpFields f{true};
	f.emplace_back("A", "1");
	REQUIRE(f.erase("missing") == 0);
	REQUIRE(f.size() == 1);
}

TEST_CASE(
	"conflux::http::HttpFields index stays consistent after set then erase") {
	conflux::http::HttpFields f{true};
	f.emplace_back("X", "1");
	f.emplace_back("X", "2");
	f.emplace_back("Y", "a");

	f.set("x", "z");
	REQUIRE(f.contains("x"));
	REQUIRE(f.get("x") == "z");
	REQUIRE(f.values("x").size() == 1);

	auto removed = f.erase("x");
	REQUIRE(removed == 1);
	REQUIRE(!f.contains("x"));
	REQUIRE(f.get("y") == "a");
	REQUIRE(f.size() == 1);
}

TEST_CASE(
	"conflux::http::HttpFields::values returns all entries for duplicate keys") {
	conflux::http::HttpFields f{true};
	f.emplace_back("Cookie", "a=1");
	f.emplace_back("Cookie", "b=2");
	f.emplace_back("Cookie", "c=3");
	f.emplace_back("Other", "x");
	auto vals = f.values("cookie");
	REQUIRE(vals.size() == 3);
	using sv = std::string_view;
	REQUIRE(std::ranges::contains(vals, sv{"a=1"}));
	REQUIRE(std::ranges::contains(vals, sv{"b=2"}));
	REQUIRE(std::ranges::contains(vals, sv{"c=3"}));
}

TEST_CASE(
	"conflux::http::HttpFields zero-allocation value callbacks visit duplicate keys") {
	conflux::http::HttpFields f{true};
	f.emplace_back("Set-Cookie", "a=1");
	f.emplace_back("set-cookie", "b=2");
	f.emplace_back("Other", "x");

	std::vector<std::string_view> vals;
	f.for_each_value("SET-COOKIE", [&](std::string_view value) { vals.push_back(value); });
	REQUIRE(vals == (std::vector<std::string_view>{"a=1", "b=2"}));
	int visits = 0;
	CHECK_FALSE(f.for_each_value_until("set-cookie", [&](std::string_view value) {
		++visits;
		return value != "a=1";
	}));
	CHECK(visits == 1);
	CHECK(f.any_value("set-cookie", [](std::string_view value) { return value == "b=2"; }));
	CHECK_FALSE(f.any_value("set-cookie", [](std::string_view value) { return value == "c=3"; }));

	conflux::http::HttpFieldsView view{f};
	vals.clear();
	conflux::http::for_each_header_value(view, "set-cookie", [&](std::string_view value) { vals.push_back(value); });
	REQUIRE(vals == (std::vector<std::string_view>{"a=1", "b=2"}));
	visits = 0;
	CHECK_FALSE(conflux::http::for_each_header_value_until(view, "set-cookie", [&](std::string_view value) {
		++visits;
		return value != "b=2";
	}));
	CHECK(visits == 2);
	CHECK(conflux::http::any_header_value(view, "set-cookie", [](std::string_view value) { return value == "a=1"; }));
	CHECK_FALSE(
		conflux::http::any_header_value(view, "set-cookie", [](std::string_view value) { return value == "z=9"; }));
}

TEST_CASE(
	"conflux::http::HttpFields::value_or returns default when key absent") {
	conflux::http::HttpFields f{true};
	f.emplace_back("A", "hello");
	REQUIRE(f.value_or("A") == "hello");
	REQUIRE(f.value_or("Missing", "default") == "default");
	REQUIRE(f.value_or("Missing") == "");
}

TEST_CASE(
	"conflux::http::HttpFields case-insensitive lookup folds only ASCII letters") {
	conflux::http::HttpFields f{true};
	f.emplace_back("X^Name", "caret");
	f.emplace_back("X~Name", "tilde");
	REQUIRE(f.get("x^name") == "caret");
	REQUIRE(f.get("x~name") == "tilde");
	REQUIRE(f.values("x^name").size() == 1);
	REQUIRE(f.values("x~name").size() == 1);

	conflux::http::HttpFieldsView view{f};
	REQUIRE(view.get("x^name") == "caret");
	REQUIRE(view.get("x~name") == "tilde");
	REQUIRE(view.values("x^name").size() == 1);
	REQUIRE(view.values("x~name").size() == 1);
}
