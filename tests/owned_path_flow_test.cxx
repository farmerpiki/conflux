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
// Runtime path for flow index 0 still holds the first path (flow index 1 gets /tmp).
CHECK(SV{rig.rt.test_owned_path_ptr(0)}=="/dev/null");
CHECK(SV{rig.rt.test_owned_path_ptr(1)}=="/tmp");
}
