// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

struct Point {
	std::int64_t x{};
	std::int64_t y{};
};

template<>
struct conflux::json::JsonMembers<Point> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("x", &Point::x),
			conflux::json::json_member("y", &Point::y),
		};
	}
	static constexpr std::string_view type_name() { return "Point"; }
};

struct OptionalConfig {
	std::int64_t required{};
	std::optional<std::int64_t> optional{};
};

template<>
struct conflux::json::JsonMembers<OptionalConfig> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("required", &OptionalConfig::required),
			conflux::json::json_member("optional", &OptionalConfig::optional),
		};
	}
};

struct NestedOptionalUser {
	std::int64_t id{};
	std::optional<std::string> role{};
};

template<>
struct conflux::json::JsonMembers<NestedOptionalUser> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("id", &NestedOptionalUser::id),
			conflux::json::json_member("role", &NestedOptionalUser::role),
		};
	}
};

struct NestedOptionalEnvelope {
	NestedOptionalUser user{};
};

template<>
struct conflux::json::JsonMembers<NestedOptionalEnvelope> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("user", &NestedOptionalEnvelope::user),
		};
	}
};

struct UnsafeRawKey {
	std::int64_t literal_backslash_n{};
};

template<>
struct conflux::json::JsonMembers<UnsafeRawKey> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("\\n", &UnsafeRawKey::literal_backslash_n),
		};
	}
};

TEST_CASE(
	"json: JsonMembers decode plain struct",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":3,"y":7})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->x == 3LL);
	CHECK(r->y == 7LL);
}

TEST_CASE(
	"json: decode_full_into updates existing JsonMembers struct",
	"[json][codec][members]") {
	Point point{.x = 1, .y = 2};
	auto r = decode_full_into(point, R"({"x":3,"y":7})");
	REQUIRE(r.has_value());
	CHECK(point.x == 3LL);
	CHECK(point.y == 7LL);
}

TEST_CASE(
	"json: decode_full_into clears missing optionals and leaves output unchanged on failure",
	"[json][codec][members]") {
	OptionalConfig cfg{.required = 5, .optional = 42};
	auto r = decode_full_into(cfg, R"({"required":7})");
	REQUIRE(r.has_value());
	CHECK(cfg.required == 7LL);
	CHECK_FALSE(cfg.optional.has_value());

	cfg = OptionalConfig{.required = 5, .optional = 42};
	auto trailing = decode_full_into(cfg, R"({"required":7,"optional":9} true)");
	CHECK_FALSE(trailing.has_value());
	CHECK(cfg.required == 5LL);
	REQUIRE(cfg.optional.has_value());
	CHECK(*cfg.optional == 42LL);
}

TEST_CASE(
	"json: JsonMembers last_wins duplicate nested object clears missing optionals",
	"[json][codec][members][duplicates]") {
	JsonParseOptions parse_opts;
	parse_opts.duplicate_key = DuplicateKeyPolicy::last_wins;
	JsonReader reader{R"({"user":{"id":1,"role":"admin"},"user":{"id":2}})", parse_opts};

	auto decoded = decode<NestedOptionalEnvelope>(reader);
	REQUIRE(decoded.has_value());
	CHECK(decoded->user.id == 2LL);
	CHECK_FALSE(decoded->user.role.has_value());
}

TEST_CASE(
	"json: fast JsonMembers key prediction does not match escaped unsafe raw names",
	"[json][codec][members]") {
	auto r = decode_full<UnsafeRawKey>(R"({"\n":123})");
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
}

TEST_CASE(
	"json: JsonMembers decode missing required member yields missing_member",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":3})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::missing_member);
	CHECK(r.error().member_name == "y");
}

TEST_CASE(
	"json: JsonMembers decode unknown member yields invalid_value",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":1,"y":2,"z":3})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
	CHECK(r.error().member_name == "z");
}

