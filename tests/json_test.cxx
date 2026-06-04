// Plain TU — not a module unit.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.json;
import conflux.json.native_provider;

using namespace conflux::json;

namespace {

struct CountingResource : std::pmr::memory_resource {
	std::pmr::memory_resource *upstream{std::pmr::new_delete_resource()};
	std::size_t alloc_count{0};
	std::size_t dealloc_count{0};
	std::size_t alloc_bytes{0};

private:
	void *do_allocate(
		std::size_t bytes,
		std::size_t align) override {
		++alloc_count;
		alloc_bytes += bytes;
		return upstream->allocate(bytes, align);
	}
	void do_deallocate(
		void *p,
		std::size_t bytes,
		std::size_t align) override {
		++dealloc_count;
		upstream->deallocate(p, bytes, align);
	}
	bool do_is_equal(
		std::pmr::memory_resource const &other) const noexcept override {
		return this == &other;
	}
};

struct DefaultPmrResourceGuard {
	std::pmr::memory_resource *previous{};

	explicit DefaultPmrResourceGuard(
		std::pmr::memory_resource *next)
		: previous{std::pmr::set_default_resource(next)} {}
	~DefaultPmrResourceGuard() { std::pmr::set_default_resource(previous); }
	DefaultPmrResourceGuard(DefaultPmrResourceGuard const &) = delete;
	DefaultPmrResourceGuard &operator =(DefaultPmrResourceGuard const &) = delete;
};

} // namespace

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

// ---------------------------------------------------------------------------
// Phase 4 — JsonReader pull parser
// ---------------------------------------------------------------------------

// Types for phase4 tests.

struct P4Person {
	std::string name{};
	std::int64_t age{};
};
template<>
struct conflux::json::JsonMembers<P4Person> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("name", &P4Person::name),
			conflux::json::json_member("age", &P4Person::age),
		};
	}
};

struct P4FastStringShape {
	std::string alpha{};
	std::vector<std::string> items{};
	std::int64_t count{};
};
template<>
struct conflux::json::JsonMembers<P4FastStringShape> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("alpha", &P4FastStringShape::alpha),
			conflux::json::json_member("items", &P4FastStringShape::items),
			conflux::json::json_member("count", &P4FastStringShape::count),
		};
	}
};
struct P4Address {
	std::string street{};
	std::optional<std::string> city{};
};
template<>
struct conflux::json::JsonMembers<P4Address> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("street", &P4Address::street),
			conflux::json::json_member("city", &P4Address::city),
		};
	}
};
struct P4Nested {
	P4Person person{};
	std::int64_t score{};
};
template<>
struct conflux::json::JsonMembers<P4Nested> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("person", &P4Nested::person),
			conflux::json::json_member("score", &P4Nested::score),
		};
	}
};
struct P4OrderedFastObject {
	std::int64_t a{};
	std::int64_t b{};
	std::int64_t c{};
	std::vector<double> values{};
	P4Nested nested{};
	bool flag{};
};
template<>
struct conflux::json::JsonMembers<P4OrderedFastObject> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("a", &P4OrderedFastObject::a),
			conflux::json::json_member("b", &P4OrderedFastObject::b),
			conflux::json::json_member("c", &P4OrderedFastObject::c),
			conflux::json::json_member("values", &P4OrderedFastObject::values),
			conflux::json::json_member("nested", &P4OrderedFastObject::nested),
			conflux::json::json_member("flag", &P4OrderedFastObject::flag),
		};
	}
};

struct P4LongKey {
	std::int64_t value{};
};
[[nodiscard]] std::string const &p4_long_key_name() {
	static std::string const key(260, 'a');
	return key;
}
template<>
struct conflux::json::JsonMembers<P4LongKey> {
	static auto members() {
		return std::tuple{
			conflux::json::json_member(std::string_view{p4_long_key_name()}, &P4LongKey::value),
		};
	}
};
struct P4BorrowedName {
	std::string_view name{};
};
template<>
struct conflux::json::JsonMembers<P4BorrowedName> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("name", &P4BorrowedName::name),
		};
	}
};
static_assert(json_contains_borrowed_view_v<P4BorrowedName>);

struct P4PmrPayload {
	std::pmr::string name{};
	std::pmr::vector<std::int64_t> scores{};
};
template<>
struct conflux::json::JsonMembers<P4PmrPayload> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("name", &P4PmrPayload::name),
			conflux::json::json_member("scores", &P4PmrPayload::scores),
		};
	}
};

struct P4DirectFieldRemainder {
	std::optional<std::int64_t> maybe{};
	std::optional<Nullable<std::int64_t>> opt_nullable{};
	Nullable<std::string> nullable_string{};
	std::map<std::string, std::int64_t> object{};
	std::array<std::int64_t, 3> fixed{};
	std::pair<std::int64_t, std::string> pair{};
	std::tuple<std::int64_t, bool, std::string> tuple{};
};
template<>
struct conflux::json::JsonMembers<P4DirectFieldRemainder> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("maybe", &P4DirectFieldRemainder::maybe),
			conflux::json::json_member("opt_nullable", &P4DirectFieldRemainder::opt_nullable),
			conflux::json::json_member("nullable_string", &P4DirectFieldRemainder::nullable_string),
			conflux::json::json_member("object", &P4DirectFieldRemainder::object),
			conflux::json::json_member("fixed", &P4DirectFieldRemainder::fixed),
			conflux::json::json_member("pair", &P4DirectFieldRemainder::pair),
			conflux::json::json_member("tuple", &P4DirectFieldRemainder::tuple),
		};
	}
};

