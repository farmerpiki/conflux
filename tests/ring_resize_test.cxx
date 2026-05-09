// Plain TU — not a module unit.
#include<catch2/catch_test_macros.hpp>
#include<cerrno>
#include<liburing.h>

import std;
import conflux.types;
import conflux.uring;

using namespace conflux::uring;
namespace{
// Drain all visible CQEs without blocking.
void drain_cq(Ring&ring){
io_uring_cqe*cqe=nullptr;
while(ring.peek_cqe(&cqe)==0&&cqe!=nullptr)
ring.cqe_seen(cqe);
}
// Submit n NOP SQEs in batches, without draining CQ.
void submit_nops_no_drain(Ring&ring,unsigned n,unsigned batch_sz){
unsigned done=0;
while(done<n){
unsigned const b=min(n-done,batch_sz);
for(unsigned i=0;i<b;++i){
auto sqe=ring.get_sqe();
if(!sqe)break;
io_uring_prep_nop(sqe.raw());
io_uring_sqe_set_data64(sqe.raw(),static_cast<u64>(done+i));
}
ring.submit();
done+=b;
}
}
}// namespace
// ── Build-cap path (-ENOSYS when symbol absent) ───────────────────────────────

TEST_CASE("uring.ring_resize: build_has_io_uring_resize_rings constant accessible","[ring_resize]"){
// Just verifies the exported constant compiles and is reachable.
[[maybe_unused]]bool const cap=build_has_io_uring_resize_rings;
SUCCEED();
}
TEST_CASE("uring.ring_resize: grow_cq_to returns -ENOSYS when built without symbol","[ring_resize]"){
if constexpr(!build_has_io_uring_resize_rings){
auto r=Ring::init(8,setup_flags::defer_taskrun|setup_flags::single_issuer);
REQUIRE(r);
auto rc=r->grow_cq_to(32);
REQUIRE(!rc);
CHECK(rc.error()==-ENOSYS);
}else{
SKIP("built with io_uring_resize_rings — ENOSYS path not exercised");
}
}
// ── Runtime tests (require build symbol + runtime cap) ───────────────────────

