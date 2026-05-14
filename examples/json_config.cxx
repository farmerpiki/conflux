import std;
import conflux.types;
import conflux.json;

using namespace conflux::json;
using std::println;

struct RateLimitConfig {
	i64 requests_per_minute{};
	Opt<i64> burst{};
};

template<>
struct JsonMembers<RateLimitConfig> {
	static constexpr auto members() {
		return Tup{
			json_member("requests_per_minute", &RateLimitConfig::requests_per_minute),
			json_member("burst", &RateLimitConfig::burst),
		};
	}
	static constexpr SV type_name() { return "RateLimitConfig"; }
};

struct ServiceConfig {
	S host;
	i64 port{};
	bool tls{};
	RateLimitConfig rate_limit{};
	Opt<S> log_level{};
};

template<>
struct JsonMembers<ServiceConfig> {
	static constexpr auto members() {
		return Tup{
			json_member("host", &ServiceConfig::host),
			json_member("port", &ServiceConfig::port),
			json_member("tls", &ServiceConfig::tls),
			json_member("rate_limit", &ServiceConfig::rate_limit),
			json_member("log_level", &ServiceConfig::log_level),
		};
	}
	static constexpr SV type_name() { return "ServiceConfig"; }
};

static S display_path(
	JsonError const &e) {
	if (!e.path.empty()) {
		return e.path.to_pointer();
	}
	if (e.member_name) {
		return S{"/"} + *e.member_name;
	}
	return "(root)";
}

static void print_json_error(
	SV context,
	JsonError const &e) {
	S const path = display_path(e);
	if (e.source) {
		println(
			"{}: {} at {} (line {}, column {}, byte {})",
			context,
			e.message,
			path,
			e.source->line,
			e.source->column,
			e.source->offset);
		return;
	}
	println("{}: {} at {}", context, e.message, path);
}

static expected<ServiceConfig, JsonError> load_config(
	SV input) {
	JsonParseOptions parse_opts{
		.duplicate_key = DuplicateKeyPolicy::reject,
		.warm_threshold = 16u,
		.mode = ParseMode::json5,
	};
	JsonDecodeOptions decode_opts{.unknown_members = UnknownMemberPolicy::reject};

	// parse_copy owns the bytes inside the Document. Use parse_view only when
	// the caller guarantees that the input buffer outlives every borrowed value.
	auto doc = parse_copy(input, parse_opts);
	if (!doc) {
		return unexpected(move(doc).error());
	}

	// schema_for<T>() is intentionally lite: useful at the API boundary before
	// the stricter typed decode gives exact field/path errors.
	auto schema = schema_for<ServiceConfig>();
	if (!schema) {
		return unexpected(move(schema).error());
	}
	if (auto ok = validate(doc->root(), schema->root()); !ok) {
		return unexpected(move(ok).error());
	}

	return decode<ServiceConfig>(*doc, decode_opts);
}

static void example_valid_config() {
	println("--- JSON config boundary ---");
	constexpr SV input = R"({
		// JSON5 subset: comments, unquoted keys, single quotes, trailing comma.
		host: '127.0.0.1',
		port: 8080,
		tls: false,
		rate_limit: {
			requests_per_minute: 1200,
			burst: 200,
		},
		log_level: 'debug',
	})";

	auto cfg = load_config(input);
	if (!cfg) {
		print_json_error("config load failed", cfg.error());
		return;
	}

	println(
		"listen {}:{} tls={} rpm={} burst={} log_level={}",
		cfg->host,
		cfg->port,
		cfg->tls,
		cfg->rate_limit.requests_per_minute,
		cfg->rate_limit.burst.value_or(0),
		cfg->log_level.value_or("info"));
}

static void example_invalid_config() {
	println("\n--- config error path ---");
	constexpr SV input = R"({
		host: '127.0.0.1',
		port: '8080',
		tls: false,
		rate_limit: { requests_per_minute: 1200 },
	})";

	auto cfg = load_config(input);
	if (!cfg) {
		print_json_error("config load failed", cfg.error());
	}
}

int main() {
	example_valid_config();
	example_invalid_config();
}
