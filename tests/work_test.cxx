// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;

TEST_CASE(
	"work: run_on_task executes callable on pool",
	"[work]") {
	WorkPool pool;
	auto result = sync_wait(run_on_task(pool, [] { return 42; }));
	CHECK(result == 42);
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
	co_spawn([gate, task = run_on_task(pool, [gate, &counter] {
								  counter.fetch_add(1, memory_order_release);
								  gate->arrive_and_wait();
							  })]() mutable -> Task<void> { co_await std::move(task); }());
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
