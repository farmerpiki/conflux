// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: Nullable<T> default-constructs to null",
	"[json][nullable]") {
	Nullable<std::int64_t> n;
	CHECK(n.is_null());
	CHECK_FALSE(n.has_value());
	CHECK_FALSE(static_cast<bool>(n));
}

TEST_CASE(
	"json: Nullable<T> value state",
	"[json][nullable]") {
	Nullable<std::int64_t> n{42LL};
	CHECK_FALSE(n.is_null());
	CHECK(n.has_value());
	CHECK(static_cast<bool>(n));
	CHECK(*n == 42LL);
	CHECK(n.value() == 42LL);
}

TEST_CASE(
	"json: Nullable<T> value_or",
	"[json][nullable]") {
	Nullable<std::int64_t> n_null;
	Nullable<std::int64_t> n_val{7LL};
	CHECK(n_null.value_or(99LL) == 99LL);
	CHECK(n_val.value_or(99LL) == 7LL);
}

TEST_CASE(
	"json: Nullable<T> equality",
	"[json][nullable]") {
	Nullable<std::int64_t> a{1LL};
	Nullable<std::int64_t> b{1LL};
	Nullable<std::int64_t> c{2LL};
	Nullable<std::int64_t> n;
	CHECK(a == b);
	CHECK_FALSE(a == c);
	CHECK_FALSE(a == n);
}

TEST_CASE(
	"json: Nullable<T> operator->",
	"[json][nullable]") {
	Nullable<std::string> n{"hello"};
	CHECK(n->size() == 5UZ);
}

TEST_CASE(
	"json: Nullable<std::int64_t> — present value",
	"[json][nullable]") {
	auto doc = parse("42");
	REQUIRE(doc.has_value());
	auto res = decode<Nullable<std::int64_t>>(doc->root());
	REQUIRE(res.has_value());
	CHECK(res->has_value());
	CHECK(**res == 42LL);
}

TEST_CASE(
	"json: Nullable<std::int64_t> — null input",
	"[json][nullable]") {
	auto doc = parse("null");
	REQUIRE(doc.has_value());
	auto res = decode<Nullable<std::int64_t>>(doc->root());
	REQUIRE(res.has_value());
	CHECK(res->is_null());
}

TEST_CASE(
	"json: decode<Nullable<std::string>> — non-null",
	"[json][nullable][codec]") {
	auto doc = parse(R"("world")");
	REQUIRE(doc.has_value());
	auto r = decode<Nullable<std::string>>(doc->root());
	REQUIRE(r.has_value());
	REQUIRE(r->has_value());
	CHECK(**r == "world");
}
