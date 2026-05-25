// storage_read_bench — storage read strategy matrix for the IOPOLL gate.
//
// This benchmark exists to keep storage-path performance claims separate from
// generic file/SQE storm smoke rows. By default it only runs on NVMe-backed
// paths and otherwise exits 0 with a clear skip reason; use --allow-non-nvme for
// local development or CI smoke builds.

#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;

import bench_common;
import bench_io_common;

using namespace std::string_view_literals;

namespace {

constexpr std::size_t kib = 1024;
constexpr std::size_t mib = 1024 * kib;

struct BenchSkip final : std::runtime_error {
	using std::runtime_error::runtime_error;
};

struct UniqueRawFd {
	int fd{-1};
	UniqueRawFd() noexcept = default;
	explicit UniqueRawFd(
		int native) noexcept
		: fd{native} {}
	UniqueRawFd(UniqueRawFd const &) = delete;
	UniqueRawFd &operator =(UniqueRawFd const &) = delete;
	UniqueRawFd(
		UniqueRawFd &&o) noexcept
		: fd{std::exchange(o.fd, -1)} {}
	UniqueRawFd &operator =(
		UniqueRawFd &&o) noexcept {
		if (this != &o) {
			reset();
			fd = std::exchange(o.fd, -1);
		}
		return *this;
	}
	~UniqueRawFd() { reset(); }
	void reset() noexcept {
		if (fd >= 0) {
			::close(fd);
			fd = -1;
		}
	}
	[[nodiscard]] bool valid() const noexcept { return fd >= 0; }
};

struct Config {
	std::size_t iterations = 256;
	std::size_t warmup = 16;
	std::size_t depth = 32;
	std::size_t chunk = 64 * kib;
	std::string path;
	std::string config_name;
	std::string mode = "all";
	bool json_out = false;
	bool keep_file = false;
	bool allow_non_nvme = false;
};

[[nodiscard]] Config parse_args(
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
		} else if (a == "--path" && i + 1 < args.size()) {
			cfg.path = args[++i];
		} else if (a == "--mode" && i + 1 < args.size()) {
			cfg.mode = args[++i];
		} else if (a == "--config-name" && i + 1 < args.size()) {
			cfg.config_name = args[++i];
		} else if (a == "--json") {
			cfg.json_out = true;
		} else if (a == "--keep-file") {
			cfg.keep_file = true;
		} else if (a == "--allow-non-nvme") {
			cfg.allow_non_nvme = true;
		}
	}
	cfg.depth = std::max<std::size_t>(1, cfg.depth);
	cfg.chunk = std::max<std::size_t>(4096, cfg.chunk);
	cfg.chunk = ((cfg.chunk + 4095U) / 4096U) * 4096U;
	return cfg;
}

[[nodiscard]] bool wants_mode(
	Config const &cfg,
	std::string_view mode) noexcept {
	return cfg.mode == "all" || cfg.mode == mode;
}

[[nodiscard]] std::string dirname_of(
	std::string_view path) {
	auto const slash = path.rfind('/');
	if (slash == std::string_view::npos) {
		return ".";
	}
	if (slash == 0) {
		return "/";
	}
	return std::string{path.substr(0, slash)};
}

[[nodiscard]] std::string read_symlink(
	std::string const &path) {
	std::string out(4096, '\0');
	ssize_t const n = ::readlink(path.c_str(), out.data(), out.size() - 1U);
	if (n < 0) {
		return {};
	}
	out.resize(static_cast<std::size_t>(n));
	return out;
}

