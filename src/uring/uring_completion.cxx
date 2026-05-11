module;
#include<cerrno>
#include<liburing.h>

export module conflux.uring.completion;

import std;
import conflux.types;

export using UserDataFn=Fn<u64(u32 slot,u32 gen)>;
export struct IoResult{
i32 res{};
u32 flags{};
};
export using CompletionFn=Fn<void(IoResult)>;
export class CompletionTable{
enum class SlotMode:u8{single,
multishot,
zc_send};
struct Slot{
u32 gen{0};
bool in_use{false};
SlotMode mode{SlotMode::single};
i32 zc_bytes{0};
bool zc_seen_send{false};
CompletionFn fn{};
};
V<Slot>slots_{};
V<u32>free_{};
public:
explicit CompletionTable(
SZ initial_capacity=64){
slots_.reserve(initial_capacity);
}
CompletionTable(CompletionTable const&)=delete;
CompletionTable&operator=(CompletionTable const&)=delete;
CompletionTable(CompletionTable&&)=delete;
CompletionTable&operator=(CompletionTable&&)=delete;
~CompletionTable(){}// NOLINT(modernize-use-equals-default) — GCC module bug
[[nodiscard]]P<u32,u32>reserve(
CompletionFn fn){
u32 slot=0;
if(!free_.empty()){
slot=free_.back();
free_.pop_back();
}else{
slot=static_cast<u32>(slots_.size());
slots_.emplace_back();
}
auto&s=slots_[slot];
s.in_use=true;
s.mode=SlotMode::single;
s.zc_bytes=0;
s.zc_seen_send=false;
s.fn=move(fn);
return{slot,s.gen};
}
[[nodiscard]]P<u32,u32>reserve_multishot(
CompletionFn fn){
auto[slot,gen]=reserve(move(fn));
slots_[slot].mode=SlotMode::multishot;
return{slot,gen};
}
[[nodiscard]]P<u32,u32>reserve_zc(
CompletionFn fn){
auto[slot,gen]=reserve(move(fn));
slots_[slot].mode=SlotMode::zc_send;
return{slot,gen};
}
void dispatch(
// NOLINT(bugprone-exception-escape) — callbacks are noexcept by contract
u32 slot,
u32 gen,
int res,
u32 flags)noexcept{
if(slot>=slots_.size())
return;
auto&s=slots_[slot];
if(!s.in_use||s.gen!=gen)
return;
if(s.mode==SlotMode::multishot&&res>=0&&(flags&IORING_CQE_F_MORE)!=0U){
if(s.fn)
s.fn(IoResult{.res=res,.flags=flags});
return;
}
if(s.mode==SlotMode::zc_send){
if((flags&IORING_CQE_F_NOTIF)==0U){
if(res>=0&&(flags&IORING_CQE_F_MORE)!=0U){
s.zc_bytes=res;
s.zc_seen_send=true;
return;
}
}else{
res=s.zc_seen_send?s.zc_bytes:-EIO;
}
}
auto fn=move(s.fn);
s.fn={};
s.in_use=false;
s.mode=SlotMode::single;
s.zc_bytes=0;
s.zc_seen_send=false;
++s.gen;
free_.push_back(slot);
if(fn)
fn(IoResult{.res=res,.flags=flags});
}
[[nodiscard]]bool has_pending_zc_notifications()const noexcept{
for(auto const&s:slots_)
if(s.in_use&&s.mode==SlotMode::zc_send&&s.zc_seen_send)return true;
return false;
}
// Returns false (and cancels nothing) if ZC notification slots are pending.
// Caller must drain the CQ until has_pending_zc_notifications() returns false, then retry.
[[nodiscard]]bool cancel_all()noexcept{// NOLINT(bugprone-exception-escape) — callbacks are noexcept by contract
if(has_pending_zc_notifications())
return false;
u32 const n=static_cast<u32>(slots_.size());// cache before callbacks can grow slots_
for(u32 slot=0;slot<n;++slot){
auto&s=slots_[slot];
if(!s.in_use)
continue;
auto fn=move(s.fn);
s.fn={};
s.in_use=false;
s.mode=SlotMode::single;
s.zc_bytes=0;
s.zc_seen_send=false;
++s.gen;
free_.push_back(slot);
if(fn)
fn(IoResult{.res=-ECANCELED,.flags=0});
}
return true;
}
[[nodiscard]]SZ pending()const noexcept{return slots_.size()-free_.size();}
};
