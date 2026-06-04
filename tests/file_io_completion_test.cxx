#include <catch2/catch_test_macros.hpp>
#include <cerrno>

import std;
import conflux.uring.completion;

using conflux::uring::CompletionTable;
using conflux::uring::IoResult;

TEST_CASE(
	"file_io: CompletionTable reserve/dispatch round-trip",
	"[file_io][unit]") {
	CompletionTable table;
	int observed = 0;
	auto [slot, gen] = table.reserve([&](IoResult r) { observed = r.res; });
	CHECK(slot == 0);
	CHECK(gen == 0);
	table.dispatch(slot, gen, 42, conflux::uring::CqeFlags{});
	CHECK(observed == 42);
	CHECK(table.pending() == 0);
}
TEST_CASE(
	"file_io: CompletionTable rejects stale gen",
	"[file_io][unit]") {
	CompletionTable table;
	int fired = 0;
	auto [slot, gen] = table.reserve([&](IoResult) { ++fired; });
	table.dispatch(slot, gen, 0, conflux::uring::CqeFlags{});
	CHECK(fired == 1);
	table.dispatch(slot, gen, 0, conflux::uring::CqeFlags{});
	CHECK(fired == 1);
}
TEST_CASE(
	"file_io: CompletionTable cancel_all fires pending with ECANCELED",
	"[file_io][unit]") {
	CompletionTable table;
	int res_a = 0;
	int res_b = 0;
	auto [slot_a, gen_a] = table.reserve([&](IoResult r) { res_a = r.res; });
	auto [slot_b, gen_b] = table.reserve([&](IoResult r) { res_b = r.res; });
	CHECK(slot_a == 0);
	CHECK(gen_a == 0);
	CHECK(slot_b == 1);
	CHECK(gen_b == 0);
	CHECK(table.cancel_all());
	CHECK(res_a == -ECANCELED);
	CHECK(res_b == -ECANCELED);
	CHECK(table.pending() == 0);
}
TEST_CASE(
	"file_io: CompletionTable zc_send waits for NOTIF CQE",
	"[file_io][unit]") {
	CompletionTable table;
	bool fired = false;
	int got_res = -1;
	auto [slot, gen] = table.reserve_zc([&](IoResult r) noexcept {
		fired = true;
		got_res = r.res;
	});
	table.dispatch(slot, gen, 17, conflux::uring::cqe_flags::more);
	CHECK(!fired);
	CHECK(table.has_pending_zc_notifications());
	table.dispatch(slot, gen, 0, conflux::uring::cqe_flags::notif);
	CHECK(fired);
	CHECK(got_res == 17);
	CHECK(!table.has_pending_zc_notifications());
	CHECK(table.pending() == 0);
}
TEST_CASE(
	"file_io: CompletionTable zc_send first CQE error fires immediately",
	"[file_io][unit]") {
	CompletionTable table;
	bool fired = false;
	int got_res = 0;
	auto [slot, gen] = table.reserve_zc([&](IoResult r) noexcept {
		fired = true;
		got_res = r.res;
	});
	table.dispatch(slot, gen, -EPERM, conflux::uring::CqeFlags{});
	CHECK(fired);
	CHECK(got_res == -EPERM);
	CHECK(table.pending() == 0);
}
TEST_CASE(
	"file_io: CompletionTable zc_send first CQE success without MORE fires immediately",
	"[file_io][unit]") {
	CompletionTable table;
	bool fired = false;
	int got_res = -1;
	auto [slot, gen] = table.reserve_zc([&](IoResult r) noexcept {
		fired = true;
		got_res = r.res;
	});
	table.dispatch(slot, gen, 17, conflux::uring::CqeFlags{});
	CHECK(fired);
	CHECK(got_res == 17);
	CHECK(table.pending() == 0);
}
TEST_CASE(
	"file_io: CompletionTable zc_send stale NOTIF after slot free is ignored",
	"[file_io][unit]") {
	CompletionTable table;
	int fire_count = 0;
	auto [slot, gen] = table.reserve_zc([&](IoResult) noexcept { ++fire_count; });
	table.dispatch(slot, gen, 5, conflux::uring::cqe_flags::more);
	table.dispatch(slot, gen, 0, conflux::uring::cqe_flags::notif);
	CHECK(fire_count == 1);
	table.dispatch(slot, gen, 0, conflux::uring::cqe_flags::notif);
	CHECK(fire_count == 1);
}
TEST_CASE(
	"file_io: CompletionTable has_pending_zc_notifications",
	"[file_io][unit]") {
	CompletionTable table;
	auto [slot, gen] = table.reserve_zc([](IoResult) noexcept {});
	CHECK(!table.has_pending_zc_notifications());
	table.dispatch(slot, gen, 8, conflux::uring::cqe_flags::more);
	CHECK(table.has_pending_zc_notifications());
	table.dispatch(slot, gen, 0, conflux::uring::cqe_flags::notif);
	CHECK(!table.has_pending_zc_notifications());
}
TEST_CASE(
	"file_io: CompletionTable cancel_all refuses pending zc notification",
	"[file_io][unit]") {
	CompletionTable table;
	bool fired = false;
	auto [slot, gen] = table.reserve_zc([&](IoResult) noexcept { fired = true; });
	table.dispatch(slot, gen, 17, conflux::uring::cqe_flags::more);
	CHECK(table.has_pending_zc_notifications());
	CHECK_FALSE(table.cancel_all());
	CHECK(!fired);
	CHECK(table.pending() == 1);
	table.dispatch(slot, gen, 0, conflux::uring::cqe_flags::notif);
	CHECK(fired);
	CHECK(table.cancel_all());
}
