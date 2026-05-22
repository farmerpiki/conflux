// file_io_fixed_bench — batches FileReader fixed-buffer read/write SQEs.
//
// The benchmark keeps kernel round-trips in the measurement, but submits enough
// independent fixed-buffer SQEs per batch for completion callback ownership
// costs to show through.

#include <fcntl.h>
#include <liburing.h>
#include <stdlib.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;

import bench_common;

using namespace std::string_view_literals;
namespace root = conflux::work::root;

namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}

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
	std::string path = std::format("/tmp/conflux_fixed_bench_{}_XXXXXX", ::getpid());
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
	std::mt19937_64 rng{0xF17EDULL};
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

template<typename T>
struct JoinSlot {
	std::atomic<std::size_t> *done{};
	std::exception_ptr err{};
	[[no_unique_address]] std::optional<T> value{};
};

template<typename T>
void install_join(
	root::Task<T> task,
	std::shared_ptr<JoinSlot<T>> slot) {
	auto handle = std::make_shared<root::TaskJoinHandle<T>>(root::into_join_handle(std::move(task)));
	handle->control().set_on_ready_or_run([slot = std::move(slot), handle]() noexcept {
		try {
			auto outcome = root::blocking_join(std::move(*handle));
			if (outcome.is_failure()) {
				slot->err = std::move(outcome).failure().error;
			} else if (outcome.is_cancelled()) {
				slot->err = std::make_exception_ptr(::Cancelled{});
			} else {
				slot->value.emplace(std::move(outcome).success().value);
			}
		} catch (...) { slot->err = std::current_exception(); }
		slot->done->fetch_add(1, std::memory_order_release);
	});
}

void pump_until_count(
	FileReader &reader,
	std::atomic<std::size_t> &done,
	std::size_t target) {
	auto *ring = reader.ring();
	auto *completions = reader.completions();
	while (done.load(std::memory_order_acquire) < target) {
		::io_uring_cqe *cqe = nullptr;
		int rc = ::io_uring_submit_and_wait(ring, 1);
		if (rc >= 0) {
			rc = ::io_uring_peek_cqe(ring, &cqe);
		}
		if (rc == -EINTR || (rc >= 0 && cqe == nullptr)) {
			continue;
		}
		if (rc < 0 || cqe == nullptr) {
			throw std::runtime_error{std::format("submit_and_wait rc={}", rc)};
		}
		std::array<::io_uring_cqe *, 64> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(ring, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				auto const *c = batch[static_cast<std::size_t>(i)];
				auto const slot = static_cast<std::uint32_t>(c->user_data & 0xFFFFFFFFU);
				auto const gen = static_cast<std::uint32_t>(c->user_data >> 32U);
				completions->dispatch(slot, gen, c->res, conflux::uring::CqeFlags{c->flags});
			}
			::io_uring_cq_advance(ring, n);
			if (done.load(std::memory_order_acquire) >= target) {
				break;
			}
		}
	}
}

template<typename T>
std::vector<std::shared_ptr<JoinSlot<T>>> make_slots(
	std::size_t depth,
	std::atomic<std::size_t> &done) {
	std::vector<std::shared_ptr<JoinSlot<T>>> slots;
	slots.reserve(depth);
	for (std::size_t i = 0; i < depth; ++i) {
		auto slot = std::make_shared<JoinSlot<T>>();
		slot->done = &done;
		slots.push_back(std::move(slot));
	}
	return slots;
}

BenchStats bench_read_fixed(
	FileReader &files,
	FileHandle const &fh,
	FixedBufferPool &pool,
	Config const &cfg,
	bool warmup) {
	std::size_t const batches = warmup ? cfg.warmup : cfg.iterations;
	std::uint64_t total_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		std::atomic<std::size_t> done{0};
		auto slots = make_slots<FileReader::ReadFixedResult>(cfg.depth, done);
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			auto buf = pool.try_acquire();
			if (!buf) {
				throw std::runtime_error{"fixed read buffer pool exhausted"};
			}
			auto offset = ((batch + i) % cfg.depth) * cfg.chunk;
			install_join(files.read_fixed(fh, offset, std::move(*buf), cfg.chunk), slots[i]);
		}
		pump_until_count(files, done, cfg.depth);
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
	Config const &cfg,
	bool warmup) {
	std::size_t const batches = warmup ? cfg.warmup : cfg.iterations;
	std::uint64_t total_bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t batch = 0; batch < batches; ++batch) {
		std::atomic<std::size_t> done{0};
		auto slots = make_slots<FileReader::WriteFixedResult>(cfg.depth, done);
		for (std::size_t i = 0; i < cfg.depth; ++i) {
			auto buf = pool.try_acquire();
			if (!buf) {
				throw std::runtime_error{"fixed write buffer pool exhausted"};
			}
			std::ranges::fill(buf->view().first(cfg.chunk), static_cast<std::byte>((batch + i) & 0xFFU));
			auto offset = ((batch + i) % cfg.depth) * cfg.chunk;
			install_join(files.write_fixed(fh, offset, std::move(*buf), cfg.chunk), slots[i]);
		}
		pump_until_count(files, done, cfg.depth);
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
		R"({"name":"file_io_fixed","parser":"standard","configs":[{"name":"depth_64_4k","extra":{"depth":64,"chunk":4096},"args":["--depth","64","--chunk","4096","--config-name","depth_64_4k","--iterations","20000","--warmup","1000"]},{"name":"depth_128_4k","extra":{"depth":128,"chunk":4096},"args":["--depth","128","--chunk","4096","--config-name","depth_128_4k","--iterations","10000","--warmup","500"]}]})");

	auto cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	TempFile file;
	fill_file(file, cfg.depth * cfg.chunk);

	::io_uring ring{};
	if (::io_uring_queue_init(static_cast<unsigned>(std::max<std::size_t>(256, cfg.depth * 2U)), &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}

	try {
		CompletionTable completions{cfg.depth * 2U};
		FileReader files{&ring, &completions, pack_ud};
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
