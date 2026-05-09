module;

export module conflux.work.carrier.streams;

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier;
export namespace conflux::work::carrier{
template<root::work_value T>
class DroppableSlotAwaiter;
template<root::work_value T>
class DroppableSlot{
struct DrainState{
root::TaskJoinHandle<T>handle;
Fn<void(root::Outcome<T>)>on_drop_fn;
};
UP<DrainState>state_;
bool consumed_=false;

friend class DroppableSlotAwaiter<T>;
void drain_()noexcept{
auto ctrl=state_->handle.control();
if(ctrl.ready()){
auto out=root::join(move(state_->handle));
if(state_->on_drop_fn)
state_->on_drop_fn(move(out));
return;
}
auto result=ctrl.try_set_on_ready([s=move(state_)]()mutable noexcept{
auto out=root::join(move(s->handle));
if(s->on_drop_fn)
s->on_drop_fn(move(out));
});
switch(result.status){
case root::ReadyRegistration::installed:return;
case root::ReadyRegistration::already_ready:
if(result.rejected_fn)
result.rejected_fn();
return;
case root::ReadyRegistration::already_installed:
#ifdef CONFLUX_WORK_CHECKED_BUILD
root::emit_carrier_diagnostic(
"DroppableSlot: single-consumer rule violated — drain hook could not install");
#endif
std::terminate();
case root::ReadyRegistration::empty:return;
}
}
public:
explicit DroppableSlot(
root::TaskJoinHandle<T>&&h)
:state_{make_unique<DrainState>(DrainState{move(h),{}})}{}
DroppableSlot(DroppableSlot&&)noexcept=default;
DroppableSlot&operator=(DroppableSlot&&)noexcept=default;
DroppableSlot(DroppableSlot const&)=delete;
DroppableSlot&operator=(DroppableSlot const&)=delete;
~DroppableSlot()noexcept{
if(consumed_||!state_)
return;
drain_();
}
template<class F>
requires std::invocable<F,root::Outcome<T>>&&std::is_nothrow_invocable_v<F,root::Outcome<T>>
void on_drop(
F&&fn)noexcept{
if(!consumed_&&state_)
state_->on_drop_fn=forward<F>(fn);
}
[[nodiscard]]bool ready()const noexcept{return state_&&state_->handle.control().ready();}
[[nodiscard]]Opt<root::Outcome<T>>try_get()&&{
if(!state_||!state_->handle.control().ready())
return nullopt;
auto out=root::join(move(state_->handle));
consumed_=true;
return out;
}
[[nodiscard]]Chain<T>wait()&&{
auto out=root::join(move(state_->handle));
consumed_=true;
return Chain<T>{move(out),CarrierKind::task};
}
[[nodiscard]]DroppableSlotAwaiter<T>operator co_await()&&noexcept;
};
template<root::work_value T>
class DroppableSlotAwaiter{
using DrainState=typename DroppableSlot<T>::DrainState;

UP<DrainState>state_;
root::BasicControl<root::ControlCategory::task>control_;
bool consumed_=false;
bool callback_installed_=false;
public:
explicit DroppableSlotAwaiter(
UP<DrainState>s)noexcept
:state_{move(s)},control_{state_->handle.control()}{}
~DroppableSlotAwaiter()noexcept{
if(consumed_||!state_)
return;
if(callback_installed_){
auto status=control_.clear_on_ready();
if(status==root::ClearOnReadyStatus::in_flight){
// Race: callback is executing concurrently. Cannot synchronize without
// blocking the dtor. Best-effort: route the join result to the abandon
// sink so the in-flight callback resolves into a safe discard path.
#ifdef CONFLUX_WORK_CHECKED_BUILD
root::emit_carrier_diagnostic_fmt(
"DroppableSlotAwaiter dtor raced commit's in-flight callback " "— best-effort abandon (awaiter=%p)",
static_cast<void*>(this));
#endif
auto _=root::try_abandon_to(move(state_->handle),root::drop_on_abandon{});
return;
}
}
auto result=control_.try_set_on_ready([s=move(state_)]()mutable noexcept{
auto out=root::join(move(s->handle));
if(s->on_drop_fn)
s->on_drop_fn(move(out));
});
switch(result.status){
case root::ReadyRegistration::installed:return;
case root::ReadyRegistration::already_ready:
if(result.rejected_fn)
result.rejected_fn();
return;
case root::ReadyRegistration::already_installed:
#ifdef CONFLUX_WORK_CHECKED_BUILD
root::emit_carrier_diagnostic("DroppableSlotAwaiter: single-consumer rule violated");
#endif
std::terminate();
case root::ReadyRegistration::empty:return;
}
}
DroppableSlotAwaiter(DroppableSlotAwaiter&&)noexcept=default;
DroppableSlotAwaiter&operator=(DroppableSlotAwaiter&&)noexcept=default;
DroppableSlotAwaiter(DroppableSlotAwaiter const&)=delete;
DroppableSlotAwaiter&operator=(DroppableSlotAwaiter const&)=delete;
[[nodiscard]]bool await_ready()const noexcept{return control_.ready();}
[[nodiscard]]bool await_suspend(
std::coroutine_handle<>h)noexcept{
auto result=control_.try_set_on_ready([h]()mutable noexcept{h.resume();});
if(result.status==root::ReadyRegistration::installed){
callback_installed_=true;
return true;
}
return false;
}
[[nodiscard]]Chain<T>await_resume(){
callback_installed_=false;
auto out=root::join(move(state_->handle));
consumed_=true;
return Chain<T>{move(out),CarrierKind::task};
}
};
template<root::work_value T>
DroppableSlotAwaiter<T>DroppableSlot<T>::operator co_await()&&noexcept{
consumed_=true;
return DroppableSlotAwaiter<T>{move(state_)};
}
template<root::work_value T>
requires(!same_as<T,void>)
class CoalescingSlot{
mutable mutex mu_;
Opt<T>slot_;
public:
CoalescingSlot()noexcept=default;
~CoalescingSlot()noexcept=default;

CoalescingSlot(CoalescingSlot&&)noexcept=delete;
CoalescingSlot&operator=(CoalescingSlot&&)noexcept=delete;
CoalescingSlot(CoalescingSlot const&)=delete;
CoalescingSlot&operator=(CoalescingSlot const&)=delete;
void commit(
T value)noexcept{
std::lock_guard const lock{mu_};
slot_=move(value);
}
[[nodiscard]]Opt<T>take()noexcept{
std::lock_guard const lock{mu_};
return exchange(slot_,nullopt);
}
[[nodiscard]]bool available()const noexcept{
std::lock_guard const lock{mu_};
return slot_.has_value();
}
};
}// namespace conflux::work::carrier
