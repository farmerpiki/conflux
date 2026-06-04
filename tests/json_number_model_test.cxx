// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: number form — integer vs non_integer",
	"[json][number]") {
	auto check = [](std::string_view input, JsonNumberForm expected_form) {
		auto doc = parse(input);
		REQUIRE(doc.has_value());
		auto n = doc->root().as_number();
		REQUIRE(n.has_value());
		CHECK(n->form() == expected_form);
	};
	check("42", JsonNumberForm::integer);
	check("-1", JsonNumberForm::integer);
	check("0", JsonNumberForm::integer);
	check("1.0", JsonNumberForm::non_integer);
	check("1e2", JsonNumberForm::non_integer);
	check("1E0", JsonNumberForm::non_integer);
	check("0.5", JsonNumberForm::non_integer);
}

TEST_CASE(
	"json: to_i64 / to_u64 reject non_integer form",
	"[json][number]") {
	auto doc = parse("1.0");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	CHECK_FALSE(n->to_i64().has_value());
	CHECK_FALSE(n->to_u64().has_value());
	CHECK(n->to_i64().error().code == JsonIssueCode::invalid_number);
}

TEST_CASE(
	"json: to_u64 sign_mismatch on negative integer",
	"[json][number]") {
	auto doc = parse("-42");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto res = n->to_u64();
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::sign_mismatch);
}

TEST_CASE(
	"json: to_u64 sign_mismatch on negative zero",
	"[json][number]") {
	auto doc = parse("-0");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto res = n->to_u64();
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::sign_mismatch);
}

TEST_CASE(
	"json: to_u64 number_out_of_range on overflow",
	"[json][number]") {
	auto doc = parse("18446744073709551616");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto res = n->to_u64();
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::number_out_of_range);
}

TEST_CASE(
	"json: to_f64 number_out_of_range on overflow to infinity",
	"[json][number]") {
	auto doc = parse("1e999");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto res = n->to_f64();
	REQUIRE_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::number_out_of_range);
}
