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
using std::atomic_bool;
using std::current_exception;
using std::make_exception_ptr;
using std::make_shared;
using std::move;
using std::span;
// ─── helpers ──────────────────────────────────────────────────────────────────

namespace{
[[nodiscard]]inline SocketHandle to_socket_handle(OwnedSocketHandle const&h)noexcept{
if(h.is_direct())
return SocketHandle::from_direct(static_cast<u32>(h.direct_slot()));
return SocketHandle::from_os(h.raw_fd());
}
}// namespace
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
[[nodiscard]]wroot::Task<SZ>do_send(
u8 const*data,
SZ len,
SP<void>keeper){
auto&st=*state_;
if(!st.handle.valid()||st.closing.load(memory_order_relaxed))
co_await[]()->wroot::Task<SZ>{
throw IoError{EBADF,"tcp: stream closed"};
co_return 0;
}();
auto[task,raw_src]=wroot::make_task_source<SZ>(wroot::SubmitOptions{.enable_cancellation=true});
auto shared_src=make_shared<wroot::TaskSource<SZ>>(move(raw_src));
SocketHandle const h=to_socket_handle(st.handle);
auto[slot,gen]=st.ring->completions().reserve([shared_src,keeper](IoResult r)mutable{
auto _=keeper;
try{
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
co_return co_await move(task);
}
auto ring_ptr=st.ring;
auto _=shared_src->install_cancel_hook([ring_ptr,ud](wroot::CancelReason)noexcept{
auto _=ring_ptr->submit_on_owner([ud](SocketTaskRing&ring){
auto[cs,cg]=ring.completions().reserve([](IoResult)noexcept{});
u64 const cud=ring.encode(cs,cg);
if(!submit_cancel_by_ud(ring.raw(),ud,cud)){
ring.completions().dispatch(cs,cg,-EBUSY,0);
return;
}
auto _=ring.raw().submit();
});
});
co_return co_await move(task);
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
[[nodiscard]]wroot::Task<SZ>read_borrowed(span<u8>dst){
auto&st=*state_;
if(!st.handle.valid()||st.closing.load(memory_order_relaxed))
co_await[]()->wroot::Task<SZ>{
throw IoError{EBADF,"tcp: stream closed"};
co_return 0;
}();
auto[task,raw_src]=wroot::make_task_source<SZ>(wroot::SubmitOptions{.enable_cancellation=true});
auto shared_src=make_shared<wroot::TaskSource<SZ>>(move(raw_src));
SocketHandle const h=to_socket_handle(st.handle);
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
co_return co_await move(task);
}
auto ring_ptr=st.ring;
{
auto _=shared_src->install_cancel_hook([ring_ptr,ud](wroot::CancelReason)noexcept{
auto _=ring_ptr->submit_on_owner([ud](SocketTaskRing&ring){
auto[cs,cg]=ring.completions().reserve([](IoResult)noexcept{});
u64 const cud=ring.encode(cs,cg);
if(!submit_cancel_by_ud(ring.raw(),ud,cud)){
ring.completions().dispatch(cs,cg,-EBUSY,0);
return;
}
auto _=ring.raw().submit();
});
});
}
co_return co_await move(task);
}
[[nodiscard]]wroot::Task<SZ>write_borrowed(span<u8 const>src){
return do_send(src.data(),src.size(),{});
}
[[nodiscard]]wroot::Task<SZ>write_copy(span<u8 const>src){
auto holder=make_shared<V<u8>>(src.begin(),src.end());
u8*data=holder->data();
SZ const len=holder->size();
return do_send(data,len,holder);
}
[[nodiscard]]wroot::Task<void>write_all_borrowed(span<u8 const>src);
[[nodiscard]]wroot::Task<void>write_all_copy(span<u8 const>src);
[[nodiscard]]wroot::Task<void>shutdown(int how=SHUT_WR){
auto&st=*state_;
if(!st.handle.valid())
co_await[]()->wroot::Task<void>{throw IoError{EBADF,"tcp: stream closed"};}();
auto[task,raw_src]=wroot::make_task_source<void>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<void>>(move(raw_src));
SocketHandle const h=to_socket_handle(st.handle);
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
SocketHandle const h=to_socket_handle(st.handle);
auto _=st.handle.release();// disown; close via SQE or sync below
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
if(h.is_os_fd())
::close(h.as_fd());// SQ full: sync close fallback for os_fd
int const close_res=h.is_os_fd()?0:-ENOSPC;// direct slot cannot be closed sync
st.ring->completions().dispatch(slot,gen,close_res,0);
}
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
SZ const n=co_await write_copy({holder->data()+sent,holder->size()-sent});
if(n==0)throw IoError{ECONNRESET,"tcp: connection closed"};
sent+=n;
}
}
// ─── tcp_connect ─────────────────────────────────────────────────────────────
// Two-stage: (1) create socket, (2) connect + linked timeout.
// addr is taken by value — lives in the coroutine frame until connect CQE.

