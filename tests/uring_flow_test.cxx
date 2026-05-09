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
explicit TestRig(
unsigned sq_size=16)
:ring{[sq_size]{
auto r=Ring::init(sq_size,{});
REQUIRE(r);
return move(*r);
}()},
rt{ring,[&](uf::FlowResult fr)noexcept{results.push_back(fr);}}{// NOLINT(bugprone-exception-escape) — reserve below prevents allocation
results.reserve(64);
}
};
}// namespace
// ── Auto-mode selection ───────────────────────────────────────────────────────

TEST_CASE(
"flow.mode_select: then_read+then_write → mode A",
"[flow][mode]"){
TestRig rig;
char buf[16]={};
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
CHECK(r.ops[1].res==-ECANCELED);
CHECK(r.ops[2].res==-ECANCELED);
}
TEST_CASE(
"flow.mode_b: close CQE arrives before last body CQE",
"[flow][mode_b]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
auto b=rig.rt.flow();
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
// ── Deferred close (mode A SQ-full at on_chain_complete) ─────────────────────

TEST_CASE(
"flow.deferred_close: SQ-full defers close, drain re-submits",
"[flow][deferred]"){
// Ring with exactly 3 SQ slots: open+read+write fills it entirely.
// on_chain_complete cannot get a SQE for the standalone close → close_pending=true.
TestRig rig{3};
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();// mode A: 3 SQEs
(void)b.submit();// SQ now full (3/3)
REQUIRE(b.rejected_flows().empty());

u32 const idx=0,gen=1;
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
// on_chain_complete fires: get_sqe() fails (SQ full) → close_pending=true
CHECK(rig.results.empty());

// Flush the SQ to the kernel to free the 3 slots, then drain deferred closes.
rig.ring.submit();
rig.rt.drain_deferred_closes();// retry → get_sqe() succeeds → close_submitted=true

// Inject the close CQE
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK(rig.results[0].open_ok());
CHECK(rig.results[0].cleanup_result()==0);
CHECK_FALSE(rig.results[0].close_in_chain);

// Double-call drain after success: idempotent, flow already done
rig.rt.drain_deferred_closes();
CHECK(rig.results.size()==1);
}
// ── Deferred close: drain retry while SQ still full keeps entry ───────────────

TEST_CASE(
"flow.deferred_close: drain while SQ still full keeps entry, succeeds on second drain",
"[flow][deferred]"){
// Ring with 3 slots: open+read+write fills it.
// on_chain_complete defers close (SQ full).
// First drain while SQ still full → stays deferred. Second drain after flush succeeds.
TestRig rig{3};
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();// mode A
(void)b.submit();// SQ full (3/3)
u32 const idx=0,gen=1;
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
// First drain: SQ still full → get_sqe() fails → entry stays
rig.rt.drain_deferred_closes();
CHECK(rig.results.empty());
// Flush to kernel → free 3 SQ slots
rig.ring.submit();
// Second drain: succeeds
rig.rt.drain_deferred_closes();
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK(rig.results[0].cleanup_result()==0);
CHECK_FALSE(rig.results[0].close_in_chain);
}
// ── Mode B: close CQE carries non-zero result (actual close failure) ──────────

TEST_CASE(
"flow.mode_b: close fails with -EIO after successful chain",
"[flow][mode_b]"){
// open + then_read (1 body then_): mode B. All body ops succeed.
// Close CQE arrives with -EIO (not -ECANCELED). cleanup_result() == -EIO.
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.then_read(buf,4,0).close_if_opened();// mode B: 1 then_ body → close in chain
(void)b.submit();
u32 const idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// awaiting close
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::close_direct,-EIO);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK(r.open_ok());
CHECK(r.close_in_chain);
CHECK(r.close_cqe_seen);
CHECK(r.cleanup_result()==-EIO);
CHECK(r.raw_close_result()==-EIO);
}
// ── resume_deferred_close idempotency before close CQE ───────────────────────

TEST_CASE(
"flow.deferred_close: resume_deferred_close double-call before CQE is no-op",
"[flow][deferred]"){
// After first resume_deferred_close succeeds (close_pending cleared),
// a second call hits the close_pending guard and is a no-op.
TestRig rig{3};
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 const idx=0,gen=1;
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
// close_pending=true; flush SQ to free slots
rig.ring.submit();
// First resume: submits close SQE, close_pending → false
rig.rt.resume_deferred_close(idx,gen);
CHECK(rig.results.empty());
// Second resume before CQE: close_pending guard → no-op (no double submission)
rig.rt.resume_deferred_close(idx,gen);
CHECK(rig.results.empty());
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK(rig.results[0].cleanup_result()==0);
}
// ── Batch-full rejection recorded in rejected_flows() ─────────────────────────

