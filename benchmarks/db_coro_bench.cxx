// Benchmark: identical PG workload run two ways — callback-style Flow pipeline
// vs coroutine (co_await). Requires PG_CONNINFO in the env; skipped otherwise.
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.db;
import bench_common;

using namespace conflux::db;
using namespace std::string_view_literals;
using conflux::work::root::Task;
namespace {

constexpr u64 pack_ud(
	u32 slot,
	u32 gen) noexcept {
	return (static_cast<u64>(gen) << 32U) | slot;
}
inline Atom<SZ> sink{};

constexpr SV kSql = "SELECT i, 'row #' || i AS label FROM generate_series(1,$1) AS i";
void consume(
	Result const &rs) {
	SZ acc = 0;
	for (auto row: rs) {
		acc += static_cast<SZ>(row.as<i64>(0));
		acc += row.as<SV>(1).size();
	}
	sink.fetch_add(acc, memory_order_relaxed);
}
Params make_params(
	i64 n,
	bool binary) {
	Params p;
	if (binary) {
		p.add_binary(n);
	} else {
		p.add(n);
	}
	return p;
}
u64 run_callback(
	FileReader &reader,
	SP<Connection> const &conn,
	SZ iters,
	i64 rows,
	bool binary) {
	auto const t0 = chrono::steady_clock::now();
	for (SZ i = 0; i < iters; ++i) {
		auto rs = block_on(reader, conn->query(S{kSql}, make_params(rows, binary)));
		consume(rs);
	}
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}
Task<void> coro_one(
	SP<Connection> const &conn,
	i64 rows,
	bool binary) {
	auto rs = co_await conn->query(S{kSql}, make_params(rows, binary));
	consume(rs);
	co_return;
}
u64 run_coroutine(
	FileReader &reader,
	SP<Connection> const &conn,
	SZ iters,
	i64 rows,
	bool binary) {
	auto const t0 = chrono::steady_clock::now();
	for (SZ i = 0; i < iters; ++i) {
		block_on(reader, coro_one(conn, rows, binary));
	}
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"db_coro","parser":"standard","configs":[{"name":"rows_3","extra":{"rows":3,"binary":false},"args":["--rows","3","--config-name","rows_3","--iterations","5000","--warmup","500"]},{"name":"rows_3_binary","extra":{"rows":3,"binary":true},"args":["--rows","3","--binary","--config-name","rows_3_binary","--iterations","5000","--warmup","500"]},{"name":"rows_100","extra":{"rows":100,"binary":false},"args":["--rows","100","--config-name","rows_100","--iterations","1000","--warmup","100"]},{"name":"rows_100_binary","extra":{"rows":100,"binary":true},"args":["--rows","100","--binary","--config-name","rows_100_binary","--iterations","1000","--warmup","100"]}]})");

	auto cfg = bench_parse_args(span{argv, static_cast<SZ>(argc)});
	i64 rows = 3;
	bool binary = false;
	for (SZ i = 1; i < static_cast<SZ>(argc); ++i) {
		SV const a = argv[i];
		if (a == "--rows" && i + 1 < static_cast<SZ>(argc)) {
			u64 v{};
			SV sv{argv[++i]};
			from_chars(sv.data(), sv.data() + sv.size(), v);
			rows = static_cast<i64>(v);
			if (cfg.config_name.empty()) {
				cfg.config_name = format("rows_{}", rows);
			}
		} else if (a == "--binary") {
			binary = true;
			if (cfg.config_name.empty()) {
				cfg.config_name = format("rows_{}_binary", rows);
			}
		}
	}
	if (cfg.config_name.empty()) {
		cfg.config_name = binary ? format("rows_{}_binary", rows) : format("rows_{}", rows);
	}

	char const *raw = std::getenv("PG_CONNINFO");
	if (raw == nullptr || *raw == '\0') {
		std::println(std::cerr, "PG_CONNINFO not set — skipping bench");
		return 0;
	}

	::io_uring ring{};
	if (::io_uring_queue_init(64, &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}
	CompletionTable ct;
	FileReader reader{&ring, &ct, pack_ud};
	CurrentFileReaderScope const scope{&reader};

	try {
		SP<Connection> conn{};
		try {
			conn = block_on(reader, Connection::connect({.conninfo = raw}));
		} catch (PgError const &e) {
			std::println(std::cerr, "PG_CONNINFO unavailable — skipping bench: {}", e.what());
			::io_uring_queue_exit(&ring);
			return 0;
		}

		(void)run_callback(reader, conn, cfg.warmup, rows, binary);
		(void)run_coroutine(reader, conn, cfg.warmup, rows, binary);

		u64 const cb_ns = run_callback(reader, conn, cfg.iterations, rows, binary);
		u64 const co_ns = run_coroutine(reader, conn, cfg.iterations, rows, binary);

		double const cb_per = static_cast<double>(cb_ns) / static_cast<double>(cfg.iterations);
		double const co_per = static_cast<double>(co_ns) / static_cast<double>(cfg.iterations);

		BenchStats cb_stats{cfg.config_name, "callback"sv, cfg.iterations, cb_ns, cb_per};
		BenchStats co_stats{cfg.config_name, "coroutine"sv, cfg.iterations, co_ns, co_per};
		bench_print(cb_stats, cfg.json_out, true);
		bench_print(co_stats, cfg.json_out, false);
		if (!cfg.json_out) {
			double const delta_pct = 100.0 * (co_per - cb_per) / cb_per;
			std::println("  delta      {:+.2f}% (coro vs callback)", delta_pct);
			std::println("  sink       {}", sink.load(memory_order_relaxed));
		}

		conn->close();
	} catch (exception const &e) {
		std::println(std::cerr, "error: {}", e.what());
		::io_uring_queue_exit(&ring);
		return 1;
	}

	::io_uring_queue_exit(&ring);
}
