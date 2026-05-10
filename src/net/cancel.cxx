export module conflux.net.cancel;
import std;
import conflux.types;
import conflux.work;
namespace wroot=conflux::work::root;
export struct ActiveTaskCancelRelay{
mutex m_;
Opt<wroot::TaskControl>active_;
Atom<bool>cancelled_{false};
void set_active(wroot::TaskControl c){
Opt<wroot::TaskControl>to_cancel;
{
lock_guard lk{m_};
active_.emplace(move(c));
if(cancelled_.load(memory_order_acquire))
to_cancel=active_;
}
if(to_cancel)
auto _=to_cancel->request_cancel();
}
void clear_active()noexcept{
try{
lock_guard lk{m_};
active_.reset();
}catch(...){}
}
void cancel()noexcept{
Opt<wroot::TaskControl>to_cancel;
{
lock_guard lk{m_};
cancelled_.store(true,memory_order_release);
to_cancel=active_;
}
if(to_cancel)
auto _=to_cancel->request_cancel();
}
[[nodiscard]]bool is_cancelled()const noexcept{
return cancelled_.load(memory_order_acquire);
}
void throw_if_cancelled()const{
if(is_cancelled())
throw wroot::CancelledError{wroot::CancelReason::requested};
}
[[nodiscard]]wroot::Task<SZ>await_child(wroot::Task<SZ>child){
set_active(child.control());
try{
auto out=co_await move(child);
clear_active();
throw_if_cancelled();
co_return out;
}catch(...){
clear_active();
throw;
}
}
[[nodiscard]]wroot::Task<void>await_child(wroot::Task<void>child){
set_active(child.control());
try{
co_await move(child);
clear_active();
throw_if_cancelled();
co_return;
}catch(...){
clear_active();
throw;
}
}
};