[[nodiscard]] bool path_is_nvme_backed(
	std::string const &path,
	std::string &reason) {
	struct stat st{};
	std::string probe = path;
	if (::stat(probe.c_str(), &st) != 0) {
		probe = dirname_of(path);
		if (::stat(probe.c_str(), &st) != 0) {
			reason = std::format("stat failed for {}: {}", probe, std::strerror(errno));
			return false;
		}
	}
	dev_t const dev = st.st_dev;
	std::string const sys_dev = std::format("/sys/dev/block/{}:{}", major(dev), minor(dev));
	std::string const target = read_symlink(sys_dev);
	if (target.empty()) {
		reason = std::format("{} has no readable sysfs block-device mapping", probe);
		return false;
	}
	if (target.find("/nvme") != std::string::npos || target.find("nvme") != std::string::npos) {
		return true;
	}
	reason = std::format("{} maps to non-NVMe device {}", probe, target);
	return false;
}

[[nodiscard]] std::string nvme_probe_path(
	Config const &cfg) {
	if (!cfg.path.empty()) {
		return cfg.path;
	}
	return "/tmp/conflux_storage_read_bench_probe";
}

struct TempBenchFile {
	std::string path;
	bool remove_on_destroy{true};

	~TempBenchFile() {
		if (remove_on_destroy && !path.empty()) {
			::unlink(path.c_str());
		}
	}
};

[[nodiscard]] TempBenchFile prepare_file(
	Config const &cfg,
	std::size_t bytes) {
	TempBenchFile out;
	out.remove_on_destroy = !cfg.keep_file;
	if (cfg.path.empty()) {
		out.path = std::format("/tmp/conflux_storage_read_bench_{}_XXXXXX", ::getpid());
		int const fd = ::mkstemp(out.path.data());
		if (fd < 0) {
			throw std::runtime_error{std::format("mkstemp failed: {}", std::strerror(errno))};
		}
		::close(fd);
	} else {
		out.path = cfg.path;
	}

	UniqueRawFd fd{::open(out.path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600)};
	if (!fd.valid()) {
		throw std::runtime_error{std::format("open seed file failed: {}", std::strerror(errno))};
	}
	std::vector<std::byte> buf(std::min<std::size_t>(bytes, mib));
	std::mt19937_64 rng{0x5710A6E5ULL};
	for (auto &b: buf) {
		b = static_cast<std::byte>(rng() & 0xFFU);
	}
	std::size_t left = bytes;
	while (left > 0) {
		std::size_t const n = std::min(left, buf.size());
		ssize_t const rc = ::write(fd.fd, buf.data(), n);
		if (rc <= 0) {
			throw std::runtime_error{std::format("seed write failed: {}", std::strerror(errno))};
		}
		left -= static_cast<std::size_t>(rc);
	}
	if (::fsync(fd.fd) != 0) {
		throw std::runtime_error{std::format("seed fsync failed: {}", std::strerror(errno))};
	}
	return out;
}

struct StorageStats {
	std::string_view config;
	std::string_view variant;
	std::size_t operations{};
	std::size_t bytes{};
	std::uint64_t total_ns{};
	double ns_per_op{};
	double mib_per_s{};
	std::size_t depth{};
	std::size_t chunk{};
	std::size_t direct_reads{};
	std::size_t fallbacks{};
	bool live_kernel_sanity{true};
};

void print_stats(
	StorageStats const &s,
	bool json_out,
	bool first) {
	(void)first;
	if (json_out) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"operations\":{},\"bytes\":{},\"total_ns\":{},"
			"\"ns_per_iter\":{:.2f},\"mib_per_s\":{:.2f},\"depth\":{},\"chunk\":{},\"direct_reads\":{},\"fallbacks\":{}"
			",\"label\":\"{}\"}}",
			s.config,
			s.variant,
			s.operations,
			s.operations,
			s.bytes,
			s.total_ns,
			s.ns_per_op,
			s.mib_per_s,
			s.depth,
			s.chunk,
			s.direct_reads,
			s.fallbacks,
			s.live_kernel_sanity ? "live-kernel-sanity" : "micro/user-space");
		return;
	}
	if (!s.config.empty()) {
		std::print("[{}] ", s.config);
	}
	std::println(
		"{:<22} {:>10} ops  {:>9.2f} ns/op  {:>10.2f} MiB/s  depth={} chunk={} direct={} fallback={}",
		s.variant,
		s.operations,
		s.ns_per_op,
		s.mib_per_s,
		s.depth,
		s.chunk,
		s.direct_reads,
		s.fallbacks);
}

