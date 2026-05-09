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
struct TestRig{
Ring ring;
V<uf::FlowResult>results;
uf::FlowRuntime rt;
explicit TestRig(bool force_unstable=false)
:ring{[]{
auto r=Ring::init(16,{});
REQUIRE(r);
return move(*r);
}()},
rt{ring,[&]{
auto caps=detect_caps(ring.ref());
if(force_unstable)
caps.path_lifetime_stable=false;
return caps;}(),[&](uf::FlowResult fr)noexcept{results.push_back(fr);}}{
results.reserve(64);
}
};
}// namespace
// ── OwnedInlinePath construction ─────────────────────────────────────────────

TEST_CASE(
"owned_inline_path: typical path round-trips",
"[owned_path]"){
auto p=uf::OwnedInlinePath::from_sv("/dev/null");
REQUIRE(p);
CHECK(SV{p->c_str()}=="/dev/null");
CHECK(p->len==9);
}
TEST_CASE(
"owned_inline_path: exactly 255 bytes accepted",
"[owned_path]"){
S s(255,'a');
auto p=uf::OwnedInlinePath::from_sv(s);
REQUIRE(p);
CHECK(p->len==255);
CHECK(p->c_str()[255]=='\0');
}
TEST_CASE(
"owned_inline_path: 256 bytes rejected with -ENAMETOOLONG",
"[owned_path]"){
S s(256,'a');
auto p=uf::OwnedInlinePath::from_sv(s);
CHECK_FALSE(p);
CHECK(p.error()==-ENAMETOOLONG);
}
TEST_CASE(
"owned_inline_path: embedded NUL rejected with -EINVAL",
"[owned_path]"){
auto p=uf::OwnedInlinePath::from_sv(SV{"abc\0def",7});
CHECK_FALSE(p);
CHECK(p.error()==-EINVAL);
}
// ── Path-stability rejection ──────────────────────────────────────────────────

TEST_CASE(
"owned_path: accepted when path_lifetime_stable=false",
"[owned_path][submit]"){
TestRig rig{true};
auto b=rig.rt.flow();
auto f=b.open_direct_owned(DirectSlot{0},AT_FDCWD,"/dev/null",O_RDONLY);
REQUIRE(f.valid());
auto n=b.submit();
CHECK(n==1);
CHECK(b.rejected_flows().empty());
}
TEST_CASE(
"owned_path: borrowed rejected, no duplicate, when path_lifetime_stable=false",
"[owned_path][submit]"){
TestRig rig{true};
auto b=rig.rt.flow();
auto f=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
REQUIRE(f.valid());
auto n=b.submit();
CHECK(n==0);
REQUIRE(b.rejected_flows().size()==1);
CHECK(b.rejected_flows()[0].err==-EOPNOTSUPP);
}
TEST_CASE(
"owned_path: early builder error not overwritten by unstable-path pre-pass",
"[owned_path][submit]"){
TestRig rig{true};
S too_long(256,'x');
auto b=rig.rt.flow();
auto f=b.open_direct_owned(DirectSlot{0},AT_FDCWD,too_long,O_RDONLY);
CHECK_FALSE(f.valid());
CHECK(f.last_error()==-ENAMETOOLONG);
auto n=b.submit();
CHECK(n==0);
REQUIRE(b.rejected_flows().size()==1);
CHECK(b.rejected_flows()[0].err==-ENAMETOOLONG);
}
// ── Mixed batch ───────────────────────────────────────────────────────────────

TEST_CASE(
"owned_path: mixed batch — borrowed rejected, owned accepted, unstable",
"[owned_path][submit]"){
TestRig rig{true};
auto b=rig.rt.flow();
auto f0=b.open_direct(DirectSlot{0},AT_FDCWD,uf::BorrowedPath{"/dev/null"},O_RDONLY);
auto f1=b.open_direct_owned(DirectSlot{1},AT_FDCWD,"/dev/null",O_RDONLY);
REQUIRE(f0.valid());
REQUIRE(f1.valid());
auto n=b.submit();
CHECK(n==1);
REQUIRE(b.rejected_flows().size()==1);
CHECK(b.rejected_flows()[0].flow_local_index==0);
CHECK(b.rejected_flows()[0].err==-EOPNOTSUPP);
}
// ── Runtime path storage lifetime ────────────────────────────────────────────

