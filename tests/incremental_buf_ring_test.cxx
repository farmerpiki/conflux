// Plain TU — not a module unit.
#include<catch2/catch_test_macros.hpp>
#include<liburing.h>
#include<sys/wait.h>
#include<unistd.h>

import std;
import conflux.types;
import conflux.uring;
import conflux.socket_io;

#ifndef ASSERT_PROBE_BIN
#error "ASSERT_PROBE_BIN must be defined by CMake"
#endif
namespace{
int run_probe(char const*probe)noexcept{
pid_t const pid=::fork();
if(pid<0)return-1;
if(pid==0){
char*args[]={const_cast<char*>(ASSERT_PROBE_BIN),
const_cast<char*>(probe),nullptr};
::execv(ASSERT_PROBE_BIN,args);
::_exit(3);
}
int status{};
::waitpid(pid,&status,0);
if(WIFEXITED(status))return WEXITSTATUS(status);
return-1;
}
}
namespace{
u32 inc_flags(u16 buf_id,bool buf_more)noexcept{
u32 f=IORING_CQE_F_BUFFER|(static_cast<u32>(buf_id)<<IORING_CQE_BUFFER_SHIFT);
if(buf_more)f|=IORING_CQE_F_BUF_MORE;
return f;
}
template<typename F>
struct ScopeExit{
F fn;
~ScopeExit()noexcept{fn();}
};
template<typename F>
ScopeExit(F)->ScopeExit<F>;
struct Rig{
conflux::uring::Ring uring;
BufferRing ring;
Rig(u32 count,SZ buf_size,u16 gid=0)
:uring{[]{
auto r=conflux::uring::Ring::init(32,{});
REQUIRE(r);
return move(*r);
}()},
ring{uring.ref(),BufferRingOptions{.count=count,.buf_size=buf_size,.group_id=gid,.huge_pages=false,.mode=BufferRingMode::incremental}}{}
};
}// namespace
// BUF_MORE CQE: head unchanged, offset=0, more()=true, bytes span correct size.
TEST_CASE("incremental: BUF_MORE leaves head unchanged","[incremental]"){
Rig rig{8,64};
u32 const h0=rig.ring.debug_head_pos();
auto slice=buffer_slice_from_incremental_cqe(rig.ring,4,inc_flags(0,true));
CHECK(rig.ring.debug_head_pos()==h0);// head must NOT advance on BUF_MORE
CHECK(slice.more());
CHECK(slice.id()==0u);
CHECK(slice.offset()==SZ{0});
CHECK(slice.size()==SZ{4});
CHECK(slice.bytes().size()==4u);
// No recycle yet — more still set.
slice.recycle_if_final();// must be a no-op
CHECK(rig.ring.debug_head_pos()==h0);
}
// Final CQE after two BUF_MORE: offsets accumulate, head advances once at the end.
TEST_CASE("incremental: final CQE advances head by 1, offsets accumulate","[incremental]"){
Rig rig{8,64};
u32 const h0=rig.ring.debug_head_pos();
auto s0=buffer_slice_from_incremental_cqe(rig.ring,4,inc_flags(0,true));
CHECK(rig.ring.debug_head_pos()==h0);
CHECK(s0.offset()==SZ{0});
CHECK(s0.size()==SZ{4});
auto s1=buffer_slice_from_incremental_cqe(rig.ring,8,inc_flags(0,true));
CHECK(rig.ring.debug_head_pos()==h0);
CHECK(s1.offset()==SZ{4});
CHECK(s1.size()==SZ{8});
auto s2=buffer_slice_from_incremental_cqe(rig.ring,20,inc_flags(0,false));
CHECK(rig.ring.debug_head_pos()==h0+1u);// head advances on final CQE
CHECK(s2.offset()==SZ{12});
CHECK(s2.size()==SZ{20});
CHECK(!s2.more());
s2.recycle_if_final();// recycles buffer back to pool
// Next CQE for ID 0 starts at offset 0 again (new buffer acquisition).
auto s3=buffer_slice_from_incremental_cqe(rig.ring,4,inc_flags(0,true));
CHECK(s3.offset()==SZ{0});
CHECK(rig.ring.debug_head_pos()==h0+1u);// still BUF_MORE — no advance
}
// Different buffer IDs track their offsets independently.
TEST_CASE("incremental: independent per-ID offset tracking","[incremental]"){
Rig rig{8,64};
u32 const h0=rig.ring.debug_head_pos();
// Start both IDs with BUF_MORE.
auto s0a=buffer_slice_from_incremental_cqe(rig.ring,10,inc_flags(0,true));
auto s1a=buffer_slice_from_incremental_cqe(rig.ring,20,inc_flags(1,true));
CHECK(rig.ring.debug_head_pos()==h0);
CHECK(s0a.offset()==SZ{0});
CHECK(s1a.offset()==SZ{0});
// Finalize ID 1 first.
auto s1b=buffer_slice_from_incremental_cqe(rig.ring,15,inc_flags(1,false));
CHECK(s1b.offset()==SZ{20});
CHECK(rig.ring.debug_head_pos()==h0+1u);
s1b.recycle_if_final();
// ID 0 continues independently.
auto s0b=buffer_slice_from_incremental_cqe(rig.ring,10,inc_flags(0,false));
CHECK(s0b.offset()==SZ{10});
CHECK(rig.ring.debug_head_pos()==h0+2u);
s0b.recycle_if_final();
}
// recycle_if_final() on BUF_MORE slice is a no-op; slice stays valid.
TEST_CASE("incremental: recycle_if_final no-op when more=true","[incremental]"){
Rig rig{8,64};
u32 const h0=rig.ring.debug_head_pos();
auto slice=buffer_slice_from_incremental_cqe(rig.ring,4,inc_flags(0,true));
CHECK(slice.more());
CHECK(slice.valid());
slice.recycle_if_final();
slice.recycle_if_final();// also idempotent for BUF_MORE slices
CHECK(rig.ring.debug_head_pos()==h0);
CHECK(slice.valid());// ring_ not cleared when more=true
}
// recycle_if_final() is idempotent on final slice: second call does nothing.
TEST_CASE("incremental: recycle_if_final idempotent on final CQE","[incremental]"){
Rig rig{8,64};
u32 const h0=rig.ring.debug_head_pos();
auto slice=buffer_slice_from_incremental_cqe(rig.ring,32,inc_flags(0,false));
CHECK(!slice.more());
CHECK(rig.ring.debug_head_pos()==h0+1u);
slice.recycle_if_final();
CHECK(!slice.valid());// ring_ cleared after first recycle
slice.recycle_if_final();// must not crash or double-recycle
}
// ScopeExit pattern: recycle_if_final() runs on exception unwind.
TEST_CASE("incremental: ScopeExit recycles on exception","[incremental]"){
Rig rig{8,64};
u32 rc_flags=inc_flags(0,false);
bool scope_ran=false;
try{
auto slice=buffer_slice_from_incremental_cqe(rig.ring,32,rc_flags);
REQUIRE(!slice.more());
ScopeExit guard{[&]()noexcept{
slice.recycle_if_final();
if(!slice.more())rc_flags=0;
scope_ran=true;
}};
throw RE{"simulated failure"};
}catch(RE const&){}
CHECK(scope_ran);
CHECK(rc_flags==0u);
// Ring still operational: next CQE works.
auto next=buffer_slice_from_incremental_cqe(rig.ring,8,inc_flags(1,false));
CHECK(next.valid());
next.recycle_if_final();
}
// bytes() span reflects the correct offset into the slab (size at minimum).
TEST_CASE("incremental: bytes() span size matches len","[incremental]"){
Rig rig{8,64};
auto s0=buffer_slice_from_incremental_cqe(rig.ring,13,inc_flags(2,true));
CHECK(s0.bytes().size()==13u);
auto s1=buffer_slice_from_incremental_cqe(rig.ring,27,inc_flags(2,false));
CHECK(s1.bytes().size()==27u);
// The two spans must be adjacent: s1.data == s0.data + s0.size
CHECK(s1.bytes().data()==s0.bytes().data()+13u);
s1.recycle_if_final();
}
// Assert probe: res<0 with IORING_CQE_F_BUFFER → assert(res>0) fires.
TEST_CASE("incremental.assert: negative res with buffer flag detected","[incremental][death]"){
#ifdef NDEBUG
SKIP("assert inactive in release build");
#else
REQUIRE(run_probe("inc_neg_res")==42);
#endif
}
// Assert probe: res>0 but IORING_CQE_F_BUFFER absent → assert(cqe_has_buffer) fires.
TEST_CASE("incremental.assert: missing buffer flag detected","[incremental][death]"){
#ifdef NDEBUG
SKIP("assert inactive in release build");
#else
REQUIRE(run_probe("inc_no_buf_flag")==42);
#endif
}