struct P4WideInner {
	std::int64_t f0{};
	std::int64_t f1{};
	std::int64_t f2{};
	std::int64_t f3{};
	std::int64_t f4{};
	std::int64_t f5{};
	std::int64_t f6{};
	std::int64_t f7{};
	std::int64_t f8{};
	std::int64_t f9{};
	std::int64_t f10{};
	std::int64_t f11{};
	std::int64_t f12{};
	std::int64_t f13{};
	std::int64_t f14{};
	std::int64_t f15{};
	std::int64_t f16{};
	std::int64_t f17{};
	std::int64_t f18{};
	std::int64_t f19{};
	std::int64_t f20{};
	std::int64_t f21{};
	std::int64_t f22{};
	std::int64_t f23{};
	std::int64_t f24{};
	std::int64_t f25{};
	std::int64_t f26{};
	std::int64_t f27{};
	std::int64_t f28{};
	std::int64_t f29{};
	std::int64_t f30{};
	std::int64_t f31{};
	std::int64_t f32{};
	std::int64_t f33{};
	std::int64_t f34{};
	std::int64_t f35{};
	std::int64_t f36{};
	std::int64_t f37{};
	std::int64_t f38{};
	std::int64_t f39{};
	std::int64_t f40{};
	std::int64_t f41{};
	std::int64_t f42{};
	std::int64_t f43{};
	std::int64_t f44{};
	std::int64_t f45{};
	std::int64_t f46{};
	std::int64_t f47{};
	std::int64_t f48{};
	std::int64_t f49{};
	std::int64_t f50{};
	std::int64_t f51{};
	std::int64_t f52{};
	std::int64_t f53{};
	std::int64_t f54{};
	std::int64_t f55{};
	std::int64_t f56{};
	std::int64_t f57{};
	std::int64_t f58{};
	std::int64_t f59{};
	std::int64_t f60{};
	std::int64_t f61{};
	std::int64_t f62{};
	std::int64_t f63{};
	std::int64_t f64{};
};
template<>
struct conflux::json::JsonMembers<P4WideInner> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("f0", &P4WideInner::f0),   conflux::json::json_member("f1", &P4WideInner::f1),
			conflux::json::json_member("f2", &P4WideInner::f2),   conflux::json::json_member("f3", &P4WideInner::f3),
			conflux::json::json_member("f4", &P4WideInner::f4),   conflux::json::json_member("f5", &P4WideInner::f5),
			conflux::json::json_member("f6", &P4WideInner::f6),   conflux::json::json_member("f7", &P4WideInner::f7),
			conflux::json::json_member("f8", &P4WideInner::f8),   conflux::json::json_member("f9", &P4WideInner::f9),
			conflux::json::json_member("f10", &P4WideInner::f10), conflux::json::json_member("f11", &P4WideInner::f11),
			conflux::json::json_member("f12", &P4WideInner::f12), conflux::json::json_member("f13", &P4WideInner::f13),
			conflux::json::json_member("f14", &P4WideInner::f14), conflux::json::json_member("f15", &P4WideInner::f15),
			conflux::json::json_member("f16", &P4WideInner::f16), conflux::json::json_member("f17", &P4WideInner::f17),
			conflux::json::json_member("f18", &P4WideInner::f18), conflux::json::json_member("f19", &P4WideInner::f19),
			conflux::json::json_member("f20", &P4WideInner::f20), conflux::json::json_member("f21", &P4WideInner::f21),
			conflux::json::json_member("f22", &P4WideInner::f22), conflux::json::json_member("f23", &P4WideInner::f23),
			conflux::json::json_member("f24", &P4WideInner::f24), conflux::json::json_member("f25", &P4WideInner::f25),
			conflux::json::json_member("f26", &P4WideInner::f26), conflux::json::json_member("f27", &P4WideInner::f27),
			conflux::json::json_member("f28", &P4WideInner::f28), conflux::json::json_member("f29", &P4WideInner::f29),
			conflux::json::json_member("f30", &P4WideInner::f30), conflux::json::json_member("f31", &P4WideInner::f31),
			conflux::json::json_member("f32", &P4WideInner::f32), conflux::json::json_member("f33", &P4WideInner::f33),
			conflux::json::json_member("f34", &P4WideInner::f34), conflux::json::json_member("f35", &P4WideInner::f35),
			conflux::json::json_member("f36", &P4WideInner::f36), conflux::json::json_member("f37", &P4WideInner::f37),
			conflux::json::json_member("f38", &P4WideInner::f38), conflux::json::json_member("f39", &P4WideInner::f39),
			conflux::json::json_member("f40", &P4WideInner::f40), conflux::json::json_member("f41", &P4WideInner::f41),
			conflux::json::json_member("f42", &P4WideInner::f42), conflux::json::json_member("f43", &P4WideInner::f43),
			conflux::json::json_member("f44", &P4WideInner::f44), conflux::json::json_member("f45", &P4WideInner::f45),
			conflux::json::json_member("f46", &P4WideInner::f46), conflux::json::json_member("f47", &P4WideInner::f47),
			conflux::json::json_member("f48", &P4WideInner::f48), conflux::json::json_member("f49", &P4WideInner::f49),
			conflux::json::json_member("f50", &P4WideInner::f50), conflux::json::json_member("f51", &P4WideInner::f51),
			conflux::json::json_member("f52", &P4WideInner::f52), conflux::json::json_member("f53", &P4WideInner::f53),
			conflux::json::json_member("f54", &P4WideInner::f54), conflux::json::json_member("f55", &P4WideInner::f55),
			conflux::json::json_member("f56", &P4WideInner::f56), conflux::json::json_member("f57", &P4WideInner::f57),
			conflux::json::json_member("f58", &P4WideInner::f58), conflux::json::json_member("f59", &P4WideInner::f59),
			conflux::json::json_member("f60", &P4WideInner::f60), conflux::json::json_member("f61", &P4WideInner::f61),
			conflux::json::json_member("f62", &P4WideInner::f62), conflux::json::json_member("f63", &P4WideInner::f63),
			conflux::json::json_member("f64", &P4WideInner::f64),
		};
	}
};
struct P4WideOuter {
	std::int64_t f0{};
	std::int64_t f1{};
	std::int64_t f2{};
	std::int64_t f3{};
	std::int64_t f4{};
	std::int64_t f5{};
	std::int64_t f6{};
	std::int64_t f7{};
	std::int64_t f8{};
	std::int64_t f9{};
	std::int64_t f10{};
	std::int64_t f11{};
	std::int64_t f12{};
	std::int64_t f13{};
	std::int64_t f14{};
	std::int64_t f15{};
	std::int64_t f16{};
	std::int64_t f17{};
	std::int64_t f18{};
	std::int64_t f19{};
	std::int64_t f20{};
	std::int64_t f21{};
	std::int64_t f22{};
	std::int64_t f23{};
	std::int64_t f24{};
	std::int64_t f25{};
	std::int64_t f26{};
	std::int64_t f27{};
	std::int64_t f28{};
	std::int64_t f29{};
	std::int64_t f30{};
	std::int64_t f31{};
	std::int64_t f32{};
	std::int64_t f33{};
	std::int64_t f34{};
	std::int64_t f35{};
	std::int64_t f36{};
	std::int64_t f37{};
	std::int64_t f38{};
	std::int64_t f39{};
	std::int64_t f40{};
	std::int64_t f41{};
	std::int64_t f42{};
	std::int64_t f43{};
	std::int64_t f44{};
	std::int64_t f45{};
	std::int64_t f46{};
	std::int64_t f47{};
	std::int64_t f48{};
	std::int64_t f49{};
	std::int64_t f50{};
	std::int64_t f51{};
	std::int64_t f52{};
	std::int64_t f53{};
	std::int64_t f54{};
	std::int64_t f55{};
	std::int64_t f56{};
	std::int64_t f57{};
	std::int64_t f58{};
	std::int64_t f59{};
	std::int64_t f60{};
	std::int64_t f61{};
	std::int64_t f62{};
	std::int64_t f63{};
	std::int64_t f64{};
	P4WideInner inner{};
	std::int64_t tail{};
};
template<>
struct conflux::json::JsonMembers<P4WideOuter> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("f0", &P4WideOuter::f0),
			conflux::json::json_member("f1", &P4WideOuter::f1),
			conflux::json::json_member("f2", &P4WideOuter::f2),
			conflux::json::json_member("f3", &P4WideOuter::f3),
			conflux::json::json_member("f4", &P4WideOuter::f4),
			conflux::json::json_member("f5", &P4WideOuter::f5),
			conflux::json::json_member("f6", &P4WideOuter::f6),
			conflux::json::json_member("f7", &P4WideOuter::f7),
			conflux::json::json_member("f8", &P4WideOuter::f8),
			conflux::json::json_member("f9", &P4WideOuter::f9),
			conflux::json::json_member("f10", &P4WideOuter::f10),
			conflux::json::json_member("f11", &P4WideOuter::f11),
			conflux::json::json_member("f12", &P4WideOuter::f12),
			conflux::json::json_member("f13", &P4WideOuter::f13),
			conflux::json::json_member("f14", &P4WideOuter::f14),
			conflux::json::json_member("f15", &P4WideOuter::f15),
			conflux::json::json_member("f16", &P4WideOuter::f16),
			conflux::json::json_member("f17", &P4WideOuter::f17),
			conflux::json::json_member("f18", &P4WideOuter::f18),
			conflux::json::json_member("f19", &P4WideOuter::f19),
			conflux::json::json_member("f20", &P4WideOuter::f20),
			conflux::json::json_member("f21", &P4WideOuter::f21),
			conflux::json::json_member("f22", &P4WideOuter::f22),
			conflux::json::json_member("f23", &P4WideOuter::f23),
			conflux::json::json_member("f24", &P4WideOuter::f24),
			conflux::json::json_member("f25", &P4WideOuter::f25),
			conflux::json::json_member("f26", &P4WideOuter::f26),
			conflux::json::json_member("f27", &P4WideOuter::f27),
			conflux::json::json_member("f28", &P4WideOuter::f28),
			conflux::json::json_member("f29", &P4WideOuter::f29),
			conflux::json::json_member("f30", &P4WideOuter::f30),
			conflux::json::json_member("f31", &P4WideOuter::f31),
			conflux::json::json_member("f32", &P4WideOuter::f32),
			conflux::json::json_member("f33", &P4WideOuter::f33),
			conflux::json::json_member("f34", &P4WideOuter::f34),
			conflux::json::json_member("f35", &P4WideOuter::f35),
			conflux::json::json_member("f36", &P4WideOuter::f36),
			conflux::json::json_member("f37", &P4WideOuter::f37),
			conflux::json::json_member("f38", &P4WideOuter::f38),
			conflux::json::json_member("f39", &P4WideOuter::f39),
			conflux::json::json_member("f40", &P4WideOuter::f40),
			conflux::json::json_member("f41", &P4WideOuter::f41),
			conflux::json::json_member("f42", &P4WideOuter::f42),
			conflux::json::json_member("f43", &P4WideOuter::f43),
			conflux::json::json_member("f44", &P4WideOuter::f44),
			conflux::json::json_member("f45", &P4WideOuter::f45),
			conflux::json::json_member("f46", &P4WideOuter::f46),
			conflux::json::json_member("f47", &P4WideOuter::f47),
			conflux::json::json_member("f48", &P4WideOuter::f48),
			conflux::json::json_member("f49", &P4WideOuter::f49),
			conflux::json::json_member("f50", &P4WideOuter::f50),
			conflux::json::json_member("f51", &P4WideOuter::f51),
			conflux::json::json_member("f52", &P4WideOuter::f52),
			conflux::json::json_member("f53", &P4WideOuter::f53),
			conflux::json::json_member("f54", &P4WideOuter::f54),
			conflux::json::json_member("f55", &P4WideOuter::f55),
			conflux::json::json_member("f56", &P4WideOuter::f56),
			conflux::json::json_member("f57", &P4WideOuter::f57),
			conflux::json::json_member("f58", &P4WideOuter::f58),
			conflux::json::json_member("f59", &P4WideOuter::f59),
			conflux::json::json_member("f60", &P4WideOuter::f60),
			conflux::json::json_member("f61", &P4WideOuter::f61),
			conflux::json::json_member("f62", &P4WideOuter::f62),
			conflux::json::json_member("f63", &P4WideOuter::f63),
			conflux::json::json_member("f64", &P4WideOuter::f64),
			conflux::json::json_member("inner", &P4WideOuter::inner),
			conflux::json::json_member("tail", &P4WideOuter::tail),
		};
	}
};
TEST_CASE(
	"phase4: decode<P4Person>(JsonReader&) basic struct",
	"[phase4]") {
	JsonReader r{R"({"name":"Alice","age":30})"};
	auto p = decode<P4Person>(r);
	REQUIRE(p.has_value());
	CHECK(p->name == "Alice");
	CHECK(p->age == 30LL);
}
TEST_CASE(
	"phase4: decode<JsonReader&> rejects trailing top-level value",
	"[phase4]") {
	JsonReader r{R"({"name":"Alice","age":30} false)"};
	auto p = decode<P4Person>(r);
	CHECK_FALSE(p.has_value());
	CHECK(p.error().code == JsonIssueCode::trailing_garbage);
}
TEST_CASE(
	"phase4: decode_next<JsonReader&> permits streaming top-level values",
	"[phase4]") {
	JsonReader r{R"({"name":"Alice","age":30} false)"};
	auto p = decode_next<P4Person>(r);
	REQUIRE(p.has_value());
	CHECK(p->name == "Alice");
	CHECK(p->age == 30LL);

	auto b = decode_next<bool>(r);
	REQUIRE(b.has_value());
	CHECK(*b == false);
}
TEST_CASE(
	"phase4: decode_full<string_view> requires a single complete input",
	"[phase4]") {
	auto v = decode_full<std::int64_t>("42 43");
	CHECK_FALSE(v.has_value());
	CHECK(v.error().code == JsonIssueCode::trailing_garbage);

	auto ok = decode_full<std::int64_t>("42");
	REQUIRE(ok.has_value());
	CHECK(*ok == 42LL);
}
TEST_CASE(
	"phase4: JsonReader direct object string fast path preserves escaped values",
	"[phase4][perf]") {
	auto value = decode_full<P4FastStringShape>(R"({"alpha":"plain","items":["one","t\\wo","three"],"count":3})");
	REQUIRE(value.has_value());
	CHECK(value->alpha == "plain");
	REQUIRE(value->items.size() == 3UZ);
	CHECK(value->items[0] == "one");
	CHECK(value->items[1] == R"(t\wo)");
	CHECK(value->items[2] == "three");
	CHECK(value->count == 3LL);
}

TEST_CASE(
	"phase4: ordered direct object key fast path preserves fallback cases",
	"[phase4][perf]") {
	auto ordered = decode_full<P4OrderedFastObject>(
		R"({"a":1,"b":2,"c":3,"values":[1.25,2.5],"nested":{"person":{"name":"Ada","age":37},"score":9},"flag":true})");
	REQUIRE(ordered.has_value());
	CHECK(ordered->a == 1LL);
	CHECK(ordered->b == 2LL);
	CHECK(ordered->c == 3LL);
	REQUIRE(ordered->values.size() == 2UZ);
	CHECK(ordered->values[1] == 2.5);
	CHECK(ordered->nested.person.name == "Ada");
	CHECK(ordered->nested.score == 9LL);
	CHECK(ordered->flag);

	auto escaped_key = decode_full<P4OrderedFastObject>(
		R"({"a":1,"b":2,"\u0063":3,"values":[1.25,2.5],"nested":{"person":{"name":"Ada","age":37},"score":9},"flag":true})");
	REQUIRE(escaped_key.has_value());
	CHECK(escaped_key->c == 3LL);

	auto out_of_order = decode_full<P4OrderedFastObject>(
		R"({"flag":true,"nested":{"score":9,"person":{"age":37,"name":"Ada"}},"values":[1.25,2.5],"c":3,"b":2,"a":1})");
	REQUIRE(out_of_order.has_value());
	CHECK(out_of_order->a == 1LL);
	CHECK(out_of_order->nested.person.age == 37LL);
}

TEST_CASE(
	"phase4: JsonReader numeric array fast path accepts strict whitespace",
	"[phase4][perf]") {
	auto values = decode_full<std::vector<double>>("[ 1.25 ,\n -2.5 , 3e2 ]");
	REQUIRE(values.has_value());
	REQUIRE(values->size() == 3UZ);
	CHECK((*values)[0] == 1.25);
	CHECK((*values)[1] == -2.5);
	CHECK((*values)[2] == 300.0);
}

