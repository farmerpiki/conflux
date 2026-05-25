module;
#include <cerrno>
#include <liburing.h>

export module bench_io_common;

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;

export [[nodiscard]] constexpr std::uint64_t bench_pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
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
				slot->err = std::make_exception_ptr(::Cancelled{});
			} else if constexpr (!std::is_void_v<T>) {
				slot->value.emplace(std::move(outcome).success().value);
			}
		} catch (...) { slot->err = std::current_exception(); }
		slot->done->fetch_add(1, std::memory_order_release);
	});
}

export inline void bench_pump_until_count(
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