export[[nodiscard]]wroot::Task<TcpStream>tcp_connect(
SocketTaskRing&ring,
int family,
sockaddr_storage addr,
socklen_t len,
ConnectOptions opts={}){
// Stage 1: create socket
{
auto[task,raw_src]=wroot::make_task_source<int>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<int>>(move(raw_src));
auto[slot,gen]=ring.completions().reserve([shared_src](IoResult r)mutable{
try{
if(r.res<0){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"tcp_connect: socket"}));
return;
}
auto _=shared_src->try_set_value(wroot::Success<int>{r.res});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
u64 const ud=ring.encode(slot,gen);
if(!submit_socket(ring.raw(),family,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP,ud))
ring.completions().dispatch(slot,gen,-ENOSPC,0);
int const raw_fd=co_await move(task);
// Stage 2: apply socket opts + connect
auto owned=OwnedSocketHandle::from_fd(raw_fd);
SocketHandle const h=to_socket_handle(owned);
if(opts.tcp_nodelay&&h.is_os_fd()){
int const one=1;
::setsockopt(h.as_fd(),IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
}
if(opts.tcp_quickack&&h.is_os_fd()){
int const one=1;
::setsockopt(h.as_fd(),IPPROTO_TCP,TCP_QUICKACK,&one,sizeof(one));
}
bool const use_timeout=opts.timeout>chrono::milliseconds{0};
if(use_timeout&&ring.raw().sq_space_left()<2){
auto[bt,brs]=wroot::make_task_source<TcpStream>(wroot::SubmitOptions{.enable_cancellation=false});
auto bs=make_shared<wroot::TaskSource<TcpStream>>(move(brs));
auto _=bs->try_set_exception(make_exception_ptr(IoError{ENOSPC,"tcp_connect: SQ full"}));
co_return co_await move(bt);
}
auto[ctask,craw_src]=wroot::make_task_source<TcpStream>(wroot::SubmitOptions{.enable_cancellation=true});
auto cshared_src=make_shared<wroot::TaskSource<TcpStream>>(move(craw_src));
auto state=make_shared<TcpStreamState>(&ring,move(owned));
auto[cslot,cgen]=ring.completions().reserve([cshared_src,state](IoResult r)mutable{
try{
if(r.res<0){
// handle is released back to TcpStream if error — let caller decide
if(r.res==-ECANCELED)
auto _=cshared_src->try_set_exception(make_exception_ptr(IoError{ETIMEDOUT,"tcp_connect: timeout"}));
else
auto _=cshared_src->try_set_exception(make_exception_ptr(IoError{-r.res,"tcp_connect: connect"}));
return;
}
auto _=cshared_src->try_set_value(wroot::Success<TcpStream>{TcpStream{state}});
}catch(...){auto _=cshared_src->try_set_exception(current_exception());}
});
u64 const connect_ud=ring.encode(cslot,cgen);
sockaddr const*saddr=reinterpret_cast<sockaddr const*>(&addr);
if(!submit_connect_borrowed(ring.raw(),h,saddr,len,connect_ud,use_timeout)){
ring.completions().dispatch(cslot,cgen,-ENOSPC,0);
co_return co_await move(ctask);
}
if(use_timeout){
auto ts_holder=make_shared<__kernel_timespec>();
auto const sec=chrono::duration_cast<chrono::seconds>(opts.timeout);
ts_holder->tv_sec=sec.count();
ts_holder->tv_nsec=(opts.timeout-sec).count()*1000000LL;
auto[tslot,tgen]=ring.completions().reserve([ts_holder](IoResult)mutable{auto _=ts_holder;});
u64 const timeout_ud=ring.encode(tslot,tgen);
if(!submit_link_timeout_borrowed(ring.raw(),ts_holder.get(),timeout_ud))
ring.completions().dispatch(tslot,tgen,-EBUSY,0);
}
// Cancel hook: cancel the connect by fd
auto ring_ptr=&ring;
{
auto _=cshared_src->install_cancel_hook([ring_ptr,h](wroot::CancelReason)noexcept{
auto _=ring_ptr->submit_on_owner([h](SocketTaskRing&r){
auto[cs,cg]=r.completions().reserve([](IoResult)noexcept{});
u64 const cud=r.encode(cs,cg);
if(!submit_cancel_fd(r.raw(),h,cud)){
r.completions().dispatch(cs,cg,-EBUSY,0);
return;
}
auto _=r.raw().submit();
});
});
}
co_return co_await move(ctask);
}
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
[[nodiscard]]wroot::Task<SZ>send_to_borrowed(
span<u8 const>data,
sockaddr_storage addr,// by value — copied into holder
socklen_t addr_len);

[[nodiscard]]wroot::Task<UdpRecvResult>recv_from(span<u8>buf);
[[nodiscard]]wroot::Task<UdpRecvResult>recv_from(span<u8>buf,chrono::milliseconds timeout);
};
[[nodiscard]]wroot::Task<SZ>UdpSocket::send_to_borrowed(
span<u8 const>data,
sockaddr_storage addr,
socklen_t addr_len){
auto[task,raw_src]=wroot::make_task_source<SZ>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<SZ>>(move(raw_src));
SocketHandle const h=to_socket_handle(handle_);
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
[[nodiscard]]wroot::Task<UdpRecvResult>UdpSocket::recv_from(span<u8>buf){
auto[task,raw_src]=wroot::make_task_source<UdpRecvResult>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<UdpRecvResult>>(move(raw_src));
SocketHandle const h=to_socket_handle(handle_);
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
auto[task,raw_src]=wroot::make_task_source<UdpRecvResult>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<UdpRecvResult>>(move(raw_src));
if(ring_->raw().sq_space_left()<2){
auto _=shared_src->try_set_exception(make_exception_ptr(IoError{ENOSPC,"udp: SQ full"}));
co_return co_await move(task);
}
SocketHandle const h=to_socket_handle(handle_);
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
auto[slot,gen]=ring_->completions().reserve([shared_src,holder](IoResult r)mutable{
try{
if(r.res==-ECANCELED){
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
auto[tslot,tgen]=ring_->completions().reserve([ts](IoResult)mutable{auto _=ts;});
u64 const timeout_ud=ring_->encode(tslot,tgen);
if(!submit_recvmsg_timeout_borrowed(ring_->raw(),h,&holder->msg,ts.get(),recv_ud,timeout_ud)){
ring_->completions().dispatch(slot,gen,-ENOSPC,0);
ring_->completions().dispatch(tslot,tgen,-EBUSY,0);
}
co_return co_await move(task);
}