TEST_CASE(
	"phase4: JsonReader numeric array fallback preserves JSON5 trailing comma",
	"[phase4][perf]") {
	JsonParseOptions parse_opts;
	parse_opts.mode = ParseMode::json5;
	auto values = decode_full<std::vector<int>>("[1,2,]", parse_opts);
	REQUIRE(values.has_value());
	REQUIRE(values->size() == 2UZ);
	CHECK((*values)[0] == 1);
	CHECK((*values)[1] == 2);
}

TEST_CASE(
	"phase4: decode<P4Person>(JsonReader&) unknown_members=ignore",
	"[phase4]") {
	JsonReader r{R"({"name":"Bob","age":25,"extra":true})"};
	JsonDecodeOptions opts;
	opts.unknown_members = UnknownMemberPolicy::ignore;
	auto p = decode<P4Person>(r, opts);
	REQUIRE(p.has_value());
	CHECK(p->name == "Bob");
	CHECK(p->age == 25LL);
}
TEST_CASE(
	"phase4: JsonReader typed decode rejects duplicate known member by default",
	"[phase4][duplicates]") {
	JsonReader r{R"({"name":"Ann","name":"Beth","age":30})"};
	auto p = decode<P4Person>(r);
	CHECK_FALSE(p.has_value());
	CHECK(p.error().code == JsonIssueCode::duplicate_member);
	CHECK(p.error().member_name == "name");
}
TEST_CASE(
	"phase4: JsonReader typed decode honors duplicate last_wins",
	"[phase4][duplicates]") {
	JsonParseOptions parse_opts;
	parse_opts.duplicate_key = DuplicateKeyPolicy::last_wins;
	JsonReader r{R"({"name":"Ann","name":"Beth","age":30})", parse_opts};
	auto p = decode<P4Person>(r);
	REQUIRE(p.has_value());
	CHECK(p->name == "Beth");
	CHECK(p->age == 30LL);
}
TEST_CASE(
	"phase4: JsonReader typed decode honors duplicate first_wins",
	"[phase4][duplicates]") {
	JsonParseOptions parse_opts;
	parse_opts.duplicate_key = DuplicateKeyPolicy::first_wins;
	JsonReader r{R"({"name":"Ann","name":"Beth","age":30})", parse_opts};
	auto p = decode<P4Person>(r);
	REQUIRE(p.has_value());
	CHECK(p->name == "Ann");
	CHECK(p->age == 30LL);
}
TEST_CASE(
	"phase4: JsonReader duplicate first_wins still validates skipped value syntax",
	"[phase4][duplicates]") {
	JsonParseOptions parse_opts;
	parse_opts.duplicate_key = DuplicateKeyPolicy::first_wins;
	std::string input = R"({"name":"Ann","name":"bad)";
	input.push_back('\x01');
	input += R"(","age":30})";
	JsonReader r{input, parse_opts};
	auto p = decode<P4Person>(r);
	CHECK_FALSE(p.has_value());
	CHECK(p.error().code == JsonIssueCode::syntax_error);
}
TEST_CASE(
	"phase4: JsonReader unknown duplicate under ignore remains streaming-allowed",
	"[phase4][duplicates]") {
	JsonReader r{R"({"name":"Ann","extra":1,"extra":2,"age":30})"};
	JsonDecodeOptions opts;
	opts.unknown_members = UnknownMemberPolicy::ignore;
	auto p = decode<P4Person>(r, opts);
	REQUIRE(p.has_value());
	CHECK(p->name == "Ann");
	CHECK(p->age == 30LL);
}
TEST_CASE(
	"phase4: JsonMembers direct-to-field decodes escaped string fields",
	"[phase4][direct]") {
	JsonReader r{R"({"name":"A\nB","age":30})"};
	auto p = decode<P4Person>(r);
	REQUIRE(p.has_value());
	CHECK(p->name == "A\nB");
}
TEST_CASE(
	"phase4: JsonMembers direct-to-field covers optional nullable map and tuple fields",
	"[phase4][direct]") {
	JsonReader r{
		R"({"maybe":null,"opt_nullable":null,"nullable_string":"value","object":{"a":1,"b":2},"fixed":[3,4,5],"pair":[6,"six"],"tuple":[7,true,"seven"]})"};
	auto p = decode<P4DirectFieldRemainder>(r);
	REQUIRE(p.has_value());
	CHECK_FALSE(p->maybe.has_value());
	REQUIRE(p->opt_nullable.has_value());
	CHECK(p->opt_nullable->is_null());
	REQUIRE(p->nullable_string.has_value());
	CHECK(*p->nullable_string == "value");
	REQUIRE(p->object.size() == 2UZ);
	CHECK(p->object["a"] == 1LL);
	CHECK(p->object["b"] == 2LL);
	CHECK(p->fixed[0] == 3LL);
	CHECK(p->fixed[1] == 4LL);
	CHECK(p->fixed[2] == 5LL);
	CHECK(p->pair.first == 6LL);
	CHECK(p->pair.second == "six");
	CHECK(get<0>(p->tuple) == 7LL);
	CHECK(get<1>(p->tuple));
	CHECK(get<2>(p->tuple) == "seven");
}
TEST_CASE(
	"phase7: JsonMembers reader decode supports pmr string and vector fields",
	"[phase7][json][pmr]") {
	CountingResource resource;
	DefaultPmrResourceGuard guard{&resource};
	std::string input{R"({"name":")"};
	input += std::string(128, 'x');
	input += R"(","scores":[1,2,3,4,5,6,7,8,9,10]})";
	JsonReader r{input};
	auto payload = decode<P4PmrPayload>(r);
	REQUIRE(payload.has_value());
	CHECK(std::string_view{payload->name} == std::string(128, 'x'));
	REQUIRE(payload->scores.size() == 10UZ);
	CHECK(payload->scores[9] == 10);
	CHECK(payload->name.get_allocator().resource() == &resource);
	CHECK(payload->scores.get_allocator().resource() == &resource);
	CHECK(resource.alloc_count > 0);
}
TEST_CASE(
	"phase7: DOM decode supports allocator-aware string and vector codecs",
	"[phase7][json][pmr]") {
	CountingResource resource;
	DefaultPmrResourceGuard guard{&resource};
	auto doc = parse(R"({"name":"dom-pmr","scores":[11,12,13]})");
	REQUIRE(doc.has_value());
	auto payload = decode<P4PmrPayload>(doc->root());
	REQUIRE(payload.has_value());
	CHECK(payload->name == "dom-pmr");
	REQUIRE(payload->scores.size() == 3UZ);
	CHECK(payload->scores[2] == 13);
	CHECK(payload->name.get_allocator().resource() == &resource);
	CHECK(payload->scores.get_allocator().resource() == &resource);
}
TEST_CASE(
	"phase4: recursive wide JsonMembers presence bits are object local",
	"[phase4][direct]") {
	auto make_object = [](std::string_view prefix, std::int64_t base) {
		std::string out{"{"};
		for (int i = 0; i < 65; ++i) {
			if (i != 0) {
				out += ',';
			}
			out += '"';
			out += prefix;
			out += std::to_string(i);
			out += "\":";
			out += std::to_string(base + i);
		}
		out += '}';
		return out;
	};
	std::string input{"{"};
	for (int i = 0; i < 65; ++i) {
		if (i != 0) {
			input += ',';
		}
		input += "\"f";
		input += std::to_string(i);
		input += "\":";
		input += std::to_string(i);
	}
	input += R"(,"inner":)";
	input += make_object("f", 1000);
	input += R"(,"tail":999})";
	JsonReader r{input};
	auto decoded = decode<P4WideOuter>(r);
	REQUIRE(decoded.has_value());
	CHECK(decoded->f64 == 64LL);
	CHECK(decoded->inner.f64 == 1064LL);
	CHECK(decoded->tail == 999LL);
}
TEST_CASE(
	"phase4: recursive wide JsonMembers missing field remains accurate after nested decode",
	"[phase4][direct]") {
	std::string input{"{"};
	for (int i = 0; i < 65; ++i) {
		if (i != 0) {
			input += ',';
		}
		input += "\"f";
		input += std::to_string(i);
		input += "\":";
		input += std::to_string(i);
	}
	input += R"(,"inner":{)";
	for (int i = 0; i < 65; ++i) {
		if (i != 0) {
			input += ',';
		}
		input += "\"f";
		input += std::to_string(i);
		input += "\":";
		input += std::to_string(1000 + i);
	}
	input += "}}";
	JsonReader r{input};
	auto decoded = decode<P4WideOuter>(r);
	CHECK_FALSE(decoded.has_value());
	CHECK(decoded.error().code == JsonIssueCode::missing_member);
	CHECK(decoded.error().member_name == "tail");
}
TEST_CASE(
	"phase8: wide JsonMembers lookup honors duplicate policy",
	"[phase8][json][direct][wide]") {
	auto make_input = [](std::int64_t duplicate_value) {
		std::string input{"{"};
		for (int i = 0; i < 65; ++i) {
			if (i != 0) {
				input += ',';
			}
			input += "\"f";
			input += std::to_string(i);
			input += "\":";
			input += std::to_string(i);
		}
		input += ",\"f40\":";
		input += std::to_string(duplicate_value);
		input += R"(,"inner":{)";
		for (int i = 0; i < 65; ++i) {
			if (i != 0) {
				input += ',';
			}
			input += "\"f";
			input += std::to_string(i);
			input += "\":";
			input += std::to_string(1000 + i);
		}
		input += R"(},"tail":999})";
		return input;
	};
	JsonParseOptions last_opts;
	last_opts.duplicate_key = DuplicateKeyPolicy::last_wins;
	auto last_input = make_input(777);
	JsonReader last_reader{last_input, last_opts};
	auto last = decode<P4WideOuter>(last_reader);
	REQUIRE(last.has_value());
	CHECK(last->f40 == 777LL);
	CHECK(last->tail == 999LL);

	JsonParseOptions first_opts;
	first_opts.duplicate_key = DuplicateKeyPolicy::first_wins;
	auto first_input = make_input(888);
	JsonReader first_reader{first_input, first_opts};
	auto first = decode<P4WideOuter>(first_reader);
	REQUIRE(first.has_value());
	CHECK(first->f40 == 40LL);
	CHECK(first->tail == 999LL);
}

TEST_CASE(
	"json boundary: copy_input rejects borrowed-view JsonMembers fields",
	"[json][boundary][lifetime]") {
	auto decoded = boundary::NativeJsonProvider::decode_json<P4BorrowedName>(R"({"name":"Ann"})", {.copy_input = true});
	CHECK_FALSE(decoded.has_value());
	CHECK(decoded.error().code == boundary::ErrorCode::invalid_value);
}
TEST_CASE(
	"json: DOM JsonMembers decode uses inline path frames on success",
	"[json][dom][perf]") {
	auto doc = parse(R"({"x":3,"y":7})");
	REQUIRE(doc.has_value());
	CountingResource resource;
	DefaultPmrResourceGuard guard{&resource};
	auto point = decode<Point>(doc->root());
	REQUIRE(point.has_value());
	CHECK(point->x == 3LL);
	CHECK(point->y == 7LL);
	CHECK(resource.alloc_count == 0UZ);
}

