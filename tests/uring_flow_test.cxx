// Plain TU — not a module unit.
#include<catch2/catch_test_macros.hpp>
#include<liburing.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.uring.flow;

namespace uf=conflux::uring::flow;
using namespace conflux::uring;
namespace{
io_uring_cqe make_cqe(
u64 user_data,
i32 res,
u32 flags=0){
return io_uring_cqe{.user_data=user_data,.res=res,.flags=flags};
}
// Build a CQE that matches what the runtime encodes for a given flow slot.
io_uring_cqe make_flow_cqe(
u32 flow_idx,
u32 gen,
u8 op_index,
uf::FlowOpKind kind,
i32 res){
return make_cqe(uf::encode_tag(flow_idx,gen,op_index,kind),res);
}
struct TestRig{
Ring ring;
V<uf::FlowResult>results;
uf::FlowRuntime rt;
TestRig()
:ring{[]{
auto r=Ring::init(16,{});
REQUIRE(r);
return move(*r);
}()},
rt{ring,[&](uf::FlowResult fr){results.push_back(fr);}}{}
};
}// namespace
// ── Auto-mode selection ───────────────────────────────────────────────────────

TEST_CASE(
"flow.mode_select: then_read+then_write → mode A",
"[flow][mode]"){
TestRig rig;
char buf[16]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
REQUIRE(f.valid());
(void)b.submit();
CHECK(b.rejected_flows().empty());
}
TEST_CASE(
"flow.mode_select: then_read+hard_write → mode B",
"[flow][mode]"){
TestRig rig;
char buf[16]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).hard_write(buf,4,0).close_if_opened();
REQUIRE(f.valid());
(void)b.submit();
CHECK(b.rejected_flows().empty());
}
TEST_CASE(
"flow.mode_select: open alone → mode B",
"[flow][mode]"){
TestRig rig;
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.close_if_opened();
REQUIRE(f.valid());
(void)b.submit();
CHECK(b.rejected_flows().empty());
}
// ── Builder error surface ─────────────────────────────────────────────────────

TEST_CASE(
"flow.builder: hard_read as first body → invalid",
"[flow][builder]"){
TestRig rig;
char buf[16]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.hard_read(buf,4,0);
CHECK_FALSE(f.valid());
CHECK(f.last_error()==-EINVAL);
f.then_read(buf,4,0);// no-op on invalid
CHECK(f.last_error()==-EINVAL);
(void)b.submit();
REQUIRE(b.rejected_flows().size()==1);
CHECK(b.rejected_flows()[0].err==-EINVAL);
}
TEST_CASE(
"flow.builder: overflow past max_initial_ops → -ENOBUFS",
"[flow][builder]"){
TestRig rig;
char buf[16]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
// max_initial_ops=8: open is op[0], so 7 body ops are the limit
for(int i=0;i<7;++i)
f.then_read(buf,1,u64(i));
CHECK(f.valid());
f.then_read(buf,1,7);// 8th body → exceeds max
CHECK_FALSE(f.valid());
CHECK(f.last_error()==-ENOBUFS);
(void)b.submit();
REQUIRE(b.rejected_flows().size()==1);
CHECK(b.rejected_flows()[0].err==-ENOBUFS);
}
TEST_CASE(
"flow.builder: len > INT32_MAX → -EOVERFLOW",
"[flow][builder]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
SZ big=static_cast<SZ>(NL<i32>::max())+1;
f.then_read(buf,big,0);
CHECK_FALSE(f.valid());
CHECK(f.last_error()==-EOVERFLOW);
}
TEST_CASE(
"flow.builder: close_if_opened on invalid is no-op",
"[flow][builder]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.hard_read(buf,1,0);// invalidate
CHECK_FALSE(f.valid());
f.close_if_opened();// must be no-op
CHECK(f.last_error()==-EINVAL);
}
// ── Mode A: happy path via CQE injection ─────────────────────────────────────

TEST_CASE(
"flow.mode_a: happy path — all succeed",
"[flow][mode_a]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
REQUIRE(b.rejected_flows().empty());
// First slab allocation: flow_index=0, generation=1
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
// chain complete; runtime submitted standalone close; cb not yet called
CHECK(rig.results.empty());
// close op_index == initial_op_count == 3
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK(r.open_ok());
CHECK(r.close_needed);
CHECK_FALSE(r.close_in_chain);
CHECK(r.close_cqe_seen);
CHECK(r.cleanup_result()==0);
REQUIRE(r.ops.size()==3);
CHECK(r.ops[0].res==0);// open
CHECK(r.ops[1].res==4);// read
CHECK(r.ops[2].res==4);// write
}
TEST_CASE(
"flow.mode_a: open fails → no close submitted",
"[flow][mode_a]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,-EACCES);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,-ECANCELED);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,-ECANCELED);
rig.rt.on_cqe(&cqe);
}
// open_ok==false → finish_flow without submitting close
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK_FALSE(r.open_ok());
CHECK_FALSE(r.close_needed);
CHECK(r.cleanup_result()==nullopt);
CHECK(r.raw_close_result()==nullopt);
}
// ── Mode A: close CQE arrives after chain complete ───────────────────────────