[[nodiscard]] std::uint64_t read_offset(
	std::size_t batch,
	std::size_t idx,
	Config const &cfg,
	std::size_t file_bytes) noexcept {
	std::size_t const slots = std::max<std::size_t>(1, file_bytes / cfg.chunk);
	return (((batch * cfg.depth) + idx) % slots) * cfg.chunk;
}

[[nodiscard]] StorageStats finish_stats(
	Config const &cfg,
	std::string_view variant,
	std::size_t operations,
	std::size_t bytes,
	std::uint64_t elapsed,
	std::size_t direct_reads = 0,
	std::size_t fallbacks = 0) {
	double const secs = static_cast<double>(elapsed) / 1'000'000'000.0;
	return StorageStats{
		.config = cfg.config_name,
		.variant = variant,
		.operations = operations,
		.bytes = bytes,
		.total_ns = elapsed,
		.ns_per_op = static_cast<double>(elapsed) / static_cast<double>(operations),
		.mib_per_s = secs > 0.0 ? (static_cast<double>(bytes) / static_cast<double>(mib)) / secs : 0.0,
		.depth = cfg.depth,
		.chunk = cfg.chunk,
		.direct_reads = direct_reads,
		.fallbacks = fallbacks,
	};
}

StorageStats bench_pread(
	int fd,
	Config const &cfg,
	std::size_t file_bytes,
	std::size_t batches,
	bool warmup) {
	std::vector<std::byte> buf(cfg.chunk);
	std::size_t total_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			ssize_t const rc =
				::pread(fd, buf.data(), cfg.chunk, static_cast<off_t>(read_offset(batch, i, cfg, file_bytes)));
			if (rc < 0) {
				throw std::runtime_error{std::format("pread failed: {}", std::strerror(errno))};
			}
			total_bytes += static_cast<std::size_t>(rc);
		}
	}
	auto const elapsed = bench_now_ns() - t0;
	if (!warmup && total_bytes == 0) {
		throw std::runtime_error{"pread read zero bytes"};
	}
	return finish_stats(cfg, "pread"sv, batches * cfg.depth, total_bytes, elapsed, 0, 0);
}

StorageStats bench_uring_read(
	FileReader &files,
	FileHandle const &fh,
	Config const &cfg,
	std::size_t file_bytes,
	std::size_t batches,
	bool warmup) {
	std::vector<std::vector<std::byte>> bufs(cfg.depth, std::vector<std::byte>(cfg.chunk));
	std::size_t total_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		std::atomic<std::size_t> done{0};
		auto slots = bench_make_join_slots<std::size_t>(cfg.depth, done);
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			bench_install_join(files.read_into(fh, read_offset(batch, i, cfg, file_bytes), bufs[i]), slots[i]);
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
	if (!warmup && total_bytes == 0) {
		throw std::runtime_error{"io_uring_read read zero bytes"};
	}
	return finish_stats(cfg, "io_uring_read"sv, batches * cfg.depth, total_bytes, elapsed, 0, 0);
}

StorageStats bench_read_fixed(
	FileReader &files,
	FileHandle const &fh,
	FixedBufferPool &pool,
	Config const &cfg,
	std::size_t file_bytes,
	std::size_t batches,
	bool warmup) {
	std::size_t total_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		std::atomic<std::size_t> done{0};
		auto slots = bench_make_join_slots<FileReader::ReadFixedResult>(cfg.depth, done);
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			auto buf = pool.try_acquire();
			if (!buf) {
				throw std::runtime_error{"fixed read buffer pool exhausted"};
			}
			bench_install_join(
				files.read_fixed(fh, read_offset(batch, i, cfg, file_bytes), std::move(*buf), cfg.chunk),
				slots[i]);
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
	if (!warmup && total_bytes == 0) {
		throw std::runtime_error{"read_fixed read zero bytes"};
	}
	return finish_stats(cfg, "read_fixed"sv, batches * cfg.depth, total_bytes, elapsed, 0, 0);
}

