module;
#include<cerrno>
#include<liburing.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#include<sys/socket.h>
#include<unistd.h>

export module conflux.socket_io.coro;

import std;
import conflux.types;
import conflux.uring.completion;
import conflux.uring.handle;
import conflux.socket_io;
import conflux.work;

namespace wroot=conflux::work::root;
using std::atomic;
using std::atomic_bool;
using std::current_exception;
using std::enable_shared_from_this;
using std::make_exception_ptr;
using std::make_shared;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_release;
using std::move;
using std::span;
using std::weak_ptr;
template<class T>using WP=weak_ptr<T>;
// ─── helpers ──────────────────────────────────────────────────────────────────

// Buffer lifetime contract for Task methods:
//   *_borrowed — caller storage is passed to the kernel. It must remain valid
//                until the operation reaches its CQE-backed terminal completion.
//                In normal awaited use, this means until co_await returns.
//                Do not destroy/drop/cancel a borrowed task unless the borrowed
//                storage outlives the underlying io_uring operation.
//   *_copy     — implementation copies input before submission; caller may drop
//                or mutate the source buffer after the call returns.
//   *_owned    — implementation takes ownership by move; no source lifetime
//                obligation remains after the call returns.

// ─── StopCause / RecvTimeoutState ────────────────────────────────────────────

enum class StopCause:u8{none,
user_cancel,
timeout};
struct RecvTimeoutState{
Atom<StopCause>stop_cause{StopCause::none};
void mark_stop(StopCause cause)noexcept{
StopCause expected=StopCause::none;
stop_cause.compare_exchange_strong(
expected,cause,
memory_order_acq_rel,
memory_order_acquire);
}
};
// ─── TcpStreamState ───────────────────────────────────────────────────────────

struct TcpStreamState{
SocketTaskRing*ring{};
OwnedSocketHandle handle{};
atomic_bool closing{false};
TcpStreamState(SocketTaskRing*r,OwnedSocketHandle h)noexcept
:ring{r},handle{move(h)}{}
};
// ─── TcpStream ───────────────────────────────────────────────────────────────

