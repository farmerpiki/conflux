// Benchmark: large file copy paths:
// - block_on per read/write chunk (callback style)
// - single block_on driving a Task<void> that co_awaits read/write in a loop
// - existing compiled splice chain (file → pipe → fd)
#include <fcntl.h>
#include <liburing.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;

using conflux::work::root::Task;
namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}
struct Config {
	std::size_t size_mib = 256;
	std::size_t chunk_kib = 64;
	std::size_t runs = 2;
	bool json_out = false;
	std::string src_path = "/tmp/conflux_copy_src.bin";
	std::string dst_path = "/tmp/conflux_copy_dst.bin";
};
namespace {

std::size_t parse_sz(
	char const *s) noexcept {
	std::string_view const sv{s};
	std::size_t v{};
	std::from_chars(sv.data(), sv.data() + sv.size(), v);
	return v;
}

} // namespace
Config parse_args(
	std::span<char *> args) {
	Config cfg;
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const a = args[i];
		if (a == "--size-mib" && i + 1 < args.size()) {
			cfg.size_mib = parse_sz(args[++i]);
		} else if (a == "--chunk-kib" && i + 1 < args.size()) {
			cfg.chunk_kib = parse_sz(args[++i]);
		} else if (a == "--runs" && i + 1 < args.size()) {
			cfg.runs = parse_sz(args[++i]);
		} else if (a == "--json") {
			cfg.json_out = true;
		} else if (a == "--help" || a == "-h") {
			std::println("Usage: conflux_file_copy_coro_bench [--size-mib N] [--chunk-kib N] [--runs N] [--json]");
			std::exit(0);
		}
	}
	return cfg;
}
void seed_source(
	std::string const &path,
	std::size_t bytes) {
	int const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		throw std::runtime_error{"open src"};
	}
	std::vector<std::byte> buf(1U << 20U); // 1 MiB pattern
	std::mt19937_64 rng{0xC0FFEEULL};
	for (auto &b: buf) {
		b = static_cast<std::byte>(rng() & 0xFFU);
	}
	std::size_t left = bytes;
	while (left > 0) {
		std::size_t const n = std::min(left, buf.size());
		ssize_t const w = ::write(fd, buf.data(), n);
		if (w < 0) {
			::close(fd);
			throw std::runtime_error{"seed write"};
		}
		left -= static_cast<std::size_t>(w);
	}
	::fsync(fd);
	::close(fd);
}
void drop_caches() noexcept {
	int const fd = ::open("/proc/sys/vm/drop_caches", O_WRONLY);
	if (fd >= 0) {
		auto _ = ::write(fd, "1\n", 2);
		::close(fd);
	}
}
std::uint64_t run_callback(
	FileReader &files,
	Config const &cfg) {
	drop_caches();
	::unlink(cfg.dst_path.c_str());
	auto const t0 = std::chrono::steady_clock::now();

	auto src = block_on(files, files.async_open(AT_FDCWD, cfg.src_path, O_RDONLY | O_CLOEXEC));
	auto dst =
		block_on(files, files.async_open(AT_FDCWD, cfg.dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));

	std::vector<std::byte> buf(cfg.chunk_kib << 10U);
	std::uint64_t off = 0;
	while (true) {
		auto got = block_on(files, files.read_into(src, off, std::span{buf}));
		if (got == 0) {
			break;
		}
		block_on(files, files.write_into(dst, off, std::span<std::byte const>{buf.data(), got}));
		off += got;
	}
	block_on(files, files.async_fsync(dst));

	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
Task<void> coro_copy(
	FileReader &files,
	std::string src_path,
	std::string dst_path,
	std::size_t chunk) {
	auto src = co_await files.async_open(AT_FDCWD, src_path, O_RDONLY | O_CLOEXEC);
	auto dst = co_await files.async_open(AT_FDCWD, dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	std::vector<std::byte> buf(chunk);
	std::uint64_t off = 0;
	while (true) {
		auto got = co_await files.read_into(src, off, std::span{buf});
		if (got == 0) {
			break;
		}
		co_await files.write_into(dst, off, std::span<std::byte const>{buf.data(), got});
		off += got;
	}
	co_await files.async_fsync(dst);
	co_return;
}
std::uint64_t run_coroutine(
	FileReader &files,
	Config const &cfg) {
	drop_caches();
	::unlink(cfg.dst_path.c_str());
	auto const t0 = std::chrono::steady_clock::now();
	block_on(files, coro_copy(files, cfg.src_path, cfg.dst_path, cfg.chunk_kib << 10U));
	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
std::uint64_t run_splice_chain(
	FileReader &files,
	Config const &cfg,
	std::size_t bytes) {
	drop_caches();
	::unlink(cfg.dst_path.c_str());
	auto const t0 = std::chrono::steady_clock::now();

	PipePool pipes{1};
	auto pipe = pipes.try_acquire();
	if (!pipe.has_value()) {
		throw std::runtime_error{"splice pipe unavailable"};
	}
	auto src = block_on(files, files.async_open(AT_FDCWD, cfg.src_path, O_RDONLY | O_CLOEXEC));
	auto dst =
		block_on(files, files.async_open(AT_FDCWD, cfg.dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));

	std::size_t const delivered = block_on(files, files.splice_to_fd(src, 0, bytes, dst.raw_fd(), std::move(*pipe)));
	if (delivered != bytes) {
		throw std::runtime_error{std::format("splice short copy: {} of {} bytes", delivered, bytes)};
	}
	block_on(files, files.async_fsync(dst));

	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
double mib_per_sec(
	std::size_t bytes,
	std::uint64_t ns) {
	return (static_cast<double>(bytes) / (1U << 20U)) / (static_cast<double>(ns) / 1e9);
}
struct Agg {
	std::uint64_t total_ns = 0;
	std::uint64_t best_ns = std::numeric_limits<std::uint64_t>::max();
};

} // namespace
int main(
	int argc,
	char **argv) {
	if (argc >= 2 && std::string_view{argv[1]} == "--bench-info") {
		std::print(
			"{}\n",
			R"({"name":"file_copy_coro","parser":"file_copy","configs":[{"name":"256mib_64kib","extra":{"size_mib":256,"chunk_kib":64},"args":["--size-mib","256","--chunk-kib","64","--runs","2"]}]})");
		return 0;
	}
	auto cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	std::size_t const bytes = cfg.size_mib << 20U;

	std::println(std::cerr, "seeding {} MiB into {}", cfg.size_mib, cfg.src_path);
	seed_source(cfg.src_path, bytes);

	::io_uring ring{};
	if (::io_uring_queue_init(256, &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}
	CompletionTable ct;
	FileReader files{&ring, &ct, pack_ud};

	try {
		// warmup: one of each, excluded from stats.
		(void)run_callback(files, cfg);
		(void)run_coroutine(files, cfg);
		(void)run_splice_chain(files, cfg, bytes);

		Agg cb;
		Agg co;
		Agg sp;
		for (std::size_t i = 0; i < cfg.runs; ++i) {
			std::uint64_t const t_cb = run_callback(files, cfg);
			cb.total_ns += t_cb;
			cb.best_ns = std::min(cb.best_ns, t_cb);
			std::uint64_t const t_co = run_coroutine(files, cfg);
			co.total_ns += t_co;
			co.best_ns = std::min(co.best_ns, t_co);
			std::uint64_t const t_sp = run_splice_chain(files, cfg, bytes);
			sp.total_ns += t_sp;
			sp.best_ns = std::min(sp.best_ns, t_sp);
		}

		double const cb_avg = static_cast<double>(cb.total_ns) / static_cast<double>(cfg.runs);
		double const co_avg = static_cast<double>(co.total_ns) / static_cast<double>(cfg.runs);
		double const sp_avg = static_cast<double>(sp.total_ns) / static_cast<double>(cfg.runs);
		double const delta_coro = 100.0 * (co_avg - cb_avg) / cb_avg;
		double const delta_splice = 100.0 * (sp_avg - cb_avg) / cb_avg;

		if (cfg.json_out) {
			std::println(
				"{{\"config\":\"default\",\"variant\":\"callback\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:"
				".2f},\"avg_mib_per_s\":{:.1f},\"best_mib_per_s\":{:.1f},\"best_ns\":{}}}",
				cfg.runs,
				cb.total_ns,
				cb_avg,
				mib_per_sec(bytes, static_cast<std::uint64_t>(cb_avg)),
				mib_per_sec(bytes, cb.best_ns),
				cb.best_ns);
			std::println(
				"{{\"config\":\"default\",\"variant\":\"coroutine\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{"
				":.2f},\"avg_mib_per_s\":{:.1f},\"best_mib_per_s\":{:.1f},\"best_ns\":{}}}",
				cfg.runs,
				co.total_ns,
				co_avg,
				mib_per_sec(bytes, static_cast<std::uint64_t>(co_avg)),
				mib_per_sec(bytes, co.best_ns),
				co.best_ns);
			std::println(
				"{{\"config\":\"default\",\"variant\":\"splice_chain\",\"iterations\":{},\"total_ns\":{},\"ns_per_"
				"iter\":{"
				":.2f},\"avg_mib_per_s\":{:.1f},\"best_mib_per_s\":{:.1f},\"best_ns\":{}}}",
				cfg.runs,
				sp.total_ns,
				sp_avg,
				mib_per_sec(bytes, static_cast<std::uint64_t>(sp_avg)),
				mib_per_sec(bytes, sp.best_ns),
				sp.best_ns);
		} else {
			std::println(
				"size: {} MiB, chunk: {} KiB, runs: {} (+1 warmup each)",
				cfg.size_mib,
				cfg.chunk_kib,
				cfg.runs);
			std::println(
				"  callback     avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
				cb_avg / 1e6,
				static_cast<double>(cb.best_ns) / 1e6,
				mib_per_sec(bytes, static_cast<std::uint64_t>(cb_avg)),
				mib_per_sec(bytes, cb.best_ns));
			std::println(
				"  coroutine    avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
				co_avg / 1e6,
				static_cast<double>(co.best_ns) / 1e6,
				mib_per_sec(bytes, static_cast<std::uint64_t>(co_avg)),
				mib_per_sec(bytes, co.best_ns));
			std::println(
				"  splice_chain avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
				sp_avg / 1e6,
				static_cast<double>(sp.best_ns) / 1e6,
				mib_per_sec(bytes, static_cast<std::uint64_t>(sp_avg)),
				mib_per_sec(bytes, sp.best_ns));
			std::println("  delta        {:+.2f}% avg (coro vs callback)", delta_coro);
			std::println("  delta        {:+.2f}% avg (splice_chain vs callback)", delta_splice);
		}
	} catch (std::exception const &e) {
		std::println(std::cerr, "error: {}", e.what());
		::io_uring_queue_exit(&ring);
		::unlink(cfg.dst_path.c_str());
		::unlink(cfg.src_path.c_str());
		return 1;
	}

	::io_uring_queue_exit(&ring);
	::unlink(cfg.dst_path.c_str());
	::unlink(cfg.src_path.c_str());
}
