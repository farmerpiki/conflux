// Plain TU - not a module unit.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;

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

constexpr std::string_view kFortyMemberObject =
	R"({"k00":0,"k01":1,"k02":2,"k03":3,"k04":4,"k05":5,"k06":6,"k07":7,"k08":8,"k09":9,"k10":10,"k11":11,"k12":12,"k13":13,"k14":14,"k15":15,"k16":16,"k17":17,"k18":18,"k19":19,"k20":20,"k21":21,"k22":22,"k23":23,"k24":24,"k25":25,"k26":26,"k27":27,"k28":28,"k29":29,"k30":30,"k31":31,"k32":32,"k33":33,"k34":34,"k35":35,"k36":36,"k37":37,"k38":38,"k39":39})";

} // namespace

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
	auto doc = arena.parse_into(kFortyMemberObject);
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

	auto doc3 = arena.parse_into(kFortyMemberObject);
	REQUIRE(doc3.has_value());
	auto warm_after_reset = doc3->warm_member_index(doc3->root());
	REQUIRE(warm_after_reset.has_value());
	CHECK(hash_resource.alloc_count > 0);
	CHECK(hash_resource.alloc_bytes > 0);
}

TEST_CASE(
	"phase5: warm_member_indices budget counts pointer cache",
	"[phase5]") {
	CountingResource hash_resource{};
	auto measured_doc = parse_copy(kFortyMemberObject, {}, &hash_resource);
	REQUIRE(measured_doc.has_value());
	hash_resource.alloc_count = 0;
	hash_resource.dealloc_count = 0;
	hash_resource.alloc_bytes = 0;
	REQUIRE(measured_doc->warm_member_index(measured_doc->root()).has_value());
	REQUIRE(hash_resource.alloc_bytes > 1);
	std::size_t const measured_hash_bytes = hash_resource.alloc_bytes;

	measured_doc = {};
	hash_resource.alloc_count = 0;
	hash_resource.dealloc_count = 0;
	hash_resource.alloc_bytes = 0;
	auto limited_doc = parse_copy(kFortyMemberObject, {}, &hash_resource);
	REQUIRE(limited_doc.has_value());
	hash_resource.alloc_count = 0;
	hash_resource.dealloc_count = 0;
	hash_resource.alloc_bytes = 0;
	auto warm = limited_doc->warm_member_indices(
		WarmIndexOptions{
			.max_objects = std::numeric_limits<std::size_t>::max(),
			.max_extra_bytes = measured_hash_bytes - 1});
	REQUIRE(warm.has_value());
	CHECK(hash_resource.alloc_count == 0);
	CHECK(hash_resource.alloc_bytes == 0);
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
