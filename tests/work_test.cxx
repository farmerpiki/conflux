// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.net.io_buffer;

namespace root = conflux::work::root;
using conflux::work::async_run_cancellable_on;
using conflux::work::async_run_on;
using conflux::work::Cancelled;
using conflux::work::join_all;
using conflux::work::RingLane;
using conflux::work::RingLaneOptions;
using conflux::work::sync_wait;
using conflux::work::WorkPool;
using conflux::work::WorkPoolOptions;
using conflux::work::WorkPoolQueueMode;
struct RejectingQueueTarget {
	template<class Job>
	bool enqueue(
		Job &&) {
		return false;
	}
	[[nodiscard]] bool stopped() const noexcept { return false; }
};
TEST_CASE(
	"work: async_run_on executes callable on pool",
	"[work]") {
	WorkPool pool;
	auto result = sync_wait(async_run_on(pool, [] { return 42; }));
	CHECK(result == 42);
}
TEST_CASE(
	"work: async_run_on high-iteration roundtrip remains live",
	"[work]") {
	WorkPool pool;
	constexpr std::size_t kIters = 50001;
	for (std::size_t i = 0; i < kIters; ++i) {
		auto const v = root::value(async_run_on(pool, [] { return 7; }));
		REQUIRE(v == 7);
	}
}
TEST_CASE(
	"work: async_run_on propagates exception",
	"[work]") {
	WorkPool pool;
	bool caught = false;
	try {
		sync_wait(async_run_on(pool, []() -> int { throw std::runtime_error{"boom"}; }));
	} catch (std::runtime_error const &e) { caught = std::string_view{e.what()} == "boom"; }
	CHECK(caught);
}
TEST_CASE(
	"work: async_run_on cancelled when pool stopped",
	"[work]") {
	WorkPool pool;
	pool.stop();
	bool cancelled = false;
	try {
		sync_wait(async_run_on(pool, [] { return 99; }));
	} catch (Cancelled const &) { cancelled = true; }
	CHECK(cancelled);
}
TEST_CASE(
	"work: async_run_on stopped pool reports shutdown cancellation",
	"[work]") {
	WorkPool pool;
	pool.stop();
	auto task = async_run_on(pool, [] { return 99; });
	auto out = root::blocking_join(std::move(task));
	REQUIRE(out.is_cancelled());
	CHECK(out.cancelled().reason == root::CancelReason::shutdown);
}
TEST_CASE(
	"work: async_run_on non-stopped rejection reports enqueue failure",
	"[work]") {
	RejectingQueueTarget target;
	auto rejected = async_run_on(target, [] { return 2; });
	auto rejected_out = root::blocking_join(std::move(rejected));
	REQUIRE(rejected_out.is_failure());
	CHECK_THROWS_AS(std::rethrow_exception(rejected_out.failure().error), root::WorkError);
}
TEST_CASE(
	"work: async_run_cancellable_on executes callable on pool",
	"[work]") {
	WorkPool pool;
	auto result = sync_wait(async_run_cancellable_on(pool, [](root::Cancellation cancel) {
		CHECK_FALSE(cancel.requested());
		return 42;
	}));
	CHECK(result == 42);
}
TEST_CASE(
	"work: async_run_cancellable_on maps CancelledError to cancelled outcome",
	"[work]") {
	WorkPool pool;
	auto task = async_run_cancellable_on(pool, [](root::Cancellation) -> int {
		throw root::CancelledError{root::CancelReason::deadline};
	});
	auto out = root::blocking_join(std::move(task));
	REQUIRE(out.is_cancelled());
	CHECK(out.cancelled().reason == root::CancelReason::deadline);
}
TEST_CASE(
	"work: async_run_cancellable_on queued cancellation skips body",
	"[work]") {
	WorkPool pool{WorkPoolOptions{.threads = 1}};
	std::mutex mtx;
	std::condition_variable cv;
	bool release_worker = false;
	bool blocker_started = false;
	REQUIRE(pool.enqueue([&] {
		std::unique_lock lk{mtx};
		blocker_started = true;
		cv.notify_one();
		cv.wait(lk, [&] { return release_worker; });
	}));
	{
		std::unique_lock lk{mtx};
		REQUIRE(cv.wait_for(lk, std::chrono::seconds{5}, [&] { return blocker_started; }));
	}

	bool body_ran = false;
	auto task = async_run_cancellable_on(pool, [&body_ran](root::Cancellation) {
		body_ran = true;
		return 7;
	});
	task.cancel(root::CancelReason::shutdown);
	{
		std::scoped_lock const lk{mtx};
		release_worker = true;
	}
	cv.notify_one();

	auto out = root::blocking_join(std::move(task));
	REQUIRE(out.is_cancelled());
	CHECK(out.cancelled().reason == root::CancelReason::shutdown);
	CHECK_FALSE(body_ran);
}
TEST_CASE(
	"work: WorkPool raw enqueue reports thrown exception to sink",
	"[work]") {
	std::mutex mtx;
	std::condition_variable cv;
	bool seen = false;
	std::string message;
	WorkPool pool{
		WorkPoolOptions{
						.threads = 1,
						.raw_exception_sink =
				[&](std::exception_ptr ep) {
					std::string local;
					try {
						rethrow_exception(ep);
					} catch (std::runtime_error const &e) { local = e.what(); } catch (...) {
						local = "unexpected";
					}
					{
						std::scoped_lock const lk{mtx};
						message = std::move(local);
						seen = true;
					}
					cv.notify_one();
				}, }
    };

	REQUIRE(pool.enqueue([] { throw std::runtime_error{"raw boom"}; }));
	std::unique_lock lk{mtx};
	REQUIRE(cv.wait_for(lk, std::chrono::seconds{5}, [&] { return seen; }));
	CHECK(message == "raw boom");
}
TEST_CASE(
	"work: WorkPool stop abandons queued raw jobs",
	"[work]") {
	std::mutex mtx;
	std::condition_variable cv;
	bool first_started = false;
	bool release_first = false;
	bool second_ran = false;
	WorkPool pool{WorkPoolOptions{.threads = 1}};
	REQUIRE(pool.enqueue([&] {
		std::unique_lock lk{mtx};
		first_started = true;
		cv.notify_all();
		cv.wait(lk, [&] { return release_first; });
	}));
	{
		std::unique_lock lk{mtx};
		REQUIRE(cv.wait_for(lk, std::chrono::seconds{5}, [&] { return first_started; }));
	}
	REQUIRE(pool.enqueue([&] { second_ran = true; }));
	pool.stop();
	{
		std::scoped_lock const lk{mtx};
		release_first = true;
	}
	cv.notify_all();
	pool.wait();
	CHECK_FALSE(second_ran);
	CHECK(pool.stopped());
}
TEST_CASE(
	"work: WorkPool drain_and_stop finishes queued raw jobs",
	"[work]") {
	std::mutex mtx;
	std::condition_variable cv;
	bool first_started = false;
	bool release_first = false;
	bool second_ran = false;
	WorkPool pool{WorkPoolOptions{.threads = 1}};
	REQUIRE(pool.enqueue([&] {
		std::unique_lock lk{mtx};
		first_started = true;
		cv.notify_all();
		cv.wait(lk, [&] { return release_first; });
	}));
	{
		std::unique_lock lk{mtx};
		REQUIRE(cv.wait_for(lk, std::chrono::seconds{5}, [&] { return first_started; }));
	}
	REQUIRE(pool.enqueue([&] {
		std::scoped_lock const lk{mtx};
		second_ran = true;
		cv.notify_all();
	}));
	std::jthread stopper{[&] { pool.drain_and_stop(); }};
	{
		std::scoped_lock const lk{mtx};
		release_first = true;
	}
	cv.notify_all();
	{
		std::unique_lock lk{mtx};
		REQUIRE(cv.wait_for(lk, std::chrono::seconds{5}, [&] { return second_ran; }));
	}
	stopper.join();
	CHECK(pool.stopped());
}
TEST_CASE(
	"work: WorkPool no_stealing queue executes async work",
	"[work]") {
	WorkPool pool{
		WorkPoolOptions{.threads = 2, .queue_mode = WorkPoolQueueMode::no_stealing}
    };
	auto result = sync_wait(async_run_on(pool, [] { return 42; }));
	CHECK(result == 42);
}
TEST_CASE(
	"work: WorkPool no_stealing queue drain_and_stop accounts for concurrent enqueue",
	"[work][stress]") {
	for (int round = 0; round < 100; ++round) {
		WorkPool pool{
			WorkPoolOptions{.threads = 2, .max_inject_queue = 4096, .queue_mode = WorkPoolQueueMode::no_stealing}
        };
		std::atomic<bool> start{false};
		std::atomic<int> accepted{0};
		std::atomic<int> ran{0};
		std::vector<std::jthread> producers;
		for (int i = 0; i < 16; ++i) {
			producers.emplace_back([&] {
				while (!start.load(std::memory_order_acquire)) {
					conflux::work::root::detail::cpu_pause();
				}
				if (pool.enqueue([&] { ran.fetch_add(1, std::memory_order_release); })) {
					accepted.fetch_add(1, std::memory_order_release);
				}
			});
		}
		start.store(true, std::memory_order_release);
		pool.drain_and_stop();
		for (auto &p: producers) {
			if (p.joinable()) {
				p.join();
			}
		}
		CHECK(ran.load(std::memory_order_acquire) == accepted.load(std::memory_order_acquire));
	}
}
TEST_CASE(
	"work: WorkPool drain_and_stop accounts for concurrent enqueue",
	"[work][stress]") {
	for (int round = 0; round < 100; ++round) {
		WorkPool pool{
			WorkPoolOptions{.threads = 2, .max_inject_queue = 4096}
        };
		std::atomic<bool> start{false};
		std::atomic<int> accepted{0};
		std::atomic<int> ran{0};
		std::vector<std::jthread> producers;
		for (int i = 0; i < 16; ++i) {
			producers.emplace_back([&] {
				while (!start.load(std::memory_order_acquire)) {
					conflux::work::root::detail::cpu_pause();
				}
				if (pool.enqueue([&] { ran.fetch_add(1, std::memory_order_release); })) {
					accepted.fetch_add(1, std::memory_order_release);
				}
			});
		}
		start.store(true, std::memory_order_release);
		pool.drain_and_stop();
		for (auto &p: producers) {
			if (p.joinable()) {
				p.join();
			}
		}
		CHECK(ran.load(std::memory_order_acquire) == accepted.load(std::memory_order_acquire));
	}
}
TEST_CASE(
	"work: join_all collects results",
	"[work]") {
	WorkPool pool;
	auto [a, b, c] = sync_wait(
		join_all(async_run_on(pool, [] { return 1; }), async_run_on(pool, [] { return 2; }), async_run_on(pool, [] {
					 return 3;
				 })));
	CHECK(a == 1);
	CHECK(b == 2);
	CHECK(c == 3);
}
TEST_CASE(
	"work: join_all maps void tasks to monostate",
	"[work]") {
	WorkPool pool;
	auto [v, i] = sync_wait(join_all(async_run_on(pool, [] {}), async_run_on(pool, [] { return 7; })));
	CHECK(i == 7);
	static_assert(std::is_same_v<decltype(v), std::monostate>);
}
TEST_CASE(
	"work: join_all stress — ready-hook arm vs fire race",
	"[work][stress]") {
	// Race repro: try_set_on_ready arms on_ready_fn_ while fire_ready_hook_if_armed_
	// CAS's open→committing→terminal lock-free. With load+store in arm path the fn
	// could become orphaned, hanging sync_wait. With CAS-armed path the late
	// registration is rejected and dispatched as already_ready.
	WorkPool pool;
	for (int i = 0; i < 2000; ++i) {
		auto [a, b, c] = sync_wait(
			join_all(async_run_on(pool, [] { return 1; }), async_run_on(pool, [] { return 2; }), async_run_on(pool, [] {
						 return 3;
					 })));
		REQUIRE(a == 1);
		REQUIRE(b == 2);
		REQUIRE(c == 3);
	}
}
TEST_CASE(
	"work: ring lane wakes through msg ring and drains on owner",
	"[work]") {
	::io_uring ring{};
	if (int const rc = ::io_uring_queue_init(8, &ring, 0); rc != 0) {
		FAIL(std::format("conflux requires a host that permits io_uring_queue_init: {}", rc));
	}
	struct RingGuard {
		io_uring *ring;
		~RingGuard() { ::io_uring_queue_exit(ring); }
	} guard{&ring};
	RingLane lane{
		{
         .ring_fd = ring.ring_fd,
         .wake_user_data = 0x57524B4CU,
         .drain_budget = 0,
         .allow_inline_on_owner = false,
		 }
    };
	lane.adopt_current_thread();

	std::atomic<int> observed{0};
	std::jthread producer([&] { CHECK(lane.enqueue([&] { observed.store(42, std::memory_order_release); })); });
	producer.join();

	io_uring_cqe *cqe = nullptr;
	REQUIRE(::io_uring_wait_cqe(&ring, &cqe) == 0);
	REQUIRE(cqe != nullptr);
	CHECK(cqe->user_data == 0x57524B4CU);
	::io_uring_cqe_seen(&ring, cqe);

	CHECK(lane.drain() == 1);
	CHECK(observed.load(std::memory_order_acquire) == 42);
}
TEST_CASE(
	"work: ring lane failed wake does not keep rejected job",
	"[work]") {
	RingLane lane{
		{
         .ring_fd = -1,
         .wake_user_data = 0x57524B4CU,
         .drain_budget = 0,
         .allow_inline_on_owner = false,
		 }
    };
	lane.adopt_current_thread();

	std::atomic<int> observed{0};
	bool queued = true;
	std::jthread producer([&] { queued = lane.enqueue([&] { observed.store(42, std::memory_order_release); }); });
	producer.join();

	CHECK_FALSE(queued);
	CHECK(lane.drain() == 0);
	CHECK(observed.load(std::memory_order_acquire) == 0);
}
TEST_CASE(
	"work: co_spawn fires and forgets",
	"[work]") {
	WorkPool pool;
	std::atomic<int> counter{0};
	auto gate = std::make_shared<std::barrier<>>(2);
	[](std::shared_ptr<std::barrier<>>, auto t) -> root::Task<void> {
		co_await std::move(t);
	}(gate, async_run_on(pool, [gate, &counter] {
													   counter.fetch_add(1, std::memory_order_release);
													   gate->arrive_and_wait();
												   })).detach();
	gate->arrive_and_wait();
	CHECK(counter.load(std::memory_order_acquire) == 1);
}
TEST_CASE(
	"work: IoBuffer from_string keeps std::string alive",
	"[work]") {
	conflux::net::IoBuffer const buf = conflux::net::IoBuffer::from_string("hello");
	CHECK(buf.bytes.size() == 5);
	CHECK(buf.bytes[0] == std::byte{'h'});
}
