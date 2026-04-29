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

struct Config {
	SZ batches = 200;
	SZ batch_n = 100;
	SZ warmup_batches = 20;
	bool csv = false;
};

u64 parse_u64(
	char const *s) noexcept {
	SV sv{s};
	u64 v{};
	(void)from_chars(sv.data(), sv.data() + sv.size(), v);
	return v;
}

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (SZ i = 1; i < args.size(); ++i) {
		SV a = args[i];
		if (a == "--batches" && i + 1 < args.size()) {
			cfg.batches = parse_u64(args[++i]);
		} else if (a == "--batch-n" && i + 1 < args.size()) {
			cfg.batch_n = parse_u64(args[++i]);
		} else if (a == "--warmup-batches" && i + 1 < args.size()) {
			cfg.warmup_batches = parse_u64(args[++i]);
		} else if (a == "--csv") {
			cfg.csv = true;
		} else if (a == "--help" || a == "-h") {
			println("Usage: conflux_db_pipeline_bench [--batches N] [--batch-n N] [--warmup-batches N] [--csv]");
			println("Needs PG_CONNINFO set.");
			std::exit(0);
		}
	}
	return cfg;
}

void setup_table(
	FileReader &reader,
	SP<Connection> const &conn) {
	(void)block_on(reader, conn->query("DROP TABLE IF EXISTS conflux_pipeline_bench"), chrono::seconds{30});
	(void)block_on(
		reader,
		conn->query("CREATE TEMP TABLE conflux_pipeline_bench (id int8 PRIMARY KEY, payload text)"),
		chrono::seconds{30});
}

u64 run_plain(
	FileReader &reader,
	SP<Connection> const &conn,
	SZ batches,
	SZ batch_n) {
	auto const t0 = chrono::steady_clock::now();
	i64 id = 0;
	for (SZ b = 0; b < batches; ++b) {
		for (SZ i = 0; i < batch_n; ++i) {
			Params p;
			p.add(id++).add("x");
			(void)block_on(
				reader,
				conn->query("INSERT INTO conflux_pipeline_bench (id, payload) VALUES ($1, $2)", move(p)),
				chrono::seconds{30});
		}
	}
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

u64 run_pipeline(
	FileReader &reader,
	SP<Connection> const &conn,
	SZ batches,
	SZ batch_n) {
	auto const t0 = chrono::steady_clock::now();
	i64 id = 0;
	for (SZ b = 0; b < batches; ++b) {
		auto pipe = block_on(reader, conn->pipeline(), chrono::seconds{30});
		V<Flow<Result>> pending;
		pending.reserve(batch_n);
		for (SZ i = 0; i < batch_n; ++i) {
			Params p;
			p.add(id++).add("x");
			pending.push_back(pipe.query("INSERT INTO conflux_pipeline_bench (id, payload) VALUES ($1, $2)", move(p)));
		}
		block_on(reader, pipe.sync(), chrono::seconds{30});
		for (auto &f: pending) {
			(void)block_on(reader, move(f), chrono::seconds{30});
		}
	}
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
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
	if (::io_uring_queue_init(128, &ring, 0) < 0) {
		println(cerr, "io_uring_queue_init failed");
		return 1;
	}
	CompletionTable ct;
	FileReader reader{&ring, &ct, pack_ud};
	CurrentFileReaderScope const scope{&reader};

	try {
		auto conn = block_on(reader, Connection::connect({.conninfo = raw}), chrono::seconds{30});
		setup_table(reader, conn);
		(void)run_plain(reader, conn, cfg.warmup_batches, cfg.batch_n);
		setup_table(reader, conn);
		(void)run_pipeline(reader, conn, cfg.warmup_batches, cfg.batch_n);

		setup_table(reader, conn);
		u64 const plain_ns = run_plain(reader, conn, cfg.batches, cfg.batch_n);
		setup_table(reader, conn);
		u64 const pipe_ns = run_pipeline(reader, conn, cfg.batches, cfg.batch_n);

		double const total_ops = static_cast<double>(cfg.batches * cfg.batch_n);
		double const plain_ops_s = total_ops * 1e9 / static_cast<double>(plain_ns);
		double const pipe_ops_s = total_ops * 1e9 / static_cast<double>(pipe_ns);
		double const speedup = pipe_ops_s / plain_ops_s;

		if (cfg.csv) {
			println("mode,batches,batch_n,total_ns,ops_per_sec");
			println("plain,{},{},{},{:.1f}", cfg.batches, cfg.batch_n, plain_ns, plain_ops_s);
			println("pipeline,{},{},{},{:.1f}", cfg.batches, cfg.batch_n, pipe_ns, pipe_ops_s);
		} else {
			println("batches: {}, batch_n: {}, warmup_batches: {}", cfg.batches, cfg.batch_n, cfg.warmup_batches);
			println("  plain      {:>10.1f} ops/s  ({} ns total)", plain_ops_s, plain_ns);
			println("  pipeline   {:>10.1f} ops/s  ({} ns total)", pipe_ops_s, pipe_ns);
			println("  speedup    {:.2f}x", speedup);
			println("  note       logical batching Pipeline::sync() (not wire-level libpq pipeline mode)");
		}

		conn->close();
	} catch (exception const &e) {
		println(cerr, "error: {}", e.what());
		::io_uring_queue_exit(&ring);
		return 1;
	}

	::io_uring_queue_exit(&ring);
	return 0;
}