TEST_CASE(
	"phase4: decode_direct<JsonMembers> uses caller scratch for key decode",
	"[phase4][direct]") {
	CountingResource resource;
	JsonDecodeScratch scratch;
	scratch.reset_resource(&resource);
	JsonReader r{R"({"x":3,"y":7})"};
	auto p = decode_direct<Point>(r, {}, &scratch);
	REQUIRE(p.has_value());
	CHECK(p->x == 3LL);
	CHECK(p->y == 7LL);
	CHECK(resource.alloc_count == 0UZ);
}
TEST_CASE(
	"phase4: decode_direct<JsonMembers> handles escaped keys through scratch",
	"[phase4][direct]") {
	CountingResource resource;
	JsonDecodeScratch scratch;
	scratch.reset_resource(&resource);
	JsonReader r{R"({"\u0078":3,"y":7})"};
	auto p = decode_direct<Point>(r, {}, &scratch);
	REQUIRE(p.has_value());
	CHECK(p->x == 3LL);
	CHECK(p->y == 7LL);
	CHECK(resource.alloc_count == 0UZ);
}

TEST_CASE(
	"json: JsonReader counts arrays without consuming them",
	"[json][reader][perf]") {
	JsonReader r{R"([1,[2,3],{"x":4},5])"};
	auto first = r.next();
	REQUIRE(first.has_value());
	REQUIRE(first->has_value());
	REQUIRE(**first == JsonReader::Event::begin_array);
	auto count = r.count_remaining_array_elements();
	REQUIRE(count.has_value());
	CHECK(*count == 4UZ);
	auto value = r.next();
	REQUIRE(value.has_value());
	REQUIRE(value->has_value());
	CHECK(**value == JsonReader::Event::number_value);
	auto n = r.number_val().to_i64();
	REQUIRE(n.has_value());
	CHECK(*n == 1LL);
}

TEST_CASE(
	"json: vector reader decode reserves counted array size",
	"[json][reader][perf]") {
	CountingResource resource;
	DefaultPmrResourceGuard guard{&resource};
	JsonReader r{R"([1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16])"};
	auto values = decode<std::pmr::vector<std::int64_t>>(r);
	REQUIRE(values.has_value());
	CHECK(values->size() == 16UZ);
	CHECK(values->capacity() == 16UZ);
	CHECK(resource.alloc_count == 1UZ);
}

TEST_CASE(
	"json: JsonReader raw array counter validates nested syntax and restores",
	"[json][reader][perf]") {
	JsonReader r{R"([1,[2,],3])"};
	auto first = r.next();
	REQUIRE(first.has_value());
	REQUIRE(first->has_value());
	REQUIRE(**first == JsonReader::Event::begin_array);
	auto count = r.count_remaining_array_elements();
	REQUIRE_FALSE(count.has_value());
	CHECK(count.error().code == JsonIssueCode::syntax_error);
	auto value = r.next();
	REQUIRE(value.has_value());
	REQUIRE(value->has_value());
	CHECK(**value == JsonReader::Event::number_value);
}

TEST_CASE(
	"json: JsonReader skip_next_value raw-skips nested containers",
	"[json][reader][perf]") {
	JsonReader r{R"({"a":[1,{"b":2}],"c":3})"};
	auto skipped = r.skip_next_value();
	REQUIRE(skipped.has_value());
	CHECK(skipped->start == 0UZ);
	CHECK(skipped->end == r.input().size());
	auto next = r.next();
	REQUIRE(next.has_value());
	CHECK_FALSE(next->has_value());
}

TEST_CASE(
	"json: JsonReader counts object members without consuming them",
	"[json][reader][perf]") {
	JsonReader r{R"({"a":1,"b":[2,{"nested":3}],"c":{"d":4}})"};
	auto first = r.next();
	REQUIRE(first.has_value());
	REQUIRE(first->has_value());
	REQUIRE(**first == JsonReader::Event::begin_object);
	auto count = r.count_remaining_object_members();
	REQUIRE(count.has_value());
	CHECK(*count == 3UZ);
	auto key = r.next();
	REQUIRE(key.has_value());
	REQUIRE(key->has_value());
	CHECK(**key == JsonReader::Event::key);
	REQUIRE(r.key_token().unescaped_borrow().has_value());
	CHECK(*r.key_token().unescaped_borrow() == "a");
}

TEST_CASE(
	"json: escaped string member decode uses scratch inline buffer",
	"[json][reader][perf]") {
	CountingResource resource;
	JsonDecodeScratch scratch;
	scratch.reset_resource(&resource);
	JsonReader r{R"({"name":"A\nB","age":5})"};
	auto person = decode_direct<P4Person>(r, {}, &scratch);
	REQUIRE(person.has_value());
	CHECK(person->name == "A\nB");
	CHECK(person->age == 5);
	CHECK(resource.alloc_count == 0UZ);
}
TEST_CASE(
	"json: dump_direct writes JsonMembers compact object",
	"[json][direct]") {
	auto dumped = dump_direct(Point{.x = 3, .y = 7});
	REQUIRE(dumped.has_value());
	CHECK(*dumped == R"({"x":3,"y":7})");
}
TEST_CASE(
	"json: NativeJsonProvider dumps JsonMembers through direct compact writer",
	"[json][direct]") {
	auto dumped = boundary::NativeJsonProvider::dump_json(Point{.x = 3, .y = 7});
	REQUIRE(dumped.has_value());
	CHECK(*dumped == R"({"x":3,"y":7})");
}
TEST_CASE(
	"phase4: decode<P4Person>(JsonReader&) unknown_members=ignore validates skipped value",
	"[phase4]") {
	std::string input = R"({"name":"Bob","age":25,"extra":"bad)";
	input.push_back('\x01');
	input += R"("})";
	JsonReader r{input};
	JsonDecodeOptions opts;
	opts.unknown_members = UnknownMemberPolicy::ignore;
	auto p = decode<P4Person>(r, opts);
	CHECK_FALSE(p.has_value());
	CHECK(p.error().code == JsonIssueCode::syntax_error);
}
TEST_CASE(
	"phase4: decode<P4Person>(JsonReader&) unknown_members=reject",
	"[phase4]") {
	JsonReader r{R"({"name":"Bob","age":25,"extra":true})"};
	auto p = decode<P4Person>(r);
	CHECK_FALSE(p.has_value());
	CHECK(p.error().code == JsonIssueCode::invalid_value);
}
TEST_CASE(
	"phase4: decode<P4Nested>(JsonReader&) nested struct",
	"[phase4]") {
	JsonReader r{R"({"person":{"name":"Carol","age":40},"score":99})"};
	auto n = decode<P4Nested>(r);
	REQUIRE(n.has_value());
	CHECK(n->person.name == "Carol");
	CHECK(n->person.age == 40LL);
	CHECK(n->score == 99LL);
}
TEST_CASE(
	"phase4: decode<std::vector<P4Person>>(JsonReader&) array of structs",
	"[phase4]") {
	JsonReader r{R"([{"name":"A","age":1},{"name":"B","age":2}])"};
	auto v = decode<std::vector<P4Person>>(r);
	REQUIRE(v.has_value());
	REQUIRE(v->size() == 2UZ);
	CHECK((*v)[0].name == "A");
	CHECK((*v)[0].age == 1LL);
	CHECK((*v)[1].name == "B");
	CHECK((*v)[1].age == 2LL);
}

TEST_CASE(
	"json: dump_direct writes string-key maps without DOM builder",
	"[json][direct][perf]") {
	std::map<std::string, std::int64_t> ordered{
		{"a", 1},
		{"b", 2}
    };
	static_assert(JsonDirectWritable<decltype(ordered)>);
	auto dumped = dump_direct(ordered);
	REQUIRE(dumped.has_value());
	CHECK(*dumped == R"({"a":1,"b":2})");

	auto sorted = dump_direct(ordered, JsonDumpOptions{.sort_object_keys = true});
	CHECK_FALSE(sorted.has_value());
	CHECK(sorted.error().code == JsonIssueCode::invalid_value);
}

TEST_CASE(
	"json: NativeJsonProvider direct-writes map chunks",
	"[json][direct][boundary][perf]") {
	std::map<std::string, std::int64_t> ordered{
		{"a", 1},
		{"b", 2}
    };
	std::string body;
	std::size_t chunks = 0;
	auto sink = [&](std::string_view chunk) {
		++chunks;
		body.append(chunk);
	};
	auto ok = boundary::NativeJsonProvider::write_json(ordered, {}, sink);
	REQUIRE(ok.has_value());
	CHECK(body == R"({"a":1,"b":2})");
	CHECK(chunks == 1UZ);
}

TEST_CASE(
	"json: reader decodes arithmetic vectors and string maps in-place",
	"[json][reader][perf]") {
	{
		JsonReader r{R"([1,2,3])"};
		auto values = decode<std::vector<int>>(r);
		REQUIRE(values.has_value());
		CHECK(*values == std::vector<int>{1, 2, 3});
	}
	{
		JsonReader r{R"([1.5,-2.25,3e2])"};
		auto values = decode<std::vector<double>>(r);
		REQUIRE(values.has_value());
		REQUIRE(values->size() == 3UZ);
		CHECK((*values)[0] == 1.5);
		CHECK((*values)[1] == -2.25);
		CHECK((*values)[2] == 300.0);
	}
	{
		JsonReader r{R"([1e9999])"};
		auto values = decode<std::vector<double>>(r);
		CHECK_FALSE(values.has_value());
		CHECK(values.error().code == JsonIssueCode::number_out_of_range);
	}
	{
		JsonReader r{R"([1e39])"};
		auto values = decode<std::vector<float>>(r);
		CHECK_FALSE(values.has_value());
		CHECK(values.error().code == JsonIssueCode::number_out_of_range);
	}
	{
		CountingResource resource;
		DefaultPmrResourceGuard guard{&resource};
		std::string input = R"([1.0,2.0,3.0])";
		input.append(1024 * 1024, ' ');
		JsonReader r{input};
		auto values = decode<std::pmr::vector<double>>(r);
		REQUIRE(values.has_value());
		CHECK(values->size() == 3UZ);
		CHECK(values->capacity() <= 4UZ);
		CHECK(resource.alloc_bytes < 1024UZ);
	}
	{
		JsonReader r{R"({"a":"x","b":"y"})"};
		auto values = decode<std::unordered_map<std::string, std::string>>(r);
		REQUIRE(values.has_value());
		CHECK(values->at("a") == "x");
		CHECK(values->at("b") == "y");
	}
}

