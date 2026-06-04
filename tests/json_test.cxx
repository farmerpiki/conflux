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
