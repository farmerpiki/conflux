// Advanced PostgreSQL runtime example.
//
// This intentionally uses lower-level DB/file APIs so the DB runtime setup is
// visible. Quickstart HTTP examples stay on the curated conflux.http facade.
//
// Build and run:
//   cmake --build build/release-clang-libcxx --target conflux_advanced_postgres
//   PG_CONNINFO='postgresql:///postgres?user=postgres' \
//     build/release-clang-libcxx/conflux_advanced_postgres
#include <liburing.h>

import std;
import conflux.file_io;
import conflux.pg;

namespace pg = conflux::pg;

namespace {

constexpr std::uint64_t pack_user_data(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}

class DbRuntime {
public:
	DbRuntime() {
		if (::io_uring_queue_init(64, &ring_, 0) < 0) {
			throw std::runtime_error{"io_uring_queue_init failed"};
		}
	}

	~DbRuntime() { ::io_uring_queue_exit(&ring_); }

	DbRuntime(DbRuntime const &) = delete;
	DbRuntime &operator =(DbRuntime const &) = delete;

	FileReader &reader() noexcept { return reader_; }

private:
	::io_uring ring_{};
	CompletionTable completions_{};
	FileReader reader_{&ring_, &completions_, pack_user_data};
	CurrentFileReaderScope scope_{&reader_};
};

} // namespace

int main() {
	auto const *conninfo = std::getenv("PG_CONNINFO");
	if (conninfo == nullptr || *conninfo == '\0') {
		std::println(std::cerr, "set PG_CONNINFO, for example postgresql:///postgres?user=postgres");
		return 2;
	}

	try {
		DbRuntime runtime;
		auto conn = block_on(runtime.reader(), pg::Connection::connect({.conninfo = conninfo}));

		pg::Params params;
		params.add(std::string_view{"conflux"});
		auto rows = block_on(
			runtime.reader(),
			conn->query(
				"SELECT $1::text AS name, current_database() AS database, "
				"pg_backend_pid()::int8 AS backend_pid",
				std::move(params)));

		auto row = rows[0];
		std::println(
			"hello {} from database={} backend_pid={}",
			row.as<std::string_view>(rows.column("name")),
			row.as<std::string_view>(rows.column("database")),
			row.as<std::int64_t>(rows.column("backend_pid")));

		conn->close();
	} catch (std::exception const &e) {
		std::println(std::cerr, "postgres quickstart failed: {}", e.what());
		return 1;
	}
}