TEST_CASE(
	"json: JsonMembers decode wrong kind for field propagates path",
	"[json][codec][members]") {
	auto doc = parse(R"({"x":"bad","y":2})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	CHECK_FALSE(r.has_value());
	auto &err = r.error();
	CHECK(err.code == JsonIssueCode::wrong_kind);
	REQUIRE(err.path.size() == 1UZ);
	CHECK(get<JsonPathMember>(err.path.segment(0)).name == "x");
}

TEST_CASE(
	"json: has_json_codec true for JsonMembers type",
	"[json][codec][members]") {
	CHECK(has_json_codec<Point>);
}

TEST_CASE(
	"json: decode with unknown_members=ignore skips extra fields",
	"[json][codec][members][phase2]") {
	auto doc = parse(R"({"x":1,"y":2,"z":3})");
	REQUIRE(doc.has_value());
	JsonDecodeOptions opts{.unknown_members = UnknownMemberPolicy::ignore};
	auto r = decode<Point>(doc->root(), opts);
	REQUIRE(r.has_value());
	CHECK(r->x == 1LL);
	CHECK(r->y == 2LL);
}

TEST_CASE(
	"json: decode with unknown_members=reject still rejects (default)",
	"[json][codec][members][phase2]") {
	auto doc = parse(R"({"x":1,"y":2,"extra":99})");
	REQUIRE(doc.has_value());
	auto r = decode<Point>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::invalid_value);
}

struct Rect {
	std::int64_t width{};
	std::int64_t height{};
};

template<>
struct conflux::json::JsonMembers<Rect> {
	static constexpr auto members() {
		return std::tuple{
			make_tuple(
				conflux::json::json_member("width", &Rect::width),
				static_cast<conflux::json::JsonConstraintFn<std::int64_t>>(
					[](std::int64_t const &v) -> std::expected<void, JsonError> {
						if (v <= 0) {
							return std::unexpected(
								JsonError{
									.stage = JsonStage::decode,
									.code = JsonIssueCode::constraint_violation,
									.message = "width must be positive"});
						}
						return {};
					})),
			conflux::json::json_member("height", &Rect::height),
		};
	}
};

TEST_CASE(
	"json: constrained member passes when valid",
	"[json][codec][members][phase2]") {
	auto doc = parse(R"({"width":10,"height":5})");
	REQUIRE(doc.has_value());
	auto r = decode<Rect>(doc->root());
	REQUIRE(r.has_value());
	CHECK(r->width == 10LL);
	CHECK(r->height == 5LL);
}

TEST_CASE(
	"json: constrained member fails on violation",
	"[json][codec][members][phase2]") {
	auto doc = parse(R"({"width":-1,"height":5})");
	REQUIRE(doc.has_value());
	auto r = decode<Rect>(doc->root());
	CHECK_FALSE(r.has_value());
	CHECK(r.error().code == JsonIssueCode::constraint_violation);
	CHECK(r.error().member_name == "width");
}

struct Inner {
	std::int64_t val{};
};

template<>
struct conflux::json::JsonMembers<Inner> {
	static constexpr auto members() { return std::tuple{conflux::json::json_member("val", &Inner::val)}; }
};

struct Outer {
	Inner inner{};
};

template<>
struct conflux::json::JsonMembers<Outer> {
	static constexpr auto members() { return std::tuple{conflux::json::json_member("inner", &Outer::inner)}; }
};

TEST_CASE(
	"json: nested struct wrong type propagates full path",
	"[json][codec][members][phase2]") {
	auto doc = parse(R"({"inner":{"val":"bad"}})");
	REQUIRE(doc.has_value());
	auto r = decode<Outer>(doc->root());
	CHECK_FALSE(r.has_value());
	auto &err = r.error();
	CHECK(err.code == JsonIssueCode::wrong_kind);
	REQUIRE(err.path.size() == 2UZ);
	CHECK(get<JsonPathMember>(err.path.segment(0)).name == "inner");
	CHECK(get<JsonPathMember>(err.path.segment(1)).name == "val");
}

TEST_CASE(
	"json: nested struct missing member has parent path",
	"[json][codec][members][phase2]") {
	auto doc = parse(R"({"inner":{}})");
	REQUIRE(doc.has_value());
	auto r = decode<Outer>(doc->root());
	CHECK_FALSE(r.has_value());
	auto &err = r.error();
	CHECK(err.code == JsonIssueCode::missing_member);
	CHECK(err.member_name == "val");
	REQUIRE(err.path.size() == 1UZ);
	CHECK(get<JsonPathMember>(err.path.segment(0)).name == "inner");
}
