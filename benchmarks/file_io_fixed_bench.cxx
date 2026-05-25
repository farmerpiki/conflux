// file_io_fixed_bench — batches FileReader fixed-buffer read/write SQEs.
//
// The benchmark keeps kernel round-trips in the measurement, but submits enough
// independent fixed-buffer SQEs per batch for completion callback ownership
// costs to show through.

#include <fcntl.h>
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;

import bench_common;
import bench_io_common;

using namespace std::string_view_literals;

namespace {

BenchStats bench_read_fixed(
	FileReader &files,
	FileHandle const &fh,
	FixedBufferPool &pool,
	BenchUringFileConfig const &cfg,
	bool warmup) {
	std::size_t const batches = warmup ? cfg.warmup : cfg.iterations;
	std::uint64_t total_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		std::atomic<std::size_t> done{0};
		auto slots = bench_make_join_slots<FileReader::ReadFixedResult>(cfg.depth, done);
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			auto buf = pool.try_acquire();
			if (!buf) {
				throw std::runtime_error{"fixed read buffer pool exhausted"};
			}
			auto offset = ((batch + i) % cfg.depth) * cfg.chunk;
			bench_install_join(files.read_fixed(fh, offset, std::move(*buf), cfg.chunk), slots[i]);
		}
		bench_pump_until_count(files, done, cfg.depth);
		for (auto const &slot: slots) {
			if (slot->err) {
				std::rethrow_exception(slot->err);
			}
			total_bytes += slot->value->bytes;
		}
	}
	auto const elapsed = bench_now_ns() - t0;
	std::size_t const ops = batches * cfg.depth;
	if (!warmup && total_bytes == 0) {
		throw std::runtime_error{"fixed read read zero bytes"};
	}
	return {
		cfg.config_name,
		"read_fixed_storm"sv,
		ops,
		elapsed,
		static_cast<double>(elapsed) / static_cast<double>(ops)};
}

BenchStats bench_write_fixed(
	FileReader &files,
	FileHandle const &fh,
	FixedBufferPool &pool,
	BenchUringFileConfig const &cfg,
	bool warmup) {
	std::size_t const batches = warmup ? cfg.warmup : cfg.iterations;
	std::uint64_t total_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		std::atomic<std::size_t> done{0};
		auto slots = bench_make_join_slots<FileReader::WriteFixedResult>(cfg.depth, done);
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			auto buf = pool.try_acquire();
			if (!buf) {
				throw std::runtime_error{"fixed write buffer pool exhausted"};
			}
			std::ranges::fill(buf->view().first(cfg.chunk), static_cast<std::byte>((batch + i) & 0xFFU));
			auto offset = ((batch + i) % cfg.depth) * cfg.chunk;
			bench_install_join(files.write_fixed(fh, offset, std::move(*buf), cfg.chunk), slots[i]);
		}
		bench_pump_until_count(files, done, cfg.depth);
		for (auto const &slot: slots) {
			if (slot->err) {
				std::rethrow_exception(slot->err);
			}
			total_bytes += slot->value->bytes;
		}
	}
	auto const elapsed = bench_now_ns() - t0;
	std::size_t const ops = batches * cfg.depth;
	if (!warmup && total_bytes == 0) {
		throw std::runtime_error{"fixed write wrote zero bytes"};
	}
	return {
		cfg.config_name,
		"write_fixed_storm"sv,
		ops,
		elapsed,
		static_cast<double>(elapsed) / static_cast<double>(ops)};
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"file_io_fixed","parser":"standard","configs":[{"name":"depth_64_4k","extra":{"depth":64,"chunk":4096},"target_ms":500,"max_iterations":20000,"calibration_iterations":4,"args":["--depth","64","--chunk","4096","--config-name","depth_64_4k","--iterations","0","--warmup","0"]},{"name":"depth_128_4k","extra":{"depth":128,"chunk":4096},"target_ms":500,"max_iterations":10000,"calibration_iterations":4,"args":["--depth","128","--chunk","4096","--config-name","depth_128_4k","--iterations","0","--warmup","0"]}]})");

	auto cfg = bench_parse_uring_file_args(std::span{argv, static_cast<std::size_t>(argc)});
	BenchTempFile file{"conflux_fixed_bench"};
	bench_fill_temp_file(file, cfg.depth * cfg.chunk, 0xF17EDULL);

	::io_uring ring{};
	if (::io_uring_queue_init(static_cast<unsigned>(std::max<std::size_t>(256, cfg.depth * 2U)), &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}

	try {
		CompletionTable completions{cfg.depth * 2U};
		FileReader files{&ring, &completions, bench_pack_ud};
		RegisteredBufferTable table{&ring, static_cast<unsigned>(cfg.depth)};
		if (!table.ok()) {
			throw std::runtime_error{"registered fixed buffers unsupported"};
		}
		FixedBufferPool pool{&table, 0, cfg.depth, cfg.chunk};
		if (!pool.ok() || pool.capacity() < cfg.depth) {
			throw std::runtime_error{"fixed buffer pool init failed"};
		}
		auto handle = block_on(files, files.async_open(AT_FDCWD, file.path, O_RDWR | O_CLOEXEC));

		(void)bench_read_fixed(files, handle, pool, cfg, true);
		(void)bench_write_fixed(files, handle, pool, cfg, true);

		BenchStats stats[] = {
			bench_read_fixed(files, handle, pool, cfg, false),
			bench_write_fixed(files, handle, pool, cfg, false),
		};
		for (std::size_t i = 0; i < std::size(stats); ++i) {
			bench_print(stats[i], cfg.json_out, i == 0);
		}
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_file_io_fixed_bench: {}", ex.what());
		::io_uring_queue_exit(&ring);
		return 1;
	}

	::io_uring_queue_exit(&ring);
}
