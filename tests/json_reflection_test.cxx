module;
export module conflux.tests.json_reflect;

import conflux.json;
import conflux.json.reflect;
import conflux.json.reflect_provider;
import conflux.types;
import std;

using namespace conflux::json;
// ---------------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------------

struct Point {
	int x{0};
	int y{0};
};
struct Config {
	[[= conflux::json::name("host_name")]] std::string host;
	int port{0};
	bool tls{false};
	std::optional<std::string> label{};
};
struct WithSkip {
	std::string name;
	[[= conflux::json::skip{}]] int internal_id{99};
	int value{0};
};
struct Nested {
	std::string id;
	Point origin{};
};
struct WithArrays {
	std::vector<Point> points;
	std::array<int, 3> weights{};
};
struct NestedOptional {
	int required{0};
	std::optional<std::string> label{};
};
struct WithNestedOptional {
	NestedOptional child{};
};
// ---------------------------------------------------------------------------
// Minimal test runner
// ---------------------------------------------------------------------------

namespace {

int g_failures = 0;
int g_checks = 0;
void check_impl(
	bool cond,
	std::string_view test,
	std::string_view msg) {
	++g_checks;
	if (!cond) {
		++g_failures;
		std::println(std::cerr, "  FAIL [{}]: {}", test, msg);
	}
}
#define CHECK(cond)check_impl(!!(cond),__func__,#cond)
#define REQUIRE(cond)do{check_impl(!!(cond),__func__,#cond);if(!(cond))return;}while(false)
// ---------------------------------------------------------------------------
// Decode tests
// ---------------------------------------------------------------------------

void test_decode_plain_aggregate() {
	auto doc = *parse(R"({"x": 1, "y": 2})");
	auto p = decode<Point>(doc);
	REQUIRE(p.has_value());
	CHECK(p->x == 1);
	CHECK(p->y == 2);
}
void test_decode_with_name_annotation() {
	auto doc = *parse(R"({"host_name": "localhost", "port": 8080, "tls": true})");
	auto cfg = decode<Config>(doc);
	REQUIRE(cfg.has_value());
	CHECK(cfg->host == "localhost");
	CHECK(cfg->port == 8080);
	CHECK(cfg->tls == true);
	CHECK(!cfg->label.has_value());
}
void test_decode_optional_field_present() {
	auto doc = *parse(R"({"host_name": "example.com", "port": 443, "tls": true, "label": "prod"})");
	auto cfg = decode<Config>(doc);
	REQUIRE(cfg.has_value());
	CHECK(cfg->label == "prod");
}
void test_decode_missing_required_field() {
	auto doc = *parse(R"({"x": 1})");
	auto p = decode<Point>(doc);
	REQUIRE(!p.has_value());
	CHECK(p.error().code == JsonIssueCode::missing_member);
	CHECK(p.error().member_name == "y");
}
void test_decode_skip_field_present_rejected() {
	auto doc = *parse(R"({"name": "alice", "internal_id": 777, "value": 42})");
	auto w = decode<WithSkip>(doc);
	REQUIRE(!w.has_value());
	CHECK(w.error().code == JsonIssueCode::invalid_value);
}
void test_decode_skip_field_absent_ok() {
	auto doc = *parse(R"({"name": "alice", "value": 42})");
	auto w = decode<WithSkip>(doc);
	REQUIRE(w.has_value());
	CHECK(w->name == "alice");
	CHECK(w->internal_id == 99);
	CHECK(w->value == 42);
}
void test_decode_nested_aggregate() {
	auto doc = *parse(R"({"id": "root", "origin": {"x": 10, "y": 20}})");
	auto n = decode<Nested>(doc);
	REQUIRE(n.has_value());
	CHECK(n->id == "root");
	CHECK(n->origin.x == 10);
	CHECK(n->origin.y == 20);
}
void test_decode_array_members() {
	auto doc = *parse(R"({"points":[{"x":1,"y":2},{"x":3,"y":4}],"weights":[5,6,7]})");
	auto value = decode<WithArrays>(doc);
	REQUIRE(value.has_value());
	REQUIRE(value->points.size() == 2UZ);
	CHECK(value->points[0].x == 1);
	CHECK(value->points[0].y == 2);
	CHECK(value->points[1].x == 3);
	CHECK(value->points[1].y == 4);
	CHECK(value->weights[0] == 5);
	CHECK(value->weights[1] == 6);
	CHECK(value->weights[2] == 7);
}

void test_decode_unknown_member_ignore_policy() {
	auto doc = *parse(R"({"x": 1, "y": 2, "z": 3})");
	auto p = decode<Point>(doc, JsonDecodeOptions{.unknown_members = UnknownMemberPolicy::ignore});
	REQUIRE(p.has_value());
	CHECK(p->x == 1);
	CHECK(p->y == 2);
}
void test_boundary_reflect_provider_decode_ignore_unknown() {
	using Provider = conflux::json::boundary::NativeReflectJsonProvider;
	static_assert(conflux::json::boundary::JsonDecodeProvider<Provider, Point>);
	auto p = conflux::json::boundary::decode_with<Provider, Point>(
		R"({"x": 4, "y": 8, "ignored": true})",
		conflux::json::boundary::DecodeOptions{
			.copy_input = false,
			.unknown_members = conflux::json::boundary::UnknownMemberPolicy::ignore});
	REQUIRE(p.has_value());
	CHECK(p->x == 4);
	CHECK(p->y == 8);
}
void test_boundary_reflect_provider_dump() {
	using Provider = conflux::json::boundary::NativeReflectJsonProvider;
	static_assert(conflux::json::boundary::JsonDumpProvider<Provider, Point>);
	auto body = conflux::json::boundary::dump_with<Provider>(Point{9, 10});
	REQUIRE(body.has_value());
	CHECK(*body == R"({"x":9,"y":10})");
}
void test_reflect_direct_dump_with_name_annotation() {
	auto body = dump_reflect_direct(Config{.host = "srv", .port = 9000, .tls = false});
	REQUIRE(body.has_value());
	CHECK(*body == R"({"host_name":"srv","port":9000,"tls":false,"label":null})");
}
void test_reflect_direct_dump_nested() {
	Nested n{
		.id = "r",
		.origin = {5, 6}
    };
	auto body = dump_reflect_direct(n);
	REQUIRE(body.has_value());
	CHECK(*body == R"({"id":"r","origin":{"x":5,"y":6}})");
}
// ---------------------------------------------------------------------------
// Encode tests
// ---------------------------------------------------------------------------

void test_encode_plain_aggregate() {
	Point p{3, 7};
	ValueBuilder vb;
	auto res = JsonCodec<Point>::encode(vb, p);
	REQUIRE(res.has_value());
	auto doc = *std::move(vb).finish();
	auto obj = *doc.root().as_object();
	auto x = *obj.find_member("x");
	auto y = *obj.find_member("y");
	CHECK(*x.as_i64() == 3);
	CHECK(*y.as_i64() == 7);
}
void test_encode_with_name_annotation() {
	Config cfg{.host = "srv", .port = 9000, .tls = false};
	ValueBuilder vb;
	auto res = JsonCodec<Config>::encode(vb, cfg);
	REQUIRE(res.has_value());
	auto doc = *std::move(vb).finish();
	auto obj = *doc.root().as_object();
	REQUIRE(obj.find_member("host_name").has_value());
	CHECK(*obj.find_member("host_name")->as_string() == "srv");
	CHECK(!obj.find_member("host").has_value());
}
void test_encode_skip_field_absent() {
	WithSkip w{.name = "bob", .internal_id = 55, .value = 7};
	ValueBuilder vb;
	auto res = JsonCodec<WithSkip>::encode(vb, w);
	REQUIRE(res.has_value());
	auto doc = *std::move(vb).finish();
	auto obj = *doc.root().as_object();
	CHECK(obj.find_member("name").has_value());
	CHECK(obj.find_member("value").has_value());
	CHECK(!obj.find_member("internal_id").has_value());
}
void test_encode_nested_aggregate() {
	Nested n{
		.id = "r",
		.origin = {5, 6}
    };
	ValueBuilder vb;
	auto res = JsonCodec<Nested>::encode(vb, n);
	REQUIRE(res.has_value());
	auto doc = *std::move(vb).finish();
	auto s = *doc.dump();
	CHECK(s.find("\"origin\"") != std::string::npos);
	CHECK(s.find("\"x\"") != std::string::npos);
	CHECK(s.find("\"y\"") != std::string::npos);
}
// ---------------------------------------------------------------------------
// Round-trip tests
// ---------------------------------------------------------------------------

void test_roundtrip_point() {
	Point orig{-1, 42};
	ValueBuilder vb;
	auto enc = JsonCodec<Point>::encode(vb, orig);
	REQUIRE(enc.has_value());
	auto doc = *std::move(vb).finish();
	auto rt = decode<Point>(doc);
	REQUIRE(rt.has_value());
	CHECK(rt->x == orig.x);
	CHECK(rt->y == orig.y);
}
void test_roundtrip_config() {
	Config orig{.host = "example.com", .port = 443, .tls = true, .label = "staging"};
	ValueBuilder vb;
	auto enc = JsonCodec<Config>::encode(vb, orig);
	REQUIRE(enc.has_value());
	auto doc = *std::move(vb).finish();
	auto rt = decode<Config>(doc);
	REQUIRE(rt.has_value());
	CHECK(rt->host == orig.host);
	CHECK(rt->port == orig.port);
	CHECK(rt->tls == orig.tls);
	CHECK(rt->label == orig.label);
}
// ---------------------------------------------------------------------------
// Concept checks
// ---------------------------------------------------------------------------

void test_has_json_codec_concept() {
	static_assert(has_json_codec<Point>);
	static_assert(has_json_codec<Config>);
	static_assert(has_json_codec<WithSkip>);
	static_assert(has_json_codec<Nested>);
}
// ---------------------------------------------------------------------------
// Reader path
// ---------------------------------------------------------------------------

void test_reader_path_decode() {
	std::string_view input = R"({"x": 5, "y": -3})";
	JsonReader reader{input};
	auto p = decode<Point>(reader);
	REQUIRE(p.has_value());
	CHECK(p->x == 5);
	CHECK(p->y == -3);
}
void test_reader_path_decode_escaped_key() {
	std::string_view input = R"({"host\u005fname": "localhost", "port": 8080, "tls": true})";
	JsonReader reader{input};
	auto cfg = decode<Config>(reader);
	REQUIRE(cfg.has_value());
	CHECK(cfg->host == "localhost");
	CHECK(cfg->port == 8080);
	CHECK(cfg->tls == true);
	CHECK(!cfg->label.has_value());
}
void test_reader_path_decode_ignore_unknown_nested() {
	std::string_view input = R"({"x": 5, "ignored": {"nested": [1, 2, 3]}, "y": -3})";
	JsonReader reader{input};
	auto p = decode<Point>(reader, JsonDecodeOptions{.unknown_members = UnknownMemberPolicy::ignore});
	REQUIRE(p.has_value());
	CHECK(p->x == 5);
	CHECK(p->y == -3);
}
void test_reader_path_decode_array_members() {
	using Provider = conflux::json::boundary::NativeReflectJsonProvider;
	auto value = conflux::json::boundary::decode_with<Provider, WithArrays>(
		R"({"points":[{"x":1,"y":2},{"x":3,"y":4}],"weights":[5,6,7]})",
		conflux::json::boundary::DecodeOptions{.copy_input = false});
	REQUIRE(value.has_value());
	REQUIRE(value->points.size() == 2UZ);
	CHECK(value->points[0].x == 1);
	CHECK(value->points[1].y == 4);
	CHECK(value->weights[2] == 7);
}
void test_reader_path_duplicate_vector_last_wins_clears_previous() {
	JsonParseOptions opts;
	opts.duplicate_key = DuplicateKeyPolicy::last_wins;
	JsonReader reader{R"({"points":[{"x":1,"y":2},{"x":3,"y":4}],"points":[{"x":5,"y":6}],"weights":[7,8,9]})", opts};
	auto value = decode<WithArrays>(reader);
	REQUIRE(value.has_value());
	REQUIRE(value->points.size() == 1UZ);
	CHECK(value->points[0].x == 5);
	CHECK(value->points[0].y == 6);
	CHECK(value->weights[2] == 9);
}
void test_reader_path_duplicate_nested_last_wins_resets_missing_optional() {
	JsonParseOptions opts;
	opts.duplicate_key = DuplicateKeyPolicy::last_wins;
	JsonReader reader{R"({"child":{"required":1,"label":"old"},"child":{"required":2}})", opts};
	auto value = decode<WithNestedOptional>(reader);
	REQUIRE(value.has_value());
	CHECK(value->child.required == 2);
	CHECK(!value->child.label.has_value());
}

void test_reader_path_decode_unknown_rejected() {
	std::string_view input = R"({"x": 5, "ignored": {"nested": [1, 2, 3]}, "y": -3})";
	JsonReader reader{input};
	auto p = decode<Point>(reader);
	REQUIRE(!p.has_value());
	CHECK(p.error().code == JsonIssueCode::invalid_value);
	CHECK(p.error().member_name == "ignored");
}
void test_reader_path_duplicate_reject() {
	JsonReader reader{R"({"x": 1, "x": 2, "y": 3})"};
	auto p = decode<Point>(reader);
	REQUIRE(!p.has_value());
	CHECK(p.error().code == JsonIssueCode::duplicate_member);
	CHECK(p.error().member_name == "x");
}
void test_reader_path_duplicate_last_wins() {
	JsonParseOptions opts;
	opts.duplicate_key = DuplicateKeyPolicy::last_wins;
	JsonReader reader{R"({"x": 1, "x": 2, "y": 3})", opts};
	auto p = decode<Point>(reader);
	REQUIRE(p.has_value());
	CHECK(p->x == 2);
	CHECK(p->y == 3);
}
void test_reader_path_duplicate_first_wins() {
	JsonParseOptions opts;
	opts.duplicate_key = DuplicateKeyPolicy::first_wins;
	JsonReader reader{R"({"x": 1, "x": 2, "y": 3})", opts};
	auto p = decode<Point>(reader);
	REQUIRE(p.has_value());
	CHECK(p->x == 1);
	CHECK(p->y == 3);
}

} // namespace
// ---------------------------------------------------------------------------

