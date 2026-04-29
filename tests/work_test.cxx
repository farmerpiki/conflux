// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <liburing.h>

import std;
import conflux.types;
import conflux.work;

TEST_CASE(
	"work: value and then chain",
	"[work]") {
	auto result = wait(value(7) | then([](int x) { return x * 6; }));
	CHECK(result == 42);
}

TEST_CASE(
	"work: run_on executes on pool",
	"[work]") {
	WorkPool pool;
	auto result = wait(run_on(pool, [] { return 21; }) | then([](int x) { return x * 2; }));
	CHECK(result == 42);
}

TEST_CASE(
	"work: flat_then composes flows",
	"[work]") {
	WorkPool pool;
	auto result = wait(
		run_on(pool, [] { return 5; }) | flat_then([&pool](int x) { return run_on(pool, [x] { return x + 3; }); }));
	CHECK(result == 8);
}

TEST_CASE(
	"work: on_error recovers",
	"[work]") {
	WorkPool pool;
	auto result = wait(run_on(pool, []() -> int { throw RE{"boom"}; }) | on_error([](EP error) {
						   try {
							   rethrow_exception(error);
						   } catch (exception const &ex) { return static_cast<int>(SV{ex.what()} == "boom"); }
						   return 0;
					   }));
	CHECK(result == 1);
}

TEST_CASE(
	"work: on_cancel recovers stopped work",
	"[work]") {
	WorkPool pool;
	pool.stop();
	auto result = wait(run_on(pool, [] { return 7; }) | on_cancel([] { return 99; }));
	CHECK(result == 99);
}

TEST_CASE(
	"work: join_all collects results",
	"[work]") {
	WorkPool pool;
	auto joined = join_all(run_on(pool, [] { return 1; }), run_on(pool, [] { return 2; }), value(3));
	auto [a, b, c] = wait(move(joined));
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
	"work: move_to transfers continuation to pool",
	"[work]") {
	WorkPool pool;
	auto result = wait(value(10) | move_to(pool) | then([](int x) { return x + 5; }));
	CHECK(result == 15);
}

TEST_CASE(
	"work: start_on transfers continuation to pool",
	"[work]") {
	WorkPool pool;
	auto result = wait(value(20) | start_on(pool) | then([](int x) { return x * 2; }));
	CHECK(result == 40);
}

TEST_CASE(
	"work: spawn fires and forgets without wait",
	"[work]") {
	WorkPool pool;
	Atom<int> counter{0};
	auto gate = make_shared<barrier<>>(2);
	spawn(run_on(pool, [gate, &counter] {
		counter.fetch_add(1, memory_order_release);
		gate->arrive_and_wait();
	}));
	gate->arrive_and_wait();
	CHECK(counter.load(memory_order_acquire) == 1);
}

TEST_CASE(
	"work: FlowSource resolve delivers value",
	"[work]") {
	FlowSource<int> src;
	auto flow = src.flow();
	src.resolve(77);
	CHECK(wait(move(flow)) == 77);
}

TEST_CASE(
	"work: FlowSource reject surfaces error via on_error",
	"[work]") {
	FlowSource<int> src;
	auto flow = src.flow();
	src.reject(make_exception_ptr(RE{"fail"}));
	auto result = wait(move(flow) | on_error([](EP) { return -1; }));
	CHECK(result == -1);
}

TEST_CASE(
	"work: FlowSource cancel surfaces cancellation via on_cancel",
	"[work]") {
	FlowSource<int> src;
	auto flow = src.flow();
	src.cancel();
	auto result = wait(move(flow) | on_cancel([] { return 0; }));
	CHECK(result == 0);
}

TEST_CASE(
	"work: FlowSource second resolve is ignored",
	"[work]") {
	FlowSource<int> src;
	auto flow = src.flow();
	src.resolve(1);
	src.resolve(2);
	CHECK(wait(move(flow)) == 1);
}

TEST_CASE(
	"work: FlowSource armed/disarmed state",
	"[work]") {
	FlowSource<int> src;
	CHECK(src.armed());
	src.resolve(0);
	CHECK_FALSE(src.armed());
}

TEST_CASE(
	"work: IoBuffer from_string keeps S alive",
	"[work]") {
	IoBuffer buf = IoBuffer::from_string("hello");
	CHECK(buf.bytes.size() == 5);
	CHECK(buf.bytes[0] == byte{'h'});
}

TEST_CASE(
	"work: IoBuffer from span does not own",
	"[work]") {
	S s{"world"};
	IoBuffer buf{
		span{reinterpret_cast<byte const *>(s.data()), s.size()}
    };
	CHECK(buf.bytes.size() == 5);
	CHECK(!buf.owner);
}

TEST_CASE(
	"work: IoPlan call stores and invokes callback",
	"[work]") {
	int called = 0;
	auto plan = IoPlan::call(work_detail::UniqueFn<void()>{[&called] { called = 1; }});
	plan.callback();
	CHECK(called == 1);
}

TEST_CASE(
	"work: Task coroutine resolves via co_return",
	"[work]") {
	auto task = []() -> Task<int> { co_return 99; }();
	CHECK(wait(move(task).flow()) == 99);
}

TEST_CASE(
	"work: Task<void> coroutine resolves",
	"[work]") {
	bool ran = false;
	auto task = [&]() -> Task<void> {
		ran = true;
		co_return;
	}();
	wait(move(task).flow());
	CHECK(ran);
}

TEST_CASE(
	"work: co_spawn fires task without explicit wait",
	"[work]") {
	int hit = 0;
	co_spawn([&]() -> Task<void> {
		hit = 1;
		co_return;
	}());
	CHECK(hit == 1);
}