TEST_CASE(
"flow.mode_a: close CQE arrives after seen==expected — not rejected",
"[flow][mode_a][regression]"){
// Regression: the duplicate-body guard must NOT fire for mode A close CQEs.
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.empty());// chain complete; awaiting standalone close
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK(rig.results[0].close_cqe_seen);
CHECK(rig.results[0].cleanup_result()==0);
}
// ── Mode B ────────────────────────────────────────────────────────────────────

TEST_CASE(
"flow.mode_b: open fails → all -ECANCELED, cleanup_result nullopt",
"[flow][mode_b]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).hard_write(buf,4,0).close_if_opened();
(void)b.submit();
// mode B: expected_cqes = 4 (open+read+write+close in chain)
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,-ENOENT);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,-ECANCELED);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,-ECANCELED);
rig.rt.on_cqe(&cqe);
}
// close op_index == initial_op_count == 3
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,-ECANCELED);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK_FALSE(r.open_ok());
CHECK_FALSE(r.close_needed);
CHECK(r.close_in_chain);
CHECK(r.close_cqe_seen);
CHECK(r.cleanup_result()==nullopt);// normalized: open failed
CHECK(r.raw_close_result()==-ECANCELED);// debug visibility
}
TEST_CASE(
"flow.mode_b: close CQE arrives before last body CQE",
"[flow][mode_b]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).hard_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
// Deliver close CQE first (out of kernel order)
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// not done yet
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
// Last body CQE → seen==expected → on_chain_complete → finish_flow
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK(rig.results[0].close_in_chain);
CHECK(rig.results[0].cleanup_result()==0);
}
// ── Generation discipline ─────────────────────────────────────────────────────