StorageStats bench_iopoll_read_fixed(
	IopollStorageRing &storage,
	FileHandle const &fh,
	Config const &cfg,
	std::size_t file_bytes,
	std::size_t batches,
	bool warmup) {
	std::size_t total_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			auto buf = storage.try_acquire_buffer();
			if (!buf) {
				throw std::runtime_error{"iopoll fixed buffer pool exhausted"};
			}
			auto result = block_on_iopoll(
				storage.reader(),
				storage.reader()
					.read_nocache_fixed(fh, read_offset(batch, i, cfg, file_bytes), std::move(*buf), cfg.chunk, 4096),
				std::chrono::seconds{5});
			total_bytes += result.bytes;
		}
	}
	auto const elapsed = bench_now_ns() - t0;
	if (!warmup && total_bytes == 0) {
		throw std::runtime_error{"iopoll_read_fixed read zero bytes"};
	}
	return finish_stats(cfg, "iopoll_read_fixed"sv, batches * cfg.depth, total_bytes, elapsed, batches * cfg.depth, 0);
}

void run_file_reader_modes(
	Config const &cfg,
	TempBenchFile const &file,
	std::size_t file_bytes,
	std::vector<StorageStats> &stats) {
	bool const need_ring = wants_mode(cfg, "io_uring_read"sv) || wants_mode(cfg, "read_fixed"sv);
	if (!need_ring) {
		return;
	}
	::io_uring ring{};
	int const qrc = ::io_uring_queue_init(static_cast<unsigned>(std::max<std::size_t>(256, cfg.depth * 2U)), &ring, 0);
	if (qrc < 0) {
		throw BenchSkip{std::format("io_uring unavailable: {}", std::strerror(-qrc))};
	}
	struct RingGuard {
		::io_uring *ring{};
		~RingGuard() {
			if (ring != nullptr) {
				::io_uring_queue_exit(ring);
			}
		}
	} guard{&ring};

	CompletionTable completions{cfg.depth * 2U};
	FileReader files{&ring, &completions, bench_pack_ud};
	auto handle = block_on(files, files.async_open(AT_FDCWD, file.path, O_RDONLY | O_CLOEXEC));

	if (wants_mode(cfg, "io_uring_read"sv)) {
		(void)bench_uring_read(files, handle, cfg, file_bytes, cfg.warmup, true);
		stats.push_back(bench_uring_read(files, handle, cfg, file_bytes, cfg.iterations, false));
	}
	if (wants_mode(cfg, "read_fixed"sv)) {
		RegisteredBufferTable table{&ring, static_cast<unsigned>(cfg.depth)};
		if (!table.ok()) {
			throw BenchSkip{"registered fixed buffers unsupported"};
		}
		FixedBufferPool pool{&table, 0, cfg.depth, cfg.chunk};
		if (!pool.ok() || pool.capacity() < cfg.depth) {
			throw BenchSkip{"fixed buffer pool init failed"};
		}
		(void)bench_read_fixed(files, handle, pool, cfg, file_bytes, cfg.warmup, true);
		stats.push_back(bench_read_fixed(files, handle, pool, cfg, file_bytes, cfg.iterations, false));
	}
}

