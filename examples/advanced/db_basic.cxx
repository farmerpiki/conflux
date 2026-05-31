// conflux.pg basic example.
//
// Connects to PostgreSQL via PG_CONNINFO, runs a SELECT, prints rows.
// Uses file_io's conflux::file_io::block_on helper to drive a single-thread io_uring.
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.pg;

using namespace conflux::pg;
namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}

} // namespace
int main() {
	char const *raw = std::getenv("PG_CONNINFO");
	if (raw == nullptr || *raw == '\0') {
		std::println(std::cerr, "set PG_CONNINFO, e.g. host=/var/run/postgresql user=postgres dbname=postgres");
		return 2;
	}

	::io_uring ring{};
	if (::io_uring_queue_init(64, &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}
	CompletionTable ct;
	conflux::file_io::FileReader reader{&ring, &ct, [](std::uint32_t s, std::uint32_t g) noexcept {
											return pack_ud(s, g);
										}};
	conflux::file_io::CurrentFileReaderScope const scope{&reader};

	try {
		auto conn = conflux::file_io::block_on(reader, Connection::connect({.conninfo = raw}));
		std::println("connected — backend pid {}, server {}", conn->backend_pid(), conn->server_version());

		Params p;
		p.add(std::int64_t{3});
		auto rs = conflux::file_io::block_on(
			reader,
			conn->query("SELECT i, 'row #' || i AS label FROM generate_series(1,$1) AS i", std::move(p)));
		std::println("rows: {} cols: {}", rs.rows(), rs.cols());
		for (auto row: rs) {
			std::println("  {} = {}", row.as<std::int64_t>(0), row.as<std::string_view>(1));
		}
		conn->close();
	} catch (std::exception const &e) {
		std::println(std::cerr, "error: {}", e.what());
		::io_uring_queue_exit(&ring);
		return 1;
	}

	::io_uring_queue_exit(&ring);
}