export class TcpStream{
SP<TcpStreamState>state_{};
// Returns the inner task directly so callers can cancel via task.cancel().
[[nodiscard]]wroot::Task<SZ>do_send(
u8 const*data,
SZ len,
SP<void>keeper){
auto&st=*state_;
if(!st.handle.valid()||st.closing.load(memory_order_relaxed)){
auto[t,s]=wroot::make_task_source<SZ>(wroot::SubmitOptions{.enable_cancellation=false});
auto ss=make_shared<wroot::TaskSource<SZ>>(move(s));
auto _=ss->try_set_exception(make_exception_ptr(IoError{EBADF,"tcp: stream closed"}));
return move(t);
}
auto[task,raw_src]=wroot::make_task_source<SZ>(wroot::SubmitOptions{.enable_cancellation=true});
auto shared_src=make_shared<wroot::TaskSource<SZ>>(move(raw_src));
SocketHandle const h=st.handle.get();
auto[slot,gen]=st.ring->completions().reserve([shared_src,keeper](IoResult r)mutable{
auto _=keeper;
try{
if(r.res==-ECANCELED){
auto _=shared_src->try_set_cancelled();
return;
}
if(r.res<0){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"tcp: send"}));
return;
}
auto _=shared_src->try_set_value(wroot::Success<SZ>{static_cast<SZ>(r.res)});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
u64 const ud=st.ring->encode(slot,gen);
if(!submit_send_borrowed(st.ring->raw(),h,data,len,ud)){
st.ring->completions().dispatch(slot,gen,-ENOSPC,0);
return move(task);
}
auto ring_ptr=st.ring;
auto weak_src=weak_ptr<wroot::TaskSource<SZ>>{shared_src};
auto _=shared_src->install_cancel_hook([ring_ptr,ud,weak_src=move(weak_src)](wroot::CancelReason)noexcept{
if(!ring_ptr->submit_on_owner([ud,weak_src](SocketTaskRing&ring){
auto[cs,cg]=ring.completions().reserve([](IoResult)noexcept{});
u64 const cud=ring.encode(cs,cg);
if(!submit_cancel_by_ud(ring.raw(),ud,cud)){
ring.completions().dispatch(cs,cg,-EBUSY,0);
if(auto src=weak_src.lock())auto _=src->try_set_cancelled();
return;
}
auto _=ring.raw().submit();
}))
if(auto src=weak_src.lock())auto _=src->try_set_cancelled();
});
return move(task);
}
public:
TcpStream()noexcept=default;
explicit TcpStream(SP<TcpStreamState>state)noexcept:state_{move(state)}{}
TcpStream(TcpStream const&)=delete;
TcpStream&operator=(TcpStream const&)=delete;
TcpStream(TcpStream&&)noexcept=default;
TcpStream&operator=(TcpStream&&)noexcept=default;
[[nodiscard]]bool valid()const noexcept{
return state_&&state_->handle.valid()&&!state_->closing.load(memory_order_relaxed);
}
[[nodiscard]]int raw_fd()const noexcept{
return state_?state_->handle.raw_fd():-1;
}
[[nodiscard]]wroot::Task<SZ>recv_borrowed(span<u8>dst){
auto&st=*state_;
if(!st.handle.valid()||st.closing.load(memory_order_relaxed)){
auto[t,s]=wroot::make_task_source<SZ>(wroot::SubmitOptions{.enable_cancellation=false});
auto ss=make_shared<wroot::TaskSource<SZ>>(move(s));
auto _=ss->try_set_exception(make_exception_ptr(IoError{EBADF,"tcp: stream closed"}));
return move(t);
}
auto[task,raw_src]=wroot::make_task_source<SZ>(wroot::SubmitOptions{.enable_cancellation=true});
auto shared_src=make_shared<wroot::TaskSource<SZ>>(move(raw_src));
SocketHandle const h=st.handle.get();
auto[slot,gen]=st.ring->completions().reserve([shared_src](IoResult r)mutable{
try{
if(r.res<0){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"tcp: recv"}));
return;
}
auto _=shared_src->try_set_value(wroot::Success<SZ>{static_cast<SZ>(r.res)});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
u64 const ud=st.ring->encode(slot,gen);
if(!submit_recv_borrowed(st.ring->raw(),h,dst.data(),dst.size(),ud)){
st.ring->completions().dispatch(slot,gen,-ENOSPC,0);
return move(task);
}
auto ring_ptr=st.ring;
auto weak_src2=weak_ptr<wroot::TaskSource<SZ>>{shared_src};
auto _=shared_src->install_cancel_hook([ring_ptr,ud,weak_src2=move(weak_src2)](wroot::CancelReason)noexcept{
if(!ring_ptr->submit_on_owner([ud,weak_src2](SocketTaskRing&ring){
auto[cs,cg]=ring.completions().reserve([](IoResult)noexcept{});
u64 const cud=ring.encode(cs,cg);
if(!submit_cancel_by_ud(ring.raw(),ud,cud)){
ring.completions().dispatch(cs,cg,-EBUSY,0);
if(auto src=weak_src2.lock())auto _=src->try_set_cancelled();
return;
}
auto _=ring.raw().submit();
}))
if(auto src=weak_src2.lock())auto _=src->try_set_cancelled();
});
return move(task);
}
[[nodiscard]]wroot::Task<SZ>recv_borrowed(span<u8>dst,chrono::milliseconds timeout);
[[deprecated("use recv_borrowed")]][[nodiscard]]wroot::Task<SZ>read_borrowed(span<u8>dst){return recv_borrowed(dst);}
[[nodiscard]]wroot::Task<V<u8>>recv_owned(SZ max_bytes);
[[nodiscard]]wroot::Task<SZ>write_borrowed(span<u8 const>src){
return do_send(src.data(),src.size(),{});
}
[[nodiscard]]wroot::Task<SZ>write_copy(span<u8 const>src){
auto holder=make_shared<V<u8>>(src.begin(),src.end());
u8*data=holder->data();
SZ const len=holder->size();
return do_send(data,len,holder);
}
[[nodiscard]]wroot::Task<SZ>write_owned(V<u8>data);
[[nodiscard]]wroot::Task<SZ>write_owned(S data);
[[nodiscard]]wroot::Task<void>write_all_borrowed(span<u8 const>src);
[[nodiscard]]wroot::Task<void>write_all_copy(span<u8 const>src);
[[nodiscard]]wroot::Task<void>write_all_owned(V<u8>data);
[[nodiscard]]wroot::Task<void>write_all_owned(S data);
[[nodiscard]]wroot::Task<void>shutdown(int how=SHUT_WR){
auto&st=*state_;
if(!st.handle.valid())
co_await[]()->wroot::Task<void>{throw IoError{EBADF,"tcp: stream closed"};}();
auto[task,raw_src]=wroot::make_task_source<void>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<void>>(move(raw_src));
SocketHandle const h=st.handle.get();
auto[slot,gen]=st.ring->completions().reserve([shared_src](IoResult r)mutable{
try{
if(r.res<0){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"tcp: shutdown"}));
return;
}
auto _=shared_src->try_set_value(wroot::Success<void>{});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
u64 const ud=st.ring->encode(slot,gen);
if(!submit_shutdown(st.ring->raw(),h,how,ud))
st.ring->completions().dispatch(slot,gen,-ENOSPC,0);
co_await move(task);
}
[[nodiscard]]wroot::Task<void>close(){
auto&st=*state_;
bool expected=false;
if(!st.closing.compare_exchange_strong(expected,true,memory_order_acq_rel))
co_return;
SocketHandle const h=st.handle.get();
auto[task,raw_src]=wroot::make_task_source<void>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<void>>(move(raw_src));
auto[slot,gen]=st.ring->completions().reserve([shared_src](IoResult r)mutable{
try{
if(r.res<0){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"tcp: close"}));
return;
}
auto _=shared_src->try_set_value(wroot::Success<void>{});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
u64 const ud=st.ring->encode(slot,gen);
if(!submit_close(st.ring->raw(),h,ud)){
if(h.is_os_fd()){
auto _=st.handle.release();
::close(h.as_fd());
st.ring->completions().dispatch(slot,gen,0,0);
}else{
// direct slot: can't close synchronously; keep handle alive, propagate error
st.ring->completions().dispatch(slot,gen,-ENOSPC,0);
}
co_await move(task);
co_return;
}
auto _=st.handle.release();// SQE submitted — kernel owns the fd now
co_await move(task);
}
};
[[nodiscard]]wroot::Task<void>TcpStream::write_all_borrowed(span<u8 const>src){
SZ sent=0;
while(sent<src.size()){
SZ const n=co_await write_borrowed({src.data()+sent,src.size()-sent});
if(n==0)throw IoError{ECONNRESET,"tcp: connection closed"};
sent+=n;
}
}
[[nodiscard]]wroot::Task<void>TcpStream::write_all_copy(span<u8 const>src){
auto holder=make_shared<V<u8>>(src.begin(),src.end());
SZ sent=0;
while(sent<holder->size()){
SZ const n=co_await do_send(holder->data()+sent,holder->size()-sent,holder);
if(n==0)throw IoError{ECONNRESET,"tcp: connection closed"};
sent+=n;
}
}
[[nodiscard]]wroot::Task<V<u8>>TcpStream::recv_owned(SZ max_bytes){
V<u8>out(max_bytes);
if(max_bytes==0)co_return out;
SZ const n=co_await recv_borrowed(span<u8>{out.data(),out.size()});
out.resize(n);
co_return out;
}
[[nodiscard]]wroot::Task<SZ>TcpStream::write_owned(V<u8>data){
auto holder=make_shared<V<u8>>(move(data));
u8 const*ptr=holder->data();
SZ const len=holder->size();
return do_send(ptr,len,holder);
}
[[nodiscard]]wroot::Task<SZ>TcpStream::write_owned(S data){
auto holder=make_shared<S>(move(data));
auto const*ptr=reinterpret_cast<u8 const*>(holder->data());
SZ const len=holder->size();
return do_send(ptr,len,holder);
}
[[nodiscard]]wroot::Task<void>TcpStream::write_all_owned(V<u8>data){
auto holder=make_shared<V<u8>>(move(data));
SZ sent=0;
while(sent<holder->size()){
SZ const n=co_await do_send(holder->data()+sent,holder->size()-sent,holder);
if(n==0)throw IoError{ECONNRESET,"tcp: connection closed"};
sent+=n;
}
}
[[nodiscard]]wroot::Task<void>TcpStream::write_all_owned(S data){
auto holder=make_shared<S>(move(data));
SZ sent=0;
while(sent<holder->size()){
auto const*ptr=reinterpret_cast<u8 const*>(holder->data()+sent);
SZ const n=co_await do_send(ptr,holder->size()-sent,holder);
if(n==0)throw IoError{ECONNRESET,"tcp: connection closed"};
sent+=n;
}
}
// ─── make_error_task ─────────────────────────────────────────────────────────

