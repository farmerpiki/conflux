// Benchmark: large file copy two ways — block_on per chunk (callback style)
// vs single block_on driving a Task<void> that co_awaits read/write in a loop.
#include <fcntl.h>
#include <liburing.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import conflux.work;
import conflux.file_io;

using namespace std;

namespace {

constexpr uint64_t pack_ud(
	uint32_t slot,
	uint32_t gen) noexcept {
	return (static_cast<uint64_t>(gen) << 32U) | slot;
}

struct Config {
	size_t size_mib = 256;
	size_t chunk_kib = 64;
	size_t runs = 5;
	bool csv = false;
	string src_path = "/tmp/conflux_copy_src.bin";
	string dst_path = "/tmp/conflux_copy_dst.bin";
};

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (size_t i = 1; i < args.size(); ++i) {
		string_view a = args[i];
		if (a == "--size-mib" && i + 1 < args.size()) {
			cfg.size_mib = stoull(args[++i]);
		} else if (a == "--chunk-kib" && i + 1 < args.size()) {
			cfg.chunk_kib = stoull(args[++i]);
		} else if (a == "--runs" && i + 1 < args.size()) {
			cfg.runs = stoull(args[++i]);
		} else if (a == "--csv") {
			cfg.csv = true;
		} else if (a == "--help" || a == "-h") {
			println("Usage: conflux_file_copy_coro_bench [--size-mib N] [--chunk-kib N] [--runs N] [--csv]");
			exit(0);
		}
	}
	return cfg;
}

void seed_source(
	string const &path,
	size_t bytes) {
	int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		throw runtime_error{"open src"};
	}
	vector<byte> buf(1U << 20U); // 1 MiB pattern
	mt19937_64 rng{0xC0FFEEULL};
	for (auto &b: buf) {
		b = static_cast<byte>(rng() & 0xFFU);
	}
	size_t left = bytes;
	while (left > 0) {
		size_t const n = min(left, buf.size());
		ssize_t const w = ::write(fd, buf.data(), n);
		if (w < 0) {
			::close(fd);
			throw runtime_error{"seed write"};
		}
		left -= static_cast<size_t>(w);
	}
	::fsync(fd);
	::close(fd);
}

void drop_caches() noexcept {
	int fd = ::open("/proc/sys/vm/drop_caches", O_WRONLY);
	if (fd >= 0) {
		(void)::write(fd, "1\n", 2);
		::close(fd);
	}
}

