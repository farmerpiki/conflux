// Plain TU — not a module unit.
#include<catch2/catch_test_macros.hpp>

import std;
import conflux.types;
#include "direct_slot_pool.hxx"
namespace{
constexpr u32 kCap=8;
DirectSlotPool make_pool(){
return DirectSlotPool{kCap};
}
}// namespace
TEST_CASE(
"dsp.adopt: kernel-allocated slot → populated, free_count decreases",
"[direct_slot_pool]"){
auto p=make_pool();
CHECK(p.free_count()==kCap);
auto r=p.adopt_kernel_allocated(3);
REQUIRE(r);
CHECK(p.slot_state(3)==DirectSlotState::populated);
CHECK(p.free_count()==kCap-1);
}
TEST_CASE(
"dsp.adopt_duplicate: second adopt on same slot → bad_state",
"[direct_slot_pool]"){
auto p=make_pool();
REQUIRE(p.adopt_kernel_allocated(2));
auto r=p.adopt_kernel_allocated(2);
REQUIRE(!r);
CHECK(r.error()==DirectSlotError::bad_state);
CHECK(p.free_count()==kCap-1);
}
TEST_CASE(
"dsp.close_success: mark_closing + release_closed → slot returns to free",
"[direct_slot_pool]"){
auto p=make_pool();
REQUIRE(p.adopt_kernel_allocated(1));
CHECK(p.free_count()==kCap-1);
REQUIRE(p.mark_closing(1));
CHECK(p.slot_state(1)==DirectSlotState::closing);
REQUIRE(p.release_closed(1));
CHECK(p.slot_state(1)==DirectSlotState::free_slot);
CHECK(p.free_count()==kCap);
CHECK(p.poisoned_count()==0);
}
TEST_CASE(
"dsp.close_failure: poison → slot never reused, poisoned_count increases",
"[direct_slot_pool]"){
auto p=make_pool();
REQUIRE(p.adopt_kernel_allocated(5));
REQUIRE(p.mark_closing(5));
p.poison(5,-9);
CHECK(p.slot_state(5)==DirectSlotState::poisoned);
CHECK(p.poisoned_count()==1);
auto r=p.adopt_kernel_allocated(5);
REQUIRE(!r);
CHECK(r.error()==DirectSlotError::bad_state);
}
TEST_CASE(
"dsp.release_bad_state: release_closed from non-closing → bad_state",
"[direct_slot_pool]"){
auto p=make_pool();
REQUIRE(p.adopt_kernel_allocated(4));
auto r=p.release_closed(4);
REQUIRE(!r);
CHECK(r.error()==DirectSlotError::bad_state);
CHECK(p.slot_state(4)==DirectSlotState::populated);

auto r2=p.release_closed(0);
REQUIRE(!r2);
CHECK(r2.error()==DirectSlotError::bad_state);
}
TEST_CASE(
"dsp.install_os_fd: listener slot excluded from free list",
"[direct_slot_pool]"){
auto p=make_pool();
REQUIRE(p.install_os_fd(0,3));
CHECK(p.slot_state(0)==DirectSlotState::populated);
CHECK(p.free_count()==kCap-1);
auto r=p.adopt_kernel_allocated(0);
REQUIRE(!r);
CHECK(r.error()==DirectSlotError::bad_state);
}
TEST_CASE(
"dsp.adopt_failure_consistency: failed adopt leaves pool count unchanged",
"[direct_slot_pool]"){
auto p=make_pool();
REQUIRE(p.adopt_kernel_allocated(7));
u32 const before=p.free_count();
auto r=p.adopt_kernel_allocated(7);
REQUIRE(!r);
CHECK(p.free_count()==before);
CHECK(p.poisoned_count()==0);
}
TEST_CASE(
"dsp.free_count_invariant: capacity - poisoned - in_use = free_count",
"[direct_slot_pool]"){
auto p=make_pool();
REQUIRE(p.install_os_fd(0,3));
REQUIRE(p.adopt_kernel_allocated(1));
REQUIRE(p.adopt_kernel_allocated(2));
REQUIRE(p.mark_closing(2));
p.poison(2,-9);
u32 const in_use=2;// slots 0 and 1: populated; slot 2: poisoned (not in free)
CHECK(p.free_count()==p.capacity()-in_use-p.poisoned_count());
}
