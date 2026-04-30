// Benchmark: identical PG workload run two ways — callback-style Flow pipeline
// vs coroutine (co_await). Requires PG_CONNINFO in the env; skipped otherwise.
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.db;

using namespace conflux::db;

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
	i64 n) {
	Params p;
	p.add(n);
	return p;
}

u64 run_callback(
	FileReader &reader,
	SP<Connection> const &conn,
	SZ iters,
	i64 rows) {
	auto const t0 = chrono::steady_clock::now();
	for (SZ i = 0; i < iters; ++i) {
		auto rs = block_on(reader, conn->query(S{kSql}, make_params(rows)));
		consume(rs);
	}
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

Task<void> coro_one(
	SP<Connection> const &conn,
	i64 rows) {
	auto rs = co_await task_as_flow(conn->query(S{kSql}, make_params(rows)));
	consume(rs);
	co_return;
}

u64 run_coroutine(
	FileReader &reader,
	SP<Connection> const &conn,
	SZ iters,
	i64 rows) {
	auto const t0 = chrono::steady_clock::now();
	for (SZ i = 0; i < iters; ++i) {
		block_on(reader, coro_one(conn, rows));
	}
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

struct Config {
	SZ iterations = 5000;
	SZ warmup = 500;
	i64 rows = 3;
	bool csv = false;
};

u64 parse_u64(
	char const *s) noexcept {
	SV sv{s};
	u64 v{};
	from_chars(sv.data(), sv.data() + sv.size(), v);
	return v;
}

i64 parse_i64(
	char const *s) noexcept {
	SV sv{s};
	i64 v{};
	from_chars(sv.data(), sv.data() + sv.size(), v);
	return v;
}

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (SZ i = 1; i < args.size(); ++i) {
		SV a = args[i];
		if (a == "--iterations" && i + 1 < args.size()) {
			cfg.iterations = parse_u64(args[++i]);
		} else if (a == "--warmup" && i + 1 < args.size()) {
			cfg.warmup = parse_u64(args[++i]);
		} else if (a == "--rows" && i + 1 < args.size()) {
			cfg.rows = parse_i64(args[++i]);
		} else if (a == "--csv") {
			cfg.csv = true;
		} else if (a == "--help" || a == "-h") {
			println("Usage: conflux_db_coro_bench [--iterations N] [--warmup N] [--rows N] [--csv]");
			println("Needs PG_CONNINFO set.");
			std::exit(0);
		}
	}
	return cfg;
}

} // namespace

int main(
	int argc,
	char **argv) {
	auto cfg = parse_args(span{argv, static_cast<SZ>(argc)});

	char const *raw = std::getenv("PG_CONNINFO");
	if (raw == nullptr || *raw == '\0') {
		println(cerr, "PG_CONNINFO not set — skipping bench");
		return 0;
	}

	::io_uring ring{};
	if (::io_uring_queue_init(64, &ring, 0) < 0) {
		println(cerr, "io_uring_queue_init failed");
		return 1;
	}
	CompletionTable ct;
	FileReader reader{&ring, &ct, pack_ud};
	CurrentFileReaderScope const scope{&reader};

	try {
		auto conn = block_on(reader, Connection::connect({.conninfo = raw}));

		(void)run_callback(reader, conn, cfg.warmup, cfg.rows);
		(void)run_coroutine(reader, conn, cfg.warmup, cfg.rows);

		u64 const cb_ns = run_callback(reader, conn, cfg.iterations, cfg.rows);
		u64 const co_ns = run_coroutine(reader, conn, cfg.iterations, cfg.rows);

		double const cb_per = static_cast<double>(cb_ns) / static_cast<double>(cfg.iterations);
		double const co_per = static_cast<double>(co_ns) / static_cast<double>(cfg.iterations);
		double const delta_pct = 100.0 * (co_per - cb_per) / cb_per;

		if (cfg.csv) {
			println("style,iterations,total_ns,ns_per_iter");
			println("callback,{},{},{:.1f}", cfg.iterations, cb_ns, cb_per);
			println("coroutine,{},{},{:.1f}", cfg.iterations, co_ns, co_per);
		} else {
			println("iterations: {}, rows/query: {}, warmup: {}", cfg.iterations, cfg.rows, cfg.warmup);
			println("  callback   {:>10.1f} ns/iter  ({} ns total)", cb_per, cb_ns);
			println("  coroutine  {:>10.1f} ns/iter  ({} ns total)", co_per, co_ns);
			println("  delta      {:+.2f}% (coro vs callback)", delta_pct);
			println("  sink       {}", sink.load(memory_order_relaxed));
		}

		conn->close();
	} catch (exception const &e) {
		println(cerr, "error: {}", e.what());
		::io_uring_queue_exit(&ring);
		return 1;
	}

	::io_uring_queue_exit(&ring);
}
