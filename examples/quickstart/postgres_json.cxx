#include <cstdio>

import conflux.http;
import conflux.pg;
import std;

namespace http = conflux::http;
namespace pg = conflux::pg;

struct DbStatus {
	std::string database;
	std::int64_t backend_pid{};
};

template<>
struct JsonMembers<DbStatus> {
	static constexpr auto members() {
		return std::tuple{
			json_member("database", &DbStatus::database),
			json_member("backend_pid", &DbStatus::backend_pid),
		};
	}
	static constexpr std::string_view type_name() { return "DbStatus"; }
};

int main() {
	auto const *conninfo = std::getenv("PG_CONNINFO");
	if (conninfo == nullptr || *conninfo == '\0') {
		std::println(stderr, "set PG_CONNINFO, for example postgresql:///postgres?user=postgres");
		return 2;
	}

	auto app = http::app();
	auto pool = pg::Pool::create({
		.conn = {.conninfo = conninfo},
		.min_connections = 1,
		.max_connections = 4,
	});
	app.state(pool);

	app.get("/db/status", [](http::State<std::shared_ptr<pg::Pool>> pool) -> http::Task<http::Json<DbStatus>> {
		auto lease = co_await (*pool)->acquire();
		auto rows =
			co_await lease->query("SELECT current_database() AS database, pg_backend_pid()::int8 AS backend_pid");
		co_return http::json(
			DbStatus{
				.database = std::string{rows[0].as<std::string_view>(rows.column("database"))},
				.backend_pid = rows[0].as<std::int64_t>(rows.column("backend_pid")),
			});
	});

	auto const status = http::run(std::move(app), {.port = 9120});
	pool->close();
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