TEST_CASE(
	"json: dump_direct writes arithmetic vectors and tuples",
	"[json][direct][perf]") {
	std::vector<int> ints{1, 2, 3};
	static_assert(JsonDirectWritable<decltype(ints)>);
	auto arr = dump_direct(ints);
	REQUIRE(arr.has_value());
	CHECK(*arr == R"([1,2,3])");

	std::tuple<std::string_view, int, bool> tup{"x", 4, true};
	static_assert(JsonDirectWritable<decltype(tup)>);
	auto tuple_dump = dump_direct(tup);
	REQUIRE(tuple_dump.has_value());
	CHECK(*tuple_dump == R"(["x",4,true])");
}

TEST_CASE(
	"json: dump_direct rejects non-finite floating point",
	"[json][direct]") {
	auto dumped = dump_direct(std::numeric_limits<double>::quiet_NaN());
	CHECK_FALSE(dumped.has_value());
	CHECK(dumped.error().code == JsonIssueCode::number_out_of_range);
}

TEST_CASE(
	"phase4: nested direct decode reuses caller scratch for long escaped keys",
	"[phase4][direct]") {
	CountingResource resource;
	JsonDecodeScratch scratch;
	scratch.reset_resource(&resource);

	std::string escaped_key;
	escaped_key.reserve(p4_long_key_name().size() + 5);
	escaped_key += R"(\u0061)";
	escaped_key.append(p4_long_key_name().size() - 1, 'a');

	std::string input;
	input.reserve((escaped_key.size() + 16) * 2);
	input += R"([{")";
	input += escaped_key;
	input += R"(":1},{")";
	input += escaped_key;
	input += R"(":2}])";

	JsonReader r{input};
	auto v = decode_direct<std::vector<P4LongKey>>(r, {}, &scratch);
	REQUIRE(v.has_value());
	REQUIRE(v->size() == 2UZ);
	CHECK((*v)[0].value == 1LL);
	CHECK((*v)[1].value == 2LL);
	CHECK(scratch.key_overflow.capacity() >= p4_long_key_name().size());
	CHECK(resource.alloc_count == 1UZ);
}
TEST_CASE(
	"phase4: decode<std::optional<std::int64_t>>(JsonReader&) with null",
	"[phase4]") {
	{
		JsonReader r{"null"};
		auto v = decode<std::optional<std::int64_t>>(r);
		REQUIRE(v.has_value());
		CHECK(!v->has_value());
	}
	{
		JsonReader r{"42"};
		auto v = decode<std::optional<std::int64_t>>(r);
		REQUIRE(v.has_value());
		REQUIRE(v->has_value());
		CHECK(**v == 42LL);
	}
}
TEST_CASE(
	"phase4: decode<std::map<std::string,i64>>(JsonReader&) map",
	"[phase4]") {
	JsonReader r{R"({"a":1,"b":2})"};
	auto m = decode<std::map<std::string, std::int64_t>>(r);
	REQUIRE(m.has_value());
	CHECK(m->size() == 2UZ);
	CHECK((*m)["a"] == 1LL);
	CHECK((*m)["b"] == 2LL);
}
TEST_CASE(
	"phase4: decode<P4Address>(JsonReader&) optional member absent",
	"[phase4]") {
	JsonReader r{R"({"street":"Main St"})"};
	auto a = decode<P4Address>(r);
	REQUIRE(a.has_value());
	CHECK(a->street == "Main St");
	CHECK(!a->city.has_value());
}
TEST_CASE(
	"phase4: decode<P4Address>(JsonReader&) optional member present",
	"[phase4]") {
	JsonReader r{R"({"street":"Main St","city":"Springfield"})"};
	auto a = decode<P4Address>(r);
	REQUIRE(a.has_value());
	CHECK(a->street == "Main St");
	REQUIRE(a->city.has_value());
	CHECK(*a->city == "Springfield");
}
TEST_CASE(
	"phase4: decode<P4Person>(JsonReader&) missing required member",
	"[phase4]") {
	JsonReader r{R"({"name":"Alice"})"};
	auto p = decode<P4Person>(r);
	CHECK_FALSE(p.has_value());
	CHECK(p.error().code == JsonIssueCode::missing_member);
}
TEST_CASE(
	"phase4: decode<bool>(JsonReader&)",
	"[phase4]") {
	{
		JsonReader r{"true"};
		auto v = decode<bool>(r);
		REQUIRE(v.has_value());
		CHECK(*v == true);
	}
	{
		JsonReader r{"false"};
		auto v = decode<bool>(r);
		REQUIRE(v.has_value());
		CHECK(*v == false);
	}
}
TEST_CASE(
	"phase4: decode<double>(JsonReader&)",
	"[phase4]") {
	JsonReader r{"3.14"};
	auto v = decode<double>(r);
	REQUIRE(v.has_value());
	CHECK(*v == Catch::Approx(3.14));
}
TEST_CASE(
	"phase4: decode<std::vector<std::int64_t>>(JsonReader&)",
	"[phase4]") {
	JsonReader r{"[1,2,3,4,5]"};
	auto v = decode<std::vector<std::int64_t>>(r);
	REQUIRE(v.has_value());
	REQUIRE(v->size() == 5UZ);
	for (std::size_t i = 0; i < 5; ++i) {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
		CHECK((*v)[i] == static_cast<std::int64_t>(i + 1));
	}
}
TEST_CASE(
	"phase4: JsonReader empty object",
	"[phase4]") {
	JsonReader r{"{}"};
	auto e0 = r.next();
	REQUIRE(e0.has_value());
	REQUIRE(e0->has_value());
	CHECK(**e0 == JsonReader::Event::begin_object);
	auto e1 = r.next();
	REQUIRE(e1.has_value());
	REQUIRE(e1->has_value());
	CHECK(**e1 == JsonReader::Event::end_object);
}
TEST_CASE(
	"phase4: JsonReader empty array",
	"[phase4]") {
	JsonReader r{"[]"};
	auto e0 = r.next();
	REQUIRE(e0.has_value());
	REQUIRE(e0->has_value());
	CHECK(**e0 == JsonReader::Event::begin_array);
	auto e1 = r.next();
	REQUIRE(e1.has_value());
	REQUIRE(e1->has_value());
	CHECK(**e1 == JsonReader::Event::end_array);
}
TEST_CASE(
	"phase4: decode<std::pair<std::string,i64>>(JsonReader&) pair",
	"[phase4]") {
	JsonReader r{R"(["hello",42])"};
	auto v = decode<std::pair<std::string, std::int64_t>>(r);
	REQUIRE(v.has_value());
	CHECK(v->first == "hello");
	CHECK(v->second == 42LL);
}
TEST_CASE(
	"phase4: decode<std::array<std::int64_t,3>>(JsonReader&) fixed array",
	"[phase4]") {
	JsonReader r{"[10,20,30]"};
	auto v = decode<std::array<std::int64_t, 3>>(r);
	REQUIRE(v.has_value());
	CHECK((*v)[0] == 10LL);
	CHECK((*v)[1] == 20LL);
	CHECK((*v)[2] == 30LL);
}
TEST_CASE(
	"phase4: decode<std::tuple<std::string,std::int64_t,bool>>(JsonReader&) tuple",
	"[phase4]") {
	JsonReader r{R"(["hello",42,true])"};
	auto v = decode<std::tuple<std::string, std::int64_t, bool>>(r);
	REQUIRE(v.has_value());
	CHECK(get<0>(*v) == "hello");
	CHECK(get<1>(*v) == 42LL);
	CHECK(get<2>(*v) == true);
}
TEST_CASE(
	"phase4: JsonReader escaped unicode string",
	"[phase4]") {
	JsonReader r{R"("Hello")"};
	auto ev = r.next();
	REQUIRE(ev.has_value());
	REQUIRE(ev->has_value());
	CHECK(**ev == JsonReader::Event::string_value);
	std::string out;
	auto res = r.string_token().append_decoded_to(out);
	REQUIRE(res.has_value());
	CHECK(out == "Hello");
}
TEST_CASE(
	"phase4: decode<std::unordered_map<std::string,i64>>(JsonReader&) unordered map",
	"[phase4]") {
	JsonReader r{R"({"x":10,"y":20})"};
	auto m = decode<std::unordered_map<std::string, std::int64_t>>(r);
	REQUIRE(m.has_value());
	CHECK(m->size() == 2UZ);
	CHECK((*m)["x"] == 10LL);
	CHECK((*m)["y"] == 20LL);
}
TEST_CASE(
	"json: reader unordered_map decode reserves counted object size",
	"[json][reader][perf]") {
	CountingResource resource;
	DefaultPmrResourceGuard guard{&resource};
	JsonReader r{
		R"({"k0":0,"k1":1,"k2":2,"k3":3,"k4":4,"k5":5,"k6":6,"k7":7,"k8":8,"k9":9,"k10":10,"k11":11,"k12":12,"k13":13,"k14":14,"k15":15})"};
	auto m = decode<std::pmr::unordered_map<std::string, std::int64_t>>(r);
	REQUIRE(m.has_value());
	CHECK(m->size() == 16UZ);
	CHECK(m->bucket_count() >= 16UZ);
	CHECK(m->at("k0") == 0LL);
	CHECK(m->at("k15") == 15LL);
	CHECK(resource.alloc_count <= m->size() + 1UZ);
}

TEST_CASE(
	"json: reader map decode honors duplicate policies",
	"[json][reader][duplicates]") {
	{
		JsonReader r{R"({"x":1,"x":2})"};
		auto m = decode<std::unordered_map<std::string, std::int64_t>>(r);
		CHECK_FALSE(m.has_value());
		CHECK(m.error().code == JsonIssueCode::duplicate_member);
	}
	{
		JsonParseOptions opts;
		opts.duplicate_key = DuplicateKeyPolicy::last_wins;
		JsonReader r{R"({"x":1,"x":2})", opts};
		auto m = decode<std::unordered_map<std::string, std::int64_t>>(r);
		REQUIRE(m.has_value());
		CHECK(m->at("x") == 2LL);
	}
	{
		JsonParseOptions opts;
		opts.duplicate_key = DuplicateKeyPolicy::first_wins;
		JsonReader r{R"({"x":1,"x":[2,3,4]})", opts};
		auto m = decode<std::unordered_map<std::string, std::int64_t>>(r);
		REQUIRE(m.has_value());
		CHECK(m->at("x") == 1LL);
	}
	{
		JsonParseOptions opts;
		opts.duplicate_key = DuplicateKeyPolicy::first_wins;
		JsonReader r{R"({"x":1,"x":[2,]})", opts};
		auto m = decode<std::unordered_map<std::string, std::int64_t>>(r);
		CHECK_FALSE(m.has_value());
		CHECK(m.error().code == JsonIssueCode::syntax_error);
	}
}

TEST_CASE(
	"phase4: JsonReader reset restores fresh state",
	"[phase4]") {
	JsonReader r{"42"};
	auto ev = r.next();
	REQUIRE(ev.has_value());
	REQUIRE(ev->has_value());
	auto eof = r.next();
	REQUIRE(eof.has_value());
	CHECK(!eof->has_value());

	r.reset();
	auto ev2 = r.next();
	REQUIRE(ev2.has_value());
	REQUIRE(ev2->has_value());
	CHECK(**ev2 == JsonReader::Event::number_value);
	auto v = r.number_val().to_i64();
	REQUIRE(v.has_value());
	CHECK(*v == 42LL);
}
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
// ─── Phase 5 — Memory Model & Performance Hardening ─────────────────────────

