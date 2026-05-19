// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>

#include <liburing.h>

import std;
import conflux.types;
import conflux.work.carrier.timer;
namespace {

constexpr std::uint64_t TIMER_TAG = 0xCAFEDEADBEEF0001ULL;
struct Ring {
	io_uring ring{};
	explicit Ring(
		unsigned entries = 32) {
		if (io_uring_queue_init(entries, &ring, 0) < 0) {
			throw std::runtime_error{"io_uring_queue_init failed"};
		}
	}
	~Ring() noexcept { io_uring_queue_exit(&ring); }
	Ring(Ring const &) = delete;
	Ring &operator =(Ring const &) = delete;
};
void run_until(
	Ring &r,
	conflux::work::carrier::TimerService &svc,
	std::function<bool()> const &done,
	int timeout_ms = 500) {
	auto const wall_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
	while (!done() && std::chrono::steady_clock::now() < wall_deadline) {
		__kernel_timespec ts{0, 5000000};
		io_uring_cqe *cqe = nullptr;
		int const ret = io_uring_wait_cqe_timeout(&r.ring, &cqe, &ts);
		if (ret == -ETIME || ret == -EINTR || cqe == nullptr) {
			continue;
		}
		auto const tag = io_uring_cqe_get_data64(cqe);
		if (tag == TIMER_TAG) {
			svc.on_cqe(cqe);
		}
		io_uring_cqe_seen(&r.ring, cqe);
	}
}

} // namespace
TEST_CASE(
	"TimerService: construct and destruct cleanly",
	"[carrier][timer]") {
	Ring r;
	conflux::work::carrier::TimerService const svc{&r.ring, TIMER_TAG};
	REQUIRE(svc.on_owner_thread());
	REQUIRE(svc.timer_fd() >= 0);
}
TEST_CASE(
	"TimerService: single timer fires callback",
	"[carrier][timer]") {
	Ring r;
	conflux::work::carrier::TimerService svc{&r.ring, TIMER_TAG};

	bool fired = false;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{20};
	conflux::work::carrier::LaneTimerScope<> const scope{svc, deadline, [&fired] { fired = true; }};

	run_until(r, svc, [&fired] { return fired; });

	REQUIRE(fired);
}
TEST_CASE(
	"TimerService: cancel before deadline — no callback",
	"[carrier][timer]") {
	Ring r;
	conflux::work::carrier::TimerService svc{&r.ring, TIMER_TAG};

	bool fired = false;
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
		conflux::work::carrier::LaneTimerScope<> const scope{svc, deadline, [&fired] { fired = true; }};
	}

	run_until(r, svc, [] { return false; }, 400);

	REQUIRE_FALSE(fired);
}
TEST_CASE(
	"TimerService: multiple timers fire in deadline order",
	"[carrier][timer]") {
	Ring r;
	conflux::work::carrier::TimerService svc{&r.ring, TIMER_TAG};

	std::vector<int> order;
	auto now = std::chrono::steady_clock::now();

	conflux::work::carrier::LaneTimerScope<> const s1{svc, now + std::chrono::milliseconds{60}, [&order] {
														  order.push_back(1);
													  }};
	conflux::work::carrier::LaneTimerScope<> const s2{svc, now + std::chrono::milliseconds{20}, [&order] {
														  order.push_back(2);
													  }};
	conflux::work::carrier::LaneTimerScope<> const s3{svc, now + std::chrono::milliseconds{40}, [&order] {
														  order.push_back(3);
													  }};

	run_until(r, svc, [&order] { return order.size() == 3; });

	REQUIRE(order.size() == 3);
	CHECK(order[0] == 2);
	CHECK(order[1] == 3);
	CHECK(order[2] == 1);
}
TEST_CASE(
	"TimerService: inserting earlier deadline rearms timerfd",
	"[carrier][timer]") {
	Ring r;
	conflux::work::carrier::TimerService svc{&r.ring, TIMER_TAG};

	bool late_fired = false;
	bool early_fired = false;
	auto now = std::chrono::steady_clock::now();

	// Insert late first, then early — early should trigger rearm
	conflux::work::carrier::LaneTimerScope<> const late{svc, now + std::chrono::milliseconds{200}, [&late_fired] {
															late_fired = true;
														}};
	conflux::work::carrier::LaneTimerScope<> const early{svc, now + std::chrono::milliseconds{20}, [&early_fired] {
															 early_fired = true;
														 }};

	run_until(r, svc, [&early_fired] { return early_fired; }, 200);

	REQUIRE(early_fired);
	REQUIRE_FALSE(late_fired);
}
TEST_CASE(
	"TimerService: cancel earliest — late timer still fires",
	"[carrier][timer]") {
	Ring r;
	conflux::work::carrier::TimerService svc{&r.ring, TIMER_TAG};

	bool early_fired = false;
	bool late_fired = false;
	auto now = std::chrono::steady_clock::now();

	conflux::work::carrier::LaneTimerScope<> const late{svc, now + std::chrono::milliseconds{80}, [&late_fired] {
															late_fired = true;
														}};
	{
		conflux::work::carrier::LaneTimerScope<> const early{svc, now + std::chrono::milliseconds{10}, [&early_fired] {
																 early_fired = true;
															 }};
	}

	run_until(r, svc, [&late_fired] { return late_fired; });

	REQUIRE_FALSE(early_fired);
	REQUIRE(late_fired);
}
TEST_CASE(
	"TimerService: move scope transfers cancellation",
	"[carrier][timer]") {
	Ring r;
	conflux::work::carrier::TimerService svc{&r.ring, TIMER_TAG};

	bool fired = false;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};

	{
		conflux::work::carrier::LaneTimerScope<> s1{svc, deadline, [&fired] { fired = true; }};
		auto s2 = std::move(s1);
		// s1 empty, s2 holds scope; destroy s2 at end of block → cancels
	}

	run_until(r, svc, [] { return false; }, 400);

	REQUIRE_FALSE(fired);
}
TEST_CASE(
	"TimerService: 10000 cancellations in < 1000ms",
	"[carrier][timer][bench]") {
	Ring r;
	conflux::work::carrier::TimerService svc{&r.ring, TIMER_TAG};

	auto const deadline = std::chrono::steady_clock::now() + std::chrono::hours{1};
	auto const n = 10000u;

	auto const start = std::chrono::steady_clock::now();
	for (auto i = 0u; i < n; ++i) {
		conflux::work::carrier::LaneTimerScope<> const scope{svc, deadline, [] {}};
	}
	auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

	REQUIRE(ms < 1000);
}
