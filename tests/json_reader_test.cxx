// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <conflux/detail/discard.hxx>

import std;
import conflux.json;

using namespace conflux::json;

TEST_CASE(
	"phase4: JsonReader scalar events",
	"[phase4]") {
	{
		JsonReader r{"42"};
		auto ev = r.next();
		REQUIRE(ev.has_value());
		REQUIRE(ev->has_value());
		CHECK(**ev == JsonReader::Event::number_value);
		auto i = r.number_val().to_i64();
		REQUIRE(i.has_value());
		CHECK(*i == 42LL);
	}
	{
		JsonReader r{"true"};
		auto ev = r.next();
		REQUIRE(ev.has_value());
		REQUIRE(ev->has_value());
		CHECK(**ev == JsonReader::Event::bool_value);
		CHECK(r.bool_val() == true);
	}
	{
		JsonReader r{"null"};
		auto ev = r.next();
		REQUIRE(ev.has_value());
		REQUIRE(ev->has_value());
		CHECK(**ev == JsonReader::Event::null_value);
	}
	{
		JsonReader r{R"("hello")"};
		auto ev = r.next();
		REQUIRE(ev.has_value());
		REQUIRE(ev->has_value());
		CHECK(**ev == JsonReader::Event::string_value);
		auto borrow = r.string_token().unescaped_borrow();
		REQUIRE(borrow.has_value());
		CHECK(*borrow == "hello");
	}
}
TEST_CASE(
	"phase4: JsonReader accepts std::byte-span input",
	"[phase4]") {
	auto const json = std::string_view{R"({"x":1,"y":2})"};
	JsonReader r{
		std::span<std::byte const>{reinterpret_cast<std::byte const *>(json.data()), json.size()}
    };

	auto e0 = r.next();
	REQUIRE(e0.has_value());
	REQUIRE(e0->has_value());
	CHECK(**e0 == JsonReader::Event::begin_object);

	auto e1 = r.next();
	REQUIRE(e1.has_value());
	REQUIRE(e1->has_value());
	CHECK(**e1 == JsonReader::Event::key);
	auto key = r.key_token().unescaped_borrow();
	REQUIRE(key.has_value());
	CHECK(*key == "x");
}
TEST_CASE(
	"phase4: JsonReader EOF returns nullopt",
	"[phase4]") {
	JsonReader r{"null"};
	auto ev1 = r.next();
	REQUIRE(ev1.has_value());
	REQUIRE(ev1->has_value());
	auto ev2 = r.next();
	REQUIRE(ev2.has_value());
	CHECK(!ev2->has_value());
}
TEST_CASE(
	"phase4: JsonReader object events",
	"[phase4]") {
	JsonReader r{R"({"x":1,"y":2})"};

	auto e0 = r.next();
	REQUIRE(e0.has_value());
	REQUIRE(e0->has_value());
	CHECK(**e0 == JsonReader::Event::begin_object);

	auto e1 = r.next();
	REQUIRE(e1.has_value());
	REQUIRE(e1->has_value());
	CHECK(**e1 == JsonReader::Event::key);
	auto borrow1 = r.key_token().unescaped_borrow();
	REQUIRE(borrow1.has_value());
	CHECK(*borrow1 == "x");

	auto e2 = r.next();
	REQUIRE(e2.has_value());
	REQUIRE(e2->has_value());
	CHECK(**e2 == JsonReader::Event::number_value);
	auto v1 = r.number_val().to_i64();
	REQUIRE(v1.has_value());
	CHECK(*v1 == 1LL);

	auto e3 = r.next();
	REQUIRE(e3.has_value());
	REQUIRE(e3->has_value());
	CHECK(**e3 == JsonReader::Event::key);
	auto borrow2 = r.key_token().unescaped_borrow();
	REQUIRE(borrow2.has_value());
	CHECK(*borrow2 == "y");

	auto e4 = r.next();
	REQUIRE(e4.has_value());
	REQUIRE(e4->has_value());
	CHECK(**e4 == JsonReader::Event::number_value);
	auto v2 = r.number_val().to_i64();
	REQUIRE(v2.has_value());
	CHECK(*v2 == 2LL);

	auto e5 = r.next();
	REQUIRE(e5.has_value());
	REQUIRE(e5->has_value());
	CHECK(**e5 == JsonReader::Event::end_object);

	auto e6 = r.next();
	REQUIRE(e6.has_value());
	CHECK(!e6->has_value());
}
TEST_CASE(
	"phase4: JsonReader array events",
	"[phase4]") {
	JsonReader r{"[1,2,3]"};

	auto e0 = r.next();
	REQUIRE(e0.has_value());
	REQUIRE(e0->has_value());
	CHECK(**e0 == JsonReader::Event::begin_array);

	for (std::int64_t expected = 1; expected <= 3; ++expected) {
		auto en = r.next();
		REQUIRE(en.has_value());
		REQUIRE(en->has_value());
		CHECK(**en == JsonReader::Event::number_value);
		auto v = r.number_val().to_i64();
		REQUIRE(v.has_value());
		CHECK(*v == expected);
	}

	auto e4 = r.next();
	REQUIRE(e4.has_value());
	REQUIRE(e4->has_value());
	CHECK(**e4 == JsonReader::Event::end_array);
}
TEST_CASE(
	"phase4: JsonReader depth tracking",
	"[phase4]") {
	JsonReader r{R"([[1,2]])"};
	CHECK(r.depth() == 0UZ);

	auto e0 = r.next();
	REQUIRE(e0.has_value());
	REQUIRE(e0->has_value());
	CHECK(**e0 == JsonReader::Event::begin_array);
	CHECK(r.depth() == 1UZ);

	auto e1 = r.next();
	REQUIRE(e1.has_value());
	REQUIRE(e1->has_value());
	CHECK(**e1 == JsonReader::Event::begin_array);
	CHECK(r.depth() == 2UZ);

	CONFLUX_DISCARD(r.next()); // 1
	CONFLUX_DISCARD(r.next()); // 2
	auto e_end = r.next();
	REQUIRE(e_end.has_value());
	REQUIRE(e_end->has_value());
	CHECK(**e_end == JsonReader::Event::end_array);
	CHECK(r.depth() == 1UZ);

	auto e_end2 = r.next();
	REQUIRE(e_end2.has_value());
	REQUIRE(e_end2->has_value());
	CHECK(**e_end2 == JsonReader::Event::end_array);
	CHECK(r.depth() == 0UZ);
}
TEST_CASE(
	"phase4: JsonReader skip_next_value returns std::byte range",
	"[phase4]") {
	JsonReader r{R"({"a":{"b":1},"c":2})"};

	auto e0 = r.next();
	REQUIRE(e0.has_value());
	REQUIRE(e0->has_value());
	CHECK(**e0 == JsonReader::Event::begin_object);

	auto e1 = r.next(); // key "a"
	REQUIRE(e1.has_value());
	REQUIRE(e1->has_value());
	CHECK(**e1 == JsonReader::Event::key);

	auto range = r.skip_next_value();
	REQUIRE(range.has_value());
	auto slice = r.input().substr(range->start, range->end - range->start);
	CHECK(slice == R"({"b":1})");

	auto e2 = r.next(); // key "c"
	REQUIRE(e2.has_value());
	REQUIRE(e2->has_value());
	CHECK(**e2 == JsonReader::Event::key);

	auto e3 = r.next(); // value 2
	REQUIRE(e3.has_value());
	REQUIRE(e3->has_value());
	CHECK(**e3 == JsonReader::Event::number_value);
	auto v = r.number_val().to_i64();
	REQUIRE(v.has_value());
	CHECK(*v == 2LL);

	auto e4 = r.next(); // end_object
	REQUIRE(e4.has_value());
	REQUIRE(e4->has_value());
	CHECK(**e4 == JsonReader::Event::end_object);
}
TEST_CASE(
	"phase4: JsonReader skip_next_value validates skipped input",
	"[phase4]") {
	JsonReader r{R"({"a":"bad\q","c":2})"};

	auto e0 = r.next();
	REQUIRE(e0.has_value());
	REQUIRE(e0->has_value());
	CHECK(**e0 == JsonReader::Event::begin_object);

	auto e1 = r.next();
	REQUIRE(e1.has_value());
	REQUIRE(e1->has_value());
	CHECK(**e1 == JsonReader::Event::key);

	auto range = r.skip_next_value();
	CHECK_FALSE(range.has_value());
	CHECK(range.error().code == JsonIssueCode::syntax_error);
}
TEST_CASE(
	"phase4: JsonReader has_error and reset",
	"[phase4]") {
	JsonReader r{"bad_input"};
	CHECK_FALSE(r.has_error());
	auto ev = r.next();
	CHECK_FALSE(ev.has_value());
	CHECK(r.has_error());

	r.reset();
	CHECK_FALSE(r.has_error());
	CHECK(r.depth() == 0UZ);

	auto ev2 = r.next();
	CHECK_FALSE(ev2.has_value()); // still fails but fresh error
}
TEST_CASE(
	"phase4: JsonStringToken unescaped_borrow for simple string",
	"[phase4]") {
	JsonReader r{R"("hello world")"};
	auto ev = r.next();
	REQUIRE(ev.has_value());
	REQUIRE(ev->has_value());
	CHECK(**ev == JsonReader::Event::string_value);
	CHECK(r.string_token().has_escapes() == false);
	auto borrow = r.string_token().unescaped_borrow();
	REQUIRE(borrow.has_value());
	CHECK(*borrow == "hello world");
}
TEST_CASE(
	"phase4: JsonStringToken append_decoded_to for escaped string",
	"[phase4]") {
	JsonReader r{R"("hello\nworld")"};
	auto ev = r.next();
	REQUIRE(ev.has_value());
	REQUIRE(ev->has_value());
	CHECK(**ev == JsonReader::Event::string_value);
	CHECK(r.string_token().has_escapes() == true);
	CHECK(!r.string_token().unescaped_borrow().has_value());
	std::string out;
	auto res = r.string_token().append_decoded_to(out);
	REQUIRE(res.has_value());
	CHECK(out == "hello\nworld");
}
TEST_CASE(
	"phase4: JsonStringToken decode_into caller buffer",
	"[phase4]") {
	JsonReader r{R"("a\tb")"};
	auto ev = r.next();
	REQUIRE(ev.has_value());
	REQUIRE(ev->has_value());
	CHECK(**ev == JsonReader::Event::string_value);
	std::vector<char> buf(r.string_token().max_decoded_size());
	auto sv_res = r.string_token().decode_into(buf);
	REQUIRE(sv_res.has_value());
	CHECK(*sv_res == "a\tb");
}