TEST_CASE(
	"phase5: parse_copy with pmr monotonic_buffer_resource",
	"[phase5]") {
	std::pmr::monotonic_buffer_resource mbr{4096};
	auto doc = parse_copy("42", {}, &mbr);
	REQUIRE(doc.has_value());
	auto v = doc->root().as_i64();
	REQUIRE(v.has_value());
	CHECK(*v == 42LL);
}
TEST_CASE(
	"phase5: parse_borrowed with pmr resource",
	"[phase5]") {
	std::string input = R"({"x":1})";
	std::pmr::monotonic_buffer_resource mbr{4096};
	auto doc = parse_borrowed(input, {}, &mbr);
	REQUIRE(doc.has_value());
	auto root = doc->root().as_object();
	REQUIRE(root.has_value());
	CHECK(root->size() == 1UZ);
}
TEST_CASE(
	"phase5: explicit borrowed parse aliases",
	"[phase5]") {
	std::string input = R"({"name":"ada"})";
	auto doc = parse_borrowed_unsafe(input);
	REQUIRE(doc.has_value());
	auto view_doc = parse_view(std::string_view{input});
	REQUIRE(view_doc.has_value());
	auto obj = doc->root().as_object();
	REQUIRE(obj.has_value());
	CHECK(*obj->member("name")->as_string() == "ada");
}
TEST_CASE(
	"phase5: pmr parse_copy returns same result as default parse",
	"[phase5]") {
	std::string_view input = R"([1,2,3,"hello"])";
	std::pmr::monotonic_buffer_resource mbr{4096};
	auto d1 = parse(input);
	auto d2 = parse_copy(input, {}, &mbr);
	REQUIRE(d1.has_value());
	REQUIRE(d2.has_value());
	auto v1 = d1->dump();
	auto v2 = d2->dump();
	REQUIRE(v1.has_value());
	REQUIRE(v2.has_value());
	CHECK(*v1 == *v2);
}
TEST_CASE(
	"phase5: JsonArena basic parse_into",
	"[phase5]") {
	JsonArena arena;
	auto doc = arena.parse_into(R"({"name":"Alice","age":30})");
	REQUIRE(doc.has_value());
	auto root = doc->root().as_object();
	REQUIRE(root.has_value());
	auto name = root->find_member("name");
	REQUIRE(name.has_value());
	auto sv = name->as_string();
	REQUIRE(sv.has_value());
	CHECK(*sv == "Alice");
}
TEST_CASE(
	"phase5: JsonArena parse_into two documents sequentially",
	"[phase5]") {
	JsonArena arena;
	auto d1 = arena.parse_into("1");
	REQUIRE(d1.has_value());
	CHECK(d1->is_live());
	CHECK(*d1->root().as_i64() == 1LL);

	// Second parse reuses arena storage
	auto d2 = arena.parse_into("2");
	REQUIRE(d2.has_value());
	CHECK_FALSE(d1->is_live());
	CHECK(d2->is_live());
	CHECK(*d2->root().as_i64() == 2LL);
}
TEST_CASE(
	"phase5: JsonArena reset then parse_into",
	"[phase5]") {
	JsonArena arena;
	auto d1 = arena.parse_into(R"("hello")");
	REQUIRE(d1.has_value());

	arena.reset();

	auto d2 = arena.parse_into(R"("world")");
	REQUIRE(d2.has_value());
	auto sv = d2->root().as_string();
	REQUIRE(sv.has_value());
	CHECK(*sv == "world");
}
TEST_CASE(
	"phase5: JsonArena slab_capacity returns configured size",
	"[phase5]") {
	JsonArena arena{JsonArenaOptions{.initial_slab = 128 * 1024}};
	CHECK(arena.slab_capacity() == 128 * 1024UZ);
	CHECK(arena.slab_used() == 0UZ);
	auto doc = arena.parse_into(R"({"name":"Alice","age":30})");
	REQUIRE(doc.has_value());
	CHECK(arena.slab_used() > 0UZ);
}
TEST_CASE(
	"phase5: JsonArena hash index allocations use the injected resource",
	"[phase5]") {
	CountingResource hash_resource{};
	JsonArena arena{
		JsonArenaOptions{.initial_slab = 128 * 1024, .hash_index_resource = &hash_resource}
    };
	auto doc = arena.parse_into(
		R"({"k00":0,"k01":1,"k02":2,"k03":3,"k04":4,"k05":5,"k06":6,"k07":7,"k08":8,"k09":9,"k10":10,"k11":11,"k12":12,"k13":13,"k14":14,"k15":15,"k16":16,"k17":17,"k18":18,"k19":19,"k20":20,"k21":21,"k22":22,"k23":23,"k24":24,"k25":25,"k26":26,"k27":27,"k28":28,"k29":29,"k30":30,"k31":31,"k32":32,"k33":33,"k34":34,"k35":35,"k36":36,"k37":37,"k38":38,"k39":39})");
	REQUIRE(doc.has_value());
	hash_resource.alloc_count = 0;
	hash_resource.dealloc_count = 0;
	hash_resource.alloc_bytes = 0;

	auto warm = doc->warm_member_index(doc->root());
	REQUIRE(warm.has_value());
	CHECK(hash_resource.alloc_count > 0);
	CHECK(hash_resource.alloc_bytes > 0);
	CHECK(hash_resource.dealloc_count == 0);

	auto doc2 = arena.parse_into(R"({"fresh":true})");
	REQUIRE(doc2.has_value());
	CHECK_FALSE(doc->is_live());
	CHECK(doc2->is_live());
	CHECK(hash_resource.dealloc_count > 0);

	arena.reset();
	hash_resource.alloc_count = 0;
	hash_resource.dealloc_count = 0;
	hash_resource.alloc_bytes = 0;

	auto doc3 = arena.parse_into(
		R"({"k00":0,"k01":1,"k02":2,"k03":3,"k04":4,"k05":5,"k06":6,"k07":7,"k08":8,"k09":9,"k10":10,"k11":11,"k12":12,"k13":13,"k14":14,"k15":15,"k16":16,"k17":17,"k18":18,"k19":19,"k20":20,"k21":21,"k22":22,"k23":23,"k24":24,"k25":25,"k26":26,"k27":27,"k28":28,"k29":29,"k30":30,"k31":31,"k32":32,"k33":33,"k34":34,"k35":35,"k36":36,"k37":37,"k38":38,"k39":39})");
	REQUIRE(doc3.has_value());
	auto warm_after_reset = doc3->warm_member_index(doc3->root());
	REQUIRE(warm_after_reset.has_value());
	CHECK(hash_resource.alloc_count > 0);
	CHECK(hash_resource.alloc_bytes > 0);
}
TEST_CASE(
	"phase5: ArenaDocument dump produces correct JSON",
	"[phase5]") {
	JsonArena arena;
	auto doc = arena.parse_into(R"([1,2,3])");
	REQUIRE(doc.has_value());
	auto dumped = doc->dump();
	REQUIRE(dumped.has_value());
	CHECK(*dumped == "[1,2,3]");
}
TEST_CASE(
	"phase5: JsonArena parse_into error propagation",
	"[phase5]") {
	JsonArena arena;
	auto doc = arena.parse_into("{bad json}");
	CHECK_FALSE(doc.has_value());
}
TEST_CASE(
	"phase5: JsonArena parse_borrowed_into preserves borrowed string views",
	"[phase5]") {
	JsonArena arena;
	std::string input = R"({"name":"Alice","age":30})";
	auto doc = arena.parse_borrowed_into(input);
	REQUIRE(doc.has_value());

	auto root = doc->root().as_object();
	REQUIRE(root.has_value());
	auto name = root->find_member("name");
	REQUIRE(name.has_value());
	auto before = name->as_string();
	REQUIRE(before.has_value());
	CHECK(*before == "Alice");

	auto const pos = input.find("Alice");
	REQUIRE(pos != std::string::npos);
	input.replace(pos, 5, "Marta");

	auto after = name->as_string();
	REQUIRE(after.has_value());
	CHECK(*after == "Marta");
}

TEST_CASE(
	"phase5: JsonArena parse_moved_into owns moved input",
	"[phase5]") {
	JsonArena arena;
	auto doc = [&] {
		std::string input = std::string{"\xEF\xBB\xBF"} + R"({"name":"Bob","age":41})";
		return arena.parse_moved_into(std::move(input));
	}();
	REQUIRE(doc.has_value());

	auto root = doc->root().as_object();
	REQUIRE(root.has_value());
	auto name = root->find_member("name");
	REQUIRE(name.has_value());
	auto sv = name->as_string();
	REQUIRE(sv.has_value());
	CHECK(*sv == "Bob");
	auto age = root->find_member("age");
	REQUIRE(age.has_value());
	auto age_i64 = age->as_i64();
	REQUIRE(age_i64.has_value());
	CHECK(*age_i64 == 41LL);
}
TEST_CASE(
	"phase5: from_chars deferred overflow returns error",
	"[phase5]") {
	auto doc = parse("1e999");
	REQUIRE(doc.has_value());
	auto n = doc->root().as_number();
	REQUIRE(n.has_value());
	auto res = n->to_f64();
	CHECK_FALSE(res.has_value());
	CHECK(res.error().code == JsonIssueCode::number_out_of_range);
}
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
	CHECK_FALSE(it->has_value()); // too long
	CHECK(it->error().code == JsonIssueCode::input_too_large);
	++it;
	REQUIRE(it != std::default_sentinel);
	CHECK(it->has_value());
	++it;
	CHECK(it == std::default_sentinel);
}
// ---------------------------------------------------------------------------
// Phase 8.1 — JSON5 relaxed parse mode
// ---------------------------------------------------------------------------

