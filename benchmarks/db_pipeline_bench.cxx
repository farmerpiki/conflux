#include <liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.db;
import bench_common;

using namespace conflux::db;
using namespace std::string_view_literals;
namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}
struct PipeConfig {
	std::size_t batches = 200;
	std::size_t batch_n = 100;
	std::size_t warmup_batches = 20;
};
PipeConfig parse_pipe_args(
	span<char *> args) {
	PipeConfig cfg;
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view a = args[i];
		if (a == "--batches" && i + 1 < args.size()) {
			cfg.batches = bench_parse_sz(args[++i]);
		} else if (a == "--batch-n" && i + 1 < args.size()) {
			cfg.batch_n = bench_parse_sz(args[++i]);
		} else if (a == "--warmup-batches" && i + 1 < args.size()) {
			cfg.warmup_batches = bench_parse_sz(args[++i]);
		}
	}
	return cfg;
}
void setup_table(
	FileReader &reader,
	std::shared_ptr<Connection> const &conn) {
	(void)block_on(reader, conn->query("DROP TABLE IF EXISTS conflux_pipeline_bench"), std::chrono::seconds{30});
	(void)block_on(
		reader,
		conn->query("CREATE TEMP TABLE conflux_pipeline_bench (id int8 PRIMARY KEY, payload text)"),
		std::chrono::seconds{30});
}
std::uint64_t run_plain(
	FileReader &reader,
	std::shared_ptr<Connection> const &conn,
	std::size_t batches,
	std::size_t batch_n) {
	auto const t0 = std::chrono::steady_clock::now();
	std::int64_t id = 0;
	for (std::size_t b = 0; b < batches; ++b) {
		for (std::size_t i = 0; i < batch_n; ++i) {
			Params p;
			p.add(id++).add("x");
			(void)block_on(
				reader,
				conn->query("INSERT INTO conflux_pipeline_bench (id, payload) VALUES ($1, $2)", move(p)),
				std::chrono::seconds{30});
		}
	}
	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
std::uint64_t run_pipeline(
	FileReader &reader,
	std::shared_ptr<Connection> const &conn,
	std::size_t batches,
	std::size_t batch_n) {
	auto const t0 = std::chrono::steady_clock::now();
	std::int64_t id = 0;
	for (std::size_t b = 0; b < batches; ++b) {
		auto pipe = block_on(reader, conn->pipeline(), std::chrono::seconds{30});
		std::vector<conflux::work::root::Task<Result>> pending;
		pending.reserve(batch_n);
		for (std::size_t i = 0; i < batch_n; ++i) {
			Params p;
			p.add(id++).add("x");
			pending.push_back(pipe.query("INSERT INTO conflux_pipeline_bench (id, payload) VALUES ($1, $2)", move(p)));
		}
		block_on(reader, pipe.sync(), std::chrono::seconds{30});
		for (auto &f: pending) {
			(void)block_on(reader, move(f), std::chrono::seconds{30});
		}
	}
	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"db_pipeline","parser":"standard","configs":[{"name":"b60_n100","extra":{"batches":60,"batch_n":100},"args":["--batches","60","--batch-n","100","--config-name","b60_n100"]}]})");

	auto cfg = bench_parse_args(span{argv, static_cast<std::size_t>(argc)});
	auto pipe_cfg = parse_pipe_args(span{argv, static_cast<std::size_t>(argc)});
	if (cfg.config_name.empty()) {
		cfg.config_name = format("b{}_n{}", pipe_cfg.batches, pipe_cfg.batch_n);
	}

	char const *raw = std::getenv("PG_CONNINFO");
	if (raw == nullptr || *raw == '\0') {
		std::println(std::cerr, "PG_CONNINFO not set — skipping bench");
		return 0;
	}

	::io_uring ring{};
	if (::io_uring_queue_init(128, &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}
	CompletionTable ct;
	FileReader reader{&ring, &ct, pack_ud};
	CurrentFileReaderScope const scope{&reader};

	try {
		auto conn = block_on(reader, Connection::connect({.conninfo = raw}), std::chrono::seconds{30});
		setup_table(reader, conn);
		(void)run_plain(reader, conn, pipe_cfg.warmup_batches, pipe_cfg.batch_n);
		setup_table(reader, conn);
		(void)run_pipeline(reader, conn, pipe_cfg.warmup_batches, pipe_cfg.batch_n);

		setup_table(reader, conn);
		std::uint64_t const plain_ns = run_plain(reader, conn, pipe_cfg.batches, pipe_cfg.batch_n);
		setup_table(reader, conn);
		std::uint64_t const pipe_ns = run_pipeline(reader, conn, pipe_cfg.batches, pipe_cfg.batch_n);

		std::size_t const total_ops = pipe_cfg.batches * pipe_cfg.batch_n;
		double const ns_per_plain = static_cast<double>(plain_ns) / static_cast<double>(total_ops);
		double const ns_per_pipe = static_cast<double>(pipe_ns) / static_cast<double>(total_ops);

		BenchStats plain_stats{cfg.config_name, "plain"sv, total_ops, plain_ns, ns_per_plain};
		BenchStats pipe_stats{cfg.config_name, "pipeline"sv, total_ops, pipe_ns, ns_per_pipe};
		bench_print(plain_stats, cfg.json_out, true);
		bench_print(pipe_stats, cfg.json_out, false);
		if (!cfg.json_out) {
			double const speedup = ns_per_plain / ns_per_pipe;
			std::println("  speedup    {:.2f}x", speedup);
			std::println("  note       libpq wire-level Pipeline::sync() with PQpipelineSync");
		}

		conn->close();
	} catch (exception const &e) {
		std::println(std::cerr, "error: {}", e.what());
		::io_uring_queue_exit(&ring);
		return 1;
	}

	::io_uring_queue_exit(&ring);
	return 0;
}