TEST_CASE(
"flow.batch_full: open_direct past kMaxBatch records rejection",
"[flow][builder]"){
// Build kMaxBatch flows to fill the builder array, then try one more.
// The overflow must appear in rejected_flows() after submit().
// Use a large ring so SQ is never the bottleneck.
TestRig rig{256};
// We can't easily reach kMaxBatch (64) valid flows without a large fixed-file table.
// Use invalid builders (hard_read as first body → -EINVAL) to fill builder slots cheaply.
char buf[4]={};

// Fill all 64 builder slots with invalid flows.
constexpr u32 kMaxBatch=64;// matches flow.cxx internal constant
auto b=rig.rt.flow();
for(u32 i=0;i<kMaxBatch;++i){
auto f=b.open_direct(DirectSlot{i},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.hard_read(buf,1,0);// force invalid so no SQ slots needed
}
// One more open_direct: batch array full → must be rejected immediately
auto overflow=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
CHECK_FALSE(overflow.valid());
CHECK(overflow.last_error()==-EINVAL);// null handle → last_error returns -EINVAL

(void)b.submit();

// rejected_flows() must include an entry for the overflow
// (plus kMaxBatch entries for the invalid hard_read builders)
auto rej=b.rejected_flows();
// Find the -ENOBUFS entry (batch-full overflow)
bool found_overflow=false;
for(auto const&r:rej)
if(r.err==-ENOBUFS)
found_overflow=true;
CHECK(found_overflow);
}
// ── Mode B: write fails, close still runs (body-fail tolerance) ───────────────

TEST_CASE(
"flow.mode_b: write fails, hard-linked close still runs",
"[flow][mode_b]"){
// open + then_read + hard_write: write→close boundary is hard (mode B auto-rule).
// open ok, read ok, write -ENOSPC → close still runs via hard link.
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).hard_write(buf,4,0).close_if_opened();
(void)b.submit();
// mode B: expected_cqes==4 (open+read+write+close)
u32 const idx=0,gen=1;
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
CHECK(r.open_ok());
CHECK(r.close_in_chain);
CHECK(r.close_cqe_seen);
// write failed but close ran: cleanup_result() reflects the close result
CHECK(r.cleanup_result()==0);
CHECK(r.ops[2].res==-ENOSPC);
}
// ── Mode A vs B observable equivalence on open failure ────────────────────────

TEST_CASE(
"flow.equiv: mode A and mode B both return cleanup_result==nullopt on open fail",
"[flow][equiv]"){
// Both shapes must satisfy cleanup_result()==nullopt when open fails.
// raw_close_result() differs: mode A → nullopt (no CQE), mode B → -ECANCELED (in-chain).

// Mode A shape: open + then_read + then_write → cascade reaches close → mode A
{
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 const idx=0,gen=1;
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
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK_FALSE(r.open_ok());
CHECK(r.cleanup_result()==nullopt);// normalized: open failed
// mode A: no close CQE was ever submitted (open_ok==false)
CHECK(r.raw_close_result()==nullopt);
CHECK_FALSE(r.close_cqe_seen);
}

// Mode B shape: open + then_read + hard_write → close in chain → mode B
{
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).hard_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 const idx=0,gen=1;
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
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,-ECANCELED);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
auto const&r=rig.results[0];
CHECK_FALSE(r.open_ok());
CHECK(r.cleanup_result()==nullopt);// same normalization as mode A
// mode B: close CQE arrived (cascade); raw result is -ECANCELED
CHECK(r.close_cqe_seen);
CHECK(r.raw_close_result()==-ECANCELED);
}
}
// ── CQE tag: body op_index interior of [initial_op_count, max_initial_ops) ────

TEST_CASE(
"flow.cqe_validation: body op_index in (initial_op_count, max_initial_ops) rejected",
"[flow][cqe_validation]"){
// initial_op_count=2 (open+read); max_initial_ops=8.
// op_index=5 is inside [2,8) — past the runtime bound but under the compile-time limit.
// The body branch checks op_index < initial_op_count (not max_initial_ops), so both
// the boundary (2) and interior values (3..7) must be rejected.
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).close_if_opened();// initial_op_count=2, mode B: expected_cqes=3
(void)b.submit();
u32 const idx=0,gen=1;
// Interior: op_index=5 is in [initial_op_count=2, max_initial_ops=8) → rejected
{
auto cqe=make_flow_cqe(idx,gen,5,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// rejected; state unchanged
// Correct CQEs still advance the flow
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// mode B: awaiting close
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
}
// ── Duplicate close CQE rejected via close_seen guard ────────────────────────

TEST_CASE(
"flow.cqe_validation: duplicate close CQE rejected after close_seen",
"[flow][cqe_validation]"){
// Mode A: submit open+read+write, complete all body CQEs, then close CQE (accepted).
// Inject a second spurious close CQE → stale generation → silently dropped.
// finish_flow must not be called twice.
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
u32 const idx=0,gen=1;
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
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
// Slab cell released; generation will differ on next alloc.
// A second spurious close with the same (idx,gen) tag must be silently dropped.
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.size()==1);// no second callback
}
// ── -EOVERFLOW verified through submit + rejected_flows() ────────────────────