static constexpr JsonParseOptions json5_opts{.mode = ParseMode::json5};
TEST_CASE(
	"phase8: json5 line comment",
	"[phase8]") {
	auto doc = parse("// comment\n42", json5_opts);
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_i64() == 42);
}
TEST_CASE(
	"phase8: json5 block comment",
	"[phase8]") {
	auto doc = parse("/* block */42", json5_opts);
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_i64() == 42);
}
TEST_CASE(
	"phase8: json5 block comment with newlines",
	"[phase8]") {
	auto doc = parse("/*\n multi\n line\n*/42", json5_opts);
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_number()->to_i64() == 42);
}
TEST_CASE(
	"phase8: json5 inline comment after value",
	"[phase8]") {
	auto doc = parse("[1, /* mid */ 2]", json5_opts);
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	CHECK(*a.element(0)->as_number()->to_i64() == 1);
	CHECK(*a.element(1)->as_number()->to_i64() == 2);
}
TEST_CASE(
	"phase8: json5 unterminated block comment",
	"[phase8]") {
	auto doc = parse("/* never closed", json5_opts);
	CHECK_FALSE(doc.has_value());
	CHECK(doc.error().message == "unterminated block comment");
	REQUIRE(doc.error().source.has_value());
	CHECK(doc.error().source->offset == 0);
}
TEST_CASE(
	"phase8: json5 SAX unterminated block comment",
	"[phase8]") {
	JsonReader r{"[1, /* never closed", json5_opts};
	REQUIRE(r.next().has_value());
	REQUIRE(r.next().has_value());
	auto ev = r.next();
	CHECK_FALSE(ev.has_value());
	CHECK(ev.error().message == "unterminated block comment");
}
TEST_CASE(
	"phase8: json5 trailing comma in array",
	"[phase8]") {
	auto doc = parse("[1, 2, 3,]", json5_opts);
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	CHECK(a.size() == 3);
	CHECK(*a.element(2)->as_number()->to_i64() == 3);
}
TEST_CASE(
	"phase8: json5 trailing comma in object",
	"[phase8]") {
	auto doc = parse(R"({"a": 1, "b": 2,})", json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("a")->as_number()->to_i64() == 1);
	CHECK(*o.member("b")->as_number()->to_i64() == 2);
}
TEST_CASE(
	"phase8: strict mode rejects trailing comma",
	"[phase8]") {
	CHECK_FALSE(parse("[1,]").has_value());
	CHECK_FALSE(parse(R"({"a":1,})").has_value());
}
TEST_CASE(
	"phase8: json5 single-quoted string value",
	"[phase8]") {
	auto doc = parse("'hello'", json5_opts);
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_string() == "hello");
}
TEST_CASE(
	"phase8: json5 single-quoted string with escapes",
	"[phase8]") {
	auto doc = parse(R"('it\'s \"fine\"')", json5_opts);
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_string() == "it's \"fine\"");
}
TEST_CASE(
	"phase8: json5 reader rejects unsupported string escapes",
	"[phase8]") {
	JsonReader double_quoted{R"("bad\q")", json5_opts};
	auto ev = double_quoted.next();
	REQUIRE_FALSE(ev.has_value());
	CHECK(ev.error().message == "invalid escape");

	JsonReader single_quoted{R"('bad\q')", json5_opts};
	ev = single_quoted.next();
	REQUIRE_FALSE(ev.has_value());
	CHECK(ev.error().message == "invalid escape");
}
TEST_CASE(
	"phase8: json5 single-quoted string in array",
	"[phase8]") {
	auto doc = parse("['a', 'b']", json5_opts);
	REQUIRE(doc.has_value());
	auto a = *doc->root().as_array();
	CHECK(*a.element(0)->as_string() == "a");
	CHECK(*a.element(1)->as_string() == "b");
}
TEST_CASE(
	"phase8: json5 unquoted key",
	"[phase8]") {
	auto doc = parse("{foo: 1, bar: 2}", json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("foo")->as_number()->to_i64() == 1);
	CHECK(*o.member("bar")->as_number()->to_i64() == 2);
}
TEST_CASE(
	"phase8: json5 unquoted key with underscore and dollar",
	"[phase8]") {
	auto doc = parse("{_priv: 1, $ref: 2, a1b2: 3}", json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("_priv")->as_number()->to_i64() == 1);
	CHECK(*o.member("$ref")->as_number()->to_i64() == 2);
	CHECK(*o.member("a1b2")->as_number()->to_i64() == 3);
}
TEST_CASE(
	"phase8: json5 mixed quoted and unquoted keys dedup",
	"[phase8]") {
	JsonParseOptions opts5{.duplicate_key = DuplicateKeyPolicy::reject, .mode = ParseMode::json5};
	auto doc = parse(R"({"a": 1, a: 2})", opts5);
	CHECK_FALSE(doc.has_value());
	CHECK(doc.error().code == JsonIssueCode::duplicate_member);
}
TEST_CASE(
	"phase8: json5 single-quoted key",
	"[phase8]") {
	auto doc = parse("{'key': 42}", json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("key")->as_number()->to_i64() == 42);
}
TEST_CASE(
	"phase8: json5 combined features",
	"[phase8]") {
	auto doc = parse(
		R"({
// line comment
name:'Alice',
age:30,
/* block comment */
tags:['a',"b",],
})",
		json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("name")->as_string() == "Alice");
	CHECK(*o.member("age")->as_number()->to_i64() == 30);
	auto tags = *o.member("tags")->as_array();
	CHECK(tags.size() == 2);
	CHECK(*tags.element(0)->as_string() == "a");
	CHECK(*tags.element(1)->as_string() == "b");
}
TEST_CASE(
	"phase8: strict mode rejects json5 features",
	"[phase8]") {
	CHECK_FALSE(parse("// comment\n42").has_value());
	CHECK_FALSE(parse("{foo: 1}").has_value());
	CHECK_FALSE(parse("'hello'").has_value());
}
TEST_CASE(
	"phase8: json5 empty trailing comma array",
	"[phase8]") {
	auto doc = parse("[,]", json5_opts);
	CHECK_FALSE(doc.has_value());
}
TEST_CASE(
	"phase8: json5 comment between key and colon",
	"[phase8]") {
	auto doc = parse("{foo /* c */ : 1}", json5_opts);
	REQUIRE(doc.has_value());
	auto o = *doc->root().as_object();
	CHECK(*o.member("foo")->as_number()->to_i64() == 1);
}
TEST_CASE(
	"phase8: json5 SAX reader with comments and trailing comma",
	"[phase8]") {
	JsonReader r{"[1, /* x */ 2,]", json5_opts};
	using Ev = JsonReader::Event;
	auto e1 = r.next();
	REQUIRE(e1.has_value());
	CHECK(**e1 == Ev::begin_array);
	auto e2 = r.next();
	REQUIRE(e2.has_value());
	CHECK(**e2 == Ev::number_value);
	auto e3 = r.next();
	REQUIRE(e3.has_value());
	CHECK(**e3 == Ev::number_value);
	auto e4 = r.next();
	REQUIRE(e4.has_value());
	CHECK(**e4 == Ev::end_array);
	auto e5 = r.next();
	REQUIRE(e5.has_value());
	CHECK_FALSE(e5->has_value());
}
TEST_CASE(
	"phase8: json5 SAX reader unquoted key",
	"[phase8]") {
	JsonReader r{"{foo: 'bar',}", json5_opts};
	using Ev = JsonReader::Event;
	auto e1 = r.next();
	REQUIRE(e1.has_value());
	CHECK(**e1 == Ev::begin_object);
	auto e2 = r.next();
	REQUIRE(e2.has_value());
	CHECK(**e2 == Ev::key);
	std::string key_str;
	REQUIRE(r.key_token().append_decoded_to(key_str).has_value());
	CHECK(key_str == "foo");
	auto e3 = r.next();
	REQUIRE(e3.has_value());
	CHECK(**e3 == Ev::string_value);
	std::string val_str;
	REQUIRE(r.string_token().append_decoded_to(val_str).has_value());
	CHECK(val_str == "bar");
	auto e4 = r.next();
	REQUIRE(e4.has_value());
	CHECK(**e4 == Ev::end_object);
}
// ---------------------------------------------------------------------------
// Phase 8.3: schema_for / validate
// ---------------------------------------------------------------------------

TEST_CASE(
	"phase8.3: schema_for generates correct schema for Point",
	"[phase8.3]") {
	auto doc_r = schema_for<Point>();
	REQUIRE(doc_r.has_value());
	auto root = *doc_r->root().as_object();
	CHECK(*root.find_member("type")->as_string() == "object");

	auto props = *root.find_member("properties")->as_object();
	auto x_obj = *props.find_member("x")->as_object();
	CHECK(*x_obj.find_member("type")->as_string() == "integer");
	auto y_obj = *props.find_member("y")->as_object();
	CHECK(*y_obj.find_member("type")->as_string() == "integer");

	auto req = *root.find_member("required")->as_array();
	CHECK(req.size() == 2);
	CHECK(*req.element(0)->as_string() == "x");
	CHECK(*req.element(1)->as_string() == "y");
}
TEST_CASE(
	"phase8.3: schema_for with optional field",
	"[phase8.3]") {
	auto doc_r = schema_for<Config>();
	REQUIRE(doc_r.has_value());
	auto root = *doc_r->root().as_object();

	auto props = *root.find_member("properties")->as_object();
	auto rf_obj = *props.find_member("required_field")->as_object();
	CHECK(*rf_obj.find_member("type")->as_string() == "integer");
	auto of_obj = *props.find_member("optional_field")->as_object();
	CHECK(*of_obj.find_member("type")->as_string() == "integer");

	auto req = *root.find_member("required")->as_array();
	CHECK(req.size() == 1);
	CHECK(*req.element(0)->as_string() == "required_field");
}
TEST_CASE(
	"phase8.3: validate passes valid object",
	"[phase8.3]") {
	auto schema_r = schema_for<Point>();
	REQUIRE(schema_r.has_value());
	auto doc_r = parse(R"({"x": 1, "y": 2})");
	REQUIRE(doc_r.has_value());
	auto result = validate(doc_r->root(), schema_r->root());
	CHECK(result.has_value());
}
TEST_CASE(
	"phase8.3: validate detects wrong type",
	"[phase8.3]") {
	auto schema_r = schema_for<Point>();
	REQUIRE(schema_r.has_value());
	auto doc_r = parse(R"([1, 2])");
	REQUIRE(doc_r.has_value());
	auto result = validate(doc_r->root(), schema_r->root());
	REQUIRE(!result.has_value());
	CHECK(result.error().code == JsonIssueCode::wrong_kind);
}
TEST_CASE(
	"phase8.3: validate detects missing required member",
	"[phase8.3]") {
	auto schema_r = schema_for<Point>();
	REQUIRE(schema_r.has_value());
	auto doc_r = parse(R"({"x": 1})");
	REQUIRE(doc_r.has_value());
	auto result = validate(doc_r->root(), schema_r->root());
	REQUIRE(!result.has_value());
	CHECK(result.error().code == JsonIssueCode::missing_member);
	CHECK(result.error().member_name == "y");
}
TEST_CASE(
	"phase8.3: validate allows missing optional field",
	"[phase8.3]") {
	auto schema_r = schema_for<Config>();
	REQUIRE(schema_r.has_value());
	auto doc_r = parse(R"({"required_field": 42})");
	REQUIRE(doc_r.has_value());
	auto result = validate(doc_r->root(), schema_r->root());
	CHECK(result.has_value());
}
TEST_CASE(
	"phase8.3: validate detects field type mismatch",
	"[phase8.3]") {
	auto schema_r = schema_for<Point>();
	REQUIRE(schema_r.has_value());
	auto doc_r = parse(R"({"x": "bad", "y": 2})");
	REQUIRE(doc_r.has_value());
	auto result = validate(doc_r->root(), schema_r->root());
	REQUIRE(!result.has_value());
	CHECK(result.error().code == JsonIssueCode::wrong_kind);
	CHECK(result.error().member_name == "x");
}

// ---------------------------------------------------------------------------
// JSON Pointer ergonomics + RFC 7396 merge patch
// ---------------------------------------------------------------------------