template<class T>
[[nodiscard]]wroot::Task<T>make_error_task(IoError e){
auto[t,s]=wroot::make_task_source<T>(wroot::SubmitOptions{.enable_cancellation=false});
auto ss=make_shared<wroot::TaskSource<T>>(move(s));
auto _=ss->try_set_exception(make_exception_ptr(move(e)));
return move(t);
}
[[nodiscard]]wroot::Task<SZ>TcpStream::recv_borrowed(span<u8>dst,chrono::milliseconds timeout){
if(timeout.count()==0)
return recv_borrowed(dst);
auto&st=*state_;
if(!st.handle.valid()||st.closing.load(memory_order_relaxed))
return make_error_task<SZ>(IoError{EBADF,"tcp: stream closed"});
auto[task,raw_src]=wroot::make_task_source<SZ>(wroot::SubmitOptions{.enable_cancellation=true});
auto shared_src=make_shared<wroot::TaskSource<SZ>>(move(raw_src));
SocketHandle const h=st.handle.get();
auto ts=make_shared<__kernel_timespec>();
auto const sec=chrono::duration_cast<chrono::seconds>(timeout);
ts->tv_sec=sec.count();
ts->tv_nsec=(timeout-sec).count()*1000000LL;
auto state=make_shared<RecvTimeoutState>();
auto[slot,gen]=st.ring->completions().reserve([shared_src,ts,state](IoResult r)mutable{
try{
if(r.res==-ECANCELED){
auto cause=state->stop_cause.load(memory_order_acquire);
if(cause==StopCause::user_cancel)
auto _=shared_src->try_set_cancelled();
else
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{ETIMEDOUT,"tcp: recv timed out"}));
return;
}
if(r.res<0){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"tcp: recv"}));
return;
}
auto _=shared_src->try_set_value(wroot::Success<SZ>{static_cast<SZ>(r.res)});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
u64 const recv_ud=st.ring->encode(slot,gen);
auto[tslot,tgen]=st.ring->completions().reserve([ts,state](IoResult r)mutable{
if(r.res==-ETIME)
state->mark_stop(StopCause::timeout);
auto _=ts;
});
u64 const timeout_ud=st.ring->encode(tslot,tgen);
if(!submit_recv_timeout_borrowed(st.ring->raw(),h,dst.data(),dst.size(),ts.get(),recv_ud,timeout_ud)){
st.ring->completions().dispatch(slot,gen,-ENOSPC,0);
st.ring->completions().dispatch(tslot,tgen,-EBUSY,0);
return move(task);
}
auto ring_ptr=st.ring;
auto weak_src=weak_ptr<wroot::TaskSource<SZ>>{shared_src};
auto _=shared_src->install_cancel_hook([ring_ptr,recv_ud,weak_src,state](wroot::CancelReason)noexcept{
state->mark_stop(StopCause::user_cancel);
if(!ring_ptr->submit_on_owner([recv_ud,weak_src](SocketTaskRing&ring_ref)noexcept{
auto[cs,cg]=ring_ref.completions().reserve([](IoResult)noexcept{});
u64 const cancel_ud=ring_ref.encode(cs,cg);
if(!submit_cancel_by_ud(ring_ref.raw(),recv_ud,cancel_ud)){
ring_ref.completions().dispatch(cs,cg,-EBUSY,0);
if(auto src=weak_src.lock())auto _=src->try_set_cancelled();
return;
}
auto _=ring_ref.raw().submit();
}))
if(auto src=weak_src.lock())auto _=src->try_set_cancelled();
});
return move(task);
}
// ─── ConnectOp ───────────────────────────────────────────────────────────────

