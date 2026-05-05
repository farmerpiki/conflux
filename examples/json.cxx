import std;
import conflux.types;
import conflux.json;

using namespace conflux::json;
using namespace std::literals;
using std::println, std::pair;

struct ApiResponse {
	S model;
	i64 tokens{};
	Opt<S> error;
};
template<> struct JsonMembers<ApiResponse> {
	static constexpr auto members() {
		return Tup{
			json_member("model", &ApiResponse::model),
			json_member("tokens", &ApiResponse::tokens),
			json_member("error", &ApiResponse::error),
		};
	}
};

static void example_parse_decode() {
	println("--- parse + decode<T> ---");
	auto doc = *parse(R"({"model":"gpt-4o","tokens":512})");
	auto resp = *decode<ApiResponse>(doc);
	println("model={} tokens={} error={}", resp.model, resp.tokens, resp.error.value_or("(none)"));
}

static void example_make_object() {
	println("\n--- make_object / make_array ---");
	auto doc = *make_object(
		pair{"model", "gpt-4o"sv},
		pair{"temperature", 0.7},
		pair{"max_tokens", 4096},
		pair{"stream", true});
	println("{}", *doc.dump(JsonDumpOptions{.pretty = true}));
}

static void example_json5() {
	println("\n--- JSON5 ---");
	SV input =
		"{\n"
		"  // server config\n"
		"  host: 'localhost',\n"
		"  port: 8080,\n"
		"  tls: true,\n"
		"  /* timeout in ms */\n"
		"  timeout: 30000,\n"
		"}";
	JsonParseOptions opts{.mode = ParseMode::json5};
	auto result = parse(input, opts);
	if (!result) { println("  parse error: {}", result.error().message); return; }
	auto obj = *result->root().as_object();
	println("  host={} port={} tls={} timeout={}",
		*obj.member("host")->as_string(),
		*obj.member("port")->as_i64(),
		*obj.member("tls")->as_bool(),
		*obj.member("timeout")->as_i64());
}

static void example_pull_parser() {
	println("\n--- JsonReader (pull parser) ---");
	SV input = R"([{"id":1,"name":"alice"},{"id":2,"name":"bob"}])";
	JsonReader reader{input};
	SZ count{};
	while (auto ev = reader.next()) {
		if (!*ev) break;
		if (**ev == JsonReader::Event::string_value) ++count;
	}
	println("found {} string values", count);
}

struct CountHandler : JsonDefaultHandler {
	SZ keys{};
	SZ strings{};
	expected<void, JsonError> on_key(SV) { ++keys; return {}; }
	expected<void, JsonError> on_string(SV) { ++strings; return {}; }
};

static void example_sax() {
	println("\n--- parse_sax ---");
	SV input = R"({"a":"hello","b":"world","c":42})";
	CountHandler h;
	(void)parse_sax(input, h);
	println("keys={} strings={}", h.keys, h.strings);
}

static void example_ndjson() {
	println("\n--- NdjsonRange ---");
	SV input = "{\"line\":1}\n{\"line\":2}\n{\"line\":3}\n";
	NdjsonRange range{input};
	SZ count{};
	for (auto const &result: range) {
		if (result.has_value()) ++count;
	}
	println("parsed {} NDJSON lines", count);
}

static void example_arena() {
	println("\n--- JsonArena (cross-parse reuse) ---");
	JsonArena arena{JsonArenaOptions{.initial_slab = 4096}};
	for (auto i : {1, 2, 3}) {
		auto input = std::format(R"({{"n":{}}})", i);
		auto doc = *arena.parse_into(input);
		auto obj = *doc.root().as_object();
		println("  n={}", *obj.member("n")->as_i64());
	}
	println("  slab used: {} / {} bytes", arena.slab_used(), arena.slab_capacity());
	arena.reset();
	println("  after reset: {} used", arena.slab_used());
}

static void example_schema_validate() {
	println("\n--- schema_for + validate ---");
	auto schema_doc = *schema_for<ApiResponse>();
	println("schema: {}", *schema_doc.dump(JsonDumpOptions{.pretty = true}));

	auto good = *parse(R"({"model":"x","tokens":1})");
	auto bad = *parse(R"({"model":123,"tokens":1})");

	auto r1 = validate(good.root(), schema_doc.root());
	println("valid input:   {}", r1.has_value() ? "OK" : r1.error().message);

	auto r2 = validate(bad.root(), schema_doc.root());
	println("invalid input: {}", r2.has_value() ? "OK" : r2.error().message);
}

int main() {
	example_parse_decode();
	example_make_object();
	example_json5();
	example_pull_parser();
	example_sax();
	example_ndjson();
	example_arena();
	example_schema_validate();
}