TEST_CASE(
"flow.generation: stale CQE with wrong generation rejected",
"[flow][gen]"){
TestRig rig;
// First flow: idx=0, gen=1; open-only mode B (initial_op_count=1, close op_index=1)
{
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.close_if_opened();
(void)b.submit();
{
auto cqe=make_flow_cqe(0,1,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(0,1,1,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
}
REQUIRE(rig.results.size()==1);
// Second flow on same slot: idx=0, gen=2
{
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.close_if_opened();
(void)b.submit();
// Stale CQE from first flow (gen=1) — must be rejected
{
auto cqe=make_flow_cqe(0,1,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.size()==1);// still 1; stale did not advance state
// Correct CQEs for second flow (gen=2)
{
auto cqe=make_flow_cqe(0,2,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(0,2,1,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
}
REQUIRE(rig.results.size()==2);
}
// ── CQE tag validation ────────────────────────────────────────────────────────

TEST_CASE(
"flow.cqe_validation: unknown op_kind rejected",
"[flow][cqe_validation]"){
TestRig rig;
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.close_if_opened();
(void)b.submit();
// Craft CQE with an out-of-range op_kind (0xFF is not a valid FlowOpKind member)
{
auto cqe=make_cqe(uf::encode_tag_raw(0,1,0,0xFF),0);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// state unchanged
}
TEST_CASE(
"flow.cqe_validation: duplicate body CQE after chain complete rejected",
"[flow][cqe_validation]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
// chain complete; inject duplicate body CQE — must be rejected
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// close not yet seen; duplicate rejected
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);// finished exactly once
}
// ── op_result accessors ───────────────────────────────────────────────────────

TEST_CASE(
"flow.op_result: short_io detected",
"[flow][op_result]"){
uf::OpResult r{};
r.kind=uf::FlowOpKind::read;
r.requested=100;
r.res=50;
CHECK(r.short_io());
CHECK_FALSE(r.full_io());
CHECK(r.is_io());
CHECK(r.ok());
}
TEST_CASE(
"flow.op_result: full_io detected",
"[flow][op_result]"){
uf::OpResult r{};
r.kind=uf::FlowOpKind::write;
r.requested=32;
r.res=32;
CHECK(r.full_io());
CHECK_FALSE(r.short_io());
}
TEST_CASE(
"flow.op_result: open is not io",
"[flow][op_result]"){
uf::OpResult r{};
r.kind=uf::FlowOpKind::open_direct;
r.res=0;
CHECK_FALSE(r.is_io());
CHECK_FALSE(r.short_io());
}
// ── Additional mode selection ──────────────────────────────────────────────────

TEST_CASE(
"flow.mode_select: then_read → mode B",
"[flow][mode]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.then_read(buf,4,0).close_if_opened();
REQUIRE(f.valid());
(void)b.submit();
CHECK(b.rejected_flows().empty());
// mode B: expected_cqes == 3 (open+read+close)
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK(rig.results[0].close_in_chain);
CHECK(rig.results[0].cleanup_result()==0);
}
TEST_CASE(
"flow.mode_select: then_read+hard_write+hard_read → mode B (tail stays hard)",
"[flow][mode]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).hard_write(buf,4,0).hard_read(buf,4,0).close_if_opened();
REQUIRE(f.valid());
(void)b.submit();
CHECK(b.rejected_flows().empty());
// mode B: expected_cqes == 5 (open+read+write+read2+close)
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());
{
auto cqe=make_flow_cqe(idx,gen,4,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK(rig.results[0].close_in_chain);
}
TEST_CASE(
"flow.mode_select: then_read+hard_write+then_read → mode A (trailing soft)",
"[flow][mode]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).hard_write(buf,4,0).then_read(buf,4,0).close_if_opened();
REQUIRE(f.valid());
(void)b.submit();
CHECK(b.rejected_flows().empty());
// mode A: expected_cqes == 4 (open+read+write+read2), standalone close after chain
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// standalone close pending
{
auto cqe=make_flow_cqe(idx,gen,4,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK_FALSE(rig.results[0].close_in_chain);
CHECK(rig.results[0].cleanup_result()==0);
}
// ── Mode A: body failures still trigger standalone close ──────────────────────

TEST_CASE(
"flow.mode_a: read fails → standalone close still submitted",
"[flow][mode_a]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,-EIO);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,-ECANCELED);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// close not yet observed
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK(r.open_ok());
CHECK(r.close_needed);
CHECK_FALSE(r.close_in_chain);
CHECK(r.close_cqe_seen);
CHECK(r.cleanup_result()==0);
CHECK(r.ops[1].res==-EIO);
CHECK(r.ops[2].res==-ECANCELED);
}
TEST_CASE(
"flow.mode_a: read short → short_io visible, close still submitted",
"[flow][mode_a]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,2);
rig.rt.on_cqe(&cqe);
}// short: 2<4
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,-ECANCELED);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK(r.ops[1].short_io());
CHECK(r.ops[1].res==2);
CHECK(r.cleanup_result()==0);
}
TEST_CASE(
"flow.mode_a: write fails → standalone close still submitted",
"[flow][mode_a]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,-ENOSPC);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK(r.ops[2].res==-ENOSPC);
CHECK(r.cleanup_result()==0);
}
TEST_CASE(
"flow.mode_a: close_direct fails → cleanup_result negative",
"[flow][mode_a]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,-EIO);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK(r.close_cqe_seen);
CHECK(r.cleanup_result()==-EIO);
CHECK(r.raw_close_result()==-EIO);
// primary ops still correct
CHECK(r.ops[0].res==0);
CHECK(r.ops[1].res==4);
CHECK(r.ops[2].res==4);
}
// ── Mode B: body-fail tolerance (hard-linked body still runs) ─────────────────

TEST_CASE(
"flow.mode_b: read fails, hard_write still runs, close runs",
"[flow][mode_b]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).hard_write(buf,4,0).close_if_opened();
(void)b.submit();
// mode B: 4 CQEs (open+read+write+close)
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,-EIO);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK(r.open_ok());
CHECK(r.close_in_chain);
CHECK(r.close_cqe_seen);
CHECK(r.cleanup_result()==0);
CHECK(r.ops[1].res==-EIO);
CHECK(r.ops[2].res==4);
}
TEST_CASE(
"flow.mode_b: read short, hard_write runs, close runs",
"[flow][mode_b]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).hard_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,1);
rig.rt.on_cqe(&cqe);
}// short
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK(rig.results[0].ops[1].short_io());
CHECK(rig.results[0].cleanup_result()==0);
}
// ── Explicit no-close ownership ───────────────────────────────────────────────

TEST_CASE(
"flow.no_close: open succeeds, no close_if_opened → cleanup nullopt",
"[flow][no_close]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0);// no close_if_opened()
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK(r.open_ok());
CHECK_FALSE(r.close_needed);
CHECK(r.cleanup_result()==nullopt);
CHECK(r.raw_close_result()==nullopt);
}
TEST_CASE(
"flow.no_close: open fails, no close_if_opened → cleanup nullopt",
"[flow][no_close]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0);// no close_if_opened()
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,-ENOENT);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,-ECANCELED);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK_FALSE(rig.results[0].open_ok());
CHECK(rig.results[0].cleanup_result()==nullopt);
}
// ── Additional CQE tag validation ─────────────────────────────────────────────

TEST_CASE(
"flow.cqe_validation: body CQE op_kind mismatch → rejected",
"[flow][cqe_validation]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
// CQE for op_index=1 (recorded as read) but tagged as write → kind mismatch
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
// State unchanged: seen_cqes still 1; correct CQE can still arrive
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// mode A awaiting close
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
}
TEST_CASE(
"flow.cqe_validation: close CQE wrong op_index → rejected",
"[flow][cqe_validation]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
// Close with wrong op_index (should be 3 == initial_op_count, send 5 instead)
{
auto cqe=make_flow_cqe(idx,gen,5,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// wrong op_index → rejected; state unchanged
// Correct close CQE finalizes
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
}
TEST_CASE(
"flow.cqe_validation: unexpected close CQE (no close requested) → rejected",
"[flow][cqe_validation]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0);// no close_if_opened()
(void)b.submit();
u32 idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
// Inject close CQE when neither close_in_chain nor close_submitted → rejected
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// rejected; flow still awaiting read CQE
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK_FALSE(rig.results[0].close_cqe_seen);
}
// ── Length edge cases ─────────────────────────────────────────────────────────

TEST_CASE(
"flow.builder: len==0 accepted",
"[flow][builder]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,0,0);
CHECK(f.valid());
CHECK(f.last_error()==0);
}
TEST_CASE(
"flow.builder: len==INT32_MAX accepted",
"[flow][builder]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,static_cast<SZ>(NL<i32>::max()),0);
CHECK(f.valid());
CHECK(f.last_error()==0);
}
// ── Multiple flows in one submit ──────────────────────────────────────────────

TEST_CASE(
"flow.multi_flow: two flows in one batch both complete",
"[flow][multi]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
// Flow 0: mode A (open+read+write, standalone close)
auto f0=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f0.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
// Flow 1: mode B (open+read, in-chain close)
auto f1=b.open_direct(DirectSlot{1},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f1.then_read(buf,4,0).close_if_opened();
REQUIRE(f0.valid());
REQUIRE(f1.valid());
(void)b.submit();
REQUIRE(b.rejected_flows().empty());

// Flow 0: idx=0,gen=1 (3 initial CQEs, then standalone close)
{
auto cqe=make_flow_cqe(0,1,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(0,1,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(0,1,2,uf::FlowOpKind::write,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// mode A awaiting standalone close

// Flow 1: idx=1,gen=1 (3 CQEs in mode B: open+read+close)
{
auto cqe=make_flow_cqe(1,1,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(1,1,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(1,1,2,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);// flow 1 done (mode B)
CHECK(rig.results[0].close_in_chain);

// Flow 0 standalone close
{
auto cqe=make_flow_cqe(0,1,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==2);// flow 0 done
CHECK_FALSE(rig.results[1].close_in_chain);
CHECK(rig.results[1].cleanup_result()==0);
}
// ── Generation: first allocation is gen=1, free does not bump ─────────────────

TEST_CASE(
"flow.generation: first alloc gen==1, second alloc gen==2",
"[flow][gen]"){
TestRig rig;
// First flow: idx=0, gen=1
{
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.close_if_opened();
(void)b.submit();
{
auto cqe=make_flow_cqe(0,1,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(0,1,1,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
}
REQUIRE(rig.results.size()==1);
// Second flow on same slot: must be gen=2 (try_allocate bumps from 1→2)
{
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.close_if_opened();
(void)b.submit();
{
auto cqe=make_flow_cqe(0,2,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(0,2,1,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
}
REQUIRE(rig.results.size()==2);
}
TEST_CASE(
"flow.generation: CQE with op_index>=initial_op_count (body branch) rejected",
"[flow][gen]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.batch();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).close_if_opened();// initial_op_count=2
(void)b.submit();
u32 idx=0,gen=1;
// Body CQE with op_index=2 but initial_op_count==2 (must be in [0,2)) → rejected
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());
// Correct CQEs proceed normally
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// mode B awaiting close CQE
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
}