struct ConnectOp:enable_shared_from_this<ConnectOp>{
enum class Stage:u8{socket_pending,
connect_pending,
done};

SocketTaskRing*ring{};
SP<wroot::TaskSource<TcpStream>>src{};
SP<TcpStreamState>stream_state{};
sockaddr_storage addr{};
socklen_t addr_len{};
ConnectOptions opts{};
__kernel_timespec timeout_ts{};

Stage stage{Stage::socket_pending};
u64 socket_ud{};
u64 connect_ud{};

atomic_bool cancel_requested{false};
atomic<StopCause>stop_cause{StopCause::none};
atomic_bool finalized{false};
[[nodiscard]]bool try_finalize()noexcept{
return!finalized.exchange(true,memory_order_acq_rel);
}
void complete_exception(IoError e)noexcept{
if(!try_finalize())return;
auto _=src->try_set_exception(make_exception_ptr(move(e)));
}
void complete_cancelled()noexcept{
if(!try_finalize())return;
auto _=src->try_set_cancelled();
}
void complete_value(TcpStream v)noexcept{
if(!try_finalize())return;
auto _=src->try_set_value(wroot::Success<TcpStream>{move(v)});
}
void submit_cancel_ud_on_owner(SocketTaskRing&r,u64 target_ud)noexcept{
auto self=shared_from_this();
auto[cs,cg]=r.completions().reserve([self](IoResult)noexcept{});
if(!submit_cancel_by_ud(r.raw(),target_ud,r.encode(cs,cg))){
r.completions().dispatch(cs,cg,-EBUSY,0);
complete_cancelled();
return;
}
auto _=r.raw().submit();
}
void submit_cancel_fd_on_owner(SocketTaskRing&r,RingFd fd)noexcept{
auto self=shared_from_this();
auto[cs,cg]=r.completions().reserve([self](IoResult)noexcept{});
if(!submit_cancel_fd(r.raw(),fd,r.encode(cs,cg))){
r.completions().dispatch(cs,cg,-EBUSY,0);
complete_cancelled();
return;
}
auto _=r.raw().submit();
}
void cancel_on_owner(SocketTaskRing&r)noexcept{
if(finalized.load(memory_order_acquire))return;
switch(stage){
case Stage::socket_pending:
submit_cancel_ud_on_owner(r,socket_ud);
break;
case Stage::connect_pending:
submit_cancel_fd_on_owner(r,stream_state->handle.get());
break;
case Stage::done:
break;
}
}
void request_cancel(wroot::CancelReason)noexcept{
stop_cause.store(StopCause::user_cancel,memory_order_release);
cancel_requested.store(true,memory_order_release);
auto self=shared_from_this();
if(!ring->submit_on_owner([self](SocketTaskRing&r){
self->cancel_on_owner(r);
}))
complete_cancelled();// may run on cancelling thread; relies on TaskSource setter MT-safety
}
void on_timeout_cqe(IoResult r)noexcept{
if(r.res!=-ETIME)return;
StopCause expected=StopCause::none;
stop_cause.compare_exchange_strong(
expected,StopCause::timeout,
memory_order_acq_rel,memory_order_acquire);
}
void on_connect_cqe(IoResult r)noexcept{
stage=Stage::done;
if(r.res>=0){
complete_value(TcpStream{move(stream_state)});
return;
}
stream_state.reset();
if(r.res==-ECANCELED)
if(cancel_requested.load(memory_order_acquire)||
stop_cause.load(memory_order_acquire)==StopCause::user_cancel)
complete_cancelled();
else
complete_exception(IoError{ETIMEDOUT,"tcp_connect: timeout"});
else
complete_exception(IoError{-r.res,"tcp_connect: connect"});
}
void on_socket_cqe(IoResult r)noexcept{
if(r.res<0){
if(cancel_requested.load(memory_order_acquire))
complete_cancelled();
else
complete_exception(IoError{-r.res,"tcp_connect: socket"});
return;
}
auto owned=OwnedSocketHandle::from_fd(r.res);
if(cancel_requested.load(memory_order_acquire)){
complete_cancelled();
return;
}
SocketHandle const h=owned.get();
if(opts.tcp_nodelay&&h.is_os_fd()){
int const one=1;
if(::setsockopt(h.as_fd(),IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))<0){
complete_exception(IoError{errno,"tcp_connect: TCP_NODELAY"});
return;
}
}
if(opts.tcp_quickack&&h.is_os_fd()){
int const one=1;
if(::setsockopt(h.as_fd(),IPPROTO_TCP,TCP_QUICKACK,&one,sizeof(one))<0){
complete_exception(IoError{errno,"tcp_connect: TCP_QUICKACK"});
return;
}
}
bool const use_timeout=opts.timeout>chrono::milliseconds{0};
u32 const sqe_needed=use_timeout?2u:1u;
if(!ring->raw().reserve_sqe_slots(sqe_needed)){
complete_exception(IoError{ENOSPC,"tcp_connect: SQ full"});
return;
}
stream_state=make_shared<TcpStreamState>(ring,move(owned));
auto self=shared_from_this();
auto[cslot,cgen]=ring->completions().reserve(
[self](IoResult cr)noexcept{self->on_connect_cqe(cr);});
connect_ud=ring->encode(cslot,cgen);
u32 tslot{},tgen{};
if(use_timeout){
auto[ts,tg]=ring->completions().reserve(
[self](IoResult tr)noexcept{self->on_timeout_cqe(tr);});
tslot=ts;
tgen=tg;
}
stage=Stage::connect_pending;
sockaddr const*saddr=reinterpret_cast<sockaddr const*>(&addr);
if(!submit_connect_borrowed(ring->raw(),h,saddr,addr_len,connect_ud,use_timeout)){
if(use_timeout)ring->completions().dispatch(tslot,tgen,-EBUSY,0);
ring->completions().dispatch(cslot,cgen,-ENOSPC,0);
return;
}
if(use_timeout){
auto const sec=chrono::duration_cast<chrono::seconds>(opts.timeout);
timeout_ts.tv_sec=sec.count();
timeout_ts.tv_nsec=(opts.timeout-sec).count()*1000000LL;
if(!submit_link_timeout_borrowed(ring->raw(),&timeout_ts,ring->encode(tslot,tgen))){
ring->completions().dispatch(tslot,tgen,-EBUSY,0);
complete_exception(IoError{EBUSY,"tcp_connect: link timeout SQE unavailable"});
return;
}
}
}
};
// ─── tcp_connect ─────────────────────────────────────────────────────────────

