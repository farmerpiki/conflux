// file_io_sqe_storm_bench — batches many simple FileReader read/write SQEs.
//
// This keeps kernel round-trips in the measurement, but submits enough
// independent SQEs per batch that completion callback/task-source overhead has
// a chance to show through.

#include <fcntl.h>
#include <liburing.h>
#include <stdlib.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;

import bench_common;
import bench_io_common;

using namespace std::string_view_literals;

namespace {

struct Config {
	std::size_t iterations = 20000;
	std::size_t warmup = 1000;
	std::size_t depth = 64;
	std::size_t chunk = 4096;
	std::string config_name;
	bool json_out = false;
};

Config parse_args(
	std::span<char *> args) {
	Config cfg;
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const a = args[i];
		if (a == "--iterations" && i + 1 < args.size()) {
			cfg.iterations = bench_parse_sz(args[++i]);
		} else if (a == "--warmup" && i + 1 < args.size()) {
			cfg.warmup = bench_parse_sz(args[++i]);
		} else if (a == "--depth" && i + 1 < args.size()) {
			cfg.depth = bench_parse_sz(args[++i]);
		} else if (a == "--chunk" && i + 1 < args.size()) {
			cfg.chunk = bench_parse_sz(args[++i]);
		} else if (a == "--config-name" && i + 1 < args.size()) {
			cfg.config_name = args[++i];
		} else if (a == "--json") {
			cfg.json_out = true;
		}
	}
	cfg.depth = std::max<std::size_t>(1, cfg.depth);
	cfg.chunk = std::max<std::size_t>(1, cfg.chunk);
	return cfg;
}

struct TempFile {
	std::string path = std::format("/tmp/conflux_sqe_storm_{}_XXXXXX", ::getpid());
	int fd = -1;

	TempFile() {
		fd = ::mkstemp(path.data());
		if (fd < 0) {
			throw std::runtime_error{"mkstemp failed"};
		}
	}
	~TempFile() {
		if (fd >= 0) {
			::close(fd);
		}
		if (!path.empty()) {
			::unlink(path.c_str());
		}
	}
	TempFile(TempFile const &) = delete;
	TempFile &operator =(TempFile const &) = delete;
};

void fill_file(
	TempFile &file,
	std::size_t bytes) {
	if (::ftruncate(file.fd, 0) != 0) {
		throw std::runtime_error{"ftruncate reset failed"};
	}
	std::vector<std::byte> buf(1U << 20U);
	std::mt19937_64 rng{0x51E57ULL};
	for (auto &b: buf) {
		b = static_cast<std::byte>(rng() & 0xFFU);
	}
	std::size_t left = bytes;
	while (left > 0) {
		std::size_t const n = std::min(left, buf.size());
		ssize_t const rc = ::write(file.fd, buf.data(), n);
		if (rc <= 0) {
			throw std::runtime_error{"seed write failed"};
		}
		left -= static_cast<std::size_t>(rc);
	}
	if (::fsync(file.fd) != 0) {
		throw std::runtime_error{"seed fsync failed"};
	}
}

BenchStats bench_read_storm(
	FileReader &files,
	FileHandle const &fh,
	Config const &cfg,
	std::vector<std::vector<std::byte>> &buffers,
	bool warmup) {
	std::size_t const batches = warmup ? cfg.warmup : cfg.iterations;
	std::uint64_t total_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		std::atomic<std::size_t> done{0};
		auto slots = bench_make_join_slots<std::size_t>(cfg.depth, done);
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			auto offset = ((batch + i) % cfg.depth) * cfg.chunk;
			bench_install_join(files.read_into(fh, offset, std::span{buffers[i]}), slots[i]);
		}
		bench_pump_until_count(files, done, cfg.depth);
		for (auto const &slot: slots) {
			if (slot->err) {
				std::rethrow_exception(slot->err);
			}
			total_bytes += *slot->value;
		}
	}
	auto const elapsed = bench_now_ns() - t0;
	std::size_t const ops = batches * cfg.depth;
	if (!warmup && total_bytes == 0) {
		throw std::runtime_error{"read storm read zero bytes"};
	}
	return {cfg.config_name, "read_storm"sv, ops, elapsed, static_cast<double>(elapsed) / static_cast<double>(ops)};
}

BenchStats bench_write_storm(
	FileReader &files,
	FileHandle const &fh,
	Config const &cfg,
	std::vector<std::vector<std::byte>> const &buffers,
	bool warmup) {
	std::size_t const batches = warmup ? cfg.warmup : cfg.iterations;
	std::uint64_t total_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		std::atomic<std::size_t> done{0};
		auto slots = bench_make_join_slots<std::size_t>(cfg.depth, done);
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			auto offset = ((batch + i) % cfg.depth) * cfg.chunk;
			bench_install_join(files.write_into(fh, offset, std::span<std::byte const>{buffers[i]}), slots[i]);
		}
		bench_pump_until_count(files, done, cfg.depth);
		for (auto const &slot: slots) {
			if (slot->err) {
				std::rethrow_exception(slot->err);
			}
			total_bytes += *slot->value;
		}
	}
	auto const elapsed = bench_now_ns() - t0;
	std::size_t const ops = batches * cfg.depth;
	if (!warmup && total_bytes == 0) {
		throw std::runtime_error{"write storm wrote zero bytes"};
	}
	return {cfg.config_name, "write_storm"sv, ops, elapsed, static_cast<double>(elapsed) / static_cast<double>(ops)};
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"file_io_sqe_storm","parser":"standard","configs":[{"name":"depth_64_4k","extra":{"depth":64,"chunk":4096},"target_ms":500,"max_iterations":20000,"calibration_iterations":4,"args":["--depth","64","--chunk","4096","--config-name","depth_64_4k","--iterations","0","--warmup","0"]},{"name":"depth_128_4k","extra":{"depth":128,"chunk":4096},"target_ms":500,"max_iterations":10000,"calibration_iterations":4,"args":["--depth","128","--chunk","4096","--config-name","depth_128_4k","--iterations","0","--warmup","0"]}]})");

	auto cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	std::vector<std::vector<std::byte>> buffers(cfg.depth, std::vector<std::byte>(cfg.chunk));
	for (std::size_t i = 0; i < buffers.size(); ++i) {
		std::ranges::fill(buffers[i], static_cast<std::byte>(i & 0xFFU));
	}

	TempFile file;
	fill_file(file, cfg.depth * cfg.chunk);

	::io_uring ring{};
	if (::io_uring_queue_init(static_cast<unsigned>(std::max<std::size_t>(256, cfg.depth * 2U)), &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}

	try {
		CompletionTable completions{cfg.depth * 2U};
		FileReader files{&ring, &completions, bench_pack_ud};
		auto handle = block_on(files, files.async_open(AT_FDCWD, file.path, O_RDWR | O_CLOEXEC));

		(void)bench_read_storm(files, handle, cfg, buffers, true);
		(void)bench_write_storm(files, handle, cfg, buffers, true);

		BenchStats stats[] = {
			bench_read_storm(files, handle, cfg, buffers, false),
			bench_write_storm(files, handle, cfg, buffers, false),
		};
		for (std::size_t i = 0; i < std::size(stats); ++i) {
			bench_print(stats[i], cfg.json_out, i == 0);
		}
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_file_io_sqe_storm_bench: {}", ex.what());
		::io_uring_queue_exit(&ring);
		return 1;
	}

	::io_uring_queue_exit(&ring);
}