uint64_t run_callback(
	FileReader &files,
	Config const &cfg) {
	drop_caches();
	::unlink(cfg.dst_path.c_str());
	auto const t0 = chrono::steady_clock::now();

	auto src = block_on(files, files.open_async(AT_FDCWD, cfg.src_path, O_RDONLY | O_CLOEXEC));
	auto dst =
		block_on(files, files.open_async(AT_FDCWD, cfg.dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));

	vector<byte> buf(cfg.chunk_kib << 10U);
	uint64_t off = 0;
	while (true) {
		auto got = block_on(files, files.read_into(src, off, span{buf}));
		if (got == 0) {
			break;
		}
		block_on(files, files.write_into(dst, off, span<byte const>{buf.data(), got}));
		off += got;
	}
	block_on(files, files.fsync_async(dst));

	auto const t1 = chrono::steady_clock::now();
	return static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

Task<void> coro_copy(
	FileReader &files,
	string src_path,
	string dst_path,
	size_t chunk) {
	auto src = co_await files.open_async(AT_FDCWD, src_path, O_RDONLY | O_CLOEXEC);
	auto dst = co_await files.open_async(AT_FDCWD, dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	vector<byte> buf(chunk);
	uint64_t off = 0;
	while (true) {
		auto got = co_await files.read_into(src, off, span{buf});
		if (got == 0) {
			break;
		}
		co_await files.write_into(dst, off, span<byte const>{buf.data(), got});
		off += got;
	}
	co_await files.fsync_async(dst);
	co_return;
}

uint64_t run_coroutine(
	FileReader &files,
	Config const &cfg) {
	drop_caches();
	::unlink(cfg.dst_path.c_str());
	auto const t0 = chrono::steady_clock::now();
	block_on(files, coro_copy(files, cfg.src_path, cfg.dst_path, cfg.chunk_kib << 10U));
	auto const t1 = chrono::steady_clock::now();
	return static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

double mib_per_sec(
	size_t bytes,
	uint64_t ns) {
	return (static_cast<double>(bytes) / (1U << 20U)) / (static_cast<double>(ns) / 1e9);
}

struct Agg {
	uint64_t total_ns = 0;
	uint64_t best_ns = numeric_limits<uint64_t>::max();
};

} // namespace

int main(
	int argc,
	char **argv) {
	auto cfg = parse_args(span{argv, static_cast<size_t>(argc)});
	size_t const bytes = cfg.size_mib << 20U;

	println("seeding {} MiB into {}", cfg.size_mib, cfg.src_path);
	seed_source(cfg.src_path, bytes);

	::io_uring ring{};
	if (::io_uring_queue_init(256, &ring, 0) < 0) {
		println(cerr, "io_uring_queue_init failed");
		return 1;
	}
	CompletionTable ct;
	FileReader files{&ring, &ct, pack_ud};

	try {
		// warmup: one of each, excluded from stats.
		(void)run_callback(files, cfg);
		(void)run_coroutine(files, cfg);

		Agg cb;
		Agg co;
		for (size_t i = 0; i < cfg.runs; ++i) {
			uint64_t const t_cb = run_callback(files, cfg);
			cb.total_ns += t_cb;
			cb.best_ns = min(cb.best_ns, t_cb);
			uint64_t const t_co = run_coroutine(files, cfg);
			co.total_ns += t_co;
			co.best_ns = min(co.best_ns, t_co);
		}

		double const cb_avg = static_cast<double>(cb.total_ns) / static_cast<double>(cfg.runs);
		double const co_avg = static_cast<double>(co.total_ns) / static_cast<double>(cfg.runs);
		double const delta = 100.0 * (co_avg - cb_avg) / cb_avg;

		if (cfg.csv) {
			println("style,runs,avg_ns,best_ns,avg_mib_per_s,best_mib_per_s");
			println(
				"callback,{},{:.0f},{},{:.1f},{:.1f}",
				cfg.runs,
				cb_avg,
				cb.best_ns,
				mib_per_sec(bytes, static_cast<uint64_t>(cb_avg)),
				mib_per_sec(bytes, cb.best_ns));
			println(
				"coroutine,{},{:.0f},{},{:.1f},{:.1f}",
				cfg.runs,
				co_avg,
				co.best_ns,
				mib_per_sec(bytes, static_cast<uint64_t>(co_avg)),
				mib_per_sec(bytes, co.best_ns));
		} else {
			println("size: {} MiB, chunk: {} KiB, runs: {} (+1 warmup each)", cfg.size_mib, cfg.chunk_kib, cfg.runs);
			println(
				"  callback   avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
				cb_avg / 1e6,
				static_cast<double>(cb.best_ns) / 1e6,
				mib_per_sec(bytes, static_cast<uint64_t>(cb_avg)),
				mib_per_sec(bytes, cb.best_ns));
			println(
				"  coroutine  avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
				co_avg / 1e6,
				static_cast<double>(co.best_ns) / 1e6,
				mib_per_sec(bytes, static_cast<uint64_t>(co_avg)),
				mib_per_sec(bytes, co.best_ns));
			println("  delta      {:+.2f}% avg (coro vs callback)", delta);
		}
	} catch (exception const &e) {
		println(cerr, "error: {}", e.what());
		::io_uring_queue_exit(&ring);
		::unlink(cfg.dst_path.c_str());
		::unlink(cfg.src_path.c_str());
		return 1;
	}

	::io_uring_queue_exit(&ring);
	::unlink(cfg.dst_path.c_str());
	::unlink(cfg.src_path.c_str());
}