export[[nodiscard]]wroot::Task<TcpStream>tcp_connect(
SocketTaskRing&ring,
int family,
sockaddr_storage addr,
socklen_t len,
ConnectOptions opts={}){
if(opts.timeout.count()<0)
return make_error_task<TcpStream>(IoError{EINVAL,"tcp_connect: negative timeout"});
if(ring.opts().fd_mode==SocketFdMode::direct_required)
return make_error_task<TcpStream>(IoError{ENOTSUP,"tcp_connect: direct fd not yet supported"});

auto[task,raw_src]=wroot::make_task_source<TcpStream>(
wroot::SubmitOptions{.enable_cancellation=true});
auto src=make_shared<wroot::TaskSource<TcpStream>>(move(raw_src));

auto op=make_shared<ConnectOp>();
op->ring=&ring;
op->src=src;
op->addr=addr;
op->addr_len=len;
op->opts=opts;

auto[slot,gen]=ring.completions().reserve(
[op](IoResult r)noexcept{op->on_socket_cqe(r);});
op->socket_ud=ring.encode(slot,gen);

if(!submit_socket(ring.raw(),family,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP,op->socket_ud))
ring.completions().dispatch(slot,gen,-ENOSPC,0);

auto _=src->install_cancel_hook(
[weak_op=WP<ConnectOp>{op}](wroot::CancelReason cr)noexcept{
if(auto sop=weak_op.lock())
sop->request_cancel(cr);
});

return move(task);
}
// ─── UdpRecvResult ───────────────────────────────────────────────────────────

