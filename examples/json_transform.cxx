import std;
import conflux.types;
import conflux.json;

using namespace conflux::json;
using std::println;

struct RouteSample {
	std::string route;
	std::int64_t latency_us{};
	bool ok{};
};

template<>
struct JsonMembers<RouteSample> {
	static constexpr auto members() {
		return std::tuple{
			json_member("route", &RouteSample::route),
			json_member("latency_us", &RouteSample::latency_us),
			json_member("ok", &RouteSample::ok),
		};
	}
	static constexpr std::string_view type_name() { return "RouteSample"; }
};

struct RouteStats {
	std::int64_t count{};
	std::int64_t failures{};
	std::int64_t total_latency_us{};
	std::int64_t max_latency_us{};
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

static expected<std::map<std::string, RouteStats>, JsonError> aggregate_routes(
	std::string_view ndjson) {
	std::map<std::string, RouteStats> routes;
	JsonDecodeOptions decode_opts{.unknown_members = UnknownMemberPolicy::ignore};

	for (auto const &row: NdjsonRange{ndjson}) {
		if (!row) {
			return unexpected(row.error());
		}
		auto sample = decode<RouteSample>(*row, decode_opts);
		if (!sample) {
			return unexpected(move(sample).error());
		}

		auto &stats = routes[sample->route];
		++stats.count;
		stats.failures += sample->ok ? 0 : 1;
		stats.total_latency_us += sample->latency_us;
		stats.max_latency_us = std::max(stats.max_latency_us, sample->latency_us);
	}

	return routes;
}

static expected<Document, JsonError> build_summary(
	std::map<std::string, RouteStats> const &routes) {
	ValueBuilder builder;
	auto root = builder.begin_object();
	if (!root) {
		return unexpected(move(root).error());
	}

	std::int64_t total_samples{};
	std::int64_t total_failures{};
	for (auto const &[route, stats]: routes) {
		total_samples += stats.count;
		total_failures += stats.failures;
	}

	if (auto ok = root->insert_i64("samples", total_samples); !ok) {
		return unexpected(move(ok).error());
	}
	if (auto ok = root->insert_i64("failures", total_failures); !ok) {
		return unexpected(move(ok).error());
	}

	auto by_route = root->insert_object("routes");
	if (!by_route) {
		return unexpected(move(by_route).error());
	}
	for (auto const &[route, stats]: routes) {
		auto route_obj = by_route->insert_object(route);
		if (!route_obj) {
			return unexpected(move(route_obj).error());
		}
		if (auto ok = route_obj->insert_i64("count", stats.count); !ok) {
			return unexpected(move(ok).error());
		}
		if (auto ok = route_obj->insert_i64("failures", stats.failures); !ok) {
			return unexpected(move(ok).error());
		}
		if (auto ok = route_obj->insert_i64("avg_latency_us", stats.total_latency_us / stats.count); !ok) {
			return unexpected(move(ok).error());
		}
		if (auto ok = route_obj->insert_i64("max_latency_us", stats.max_latency_us); !ok) {
			return unexpected(move(ok).error());
		}
		move(*route_obj).commit();
	}
	move(*by_route).commit();

	auto slow_routes = root->insert_array("slow_routes");
	if (!slow_routes) {
		return unexpected(move(slow_routes).error());
	}
	for (auto const &[route, stats]: routes) {
		if (stats.max_latency_us >= 10'000) {
			if (auto ok = slow_routes->append_string(route); !ok) {
				return unexpected(move(ok).error());
			}
		}
	}
	move(*slow_routes).commit();

	move(*root).commit();
	return move(builder).finish();
}

static void example_transform() {
	std::println("--- NDJSON aggregate + builder output ---");
	constexpr std::string_view input =
		R"({"route":"/v1/chat","latency_us":7100,"ok":true})"
		"\n"
		R"({"route":"/v1/chat","latency_us":12100,"ok":false})"
		"\n"
		R"({"route":"/v1/embed","latency_us":1800,"ok":true})"
		"\n";

	auto routes = aggregate_routes(input);
	if (!routes) {
		print_json_error("aggregate failed", routes.error());
		return;
	}

	auto summary = build_summary(*routes);
	if (!summary) {
		print_json_error("summary build failed", summary.error());
		return;
	}
	std::println("{}", *summary->dump(JsonDumpOptions{.pretty = true}));
}

int main() {
	example_transform();
}