TEST_CASE("uring.ring_resize: grow_cq_to grows CQ on DEFER_TASKRUN ring","[ring_resize]"){
if constexpr(!build_has_io_uring_resize_rings)
SKIP("built without io_uring_resize_rings");

auto r=Ring::init(8,setup_flags::defer_taskrun|setup_flags::single_issuer);
REQUIRE(r);
auto caps=detect_caps(*r);
if(!caps.resize_rings)
SKIP("kernel lacks IORING_FEAT_RESIZE_RINGS");

u32 const before=r->ref().cq_entries();
auto rc=r->grow_cq_to(32);
REQUIRE(rc);
CHECK(r->ref().cq_entries()>=32u);
CHECK(r->ref().cq_entries()>=before);
}
TEST_CASE("uring.ring_resize: resize({current_sq,current_cq}) succeeds as identity resize","[ring_resize]"){
if constexpr(!build_has_io_uring_resize_rings)
SKIP("built without io_uring_resize_rings");

auto r=Ring::init(8,setup_flags::defer_taskrun|setup_flags::single_issuer);
REQUIRE(r);
auto caps=detect_caps(*r);
if(!caps.resize_rings)
SKIP("kernel lacks IORING_FEAT_RESIZE_RINGS");

u32 const sq=r->ref().sq_entries();
u32 const cq=r->ref().cq_entries();
auto rc=r->resize({.sq_entries=sq,.cq_entries=cq});
REQUIRE(rc);
CHECK(r->ref().sq_entries()==sq);
CHECK(r->ref().cq_entries()>=cq);// kernel may round up
}
TEST_CASE("uring.ring_resize: grow_cq_to(current) is no-op success","[ring_resize]"){
if constexpr(!build_has_io_uring_resize_rings)
SKIP("built without io_uring_resize_rings");

auto r=Ring::init(8,setup_flags::defer_taskrun|setup_flags::single_issuer);
REQUIRE(r);
auto caps=detect_caps(*r);
if(!caps.resize_rings)
SKIP("kernel lacks IORING_FEAT_RESIZE_RINGS");

u32 const cq=r->ref().cq_entries();
auto rc=r->grow_cq_to(cq);// entries <= current_cq → early return
REQUIRE(rc);
CHECK(r->ref().cq_entries()==cq);
}
TEST_CASE("uring.ring_resize: grow_cq_to(smaller) is no-op success","[ring_resize]"){
if constexpr(!build_has_io_uring_resize_rings)
SKIP("built without io_uring_resize_rings");

auto r=Ring::init(32,setup_flags::defer_taskrun|setup_flags::single_issuer);
REQUIRE(r);
auto caps=detect_caps(*r);
if(!caps.resize_rings)
SKIP("kernel lacks IORING_FEAT_RESIZE_RINGS");

u32 const cq=r->ref().cq_entries();
REQUIRE(cq>4u);
auto rc=r->grow_cq_to(4u);// smaller → no-op
REQUIRE(rc);
CHECK(r->ref().cq_entries()==cq);// unchanged
}
TEST_CASE("uring.ring_resize: resize blocked when CQ overflow active","[ring_resize]"){
if constexpr(!build_has_io_uring_resize_rings)
SKIP("built without io_uring_resize_rings");

// SQ=4, DEFER_TASKRUN: CQ defaults to 8 (2×SQ).
auto r=Ring::init(4,setup_flags::defer_taskrun|setup_flags::single_issuer);
REQUIRE(r);
auto caps=detect_caps(*r);
if(!caps.resize_rings)
SKIP("kernel lacks IORING_FEAT_RESIZE_RINGS");
if(!caps.feat_nodrop)
SKIP("overflow test requires NODROP to guarantee overflow list exists");

// Submit 12 NOPs without draining to force CQ-8 into overflow.
submit_nops_no_drain(*r,12,4);
if(!r->cq_has_overflow())
SKIP("could not force CQ overflow on this runner — skipping");

auto rc=r->grow_cq_to(64);
REQUIRE(!rc);
CHECK(rc.error()==-EBUSY);

// After drain, resize must succeed.
drain_cq(*r);
REQUIRE(!r->cq_has_overflow());
auto rc2=r->grow_cq_to(64);
REQUIRE(rc2);
CHECK(r->ref().cq_entries()>=64u);
}
TEST_CASE("uring.ring_resize: resize fails without DEFER_TASKRUN","[ring_resize]"){
if constexpr(!build_has_io_uring_resize_rings)
SKIP("built without io_uring_resize_rings");

// Our wrapper preflights DEFER_TASKRUN before calling liburing,
// so this test does not need the runtime caps.resize_rings flag.
auto r=Ring::init(8,{});
REQUIRE(r);
u32 const sq=r->ref().sq_entries();
u32 const cq=r->ref().cq_entries();
auto rc=r->resize({.sq_entries=sq,.cq_entries=cq*2u});
REQUIRE(!rc);
CHECK(rc.error()==-EINVAL);
}
TEST_CASE("uring.ring_resize: resize fails on NO_MMAP ring","[ring_resize]"){
if constexpr(!build_has_io_uring_resize_rings)
SKIP("built without io_uring_resize_rings");

static constexpr unsigned sq_sz=8;
io_uring_params p{};
p.flags=IORING_SETUP_NO_MMAP|IORING_SETUP_DEFER_TASKRUN|IORING_SETUP_SINGLE_ISSUER;
ssize_t const needed=io_uring_memory_size_params(sq_sz,&p);
if(needed<=0)
SKIP("io_uring_memory_size_params failed — skipping");
struct Free{
void operator()(void*ptr)const noexcept{free(ptr);}
};
UPD<void,Free>buf{aligned_alloc(4096,static_cast<SZ>(needed))};
if(!buf)
SKIP("aligned_alloc failed — skipping");

auto r=Ring::init_mem(sq_sz,p,buf.get(),static_cast<SZ>(needed));
if(!r)
SKIP("NO_MMAP ring init failed — skipping");

auto rc=r->grow_cq_to(32);
REQUIRE(!rc);
// wrapper preflights NO_MMAP before calling liburing
CHECK(rc.error()==-EOPNOTSUPP);
}
TEST_CASE("uring.ring_resize: RingRef accessors match Ring entries","[ring_resize]"){
auto r=Ring::init(16,setup_flags::defer_taskrun|setup_flags::single_issuer);
REQUIRE(r);
RingRef ref=r->ref();
CHECK(ref.sq_entries()==r->ref().sq_entries());
CHECK(ref.cq_entries()==r->ref().cq_entries());
CHECK(ref.sq_entries()>0u);
CHECK(ref.cq_entries()>0u);
}
