// conflux.pg pool example.
//
// Acquires two leases from a Pool, runs a parametrised query on each.
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
		std::println(std::cerr, "set PG_CONNINFO");
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

	auto pool = Pool::create({
		.conn = {.conninfo = raw},
		.min_connections = 2,
		.max_connections = 4,
	});

	try {
		auto a = conflux::file_io::block_on(reader, pool->acquire());
		auto b = conflux::file_io::block_on(reader, pool->acquire());

		Params pa;
		pa.add(std::string_view{"alpha"});
		auto ra = conflux::file_io::block_on(
			reader,
			a->query("SELECT $1::text || ' from pid ' || pg_backend_pid()", std::move(pa)));
		std::println("a: {}", ra[0].as<std::string_view>(0));

		Params pb;
		pb.add(std::string_view{"beta"});
		auto rb = conflux::file_io::block_on(
			reader,
			b->query("SELECT $1::text || ' from pid ' || pg_backend_pid()", std::move(pb)));
		std::println("b: {}", rb[0].as<std::string_view>(0));
	} catch (std::exception const &e) { std::println(std::cerr, "error: {}", e.what()); }

	pool->close();
	::io_uring_queue_exit(&ring);
}