void run_iopoll_mode(
	Config const &cfg,
	TempBenchFile const &file,
	std::size_t file_bytes,
	std::vector<StorageStats> &stats) {
	if (!wants_mode(cfg, "iopoll_read_fixed"sv)) {
		return;
	}
	IopollStorageRingOptions options{
		.entries = static_cast<unsigned>(std::max<std::size_t>(256, cfg.depth * 2U)),
		.fixed_buffer_slots = static_cast<unsigned>(cfg.depth),
		.fixed_buffer_bytes = cfg.chunk,
	};
	auto storage_result = IopollStorageRing::create(options);
	if (!storage_result) {
		throw BenchSkip{std::format("iopoll storage ring unavailable: {}", storage_result.error().what())};
	}
	auto storage = std::move(*storage_result);
	UniqueRawFd direct_fd{::open(file.path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT)};
	if (!direct_fd.valid()) {
		throw BenchSkip{std::format("O_DIRECT open failed: {}", std::strerror(errno))};
	}
	FileHandle handle = FileHandle::from_fd(direct_fd.fd);
	direct_fd.fd = -1;
	(void)bench_iopoll_read_fixed(*storage, handle, cfg, file_bytes, cfg.warmup, true);
	stats.push_back(bench_iopoll_read_fixed(*storage, handle, cfg, file_bytes, cfg.iterations, false));
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"storage_read","parser":"standard","configs":[{"name":"depth_1_4k","extra":{"depth":1,"chunk":4096,"label":"live-kernel-sanity"},"target_ms":500,"max_iterations":20000,"calibration_iterations":4,"args":["--depth","1","--chunk","4096","--config-name","depth_1_4k","--iterations","0","--warmup","0"]},{"name":"depth_8_16k","extra":{"depth":8,"chunk":16384,"label":"live-kernel-sanity"},"target_ms":500,"max_iterations":10000,"calibration_iterations":4,"args":["--depth","8","--chunk","16384","--config-name","depth_8_16k","--iterations","0","--warmup","0"]},{"name":"depth_32_64k","extra":{"depth":32,"chunk":65536,"label":"live-kernel-sanity"},"target_ms":500,"max_iterations":4000,"calibration_iterations":4,"args":["--depth","32","--chunk","65536","--config-name","depth_32_64k","--iterations","0","--warmup","0"]},{"name":"depth_128_1m","extra":{"depth":128,"chunk":1048576,"label":"live-kernel-sanity"},"target_ms":500,"max_iterations":256,"calibration_iterations":2,"args":["--depth","128","--chunk","1048576","--config-name","depth_128_1m","--iterations","0","--warmup","0"]}]})");

	try {
		auto cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
		std::size_t const file_bytes = std::max<std::size_t>(cfg.depth * cfg.chunk, 128 * mib);

		if (!cfg.allow_non_nvme) {
			std::string reason;
			if (!path_is_nvme_backed(nvme_probe_path(cfg), reason)) {
				throw BenchSkip{
					std::format("storage_read_bench skipped: {}; pass --allow-non-nvme for smoke runs", reason)};
			}
		}

		auto file = prepare_file(cfg, file_bytes);

		std::vector<StorageStats> stats;
		stats.reserve(4);

		if (wants_mode(cfg, "pread"sv)) {
			UniqueRawFd fd{::open(file.path.c_str(), O_RDONLY | O_CLOEXEC)};
			if (!fd.valid()) {
				throw std::runtime_error{std::format("open pread file failed: {}", std::strerror(errno))};
			}
			(void)bench_pread(fd.fd, cfg, file_bytes, cfg.warmup, true);
			stats.push_back(bench_pread(fd.fd, cfg, file_bytes, cfg.iterations, false));
		}

		run_file_reader_modes(cfg, file, file_bytes, stats);
		run_iopoll_mode(cfg, file, file_bytes, stats);

		for (std::size_t i = 0; i < stats.size(); ++i) {
			print_stats(stats[i], cfg.json_out, i == 0);
		}
		if (stats.empty()) {
			throw BenchSkip{std::format("no selected storage_read_bench modes for --mode {}", cfg.mode)};
		}
	} catch (BenchSkip const &skip) {
		std::println(std::cerr, "{}", skip.what());
		return 0;
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_storage_read_bench: {}", ex.what());
		return 1;
	}
}