export int run_tests() {
	using fn_t = void (*)();
	struct {
		fn_t fn;
		std::string_view name;
	} tests[] = {
		{										test_decode_plain_aggregate,"decode plain aggregate"																			 },
		{								   test_decode_with_name_annotation,                     "decode with name annotation"},
		{								 test_decode_optional_field_present,                   "decode optional field present"},
		{								 test_decode_missing_required_field,                   "decode missing required field"},
		{							test_decode_skip_field_present_rejected,              "decode skip field present rejected"},
		{								   test_decode_skip_field_absent_ok,                     "decode skip field absent ok"},
		{									   test_decode_nested_aggregate,                         "decode nested aggregate"},
		{										  test_decode_array_members,                            "decode array members"},
		{						   test_decode_unknown_member_ignore_policy,             "decode unknown member ignore policy"},
		{			   test_boundary_reflect_provider_decode_ignore_unknown, "boundary reflect provider decode ignore unknown"},
		{								test_boundary_reflect_provider_dump,                  "boundary reflect provider dump"},
		{					  test_reflect_direct_dump_with_name_annotation,        "reflect direct dump with name annotation"},
		{									test_reflect_direct_dump_nested,                      "reflect direct dump nested"},
		{										test_encode_plain_aggregate,                          "encode plain aggregate"},
		{								   test_encode_with_name_annotation,                     "encode with name annotation"},
		{									  test_encode_skip_field_absent,                        "encode skip field absent"},
		{									   test_encode_nested_aggregate,                         "encode nested aggregate"},
		{											   test_roundtrip_point,								"round-trip Point"},
		{											  test_roundtrip_config,                               "round-trip Config"},
		{										test_has_json_codec_concept,                          "has_json_codec concept"},
		{											test_reader_path_decode,                              "reader path decode"},
		{								test_reader_path_decode_escaped_key,                  "reader path decode escaped key"},
		{					  test_reader_path_decode_ignore_unknown_nested,        "reader path decode ignore unknown nested"},
		{							  test_reader_path_decode_array_members,                "reader path decode array members"},
		{        test_reader_path_duplicate_vector_last_wins_clears_previous,
		 "reader path duplicate vector last_wins clears previous"                                                              },
		{test_reader_path_duplicate_nested_last_wins_resets_missing_optional,
		 "reader path duplicate nested last_wins resets missing optional"                                                      },
		{						   test_reader_path_decode_unknown_rejected,             "reader path decode unknown rejected"},
		{								  test_reader_path_duplicate_reject,                    "reader path duplicate reject"},
		{							   test_reader_path_duplicate_last_wins,                 "reader path duplicate last_wins"},
		{							  test_reader_path_duplicate_first_wins,                "reader path duplicate first_wins"},
	};
	int saved = g_failures;
	for (auto const &t: tests) {
		int before = g_failures;
		t.fn();
		if (g_failures == before) {
			std::println("PASS: {}", t.name);
		} else {
			std::println("FAIL: {}", t.name);
		}
	}
	int total_failed = g_failures - saved;
	std::println(
		"{}/{} tests passed.",
		static_cast<int>(std::size(tests)) - total_failed,
		static_cast<int>(std::size(tests)));
	return total_failed;
}