TEST_CASE(
"flow.builder: len > INT32_MAX appears in rejected_flows() with -EOVERFLOW",
"[flow][builder]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
SZ big=static_cast<SZ>(NL<i32>::max())+1;
f.then_read(buf,big,0);
CHECK(f.last_error()==-EOVERFLOW);
(void)b.submit();
auto rej=b.rejected_flows();
REQUIRE(rej.size()==1);
CHECK(rej[0].err==-EOVERFLOW);
}
// ── -EAGAIN when SQ exhausted by prior flows in the same batch ────────────────

TEST_CASE(
"flow.submit: SQ-full mid-batch produces -EAGAIN in rejected_flows()",
"[flow][submit]"){
// Ring with 3 SQ slots. Flow A (mode A body): open+read+write = 3 SQEs.
// After flow A fills the ring, flow B gets -EAGAIN.
TestRig rig{3};
char buf[4]={};
auto b=rig.rt.flow();
// Flow A fills all 3 SQ slots
auto fa=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
fa.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
// Flow B: ring full → -EAGAIN
auto fb=b.open_direct(DirectSlot{1},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
fb.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
(void)b.submit();
auto rej=b.rejected_flows();
bool found_eagain=false;
for(auto const&r:rej)
if(r.err==-EAGAIN)
found_eagain=true;
CHECK(found_eagain);
// Flow A still completes normally
u32 const idx=0,gen=1;
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
rig.ring.submit();
rig.rt.drain_deferred_closes();
{
auto cqe=make_flow_cqe(idx,gen,3,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK(rig.results[0].open_ok());
CHECK(rig.results[0].cleanup_result()==0);
}
// ── with_direct_file scoped form ─────────────────────────────────────────────

TEST_CASE(
"flow.with_direct_file: scoped form auto-closes and delivers result",
"[flow][scoped]"){
// with_direct_file desugars to open_direct + build_ops(f) + f.close_if_opened().
// Verify close is auto-injected: 1 then_ body → mode B.
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
b.with_direct_file(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY,0,[&](uf::DirectFileFlow&f){
f.then_read(buf,4,0);
});
(void)b.submit();
CHECK(b.rejected_flows().empty());
u32 const idx=0,gen=1;
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
CHECK(rig.results[0].open_ok());
CHECK(rig.results[0].close_in_chain);
CHECK(rig.results[0].cleanup_result()==0);
}
TEST_CASE(
"flow.with_direct_file: invalid body → close_if_opened is no-op",
"[flow][scoped]"){
// If the lambda makes the flow invalid, close_if_opened() called by the
// scoped wrapper must be a no-op.
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
b.with_direct_file(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY,0,[&](uf::DirectFileFlow&f){
f.hard_read(buf,1,0);
});// hard as first body → invalid
(void)b.submit();
auto rej=b.rejected_flows();
REQUIRE(rej.size()==1);
CHECK(rej[0].err==-EINVAL);
CHECK(rig.results.empty());
}
// ── Terminal SQE link-flag inspection ─────────────────────────────────────────

TEST_CASE(
"flow.sqe_flags: terminal SQE of each flow has both link flags cleared",
"[flow][sqe_flags]"){
// Two flows emitted in one submit; terminal SQE of each must have
// IOSQE_IO_LINK and IOSQE_IO_HARDLINK cleared so flow A cannot chain into flow B.
// Flow A: mode A (open+read+write = 3 SQEs), terminal = write.
// Flow B: mode B (open+read+close = 3 SQEs), terminal = close.
TestRig rig{16};
char buf[4]={};
auto b=rig.rt.flow();
auto fa=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
fa.then_read(buf,4,0).then_write(buf,4,0).close_if_opened();
auto fb=b.open_direct(DirectSlot{1},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
fb.then_read(buf,4,0).close_if_opened();

auto*ring_raw=rig.ring.raw();
unsigned const tail_before=ring_raw->sq.sqe_tail;

(void)b.submit();
REQUIRE(b.rejected_flows().empty());

unsigned const tail_after=ring_raw->sq.sqe_tail;
CHECK(tail_after-tail_before==6);// 3+3 SQEs

unsigned const mask=ring_raw->sq.ring_mask;
constexpr unsigned lmask=IOSQE_IO_LINK|IOSQE_IO_HARDLINK;

// Flow A terminal (write, index 2 from tail_before)
auto const&sqe_a_term=ring_raw->sq.sqes[(tail_before+2)&mask];
CHECK((sqe_a_term.flags&lmask)==0);

// Flow B terminal (close, index 5 from tail_before)
auto const&sqe_b_term=ring_raw->sq.sqes[(tail_before+5)&mask];
CHECK((sqe_b_term.flags&lmask)==0);
CHECK((sqe_b_term.flags&IOSQE_FIXED_FILE)==0);// close_direct must not use fixed-file fd

// Non-terminal SQEs must carry a link flag
auto const&sqe_a_open=ring_raw->sq.sqes[(tail_before+0)&mask];
CHECK((sqe_a_open.flags&lmask)!=0);// open → read
auto const&sqe_b_open=ring_raw->sq.sqes[(tail_before+3)&mask];
CHECK((sqe_b_open.flags&lmask)!=0);// open → read
}
// ── Generation wrap: skip-zero discipline ─────────────────────────────────────

TEST_CASE(
"flow.generation: wrap from 0xFFFFFF skips 0 and lands on 1",
"[flow][gen]"){
// Hack the slab cell's generation to 0xFFFFFF (the max 24-bit value).
// The next try_allocate must increment to 0x1000000, mask to 0, detect zero, skip to 1.
// Verified by observing that gen=0 CQEs are rejected and gen=1 CQEs are accepted.
TestRig rig;
rig.rt.test_hack_slab_generation(0,0xFFFFFFu);

auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.close_if_opened();// mode B: initial_op_count=1, expected_cqes=2
(void)b.submit();
REQUIRE(b.rejected_flows().empty());

// CQE with gen=0 must be rejected (live SQEs never carry gen==0)
{
auto cqe=make_flow_cqe(0,0,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());

// CQE with gen=1 must be accepted
{
auto cqe=make_flow_cqe(0,1,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// mode B: awaiting close
{
auto cqe=make_flow_cqe(0,1,1,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
CHECK(rig.results[0].open_ok());
}
// ── Slab: try_get with gen=0 on never-allocated cell ─────────────────────────

TEST_CASE(
"flow.slab: CQE with gen=0 on never-allocated cell rejected",
"[flow][slab]"){
TestRig rig;
// No flow allocated; slab cell 0 has generation==0 (initial state).
// A CQE with gen=0 must be rejected.
auto cqe=make_flow_cqe(0,0,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
CHECK(rig.results.empty());
}
// ── Slab: slab full → -ENOSPC ────────────────────────────────────────────────

TEST_CASE(
"flow.slab: slab full → -ENOSPC in rejected_flows()",
"[flow][slab]"){
TestRig rig{16};
rig.rt.test_hack_drain_slab_freelist();
auto b=rig.rt.flow();
(void)b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
(void)b.submit();
auto rej=b.rejected_flows();
REQUIRE(rej.size()==1);
CHECK(rej[0].err==-ENOSPC);
}
// ── rejected_flows: span stable after submit before next flow() ───────────────

TEST_CASE(
"flow.rejected_flows: span stable after submit before next flow()",
"[flow][builder]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
f.hard_read(buf,1,0);// force invalid → rejection
(void)b.submit();
auto rej=b.rejected_flows();// span into rt_.rejections_
REQUIRE(rej.size()==1);
CHECK(rej[0].err==-EINVAL);
CHECK(rej[0].flow_local_index==0);
// Re-read through the span: still valid (no flow() call yet)
CHECK(rej.data()==b.rejected_flows().data());
}
// ── CQE tag: op_index=255 (max u8) rejected in body branch ───────────────────

TEST_CASE(
"flow.cqe_validation: body CQE with op_index=255 rejected",
"[flow][cqe_validation]"){
TestRig rig;
char buf[4]={};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDWR);
f.then_read(buf,4,0).close_if_opened();// initial_op_count=2, mode B: expected_cqes=3
(void)b.submit();
u32 const idx=0,gen=1;
{
auto cqe=make_flow_cqe(idx,gen,255,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
CHECK(rig.results.empty());// rejected; state unchanged
{
auto cqe=make_flow_cqe(idx,gen,0,uf::FlowOpKind::open_direct,0);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,1,uf::FlowOpKind::read,4);
rig.rt.on_cqe(&cqe);
}
{
auto cqe=make_flow_cqe(idx,gen,2,uf::FlowOpKind::close_direct,0);
rig.rt.on_cqe(&cqe);
}
REQUIRE(rig.results.size()==1);
}
