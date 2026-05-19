import std;
import conflux.types;
import conflux.json;

using namespace conflux::json;
struct ApiEvent {
	std::int64_t id{};
	std::string type;
	std::int64_t tokens{};
	std::optional<std::string> error{};
};

template<>
struct JsonMembers<ApiEvent> {
	static constexpr auto members() {
		return std::tuple{
			json_member("id", &ApiEvent::id),
			json_member("type", &ApiEvent::type),
			json_member("tokens", &ApiEvent::tokens),
			json_member("error", &ApiEvent::error),
		};
	}
	static constexpr std::string_view type_name() { return "ApiEvent"; }
};

struct IngestStats {
	std::size_t rows{};
	std::int64_t token_total{};
	std::size_t errors{};
};

static std::string display_path(
	JsonError const &e) {
	if (!e.path.empty()) {
		return e.path.to_pointer();
	}
	if (e.member_name) {
		return std::string{"/"} + *e.member_name;
	}
	return "(root)";
}

static void print_json_error(
	std::string_view context,
	JsonError const &e) {
	std::string const path = display_path(e);
	if (e.source) {
		std::println(
			"{}: {} at {} (line {}, column {}, byte {})",
			context,
			e.message,
			path,
			e.source->line,
			e.source->column,
			e.source->offset);
		return;
	}
	std::println("{}: {} at {}", context, e.message, path);
}

static expected<IngestStats, JsonError> ingest_ndjson(
	std::string_view input) {
	JsonParseOptions parse_opts{
		.duplicate_key = DuplicateKeyPolicy::reject,
		.warm_threshold = 8u,
	};
	JsonDecodeOptions decode_opts{.unknown_members = UnknownMemberPolicy::ignore};
	IngestStats stats;

	// NdjsonRange parses one borrowed document per non-empty line. The backing
	// input buffer must remain stable for the current iteration.
	for (auto const &line: NdjsonRange{input, parse_opts}) {
		if (!line) {
			return unexpected(line.error());
		}
		auto event = decode<ApiEvent>(*line, decode_opts);
		if (!event) {
			return unexpected(move(event).error());
		}

		++stats.rows;
		stats.token_total += event->tokens;
		if (event->error) {
			++stats.errors;
		}
	}

	return stats;
}

static void example_ndjson_ingest() {
	std::println("--- NDJSON ingest ---");
	constexpr std::string_view input =
		R"({"id":1,"type":"completion","tokens":312})"
		"\n"
		R"({"id":2,"type":"completion","tokens":128,"error":"rate_limited","ignored":true})"
		"\n"
		R"({"id":3,"type":"embedding","tokens":64})"
		"\n";

	auto stats = ingest_ndjson(input);
	if (!stats) {
		print_json_error("ingest failed", stats.error());
		return;
	}
	std::println("rows={} tokens={} errored_rows={}", stats->rows, stats->token_total, stats->errors);
}

static void example_reader_sequence() {
	std::println("\n--- JsonReader typed sequence ---");
	JsonReader reader{
		R"({"id":10,"type":"completion","tokens":7})"
		R"({"id":11,"type":"completion","tokens":9,"error":null})"};

	auto first = decode_next<ApiEvent>(reader);
	auto second = decode_next<ApiEvent>(reader);
	if (!first) {
		print_json_error("first event", first.error());
		return;
	}
	if (!second) {
		print_json_error("second event", second.error());
		return;
	}
	std::println("event {} + event {} -> {} tokens", first->id, second->id, first->tokens + second->tokens);
}

int main() {
	example_ndjson_ingest();
	example_reader_sequence();
}
