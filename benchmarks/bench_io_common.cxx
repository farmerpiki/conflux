module;
#include <cerrno>
#include <liburing.h>
#include <stdlib.h>
#include <unistd.h>

export module bench_io_common;

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import bench_common;

export [[nodiscard]] constexpr std::uint64_t bench_pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}

export struct BenchUringFileConfig {
	std::size_t iterations = 20000;
	std::size_t warmup = 1000;
	std::size_t depth = 64;
	std::size_t chunk = 4096;
	std::string config_name;
	bool json_out = false;
	BenchArgs bench;
};

export [[nodiscard]] BenchUringFileConfig bench_parse_uring_file_args(
	std::span<char *> args) {
	BenchUringFileConfig cfg;
	auto base = bench_parse_args(args);
	if (!base.iterations_explicit || base.iterations > 0) {
		cfg.iterations = base.iterations;
	}
	cfg.warmup = base.warmup;
	cfg.config_name = std::move(base.config_name);
	cfg.json_out = base.json_out;
	cfg.bench = std::move(base);
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const a = args[i];
		if (a == "--depth" && i + 1 < args.size()) {
			cfg.depth = bench_parse_sz(args[++i]);
		} else if (a == "--chunk" && i + 1 < args.size()) {
			cfg.chunk = bench_parse_sz(args[++i]);
		}
	}
	cfg.depth = std::max<std::size_t>(1, cfg.depth);
	cfg.chunk = std::max<std::size_t>(1, cfg.chunk);
	return cfg;
}

export struct BenchTempFile {
	std::string path;
	int fd = -1;

	explicit BenchTempFile(
		std::string_view prefix)
		: path{std::format("/tmp/{}_{}_XXXXXX", prefix, ::getpid())} {
		fd = ::mkstemp(path.data());
		if (fd < 0) {
			throw std::runtime_error{"mkstemp failed"};
		}
	}
	~BenchTempFile() {
		if (fd >= 0) {
			::close(fd);
		}
		if (!path.empty()) {
			::unlink(path.c_str());
		}
	}
	BenchTempFile(BenchTempFile const &) = delete;
	BenchTempFile &operator =(BenchTempFile const &) = delete;
};

export void bench_fill_temp_file(
	BenchTempFile &file,
	std::size_t bytes,
	std::uint64_t seed) {
	if (::ftruncate(file.fd, 0) != 0) {
		throw std::runtime_error{"ftruncate reset failed"};
	}
	std::vector<std::byte> buf(1U << 20U);
	std::mt19937_64 rng{seed};
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

export template<typename T>
struct BenchJoinSlot {
	std::atomic<std::size_t> *done{};
	std::exception_ptr err{};
	[[no_unique_address]] std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>> value{};
};

export template<typename T>
void bench_install_join(
	conflux::work::root::Task<T> task,
	std::shared_ptr<BenchJoinSlot<T>> slot) {
	namespace root = conflux::work::root;
	auto handle = std::make_shared<root::TaskJoinHandle<T>>(root::into_join_handle(std::move(task)));
	handle->control().set_on_ready_or_run([slot = std::move(slot), handle]() noexcept {
		try {
			auto outcome = root::blocking_join(std::move(*handle));
			if (outcome.is_failure()) {
				slot->err = std::move(outcome).failure().error;
			} else if (outcome.is_cancelled()) {
				slot->err = std::make_exception_ptr(conflux::work::Cancelled{});
			} else if constexpr (!std::is_void_v<T>) {
				slot->value.emplace(std::move(outcome).success().value);
			}
		} catch (...) { slot->err = std::current_exception(); }
		slot->done->fetch_add(1, std::memory_order_release);
	});
}

export inline void bench_pump_until_count(
	conflux::file_io::FileReader &reader,
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
		if (rc == -EINTR || rc == -EAGAIN || (rc >= 0 && cqe == nullptr)) {
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

export inline void bench_dispatch_cqes(
	::io_uring &ring,
	conflux::uring::CompletionTable &completions,
	std::size_t expected) {
	std::size_t completed = 0;
	while (completed < expected) {
		::io_uring_cqe *cqe = nullptr;
		int rc = ::io_uring_submit_and_wait(&ring, 1);
		if (rc >= 0) {
			rc = ::io_uring_peek_cqe(&ring, &cqe);
		}
		if (rc == -EINTR || rc == -EAGAIN || (rc >= 0 && cqe == nullptr)) {
			continue;
		}
		if (rc < 0 || cqe == nullptr) {
			throw std::runtime_error{std::format("submit_and_wait rc={}", rc)};
		}
		std::array<::io_uring_cqe *, 128> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(&ring, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				auto const *c = batch[static_cast<std::size_t>(i)];
				auto const slot = static_cast<std::uint32_t>(c->user_data & 0xFFFFFFFFU);
				auto const gen = static_cast<std::uint32_t>(c->user_data >> 32U);
				completions.dispatch(slot, gen, c->res, conflux::uring::CqeFlags{c->flags});
			}
			::io_uring_cq_advance(&ring, n);
			completed += n;
		}
	}
}

export inline void bench_drain_raw_cqes(
	::io_uring &ring,
	std::size_t expected) {
	std::size_t completed = 0;
	while (completed < expected) {
		::io_uring_cqe *cqe = nullptr;
		int rc = ::io_uring_submit_and_wait(&ring, 1);
		if (rc == -EINTR) {
			continue;
		}
		if (rc >= 0) {
			rc = ::io_uring_peek_cqe(&ring, &cqe);
		}
		if (rc == -EINTR || rc == -EAGAIN || (rc >= 0 && cqe == nullptr)) {
			continue;
		}
		if (rc < 0 || cqe == nullptr) {
			throw std::runtime_error{std::format("raw submit_and_wait rc={}", rc)};
		}
		std::array<::io_uring_cqe *, 128> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(&ring, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			::io_uring_cq_advance(&ring, n);
			completed += n;
		}
	}
}

export template<typename T>
std::vector<std::shared_ptr<BenchJoinSlot<T>>> bench_make_join_slots(
	std::size_t depth,
	std::atomic<std::size_t> &done) {
	std::vector<std::shared_ptr<BenchJoinSlot<T>>> slots;
	slots.reserve(depth);
	for (std::size_t i = 0; i < depth; ++i) {
		auto slot = std::make_shared<BenchJoinSlot<T>>();
		slot->done = &done;
		slots.push_back(std::move(slot));
	}
	return slots;
}