export struct UdpRecvResult{
SZ bytes{0};
sockaddr_storage from{};
socklen_t from_len{0};
};
// ─── UdpSocket ───────────────────────────────────────────────────────────────

export class UdpSocket{
SocketTaskRing*ring_{};
OwnedSocketHandle handle_{};
struct MsgHolder{
msghdr msg{};
iovec iov{};
sockaddr_storage from{};
};
public:
UdpSocket()noexcept=default;
explicit UdpSocket(SocketTaskRing&ring,OwnedSocketHandle fh)noexcept
:ring_{&ring},handle_{move(fh)}{}
UdpSocket(UdpSocket const&)=delete;
UdpSocket&operator=(UdpSocket const&)=delete;
UdpSocket(UdpSocket&&)noexcept=default;
UdpSocket&operator=(UdpSocket&&)noexcept=default;
[[nodiscard]]bool valid()const noexcept{return ring_!=nullptr&&handle_.valid();}
[[nodiscard]]int raw_fd()const noexcept{return handle_.raw_fd();}
[[nodiscard]]static UdpSocket ephemeral(SocketTaskRing&ring,int family){
int const fd=::socket(family,SOCK_DGRAM|SOCK_CLOEXEC,IPPROTO_UDP);
if(fd<0)throw IoError{errno,"udp: socket"};
if(family==AF_INET){
sockaddr_in sa{};
sa.sin_family=AF_INET;
sa.sin_addr.s_addr=htonl(INADDR_ANY);
if(::bind(fd,reinterpret_cast<sockaddr*>(&sa),sizeof(sa))<0){
int const e=errno;
::close(fd);
throw IoError{e,"udp: bind"};
}
}else if(family==AF_INET6){
sockaddr_in6 sa{};
sa.sin6_family=AF_INET6;
sa.sin6_addr=in6addr_any;
if(::bind(fd,reinterpret_cast<sockaddr*>(&sa),sizeof(sa))<0){
int const e=errno;
::close(fd);
throw IoError{e,"udp: bind"};
}
}else{
::close(fd);
throw IoError{EAFNOSUPPORT,"udp: unsupported family"};
}
return UdpSocket{ring,OwnedSocketHandle::from_fd(fd)};
}
// payload (span<u8 const>) is NOT copied — caller must keep it valid until co_await returns;
// if abandoned/detached/cancelled, storage must outlive the underlying io_uring op. Use send_to_copy otherwise.
[[nodiscard]]wroot::Task<SZ>send_to_borrowed(
span<u8 const>data,
sockaddr_storage addr,// by value — copied into holder
socklen_t addr_len);

[[nodiscard]]wroot::Task<SZ>send_to_copy(
span<u8 const>data,
sockaddr_storage addr,
socklen_t addr_len);

[[nodiscard]]wroot::Task<UdpRecvResult>recv_from(span<u8>buf);
[[nodiscard]]wroot::Task<UdpRecvResult>recv_from(span<u8>buf,chrono::milliseconds timeout);
};
// NOTE: msghdr, iovec, and address are copied into an internal keeper.
// The payload data (span<u8 const>) is NOT copied.
// In normal awaited use, keep it valid until co_await returns.
// If the task is abandoned/detached/cancelled, the general *_borrowed rule applies:
// storage must outlive the underlying io_uring operation.
// Use send_to_copy if you cannot guarantee the payload lifetime.
[[nodiscard]]wroot::Task<SZ>UdpSocket::send_to_borrowed(
span<u8 const>data,
sockaddr_storage addr,
socklen_t addr_len){
auto[task,raw_src]=wroot::make_task_source<SZ>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<SZ>>(move(raw_src));
SocketHandle const h=handle_.get();
struct SendHolder{
msghdr msg{};
iovec iov{};
sockaddr_storage to{};
};
auto holder=make_shared<SendHolder>();
holder->to=addr;
holder->iov.iov_base=const_cast<void*>(static_cast<void const*>(data.data()));
holder->iov.iov_len=data.size();
holder->msg.msg_name=&holder->to;
holder->msg.msg_namelen=addr_len;
holder->msg.msg_iov=&holder->iov;
holder->msg.msg_iovlen=1;
auto[slot,gen]=ring_->completions().reserve([shared_src,holder](IoResult r)mutable{
auto _=holder;
try{
if(r.res<0){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"udp: sendto"}));
return;
}
auto _=shared_src->try_set_value(wroot::Success<SZ>{static_cast<SZ>(r.res)});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
u64 const ud=ring_->encode(slot,gen);
if(!submit_sendmsg_borrowed(ring_->raw(),h,&holder->msg,ud))
ring_->completions().dispatch(slot,gen,-ENOSPC,0);
co_return co_await move(task);
}
[[nodiscard]]wroot::Task<SZ>UdpSocket::send_to_copy(
span<u8 const>data,
sockaddr_storage addr,
socklen_t addr_len){
auto[task,raw_src]=wroot::make_task_source<SZ>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<SZ>>(move(raw_src));
SocketHandle const h=handle_.get();
struct SendCopyHolder{
V<u8>payload;
msghdr msg{};
iovec iov{};
sockaddr_storage to{};
};
auto holder=make_shared<SendCopyHolder>();
holder->payload.assign(data.begin(),data.end());
holder->to=addr;
holder->iov.iov_base=holder->payload.empty()?nullptr:holder->payload.data();
holder->iov.iov_len=holder->payload.size();
holder->msg.msg_name=&holder->to;
holder->msg.msg_namelen=addr_len;
holder->msg.msg_iov=&holder->iov;
holder->msg.msg_iovlen=1;
auto[slot,gen]=ring_->completions().reserve([shared_src,holder](IoResult r)mutable{
auto _=holder;
try{
if(r.res<0){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"udp: sendto"}));
return;
}
auto _=shared_src->try_set_value(wroot::Success<SZ>{static_cast<SZ>(r.res)});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
u64 const ud=ring_->encode(slot,gen);
if(!submit_sendmsg_borrowed(ring_->raw(),h,&holder->msg,ud))
ring_->completions().dispatch(slot,gen,-ENOSPC,0);
co_return co_await move(task);
}
[[nodiscard]]wroot::Task<UdpRecvResult>UdpSocket::recv_from(span<u8>buf){
auto[task,raw_src]=wroot::make_task_source<UdpRecvResult>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<UdpRecvResult>>(move(raw_src));
SocketHandle const h=handle_.get();
auto holder=make_shared<MsgHolder>();
holder->iov.iov_base=buf.data();
holder->iov.iov_len=buf.size();
holder->msg.msg_name=&holder->from;
holder->msg.msg_namelen=sizeof(holder->from);
holder->msg.msg_iov=&holder->iov;
holder->msg.msg_iovlen=1;
auto[slot,gen]=ring_->completions().reserve([shared_src,holder](IoResult r)mutable{
try{
if(r.res<0){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"udp: recvfrom"}));
return;
}
UdpRecvResult result;
result.bytes=static_cast<SZ>(r.res);
result.from=holder->from;
result.from_len=holder->msg.msg_namelen;
auto _=shared_src->try_set_value(wroot::Success<UdpRecvResult>{move(result)});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
u64 const ud=ring_->encode(slot,gen);
if(!submit_recvmsg_borrowed(ring_->raw(),h,&holder->msg,ud))
ring_->completions().dispatch(slot,gen,-ENOSPC,0);
co_return co_await move(task);
}
[[nodiscard]]wroot::Task<UdpRecvResult>UdpSocket::recv_from(span<u8>buf,chrono::milliseconds timeout){
if(timeout.count()<0)
return make_error_task<UdpRecvResult>(IoError{EINVAL,"udp: negative timeout"});
auto[task,raw_src]=wroot::make_task_source<UdpRecvResult>(wroot::SubmitOptions{.enable_cancellation=true});
auto shared_src=make_shared<wroot::TaskSource<UdpRecvResult>>(move(raw_src));
SocketHandle const h=handle_.get();
auto holder=make_shared<MsgHolder>();
holder->iov.iov_base=buf.data();
holder->iov.iov_len=buf.size();
holder->msg.msg_name=&holder->from;
holder->msg.msg_namelen=sizeof(holder->from);
holder->msg.msg_iov=&holder->iov;
holder->msg.msg_iovlen=1;
auto ts=make_shared<__kernel_timespec>();
auto const sec=chrono::duration_cast<chrono::seconds>(timeout);
ts->tv_sec=sec.count();
ts->tv_nsec=(timeout-sec).count()*1000000LL;
auto state=make_shared<RecvTimeoutState>();
auto[slot,gen]=ring_->completions().reserve([shared_src,holder,state](IoResult r)mutable{
try{
if(r.res==-ECANCELED){
auto cause=state->stop_cause.load(memory_order_acquire);
if(cause==StopCause::user_cancel)
auto _=shared_src->try_set_cancelled();
else
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{ETIMEDOUT,"udp: recv timed out"}));
return;
}
if(r.res<0){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"udp: recvfrom"}));
return;
}
UdpRecvResult result;
result.bytes=static_cast<SZ>(r.res);
result.from=holder->from;
result.from_len=holder->msg.msg_namelen;
auto _=shared_src->try_set_value(wroot::Success<UdpRecvResult>{move(result)});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
u64 const recv_ud=ring_->encode(slot,gen);
auto[tslot,tgen]=ring_->completions().reserve([ts,state](IoResult r)mutable{
if(r.res==-ETIME)
state->mark_stop(StopCause::timeout);
auto _=ts;
});
u64 const timeout_ud=ring_->encode(tslot,tgen);
if(!submit_recvmsg_timeout_borrowed(ring_->raw(),h,&holder->msg,ts.get(),recv_ud,timeout_ud)){
ring_->completions().dispatch(slot,gen,-ENOSPC,0);
ring_->completions().dispatch(tslot,tgen,-EBUSY,0);
return move(task);
}
auto ring_ptr=ring_;
auto weak_src=weak_ptr<wroot::TaskSource<UdpRecvResult>>{shared_src};
auto _=shared_src->install_cancel_hook([ring_ptr,recv_ud,weak_src,state](wroot::CancelReason)noexcept{
state->mark_stop(StopCause::user_cancel);
if(!ring_ptr->submit_on_owner([recv_ud,weak_src](SocketTaskRing&ring_ref)noexcept{
auto[cs,cg]=ring_ref.completions().reserve([](IoResult)noexcept{});
u64 const cancel_ud=ring_ref.encode(cs,cg);
if(!submit_cancel_by_ud(ring_ref.raw(),recv_ud,cancel_ud)){
ring_ref.completions().dispatch(cs,cg,-EBUSY,0);
if(auto src=weak_src.lock())auto _=src->try_set_cancelled();
return;
}
auto _=ring_ref.raw().submit();
}))
if(auto src=weak_src.lock())auto _=src->try_set_cancelled();
});
return move(task);
}
