import std;
import conflux.types;
import conflux.json;

using namespace conflux::json;

struct ApiResponse {
	std::string model;
	std::int64_t tokens{};
	std::optional<std::string> error;
};
template<>
struct JsonMembers<ApiResponse> {
	static constexpr auto members() {
		return std::tuple{
			json_member("model", &ApiResponse::model),
			json_member("tokens", &ApiResponse::tokens),
			json_member("error", &ApiResponse::error),
		};
	}
};
static void example_parse_decode() {
	std::println("--- parse + decode<T> ---");
	auto doc = *parse_view(R"({"model":"gpt-4o","tokens":512})");
	auto resp = *decode<ApiResponse>(doc);
	std::println("model={} tokens={} error={}", resp.model, resp.tokens, resp.error.value_or("(none)"));
}
static void example_make_object() {
	std::println("\n--- make_object / make_array ---");
	auto doc = *make_object(
		std::pair{"model", std::string_view{"gpt-4o"}},
		std::pair{"temperature", 0.7},
		std::pair{"max_tokens", 4096},
		std::pair{"stream", true});
	std::println("{}", *doc.dump(JsonDumpOptions{.pretty = true}));
}
static void example_json5() {
	std::println("\n--- JSON5 ---");
	std::string_view input =
		"{\n"
		"  // server config\n"
		"  host: 'localhost',\n"
		"  port: 8080,\n"
		"  tls: true,\n"
		"  /* timeout in ms */\n"
		"  timeout: 30000,\n"
		"}";
	JsonParseOptions opts{.mode = ParseMode::json5};
	auto result = parse_view(input, opts);
	if (!result) {
		std::println("  parse error: {}", result.error().message);
		return;
	}
	auto obj = *result->root().as_object();
	std::println(
		"  host={} port={} tls={} timeout={}",
		*obj.member("host")->as_string(),
		*obj.member("port")->as_i64(),
		*obj.member("tls")->as_bool(),
		*obj.member("timeout")->as_i64());
}
static void example_pull_parser() {
	std::println("\n--- JsonReader (pull parser) ---");
	std::string_view input = R"([{"id":1,"name":"alice"},{"id":2,"name":"bob"}])";
	JsonReader reader{input};
	std::size_t count{};
	while (auto ev = reader.next()) {
		if (!*ev) {
			break;
		}
		if (**ev == JsonReader::Event::string_value) {
			++count;
		}
	}
	std::println("found {} string values", count);
}
struct CountHandler : JsonDefaultHandler {
	std::size_t keys{};
	std::size_t strings{};
	std::expected<void, JsonError> on_key(
		std::string_view) {
		++keys;
		return {};
	}
	std::expected<void, JsonError> on_string(
		std::string_view) {
		++strings;
		return {};
	}
};
static void example_sax() {
	std::println("\n--- parse_sax ---");
	std::string_view input = R"({"a":"hello","b":"world","c":42})";
	CountHandler h;
	auto sax = parse_sax(input, h);
	if (!sax) {
		std::println("parse_sax error: {}", sax.error().message);
		return;
	}
	std::println("keys={} strings={}", h.keys, h.strings);
}
static void example_ndjson() {
	std::println("\n--- NdjsonRange ---");
	std::string_view input = "{\"line\":1}\n{\"line\":2}\n{\"line\":3}\n";
	NdjsonRange range{input};
	std::size_t count{};
	for (auto const &result: range) {
		if (result.has_value()) {
			++count;
		}
	}
	std::println("parsed {} NDJSON lines", count);
}
static void example_arena() {
	std::println("\n--- JsonArena (cross-parse reuse) ---");
	JsonArena arena{JsonArenaOptions{.initial_slab = 4096}};
	for (auto i: {1, 2, 3}) {
		auto input = std::format(R"({{"n":{}}})", i);
		auto doc = *arena.parse_into(input);
		auto obj = *doc.root().as_object();
		std::println("  n={}", *obj.member("n")->as_i64());
	}
	std::println("  slab used: {} / {} bytes", arena.slab_used(), arena.slab_capacity());
	arena.reset();
	std::println("  after reset: {} used", arena.slab_used());
}
static void example_schema_validate() {
	std::println("\n--- schema_for + validate ---");
	auto schema_doc = *schema_for<ApiResponse>();
	std::println("schema: {}", *schema_doc.dump(JsonDumpOptions{.pretty = true}));

	auto good = *parse_view(R"({"model":"x","tokens":1})");
	auto bad = *parse_view(R"({"model":123,"tokens":1})");

	auto r1 = validate(good.root(), schema_doc.root());
	std::println("valid input:   {}", r1.has_value() ? "OK" : r1.error().message);

	auto r2 = validate(bad.root(), schema_doc.root());
	std::println("invalid input: {}", r2.has_value() ? "OK" : r2.error().message);
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
