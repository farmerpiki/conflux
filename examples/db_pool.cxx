// conflux.db pool example.
//
// Acquires two leases from a Pool, runs a parametrised query on each.
#include <liburing.h>

import std;
import conflux.work;
import conflux.file_io;
import conflux.db;

using namespace std;
using namespace conflux::db;

namespace {

constexpr uint64_t pack_ud(
	uint32_t slot,
	uint32_t gen) noexcept {
	return (static_cast<uint64_t>(gen) << 32U) | slot;
}

} // namespace

int main() {
	char const *raw = ::getenv("PG_CONNINFO");
	if (raw == nullptr || *raw == '\0') {
		println(cerr, "set PG_CONNINFO");
		return 2;
	}

	::io_uring ring{};
	if (::io_uring_queue_init(64, &ring, 0) < 0) {
		println(cerr, "io_uring_queue_init failed");
		return 1;
	}
	CompletionTable ct;
	FileReader reader{&ring, &ct, [](uint32_t s, uint32_t g) noexcept { return pack_ud(s, g); }};
	CurrentFileReaderScope const scope{&reader};

	auto pool = Pool::create({
		.conn = {.conninfo = raw},
		.min_connections = 2,
		.max_connections = 4,
	});

	try {
		auto a = block_on(reader, pool->acquire());
		auto b = block_on(reader, pool->acquire());

		Params pa;
		pa.add(string_view{"alpha"});
		auto ra = block_on(reader, a->query("SELECT $1::text || ' from pid ' || pg_backend_pid()", move(pa)));
		println("a: {}", ra[0].as<string_view>(0));

		Params pb;
		pb.add(string_view{"beta"});
		auto rb = block_on(reader, b->query("SELECT $1::text || ' from pid ' || pg_backend_pid()", move(pb)));
		println("b: {}", rb[0].as<string_view>(0));
	} catch (exception const &e) { println(cerr, "error: {}", e.what()); }

	pool->close();
	::io_uring_queue_exit(&ring);
}