TEST_CASE(
"owned_path: runtime storage survives builder slot reuse",
"[owned_path][lifetime]"){
TestRig rig{true};
{
auto b=rig.rt.flow();
auto f=b.open_direct_owned(DirectSlot{0},AT_FDCWD,"/dev/null",O_RDONLY);
REQUIRE(f.valid());
(void)b.submit();
}
// Flow index 0 was allocated. Capture the runtime path pointer before any reuse.
char const*stored=rig.rt.test_owned_path_ptr(0);
REQUIRE(stored!=nullptr);
CHECK(SV{stored}=="/dev/null");

// Submit a second flow — reuses builder slot 0 in the builders_ array.
{
auto b=rig.rt.flow();
auto f=b.open_direct_owned(DirectSlot{1},AT_FDCWD,"/tmp",O_RDONLY);
REQUIRE(f.valid());
(void)b.submit();
}
// FlowSlab freelist is initialized in reverse (flow.cxx: free_[i]=kMaxFlows-1-i),
// so first try_allocate yields index 0 and second yields index 1 on a fresh slab.
// CQEs are never delivered in this test, so slab slots are never released.
CHECK(SV{rig.rt.test_owned_path_ptr(0)}=="/dev/null");
CHECK(SV{rig.rt.test_owned_path_ptr(1)}=="/tmp");
}
// ── OwnedInlinePath overload called directly ─────────────────────────────────

TEST_CASE(
"owned_path: OwnedInlinePath overload accepted under !stable",
"[owned_path][submit]"){
TestRig rig{true};
auto p=uf::OwnedInlinePath::from_sv("/dev/null");
REQUIRE(p);
auto b=rig.rt.flow();
auto f=b.open_direct_owned(DirectSlot{0},AT_FDCWD,*p,O_RDONLY);
REQUIRE(f.valid());
auto n=b.submit();
CHECK(n==1);
CHECK(b.rejected_flows().empty());
}
// ── with_direct_file_owned ────────────────────────────────────────────────────

TEST_CASE(
"owned_path: with_direct_file_owned builds close-in-chain flow",
"[owned_path][submit]"){
TestRig rig{true};
char buf[4]={};
auto p=uf::OwnedInlinePath::from_sv("/dev/null");
REQUIRE(p);
auto b=rig.rt.flow();
b.with_direct_file_owned(DirectSlot{0},AT_FDCWD,*p,O_RDONLY,0,
[&](uf::DirectFileFlow f)noexcept{
f.then_read(buf,4,0);
});
auto n=b.submit();
CHECK(n==1);
CHECK(b.rejected_flows().empty());
}
// ── Kernel round-trip ─────────────────────────────────────────────────────────

TEST_CASE(
"owned_path: kernel round-trip opens /dev/null via owned path",
"[owned_path][kernel]"){
// Uses stable caps (force_unstable=false); real submission to kernel.
Ring ring{[]{auto r=Ring::init(8,{});REQUIRE(r);return move(*r);}()};
REQUIRE(ring.register_files_sparse(4)==0);
auto caps=detect_caps(ring.ref());
V<uf::FlowResult>results;
results.reserve(4);
uf::FlowRuntime rt{ring,caps,[&](uf::FlowResult fr)noexcept{results.push_back(fr);}};

auto b=rt.flow();
auto f=b.open_direct_owned(DirectSlot{0},AT_FDCWD,"/dev/null",O_RDONLY);
REQUIRE(f.valid());
auto n=b.submit();
REQUIRE(n==1);

// Submit to kernel and wait for one CQE.
ring.submit();
io_uring_cqe*cqe=nullptr;
int rc=ring.wait_cqe(&cqe);
REQUIRE(rc==0);
REQUIRE(cqe!=nullptr);
rt.on_cqe(cqe);
ring.cqe_seen(cqe);

REQUIRE(results.size()==1);
CHECK(results[0].open_ok());
CHECK(results[0].ops[0].res>=0);
}
