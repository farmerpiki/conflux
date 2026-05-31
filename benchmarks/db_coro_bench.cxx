// Benchmark: identical PG workload run two ways — callback-style Flow pipeline
// vs coroutine (co_await). Requires PG_CONNINFO in the env; skipped otherwise.
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.pg;
import bench_common;

using namespace conflux::pg;
using namespace std::string_view_literals;
using conflux::work::root::Task;
namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}
inline std::atomic<std::size_t> sink{};

constexpr std::string_view kSql = "SELECT i, 'row #' || i AS label FROM generate_series(1,$1) AS i";
void consume(
	Result const &rs) {
	std::size_t acc = 0;
	for (auto row: rs) {
		acc += static_cast<std::size_t>(row.as<std::int64_t>(0));
		acc += row.as<std::string_view>(1).size();
	}
	sink.fetch_add(acc, std::memory_order_relaxed);
}
Params make_params(
	std::int64_t n,
	bool binary) {
	Params p;
	if (binary) {
		p.add_binary(n);
	} else {
		p.add(n);
	}
	return p;
}
void run_callback_once(
	conflux::file_io::FileReader &reader,
	std::shared_ptr<Connection> const &conn,
	std::int64_t rows,
	bool binary) {
	auto rs = block_on(reader, conn->query(std::string{kSql}, make_params(rows, binary)));
	consume(rs);
}
Task<void> coro_one(
	std::shared_ptr<Connection> const &conn,
	std::int64_t rows,
	bool binary) {
	auto rs = co_await conn->query(std::string{kSql}, make_params(rows, binary));
	consume(rs);
	co_return;
}
void run_coroutine_once(
	conflux::file_io::FileReader &reader,
	std::shared_ptr<Connection> const &conn,
	std::int64_t rows,
	bool binary) {
	block_on(reader, coro_one(conn, rows, binary));
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"db_coro","parser":"standard","configs":[{"name":"rows_3","extra":{"rows":3,"binary":false},"target_ms":500,"max_iterations":5000,"calibration_iterations":4,"args":["--rows","3","--config-name","rows_3","--iterations","0","--warmup","0"]},{"name":"rows_3_binary","extra":{"rows":3,"binary":true},"target_ms":500,"max_iterations":5000,"calibration_iterations":4,"args":["--rows","3","--binary","--config-name","rows_3_binary","--iterations","0","--warmup","0"]},{"name":"rows_100","extra":{"rows":100,"binary":false},"target_ms":500,"max_iterations":1000,"calibration_iterations":2,"args":["--rows","100","--config-name","rows_100","--iterations","0","--warmup","0"]},{"name":"rows_100_binary","extra":{"rows":100,"binary":true},"target_ms":500,"max_iterations":1000,"calibration_iterations":2,"args":["--rows","100","--binary","--config-name","rows_100_binary","--iterations","0","--warmup","0"]}]})");

	auto cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	std::int64_t rows = 3;
	bool binary = false;
	for (std::size_t i = 1; i < static_cast<std::size_t>(argc); ++i) {
		std::string_view const a = argv[i];
		if (a == "--rows" && i + 1 < static_cast<std::size_t>(argc)) {
			std::uint64_t v{};
			std::string_view sv{argv[++i]};
			std::from_chars(sv.data(), sv.data() + sv.size(), v);
			rows = static_cast<std::int64_t>(v);
			if (cfg.config_name.empty()) {
				cfg.config_name = std::format("rows_{}", rows);
			}
		} else if (a == "--binary") {
			binary = true;
			if (cfg.config_name.empty()) {
				cfg.config_name = std::format("rows_{}_binary", rows);
			}
		}
	}
	if (cfg.config_name.empty()) {
		cfg.config_name = binary ? std::format("rows_{}_binary", rows) : std::format("rows_{}", rows);
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
	conflux::uring::CompletionTable ct;
	conflux::file_io::FileReader reader{&ring, &ct, pack_ud};
	conflux::file_io::CurrentFileReaderScope const scope{&reader};

	try {
		std::shared_ptr<Connection> conn{};
		try {
			conn = block_on(reader, Connection::connect({.conninfo = raw}));
		} catch (PgError const &e) {
			std::println(std::cerr, "PG_CONNINFO unavailable — skipping bench: {}", e.what());
			::io_uring_queue_exit(&ring);
			return 0;
		}

		BenchSamplePlan const plan = bench_sample_plan(cfg.iterations, cfg.warmup, cfg.samples, cfg.batch);
		auto cb_stats = bench_measure_batched([&] { run_callback_once(reader, conn, rows, binary); }, plan);
		auto co_stats = bench_measure_batched([&] { run_coroutine_once(reader, conn, rows, binary); }, plan);
		cb_stats.config = cfg.config_name;
		cb_stats.variant = "callback"sv;
		co_stats.config = cfg.config_name;
		co_stats.variant = "coroutine"sv;
		bench_print(cb_stats, cfg.json_out, true);
		bench_print(co_stats, cfg.json_out, false);
		if (!cfg.json_out) {
			double const delta_pct = 100.0 * (co_stats.ns_per_iter - cb_stats.ns_per_iter) / cb_stats.ns_per_iter;
			std::println("  delta      {:+.2f}% (coro vs callback)", delta_pct);
			std::println("  sink       {}", sink.load(std::memory_order_relaxed));
		}

		conn->close();
	} catch (std::exception const &e) {
		std::println(std::cerr, "error: {}", e.what());
		::io_uring_queue_exit(&ring);
		return 1;
	}

	::io_uring_queue_exit(&ring);
}
