// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;
import conflux.net.io_buffer;

namespace root = conflux::work::root;
TEST_CASE(
	"work: run_on_task executes callable on pool",
	"[work]") {
	WorkPool pool;
	auto result = sync_wait(run_on_task(pool, [] { return 42; }));
	CHECK(result == 42);
}
TEST_CASE(
	"work: run_on_task high-iteration roundtrip remains live",
	"[work]") {
	WorkPool pool;
	constexpr SZ kIters = 50001;
	for (SZ i = 0; i < kIters; ++i) {
		auto const v = root::value(run_on_task(pool, [] { return 7; }));
		REQUIRE(v == 7);
	}
}
TEST_CASE(
	"work: run_on_task propagates exception",
	"[work]") {
	WorkPool pool;
	bool caught = false;
	try {
		sync_wait(run_on_task(pool, []() -> int { throw RE{"boom"}; }));
	} catch (RE const &e) { caught = SV{e.what()} == "boom"; }
	CHECK(caught);
}
TEST_CASE(
	"work: run_on_task cancelled when pool stopped",
	"[work]") {
	WorkPool pool;
	pool.stop();
	bool cancelled = false;
	try {
		sync_wait(run_on_task(pool, [] { return 99; }));
	} catch (Cancelled const &) { cancelled = true; }
	CHECK(cancelled);
}
TEST_CASE(
	"work: WorkPool raw enqueue reports thrown exception to sink",
	"[work]") {
	mutex mtx;
	std::condition_variable cv;
	bool seen = false;
	S message;
	WorkPool pool{
		WorkPoolOptions{
						.threads = 1,
						.raw_exception_sink =
				[&](EP ep) {
					S local;
					try {
						rethrow_exception(ep);
					} catch (RE const &e) { local = e.what(); } catch (...) {
						local = "unexpected";
					}
					{
						SL const lk{mtx};
						message = move(local);
						seen = true;
					}
					cv.notify_one();
				}, }
    };

	REQUIRE(pool.enqueue([] { throw RE{"raw boom"}; }));
	std::unique_lock lk{mtx};
	REQUIRE(cv.wait_for(lk, chrono::seconds{5}, [&] { return seen; }));
	CHECK(message == "raw boom");
}
TEST_CASE(
	"work: WorkPool stop abandons queued raw jobs",
	"[work]") {
	mutex mtx;
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
		REQUIRE(cv.wait_for(lk, chrono::seconds{5}, [&] { return first_started; }));
	}
	REQUIRE(pool.enqueue([&] { second_ran = true; }));
	pool.stop();
	{
		SL const lk{mtx};
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
	mutex mtx;
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
		REQUIRE(cv.wait_for(lk, chrono::seconds{5}, [&] { return first_started; }));
	}
	REQUIRE(pool.enqueue([&] {
		SL const lk{mtx};
		second_ran = true;
		cv.notify_all();
	}));
	jthread stopper{[&] { pool.drain_and_stop(); }};
	{
		SL const lk{mtx};
		release_first = true;
	}
	cv.notify_all();
	{
		std::unique_lock lk{mtx};
		REQUIRE(cv.wait_for(lk, chrono::seconds{5}, [&] { return second_ran; }));
	}
	stopper.join();
	CHECK(pool.stopped());
}
TEST_CASE(
	"work: WorkPool drain_and_stop accounts for concurrent enqueue",
	"[work][stress]") {
	for (int round = 0; round < 100; ++round) {
		WorkPool pool{
			WorkPoolOptions{.threads = 2, .max_inject_queue = 4096}
        };
		Atom<bool> start{false};
		Atom<int> accepted{0};
		Atom<int> ran{0};
		V<jthread> producers;
		for (int i = 0; i < 16; ++i) {
			producers.emplace_back([&] {
				while (!start.load(memory_order_acquire)) {
					conflux::work::root::detail::cpu_pause();
				}
				if (pool.enqueue([&] { ran.fetch_add(1, memory_order_release); })) {
					accepted.fetch_add(1, memory_order_release);
				}
			});
		}
		start.store(true, memory_order_release);
		pool.drain_and_stop();
		for (auto &p: producers) {
			if (p.joinable()) {
				p.join();
			}
		}
		CHECK(ran.load(memory_order_acquire) == accepted.load(memory_order_acquire));
	}
}
TEST_CASE(
	"work: join_all collects results",
	"[work]") {
	WorkPool pool;
	auto [a, b, c] = sync_wait(
		join_all(run_on_task(pool, [] { return 1; }), run_on_task(pool, [] { return 2; }), run_on_task(pool, [] {
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
	auto [v, i] = sync_wait(join_all(run_on_task(pool, [] {}), run_on_task(pool, [] { return 7; })));
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
			join_all(run_on_task(pool, [] { return 1; }), run_on_task(pool, [] { return 2; }), run_on_task(pool, [] {
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
		FAIL(format("conflux requires a host that permits io_uring_queue_init: {}", rc));
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

	Atom<int> observed{0};
	jthread producer([&] { CHECK(lane.enqueue([&] { observed.store(42, memory_order_release); })); });
	producer.join();

	io_uring_cqe *cqe = nullptr;
	REQUIRE(::io_uring_wait_cqe(&ring, &cqe) == 0);
	REQUIRE(cqe != nullptr);
	CHECK(cqe->user_data == 0x57524B4CU);
	::io_uring_cqe_seen(&ring, cqe);

	CHECK(lane.drain() == 1);
	CHECK(observed.load(memory_order_acquire) == 42);
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

	Atom<int> observed{0};
	bool queued = true;
	jthread producer([&] { queued = lane.enqueue([&] { observed.store(42, memory_order_release); }); });
	producer.join();

	CHECK_FALSE(queued);
	CHECK(lane.drain() == 0);
	CHECK(observed.load(memory_order_acquire) == 0);
}
TEST_CASE(
	"work: co_spawn fires and forgets",
	"[work]") {
	WorkPool pool;
	Atom<int> counter{0};
	auto gate = make_shared<barrier<>>(2);
	[](SP<barrier<>>, auto t) -> root::Task<void> {
		co_await move(t);
	}(gate, run_on_task(pool, [gate, &counter] {
									 counter.fetch_add(1, memory_order_release);
									 gate->arrive_and_wait();
								 })).detach();
	gate->arrive_and_wait();
	CHECK(counter.load(memory_order_acquire) == 1);
}
TEST_CASE(
	"work: IoBuffer from_string keeps S alive",
	"[work]") {
	IoBuffer const buf = IoBuffer::from_string("hello");
	CHECK(buf.bytes.size() == 5);
	CHECK(buf.bytes[0] == byte{'h'});
}
