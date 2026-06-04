// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

using namespace conflux::json;

// ---------------------------------------------------------------------------
// Phase 7 — NDJSON / JsonAccumulator
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase7: NdjsonRange basic two-line NDJSON",
	"[phase7]") {
	std::string_view ndjson = R"({"a":1}
{"b":2})";
	NdjsonRange range{ndjson};
	std::vector<std::string> results;
	for (auto const &d: range) {
		REQUIRE(d.has_value());
		auto dumped = d->dump();
		REQUIRE(dumped.has_value());
		results.push_back(*dumped);
	}
	REQUIRE(results.size() == 2);
	CHECK(results[0] == R"({"a":1})");
	CHECK(results[1] == R"({"b":2})");
}
TEST_CASE(
	"phase7: NdjsonRange skips blank lines",
	"[phase7]") {
	std::string_view ndjson = "1\n\n2\n\n3";
	NdjsonRange range{ndjson};
	int count = 0;
	for (auto const &d: range) {
		REQUIRE(d.has_value());
		++count;
	}
	CHECK(count == 3);
}
TEST_CASE(
	"phase7: NdjsonRange strips CRLF",
	"[phase7]") {
	std::string_view ndjson = "\"hello\"\r\n\"world\"\r\n";
	NdjsonRange range{ndjson};
	std::vector<std::string> results;
	for (auto const &d: range) {
		REQUIRE(d.has_value());
		results.push_back(*d->dump());
	}
	REQUIRE(results.size() == 2);
	CHECK(results[0] == "\"hello\"");
	CHECK(results[1] == "\"world\"");
}
TEST_CASE(
	"phase7: NdjsonRange propagates parse error per line",
	"[phase7]") {
	std::string_view ndjson = "1\nbad json\n3";
	NdjsonRange range{ndjson};
	auto it = range.begin();

	REQUIRE(it != std::default_sentinel);
	CHECK(it->has_value());
	++it;

	REQUIRE(it != std::default_sentinel);
	CHECK_FALSE(it->has_value());
	++it;

	REQUIRE(it != std::default_sentinel);
	CHECK(it->has_value());
	++it;

	CHECK(it == std::default_sentinel);
}
TEST_CASE(
	"phase7: NdjsonRange empty input yields no elements",
	"[phase7]") {
	NdjsonRange range{""};
	int count = 0;
	for ([[maybe_unused]] auto const &element: range) {
		++count;
	}
	CHECK(count == 0);
}
TEST_CASE(
	"phase7: NdjsonRange single line no trailing newline",
	"[phase7]") {
	NdjsonRange range{R"([1,2,3])"};
	auto it = range.begin();
	REQUIRE(it != std::default_sentinel);
	REQUIRE(it->has_value());
	CHECK(*it->value().dump() == "[1,2,3]");
	++it;
	CHECK(it == std::default_sentinel);
}
TEST_CASE(
	"phase7: JsonAccumulator basic feed and finish",
	"[phase7]") {
	JsonAccumulator acc;
	REQUIRE(acc.feed("{\"x\":").has_value());
	REQUIRE(acc.feed("42}").has_value());
	auto doc = acc.finish();
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	auto n = obj->find_member("x");
	REQUIRE(n.has_value());
	auto num = n->as_number();
	REQUIRE(num.has_value());
	CHECK(*num->to_i64() == 42);
}
TEST_CASE(
	"phase7: JsonAccumulator single feed",
	"[phase7]") {
	JsonAccumulator acc;
	REQUIRE(acc.feed(R"("hello")").has_value());
	auto doc = acc.finish();
	REQUIRE(doc.has_value());
	auto sv = doc->root().as_string();
	REQUIRE(sv.has_value());
	CHECK(*sv == "hello");
}
TEST_CASE(
	"phase7: JsonAccumulator accepts std::byte-span feed",
	"[phase7]") {
	JsonAccumulator acc;
	auto const json = std::string_view{R"({"a":[1,2,3],"b":true})"};
	REQUIRE(acc.feed(std::span<std::byte const>{reinterpret_cast<std::byte const *>(json.data()), json.size()})
				.has_value());
	auto doc = acc.finish();
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	auto a = obj->find_member("a");
	REQUIRE(a.has_value());
	auto arr = a->as_array();
	REQUIRE(arr.has_value());
	CHECK(arr->size() == 3);
}
TEST_CASE(
	"phase7: JsonAccumulator finish with invalid JSON returns error",
	"[phase7]") {
	JsonAccumulator acc;
	REQUIRE(acc.feed("{broken").has_value());
	auto doc = acc.finish();
	CHECK_FALSE(doc.has_value());
}
TEST_CASE(
	"phase7: JsonAccumulator feed rejects over max_input_size",
	"[phase7]") {
	JsonParseOptions opts;
	opts.max_input_size = LimitOption::bound(10);
	JsonAccumulator acc{opts};
	REQUIRE(acc.feed("12345").has_value());
	auto res = acc.feed("678901");
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::input_too_large);
}
TEST_CASE(
	"phase7: JsonAccumulator accepts exact max_input_size boundary",
	"[phase7]") {
	JsonParseOptions opts;
	opts.max_input_size = LimitOption::bound(10);
	JsonAccumulator acc{opts};
	REQUIRE(acc.feed("12345").has_value());
	REQUIRE(acc.feed("67890").has_value());
	CHECK(acc.buffered_bytes() == 10);
	auto res = acc.feed("1");
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::input_too_large);
}
TEST_CASE(
	"phase7: JsonAccumulator reset clears buffer",
	"[phase7]") {
	JsonAccumulator acc;
	REQUIRE(acc.feed("[1,2,3]").has_value());
	CHECK(acc.buffered_bytes() == 7);
	acc.reset();
	CHECK(acc.buffered_bytes() == 0);
	REQUIRE(acc.feed("null").has_value());
	auto doc = acc.finish();
	REQUIRE(doc.has_value());
	CHECK(doc->root().is_null());
}
TEST_CASE(
	"phase7: JsonAccumulator buffered_bytes tracks feed",
	"[phase7]") {
	JsonAccumulator acc;
	CHECK(acc.buffered_bytes() == 0);
	REQUIRE(acc.feed("abc").has_value());
	CHECK(acc.buffered_bytes() == 3);
	REQUIRE(acc.feed("def").has_value());
	CHECK(acc.buffered_bytes() == 6);
}
TEST_CASE(
	"phase7: JsonStreamReader emits events across fragmented feeds",
	"[phase7]") {
	JsonStreamReader r;
	using Ev = JsonReader::Event;

	REQUIRE(r.feed(R"({"a")").has_value());

	auto e0 = r.next();
	REQUIRE(e0.has_value());
	REQUIRE(e0->has_value());
	CHECK(**e0 == Ev::begin_object);

	auto pending_key = r.next();
	REQUIRE(pending_key.has_value());
	CHECK_FALSE(pending_key->has_value());
	CHECK(r.pos() == 1UZ);

	REQUIRE(r.feed(R"(:[1)").has_value());

	auto e1 = r.next();
	REQUIRE(e1.has_value());
	REQUIRE(e1->has_value());
	CHECK(**e1 == Ev::key);
	auto key = r.key_token().unescaped_borrow();
	REQUIRE(key.has_value());
	CHECK(*key == "a");

	auto e2 = r.next();
	REQUIRE(e2.has_value());
	REQUIRE(e2->has_value());
	CHECK(**e2 == Ev::begin_array);

	auto pending_number = r.next();
	REQUIRE(pending_number.has_value());
	CHECK_FALSE(pending_number->has_value());

	REQUIRE(r.feed(R"(,2]} true)").has_value());

	auto e3 = r.next();
	REQUIRE(e3.has_value());
	REQUIRE(e3->has_value());
	CHECK(**e3 == Ev::number_value);
	REQUIRE(r.number_val().to_i64().has_value());
	CHECK(*r.number_val().to_i64() == 1LL);

	auto e4 = r.next();
	REQUIRE(e4.has_value());
	REQUIRE(e4->has_value());
	CHECK(**e4 == Ev::number_value);
	REQUIRE(r.number_val().to_i64().has_value());
	CHECK(*r.number_val().to_i64() == 2LL);

	auto e5 = r.next();
	REQUIRE(e5.has_value());
	REQUIRE(e5->has_value());
	CHECK(**e5 == Ev::end_array);

	auto e6 = r.next();
	REQUIRE(e6.has_value());
	REQUIRE(e6->has_value());
	CHECK(**e6 == Ev::end_object);

	auto e7 = r.next();
	REQUIRE(e7.has_value());
	REQUIRE(e7->has_value());
	CHECK(**e7 == Ev::bool_value);
	CHECK(r.bool_val() == true);

	REQUIRE(r.close().has_value());
	auto eof = r.next();
	REQUIRE(eof.has_value());
	CHECK_FALSE(eof->has_value());
}
TEST_CASE(
	"phase7: JsonStreamReader waits for delimiter or close before terminal number",
	"[phase7]") {
	JsonStreamReader r;
	using Ev = JsonReader::Event;

	REQUIRE(r.feed("42").has_value());
	auto pending = r.next();
	REQUIRE(pending.has_value());
	CHECK_FALSE(pending->has_value());

	REQUIRE(r.close().has_value());
	auto ev = r.next();
	REQUIRE(ev.has_value());
	REQUIRE(ev->has_value());
	CHECK(**ev == Ev::number_value);
	REQUIRE(r.number_val().to_i64().has_value());
	CHECK(*r.number_val().to_i64() == 42LL);
}
TEST_CASE(
	"phase7: JsonStreamReader resumes fragmented string escape",
	"[phase7]") {
	JsonStreamReader r;
	using Ev = JsonReader::Event;

	REQUIRE(r.feed(R"("x\u00)").has_value());
	auto pending = r.next();
	REQUIRE(pending.has_value());
	CHECK_FALSE(pending->has_value());

	REQUIRE(r.feed(R"(61")").has_value());
	auto ev = r.next();
	REQUIRE(ev.has_value());
	REQUIRE(ev->has_value());
	CHECK(**ev == Ev::string_value);

	std::string decoded;
	REQUIRE(r.string_token().append_decoded_to(decoded).has_value());
	CHECK(decoded == "xa");
}
TEST_CASE(
	"phase7: JsonStreamReader close turns incomplete input into parse error",
	"[phase7]") {
	JsonStreamReader r;
	using Ev = JsonReader::Event;

	REQUIRE(r.feed("[1").has_value());
	auto begin = r.next();
	REQUIRE(begin.has_value());
	REQUIRE(begin->has_value());
	CHECK(**begin == Ev::begin_array);

	auto pending = r.next();
	REQUIRE(pending.has_value());
	CHECK_FALSE(pending->has_value());

	REQUIRE(r.close().has_value());
	auto number = r.next();
	REQUIRE(number.has_value());
	REQUIRE(number->has_value());
	CHECK(**number == Ev::number_value);

	auto err = r.next();
	REQUIRE_FALSE(err.has_value());
	CHECK(err.error().code == JsonIssueCode::syntax_error);
}

TEST_CASE(
	"phase7: NdjsonRange with parse options",
	"[phase7]") {
	JsonParseOptions opts;
	opts.max_input_size = LimitOption::bound(5);
	std::string_view ndjson = "1\n\"toolongstring\"\n2";
	NdjsonRange range{ndjson, opts};
	auto it = range.begin();
	REQUIRE(it != std::default_sentinel);
	CHECK(it->has_value());
	++it;
	REQUIRE(it != std::default_sentinel);
	CHECK_FALSE(it->has_value());
	CHECK(it->error().code == JsonIssueCode::input_too_large);
	++it;
	REQUIRE(it != std::default_sentinel);
	CHECK(it->has_value());
	++it;
	CHECK(it == std::default_sentinel);
}
