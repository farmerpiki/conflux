// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

TEST_CASE(
	"json: find_member_escaped_key_literal_escape",
	"[json][escape]") {
	auto doc = parse(R"({"a\nb": 1})");
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	auto m = obj->find_member("a\nb");
	REQUIRE(m.has_value());
	auto n = m->as_number();
	REQUIRE(n.has_value());
	CHECK(*n->to_i64() == 1LL);
}

TEST_CASE(
	"json: find_member_escaped_key_null_byte",
	"[json][escape]") {
	auto doc = parse(R"({"a\u0000b": 1})");
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	std::string_view const key{"a\0b", 3};
	auto m = obj->find_member(key);
	REQUIRE(m.has_value());
	auto n = m->as_number();
	REQUIRE(n.has_value());
	CHECK(*n->to_i64() == 1LL);
}

TEST_CASE(
	"json: find_member_unicode_escaped_ascii_key",
	"[json][escape]") {
	auto doc = parse(R"({"\u0061": 1})");
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	auto m = obj->find_member("a");
	REQUIRE(m.has_value());
	auto n = m->as_number();
	REQUIRE(n.has_value());
	CHECK(*n->to_i64() == 1LL);
}

TEST_CASE(
	"json: duplicate_member_mixed_escape",
	"[json][escape]") {
	auto doc = parse(R"({"a": 1, "\u0061": 2})");
	REQUIRE_FALSE(doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::duplicate_member);
}

TEST_CASE(
	"json: hash_fallback_linear_on_escaped",
	"[json][escape][hash]") {
	std::string js = "{";
	for (int i = 0; i < 16; ++i) {
		if (i > 0) {
			js += ',';
		}
		js += std::format(R"("k{}": {})", i, i);
	}
	js += R"(, "\u0061_key": 999)";
	js += "}";
	auto doc = parse(js);
	REQUIRE(doc.has_value());
	auto warm = doc->warm_member_index(doc->root());
	REQUIRE(warm.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	auto m = obj->find_member("a_key");
	REQUIRE(m.has_value());
	auto n = m->as_number();
	REQUIRE(n.has_value());
	CHECK(*n->to_i64() == 999LL);
}

TEST_CASE(
	"json: build_probe_cap_adversarial_hash",
	"[json][hash][adversarial]") {
	constexpr std::size_t kTargetCount = 80;
	std::vector<std::string> formerly_colliding_keys;
	formerly_colliding_keys.reserve(kTargetCount);
	for (std::size_t i = 0; formerly_colliding_keys.size() < kTargetCount && i < 1000000UZ; ++i) {
		std::string s = std::format("k_{}", i);
		auto const h = static_cast<std::uint32_t>(std::hash<std::string_view>{}(std::string_view{s}));
		if ((h & 0xFFu) == 0u) {
			formerly_colliding_keys.push_back(std::move(s));
		}
	}
	if (formerly_colliding_keys.size() < kTargetCount) {
		WARN("could not synthesize enough formerly-colliding keys; skipping");
		return;
	}
	std::string js = "{";
	for (std::size_t i = 0; i < formerly_colliding_keys.size(); ++i) {
		if (i > 0) {
			js += ',';
		}
		js += std::format(R"("{}": {})", formerly_colliding_keys[i], i);
	}
	js += "}";
	auto doc = parse(js);
	REQUIRE(doc.has_value());
	auto warm = doc->warm_member_index(doc->root());
	REQUIRE(warm.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	auto m = obj->find_member(formerly_colliding_keys[42]);
	REQUIRE(m.has_value());
	auto n = m->as_number();
	REQUIRE(n.has_value());
	CHECK(*n->to_i64() == 42LL);
}

TEST_CASE(
	"json: duplicate-key detection after hash promotion",
	"[json][hash]") {
	JsonParseOptions opts;
	opts.duplicate_key = DuplicateKeyPolicy::reject;
	std::string js = "{";
	for (int i = 0; i < 10; ++i) {
		if (i > 0) {
			js += ',';
		}
		js += std::format(R"("k{}": {})", i, i);
	}
	js += R"(, "k3": 99})";
	auto doc = parse(js, opts);
	REQUIRE_FALSE(doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::duplicate_member);
}

TEST_CASE(
	"json: first_wins skips duplicate value materialization",
	"[json][perf]") {
	JsonParseOptions opts;
	opts.duplicate_key = DuplicateKeyPolicy::first_wins;
	std::string js = R"({"k":"first","k":")";
	for (std::size_t i = 0; i < 4096; ++i) {
		js += R"(\n)";
	}
	js += R"(","z":1})";
	auto doc = parse(js, opts);
	REQUIRE(doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	REQUIRE(obj->find_member("k").has_value());
	CHECK(*obj->member("k")->as_string() == "first");
	REQUIRE(obj->find_member("z").has_value());
	auto stats = doc->parse_storage_stats();
	CHECK(stats.duplicate_member_hits == 1);
	CHECK(stats.first_wins_rollbacks == 1);
	CHECK(stats.string_arena_reserve_bytes == 0);
}

TEST_CASE(
	"json: first_wins skipped duplicate value still validates syntax",
	"[json][perf]") {
	JsonParseOptions opts;
	opts.duplicate_key = DuplicateKeyPolicy::first_wins;
	auto doc = parse(R"({"k":1,"k":[1,]})", opts);
	REQUIRE_FALSE(doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::syntax_error);
}

TEST_CASE(
	"json: parse storage stats report arena reserve and duplicate hash activity",
	"[json][perf]") {
	JsonParseOptions opts;
	opts.duplicate_key = DuplicateKeyPolicy::first_wins;
	std::string js = "{";
	for (int i = 0; i < 10; ++i) {
		if (i > 0) {
			js += ',';
		}
		js += std::format(R"("k{}": {})", i, i);
	}
	js += R"(, "k3": 99})";
	auto doc = parse(js, opts);
	REQUIRE(doc.has_value());
	auto stats = doc->parse_storage_stats();
	CHECK(stats.input_bytes == js.size());
	CHECK(stats.string_arena_reserve_bytes == 0);
	CHECK(stats.string_arena_capacity >= stats.string_arena_size);
	CHECK(stats.duplicate_hash_promotions == 1);
	CHECK(stats.duplicate_hash_inserts >= 10);
	CHECK(stats.duplicate_member_hits == 1);
	CHECK(stats.first_wins_rollbacks == 1);
	CHECK(stats.last_wins_updates == 0);
}