TEST_CASE(
	"json: at_pointer parses and resolves in one call",
	"[json][pointer]") {
	auto doc = parse(R"({"users":[{"name":"Ada"}]})");
	REQUIRE(doc.has_value());

	auto name = doc->root().at_pointer("/users/0/name");
	REQUIRE(name.has_value());
	CHECK(*name->as_string() == "Ada");

	auto bad = doc->root().at_pointer("users/0/name");
	REQUIRE(!bad.has_value());
	CHECK(bad.error().code == JsonIssueCode::invalid_pointer);
}

TEST_CASE(
	"json: merge_patch applies RFC 7396 object changes",
	"[json][merge_patch]") {
	auto target = parse(R"({
		"title": "Goodbye!",
		"author": {"givenName": "John", "familyName": "Doe"},
		"tags": ["example", "sample"],
		"content": "This will be unchanged"
	})");
	auto patch = parse(R"({
		"title": "Hello!",
		"phoneNumber": "+01-123-456-7890",
		"author": {"familyName": null},
		"tags": ["example"]
	})");
	REQUIRE(target.has_value());
	REQUIRE(patch.has_value());

	auto merged = merge_patch(*target, *patch);
	REQUIRE(merged.has_value());

	CHECK(*merged->root().at_pointer("/title")->as_string() == "Hello!");
	CHECK(*merged->root().at_pointer("/content")->as_string() == "This will be unchanged");
	CHECK(*merged->root().at_pointer("/phoneNumber")->as_string() == "+01-123-456-7890");
	CHECK(*merged->root().at_pointer("/author/givenName")->as_string() == "John");
	CHECK_FALSE(merged->root().at_pointer("/author/familyName").has_value());
	auto tags = merged->root().at_pointer("/tags")->as_array();
	REQUIRE(tags.has_value());
	REQUIRE(tags->size() == 1UZ);
	CHECK(*tags->element(0)->as_string() == "example");

	CHECK(*target->root().at_pointer("/author/familyName")->as_string() == "Doe");
}

TEST_CASE(
	"json: merge_patch replaces root with non-object patch",
	"[json][merge_patch]") {
	auto target = parse(R"({"a":1})");
	auto patch = parse(R"([1,2,3])");
	REQUIRE(target.has_value());
	REQUIRE(patch.has_value());

	auto merged = merge_patch(target->root(), patch->root());
	REQUIRE(merged.has_value());
	auto arr = merged->root().as_array();
	REQUIRE(arr.has_value());
	CHECK(arr->size() == 3UZ);
	CHECK(*arr->element(2)->as_i64() == 3);
}

TEST_CASE(
	"json: merge_patch null patch replaces root but deletes object members inside objects",
	"[json][merge_patch]") {
	{
		auto target = parse(R"({"a":1})");
		auto patch = parse("null");
		REQUIRE(target.has_value());
		REQUIRE(patch.has_value());
		auto merged = merge_patch(*target, *patch);
		REQUIRE(merged.has_value());
		CHECK(merged->root().is_null());
	}
	{
		auto target = parse(R"({"a":1,"b":2})");
		auto patch = parse(R"({"a":null})");
		REQUIRE(target.has_value());
		REQUIRE(patch.has_value());
		auto merged = merge_patch(*target, *patch);
		REQUIRE(merged.has_value());
		CHECK_FALSE(merged->root().at_pointer("/a").has_value());
		CHECK(*merged->root().at_pointer("/b")->as_i64() == 2);
	}
}

TEST_CASE(
	"json: apply_patch supports RFC 6902 operations without mutating input",
	"[json][patch]") {
	auto target = parse(R"({"foo":"bar","numbers":[1,2,3],"obj":{"a":1}})");
	auto patch = parse(R"([
		{"op":"add","path":"/baz","value":"qux"},
		{"op":"add","path":"/numbers/-","value":4},
		{"op":"remove","path":"/foo"},
		{"op":"replace","path":"/obj/a","value":2},
		{"op":"copy","from":"/baz","path":"/copied"},
		{"op":"move","from":"/copied","path":"/moved"},
		{"op":"test","path":"/moved","value":"qux"}
	])");
	REQUIRE(target.has_value());
	REQUIRE(patch.has_value());

	auto result = conflux::json::apply_patch(*target, *patch);
	REQUIRE(result.has_value());
	CHECK_FALSE(result->root().at_pointer("/foo").has_value());
	CHECK(*result->root().at_pointer("/baz")->as_string() == "qux");
	CHECK(*result->root().at_pointer("/moved")->as_string() == "qux");
	CHECK(*result->root().at_pointer("/obj/a")->as_i64() == 2);
	auto numbers = result->root().at_pointer("/numbers")->as_array();
	REQUIRE(numbers.has_value());
	CHECK(numbers->size() == 4UZ);
	CHECK(*numbers->element(3)->as_i64() == 4);

	CHECK(*target->root().at_pointer("/foo")->as_string() == "bar");
	CHECK_FALSE(target->root().at_pointer("/baz").has_value());
}

TEST_CASE(
	"json: apply_patch reports stable RFC 6902 error codes",
	"[json][patch]") {
	auto target = parse(R"({"a":{"b":1},"items":[1]})");
	REQUIRE(target.has_value());
	{
		auto patch = parse(R"({"op":"add","path":"/x","value":1})");
		REQUIRE(patch.has_value());
		auto result = conflux::json::validate_patch(patch->root());
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::invalid_patch);
	}
	{
		auto patch = parse(R"([{"path":"/x","value":1}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::validate_patch(patch->root());
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_op_missing);
		CHECK(result.error().operation_index == 0UZ);
	}
	{
		auto patch = parse(R"([{"op":"bogus","path":"/x"}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::validate_patch(patch->root());
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_op_unknown);
	}
	{
		auto patch = parse(R"([{"op":"remove","path":"/items/2"}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::apply_patch(*target, *patch);
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_array_index_out_of_range);
		CHECK(result.error().pointer == "/items/2");
	}
	{
		auto patch = parse(R"([{"op":"move","from":"/a","path":"/a/b/c"}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::apply_patch(*target, *patch);
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_move_into_child);
	}
	{
		auto patch = parse(R"([{"op":"test","path":"/a/b","value":2}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::apply_patch(*target, *patch);
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_test_failed);
		CHECK(*target->root().at_pointer("/a/b")->as_i64() == 1);
	}
}

TEST_CASE(
	"json: apply_patch enforces limits and remove policy",
	"[json][patch]") {
	auto target = parse(R"({"a":1})");
	REQUIRE(target.has_value());
	{
		auto patch = parse(R"([{"op":"remove","path":""}])");
		REQUIRE(patch.has_value());
		auto result = conflux::json::apply_patch(*target, *patch);
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_remove_document_root);
	}
	{
		auto patch = parse(R"([{"op":"remove","path":"/missing"}])");
		REQUIRE(patch.has_value());
		auto result =
			conflux::json::apply_patch(*target, *patch, conflux::json::JsonPatchOptions{.allow_missing_remove = true});
		REQUIRE(result.has_value());
		CHECK(*result->root().at_pointer("/a")->as_i64() == 1);
	}
	{
		auto patch = parse(R"([{"op":"add","path":"/a/b","value":1}])");
		REQUIRE(patch.has_value());
		auto result =
			conflux::json::validate_patch(patch->root(), conflux::json::JsonPatchOptions{.max_pointer_depth = 1});
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_pointer_too_deep);
	}
	{
		auto patch = parse(R"([{"op":"test","path":"/a","value":1},{"op":"test","path":"/a","value":1}])");
		REQUIRE(patch.has_value());
		auto result =
			conflux::json::validate_patch(patch->root(), conflux::json::JsonPatchOptions{.max_operations = 1});
		REQUIRE(!result.has_value());
		CHECK(result.error().code == JsonIssueCode::patch_too_many_operations);
	}
}

TEST_CASE(
	"json dom prototype: policy factories name storage and validation model",
	"[json][dom]") {
	constexpr JsonDomPolicy view = JsonDomPolicy::view_first();
	static_assert(view.input == JsonDomInputOwnership::borrowed_view);
	static_assert(view.storage == JsonDomStorageModel::standalone_document);
	static_assert(view.strings == JsonDomStringModel::view_unescaped_copy_decoded);
	static_assert(view.numbers == JsonDomNumberModel::preserve_lexeme_parse_on_access);
	static_assert(view.utf8 == JsonDomUtf8Model::strict_validate_on_parse);
	static_assert(view.errors == JsonDomErrorModel::expected_json_error);
	static_assert(view.object_index == JsonDomObjectIndexModel::preserve_order_warm_hash_on_demand);

	constexpr JsonDomPolicy arena = JsonDomPolicy::arena_reuse();
	static_assert(arena.storage == JsonDomStorageModel::reusable_arena);
	static_assert(arena.input == JsonDomInputOwnership::owned_copy);
}

TEST_CASE(
	"json dom prototype: standalone parse facade preserves view and copy choices",
	"[json][dom]") {
	std::string stable = R"({"name":"view","n":7})";
	auto view_doc = parse_dom(std::string_view{stable}, JsonDomPolicy::view_first());
	REQUIRE(view_doc.has_value());
	CHECK(*view_doc->root().as_object()->member("name")->as_string() == "view");

	auto copy_doc = parse_dom(std::string_view{stable}, JsonDomPolicy::owning_document());
	REQUIRE(copy_doc.has_value());
	CHECK(*copy_doc->root().as_object()->member("n")->as_number()->to_i64() == 7LL);

	auto moved_doc = parse_dom(std::string{R"({"name":"moved"})"});
	REQUIRE(moved_doc.has_value());
	CHECK(*moved_doc->root().as_object()->member("name")->as_string() == "moved");
}

TEST_CASE(
	"json dom prototype: arena facade names reusable storage path",
	"[json][dom]") {
	JsonArena arena;
	auto doc = parse_dom(arena, std::string_view{R"({"ok":true})"}, JsonDomPolicy::arena_reuse());
	REQUIRE(doc.has_value());
	CHECK(*doc->root().as_object()->member("ok")->as_bool());

	arena.reset();
	auto borrowed = parse_dom(arena, std::string_view{R"([1,2,3])"}, JsonDomPolicy::arena_borrowed());
	REQUIRE(borrowed.has_value());
	CHECK(borrowed->root().as_array()->size() == 3UZ);
}

TEST_CASE(
	"json dom prototype: policy mismatches fail through JsonError",
	"[json][dom]") {
	auto wrong_storage = parse_dom(std::string_view{"{}"}, JsonDomPolicy::arena_reuse());
	REQUIRE_FALSE(wrong_storage.has_value());
	CHECK(wrong_storage.error().stage == JsonStage::parse);
	CHECK(wrong_storage.error().code == JsonIssueCode::constraint_violation);

	auto unsafe_borrow = parse_dom(std::string{"{}"}, JsonDomPolicy::view_first());
	REQUIRE_FALSE(unsafe_borrow.has_value());
	CHECK(unsafe_borrow.error().code == JsonIssueCode::constraint_violation);
}
