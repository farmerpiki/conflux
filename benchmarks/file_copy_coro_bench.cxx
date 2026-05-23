// Benchmark: large file copy paths:
// - block_on per read/write chunk (callback style)
// - single block_on driving a Task<void> that co_awaits read/write in a loop
// - existing compiled splice chain (file → pipe → fd)
#include <cstdlib>
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
	bool no_odirect = false;
	std::string src_path = std::format("/tmp/conflux_copy_src_{}.bin", ::getpid());
	std::string dst_path = std::format("/tmp/conflux_copy_dst_{}.bin", ::getpid());
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
		} else if (a == "--no-odirect") {
			cfg.no_odirect = true;
		} else if (a == "--help" || a == "-h") {
			std::println("Usage: conflux_file_copy_coro_bench [--size-mib N] [--chunk-kib N] [--runs N] [--json] [--no-odirect]");
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
	Config const &cfg,
	bool cold_cache,
	bool sync_dst) {
	if (cold_cache) {
		drop_caches();
	}
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
	if (sync_dst) {
		block_on(files, files.async_fsync(dst));
	}

	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
Task<void> coro_copy(
	FileReader &files,
	std::string src_path,
	std::string dst_path,
	std::size_t chunk,
	bool sync_dst) {
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
	if (sync_dst) {
		co_await files.async_fsync(dst);
	}
	co_return;
}
std::uint64_t run_coroutine(
	FileReader &files,
	Config const &cfg,
	bool cold_cache,
	bool sync_dst) {
	if (cold_cache) {
		drop_caches();
	}
	::unlink(cfg.dst_path.c_str());
	auto const t0 = std::chrono::steady_clock::now();
	block_on(files, coro_copy(files, cfg.src_path, cfg.dst_path, cfg.chunk_kib << 10U, sync_dst));
	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
std::uint64_t run_splice_chain(
	FileReader &files,
	Config const &cfg,
	std::size_t bytes,
	bool cold_cache,
	bool sync_dst) {
	if (cold_cache) {
		drop_caches();
	}
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
	if (sync_dst) {
		block_on(files, files.async_fsync(dst));
	}

	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

struct DirectCopyResult {
	std::uint64_t ns{};
	bool skipped{};
	std::string reason{};
};

[[nodiscard]] std::string errno_text(
	char const *what) {
	return std::format("{}: {}", what, std::strerror(errno));
}

[[nodiscard]] std::size_t align_up(
	std::size_t n,
	std::size_t alignment) noexcept {
	return ((n + alignment - 1U) / alignment) * alignment;
}

DirectCopyResult run_odirect_copy(
	Config const &cfg,
	std::size_t bytes) {
	static constexpr std::size_t kDirectAlignment = 4096;
	::unlink(cfg.dst_path.c_str());
	int const src = ::open(cfg.src_path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT);
	if (src < 0) {
		return DirectCopyResult{.skipped = true, .reason = errno_text("O_DIRECT source open")};
	}
	struct FdGuard {
		int fd{-1};
		~FdGuard() {
			if (fd >= 0) {
				::close(fd);
			}
		}
	} src_guard{src};

	int const dst = ::open(cfg.dst_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_DIRECT, 0644);
	if (dst < 0) {
		return DirectCopyResult{.skipped = true, .reason = errno_text("O_DIRECT destination open")};
	}
	FdGuard dst_guard{dst};

	std::size_t const chunk = std::max<std::size_t>(kDirectAlignment, align_up(cfg.chunk_kib << 10U, kDirectAlignment));
	void *raw{};
	if (::posix_memalign(&raw, kDirectAlignment, chunk) != 0 || raw == nullptr) {
		return DirectCopyResult{.skipped = true, .reason = "posix_memalign failed"};
	}
	struct FreeGuard {
		void *ptr{};
		~FreeGuard() { std::free(ptr); }
	} buf_guard{raw};

	auto const t0 = std::chrono::steady_clock::now();
	std::size_t off = 0;
	while (off < bytes) {
		std::size_t const want = std::min(chunk, bytes - off);
		ssize_t r{};
		for (;;) {
			r = ::pread(src, raw, want, static_cast<off_t>(off));
			if (r < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		if (r < 0) {
			return DirectCopyResult{.skipped = true, .reason = errno_text("O_DIRECT pread")};
		}
		if (r == 0) {
			break;
		}
		std::size_t written = 0;
		while (written < static_cast<std::size_t>(r)) {
			ssize_t w{};
			for (;;) {
				w = ::pwrite(
					dst,
					static_cast<std::byte const *>(raw) + written,
					static_cast<std::size_t>(r) - written,
					static_cast<off_t>(off + written));
				if (w < 0 && errno == EINTR) {
					continue;
				}
				break;
			}
			if (w <= 0) {
				return DirectCopyResult{.skipped = true, .reason = errno_text("O_DIRECT pwrite")};
			}
			written += static_cast<std::size_t>(w);
		}
		off += static_cast<std::size_t>(r);
	}
	auto const t1 = std::chrono::steady_clock::now();
	return DirectCopyResult{
		.ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())};
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
		bool odirect_available = !cfg.no_odirect;
		std::string odirect_skip_reason;
		// warmup: one of each, excluded from stats.
		(void)run_callback(files, cfg, true, true);
		(void)run_coroutine(files, cfg, true, true);
		(void)run_splice_chain(files, cfg, bytes, true, true);
		(void)run_callback(files, cfg, false, false);
		(void)run_coroutine(files, cfg, false, false);
		(void)run_splice_chain(files, cfg, bytes, false, false);
		if (!cfg.no_odirect) {
			auto warm = run_odirect_copy(cfg, bytes);
			if (warm.skipped) {
				odirect_available = false;
				odirect_skip_reason = std::move(warm.reason);
				std::println(std::cerr, "copy_odirect skipped: {}", odirect_skip_reason);
			}
		}

		Agg cb;
		Agg co;
		Agg sp;
		Agg cb_cached;
		Agg co_cached;
		Agg sp_cached;
		Agg odirect;
		auto record = [](Agg &agg, std::uint64_t ns) {
			agg.total_ns += ns;
			agg.best_ns = std::min(agg.best_ns, ns);
		};
		for (std::size_t i = 0; i < cfg.runs; ++i) {
			record(cb, run_callback(files, cfg, true, true));
			record(co, run_coroutine(files, cfg, true, true));
			record(sp, run_splice_chain(files, cfg, bytes, true, true));
			record(cb_cached, run_callback(files, cfg, false, false));
			record(co_cached, run_coroutine(files, cfg, false, false));
			record(sp_cached, run_splice_chain(files, cfg, bytes, false, false));
			if (odirect_available) {
				auto direct = run_odirect_copy(cfg, bytes);
				if (direct.skipped) {
					odirect_available = false;
					odirect_skip_reason = std::move(direct.reason);
					std::println(std::cerr, "copy_odirect skipped: {}", odirect_skip_reason);
				} else {
					record(odirect, direct.ns);
				}
			}
		}

		double const cb_avg = static_cast<double>(cb.total_ns) / static_cast<double>(cfg.runs);
		double const co_avg = static_cast<double>(co.total_ns) / static_cast<double>(cfg.runs);
		double const sp_avg = static_cast<double>(sp.total_ns) / static_cast<double>(cfg.runs);
		double const cb_cached_avg = static_cast<double>(cb_cached.total_ns) / static_cast<double>(cfg.runs);
		double const co_cached_avg = static_cast<double>(co_cached.total_ns) / static_cast<double>(cfg.runs);
		double const sp_cached_avg = static_cast<double>(sp_cached.total_ns) / static_cast<double>(cfg.runs);
		double const odirect_avg = odirect_available ? static_cast<double>(odirect.total_ns) / static_cast<double>(cfg.runs) : 0.0;
		double const delta_coro = 100.0 * (co_avg - cb_avg) / cb_avg;
		double const delta_splice = 100.0 * (sp_avg - cb_avg) / cb_avg;
		double const delta_coro_cached = 100.0 * (co_cached_avg - cb_cached_avg) / cb_cached_avg;
		double const delta_splice_cached = 100.0 * (sp_cached_avg - cb_cached_avg) / cb_cached_avg;

		if (cfg.json_out) {
			auto print_json = [&](std::string_view variant, Agg const &agg, double avg, bool cold_cache, bool sync_dst, bool direct_io = false) {
				std::println(
					"{{\"config\":\"default\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},\"avg_mib_per_s\":{:.1f},\"best_mib_per_s\":{:.1f},\"best_ns\":{},\"label\":\"live-kernel-sanity\",\"cold_cache\":{},\"sync_dst\":{},\"direct_io\":{}}}",
					variant,
					cfg.runs,
					agg.total_ns,
					avg,
					mib_per_sec(bytes, static_cast<std::uint64_t>(avg)),
					mib_per_sec(bytes, agg.best_ns),
					agg.best_ns,
					cold_cache ? "true" : "false",
					sync_dst ? "true" : "false",
					direct_io ? "true" : "false");
			};
			print_json("callback", cb, cb_avg, true, true);
			print_json("coroutine", co, co_avg, true, true);
			print_json("splice_chain", sp, sp_avg, true, true);
			print_json("callback_cached_no_fsync", cb_cached, cb_cached_avg, false, false);
			print_json("coroutine_cached_no_fsync", co_cached, co_cached_avg, false, false);
			print_json("splice_chain_cached_no_fsync", sp_cached, sp_cached_avg, false, false);
			if (odirect_available) {
				print_json("copy_odirect", odirect, odirect_avg, false, false, true);
			}
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
			std::println(
				"  callback cached_no_fsync     avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
				cb_cached_avg / 1e6,
				static_cast<double>(cb_cached.best_ns) / 1e6,
				mib_per_sec(bytes, static_cast<std::uint64_t>(cb_cached_avg)),
				mib_per_sec(bytes, cb_cached.best_ns));
			std::println(
				"  coroutine cached_no_fsync    avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
				co_cached_avg / 1e6,
				static_cast<double>(co_cached.best_ns) / 1e6,
				mib_per_sec(bytes, static_cast<std::uint64_t>(co_cached_avg)),
				mib_per_sec(bytes, co_cached.best_ns));
			std::println(
				"  splice cached_no_fsync       avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
				sp_cached_avg / 1e6,
				static_cast<double>(sp_cached.best_ns) / 1e6,
				mib_per_sec(bytes, static_cast<std::uint64_t>(sp_cached_avg)),
				mib_per_sec(bytes, sp_cached.best_ns));
			std::println("  delta        {:+.2f}% avg (coro vs callback)", delta_coro);
			if (odirect_available) {
				std::println(
					"  copy_odirect no_fsync       avg {:>9.1f} ms  best {:>9.1f} ms  avg {:>6.1f} MiB/s  best {:>6.1f} MiB/s",
					odirect_avg / 1e6,
					static_cast<double>(odirect.best_ns) / 1e6,
					mib_per_sec(bytes, static_cast<std::uint64_t>(odirect_avg)),
					mib_per_sec(bytes, odirect.best_ns));
			} else if (!odirect_skip_reason.empty()) {
				std::println("  copy_odirect skipped: {}", odirect_skip_reason);
			}
			std::println("  delta        {:+.2f}% avg (splice_chain vs callback)", delta_splice);
			std::println("  delta cached {:+.2f}% avg (coro vs callback)", delta_coro_cached);
			std::println("  delta cached {:+.2f}% avg (splice_chain vs callback)", delta_splice_cached);
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
