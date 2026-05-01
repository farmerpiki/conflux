// Benchmark: large file copy two ways — block_on per chunk (callback style)
// vs single block_on driving a Task<void> that co_awaits read/write in a loop.
#include <fcntl.h>
#include <liburing.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;

namespace {

constexpr u64 pack_ud(
	u32 slot,
	u32 gen) noexcept {
	return (static_cast<u64>(gen) << 32U) | slot;
}

struct Config {
	SZ size_mib = 256;
	SZ chunk_kib = 64;
	SZ runs = 2;
	bool csv = false;
	S src_path = "/tmp/conflux_copy_src.bin";
	S dst_path = "/tmp/conflux_copy_dst.bin";
};

namespace {

SZ parse_sz(
	char const *s) noexcept {
	SV const sv{s};
	SZ v{};
	from_chars(sv.data(), sv.data() + sv.size(), v);
	return v;
}

} // namespace

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (SZ i = 1; i < args.size(); ++i) {
		SV const a = args[i];
		if (a == "--size-mib" && i + 1 < args.size()) {
			cfg.size_mib = parse_sz(args[++i]);
		} else if (a == "--chunk-kib" && i + 1 < args.size()) {
			cfg.chunk_kib = parse_sz(args[++i]);
		} else if (a == "--runs" && i + 1 < args.size()) {
			cfg.runs = parse_sz(args[++i]);
		} else if (a == "--csv") {
			cfg.csv = true;
		} else if (a == "--help" || a == "-h") {
			println("Usage: conflux_file_copy_coro_bench [--size-mib N] [--chunk-kib N] [--runs N] [--csv]");
			std::exit(0);
		}
	}
	return cfg;
}

void seed_source(
	S const &path,
	SZ bytes) {
	int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		throw RE{"open src"};
	}
	V<byte> buf(1U << 20U); // 1 MiB pattern
	std::mt19937_64 rng{0xC0FFEEULL};
	for (auto &b: buf) {
		b = static_cast<byte>(rng() & 0xFFU);
	}
	SZ left = bytes;
	while (left > 0) {
		SZ const n = min(left, buf.size());
		ssize_t const w = ::write(fd, buf.data(), n);
		if (w < 0) {
			::close(fd);
			throw RE{"seed write"};
		}
		left -= static_cast<SZ>(w);
	}
	::fsync(fd);
	::close(fd);
}

void drop_caches() noexcept {
	int const fd = ::open("/proc/sys/vm/drop_caches", O_WRONLY);
	if (fd >= 0) {
		(void)::write(fd, "1\n", 2);
		::close(fd);
	}
}

u64 run_callback(
	FileReader &files,
	Config const &cfg) {
	drop_caches();
	::unlink(cfg.dst_path.c_str());
	auto const t0 = chrono::steady_clock::now();

	auto src = block_on(files, files.open_async(AT_FDCWD, cfg.src_path, O_RDONLY | O_CLOEXEC));
	auto dst =
		block_on(files, files.open_async(AT_FDCWD, cfg.dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));

	V<byte> buf(cfg.chunk_kib << 10U);
	u64 off = 0;
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
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

Task<void> coro_copy(
	FileReader &files,
	S src_path,
	S dst_path,
	SZ chunk) {
	auto src = co_await files.open_async(AT_FDCWD, src_path, O_RDONLY | O_CLOEXEC);
	auto dst = co_await files.open_async(AT_FDCWD, dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	V<byte> buf(chunk);
	u64 off = 0;
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

u64 run_coroutine(
	FileReader &files,
	Config const &cfg) {
	drop_caches();
	::unlink(cfg.dst_path.c_str());
	auto const t0 = chrono::steady_clock::now();
	block_on(files, coro_copy(files, cfg.src_path, cfg.dst_path, cfg.chunk_kib << 10U));
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

double mib_per_sec(
	SZ bytes,
	u64 ns) {
	return (static_cast<double>(bytes) / (1U << 20U)) / (static_cast<double>(ns) / 1e9);
}

struct Agg {
	u64 total_ns = 0;
	u64 best_ns = NL<u64>::max();
};

} // namespace

int main(
	int argc,
	char **argv) {
	if (argc >= 2 && SV{argv[1]} == "--bench-info") {
		std::print(
			"{}\n",
			R"({"name":"file_copy_coro","parser":"file_copy","configs":[{"name":"256mib_64kib","extra":{"size_mib":256,"chunk_kib":64},"args":["--size-mib","256","--chunk-kib","64","--runs","2"]}]})");
		return 0;
	}
	auto cfg = parse_args(span{argv, static_cast<SZ>(argc)});
	SZ const bytes = cfg.size_mib << 20U;

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
		for (SZ i = 0; i < cfg.runs; ++i) {
			u64 const t_cb = run_callback(files, cfg);
			cb.total_ns += t_cb;
			cb.best_ns = min(cb.best_ns, t_cb);
			u64 const t_co = run_coroutine(files, cfg);
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
				mib_per_sec(bytes, static_cast<u64>(cb_avg)),
				mib_per_sec(bytes, cb.best_ns));
			println(
				"coroutine,{},{:.0f},{},{:.1f},{:.1f}",
				cfg.runs,
				co_avg,
				co.best_ns,
				mib_per_sec(bytes, static_cast<u64>(co_avg)),
				mib_per_sec(bytes, co.best_ns));
		} else {
			println("size: {} MiB, chunk: {} KiB, runs: {} (+1 warmup each)", cfg.size_mib, cfg.chunk_kib, cfg.runs);
			println(
				"  callback   avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
				cb_avg / 1e6,
				static_cast<double>(cb.best_ns) / 1e6,
				mib_per_sec(bytes, static_cast<u64>(cb_avg)),
				mib_per_sec(bytes, cb.best_ns));
			println(
				"  coroutine  avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
				co_avg / 1e6,
				static_cast<double>(co.best_ns) / 1e6,
				mib_per_sec(bytes, static_cast<u64>(co_avg)),
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
