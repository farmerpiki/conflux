module;
#include<cerrno>
#include<liburing.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<unistd.h>

export module conflux.socket_io.coro;

import std;
import conflux.types;
import conflux.file_io;
import conflux.work;

using std::chrono::milliseconds;
using std::current_exception;
using std::exchange;
using std::make_exception_ptr;
using std::make_shared;
using std::move;
using std::span;
// ─── TcpStream ───────────────────────────────────────────────────────────────
// Coroutine-friendly TCP stream. Wraps FileReader for async send/recv/connect.
// Borrowed lifetime: caller must not destroy TcpStream while ops are in flight.

export class TcpStream{
FileReader*reader_{nullptr};
FileHandle fh_{};
public:
TcpStream()noexcept=default;
TcpStream(TcpStream const&)=delete;
TcpStream&operator=(TcpStream const&)=delete;
TcpStream(TcpStream&&)noexcept=default;
TcpStream&operator=(TcpStream&&)noexcept=default;
explicit TcpStream(FileReader&reader,FileHandle fh)noexcept
:reader_{&reader},fh_{move(fh)}{}
[[nodiscard]]static TcpStream from_handle(FileReader&reader,FileHandle fh)noexcept{
return TcpStream{reader,move(fh)};
}
[[nodiscard]]conflux::work::root::Task<SZ>send(span<u8 const>data,int flags=MSG_NOSIGNAL){
return reader_->send_async(fh_,data.data(),data.size(),flags);
}
[[nodiscard]]conflux::work::root::Task<void>send_all(span<u8 const>data,int flags=MSG_NOSIGNAL);
[[nodiscard]]conflux::work::root::Task<SZ>recv(span<u8>buf,int flags=0){
return reader_->recv_async(fh_,buf.data(),buf.size(),flags);
}
[[nodiscard]]conflux::work::root::Task<void>close(){
return reader_->close_async(move(fh_));
}
[[nodiscard]]int raw_fd()const noexcept{return fh_.raw_fd();}
[[nodiscard]]FileHandle const&handle()const noexcept{return fh_;}
[[nodiscard]]FileReader&reader()noexcept{return*reader_;}
[[nodiscard]]bool valid()const noexcept{return reader_!=nullptr&&fh_.valid();}
};
export[[nodiscard]]conflux::work::root::Task<TcpStream>tcp_connect(
FileReader&reader,int family,sockaddr_storage addr,socklen_t len){
FileHandle fh=co_await reader.socket_async(family,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP);
co_await reader.connect_async(fh,addr,len);
co_return TcpStream{reader,move(fh)};
}
[[nodiscard]]conflux::work::root::Task<void>TcpStream::send_all(
span<u8 const>data,int flags){
SZ sent=0;
while(sent<data.size()){
SZ const n=co_await reader_->send_async(fh_,data.data()+sent,data.size()-sent,flags);
if(n==0)
throw FileIoError{ECONNRESET,"tcp: connection closed"};
sent+=n;
}
}
// ─── UdpRecvResult ───────────────────────────────────────────────────────────

export struct UdpRecvResult{
SZ bytes{0};
sockaddr_storage from{};
socklen_t from_len{0};
};
// ─── UdpSocket ───────────────────────────────────────────────────────────────
// Coroutine-friendly UDP socket. send_to and recv_from are borrowed: caller
// owns the data/buffer lifetime until the awaited Task completes.

export class UdpSocket{
FileReader*reader_{nullptr};
FileHandle fh_{};
int family_{AF_INET};
struct MsgHolder{
msghdr msg{};
iovec iov{};
sockaddr_storage from{};
};
public:
UdpSocket()noexcept=default;
UdpSocket(UdpSocket const&)=delete;
UdpSocket&operator=(UdpSocket const&)=delete;
UdpSocket(UdpSocket&&)noexcept=default;
UdpSocket&operator=(UdpSocket&&)noexcept=default;
explicit UdpSocket(FileReader&reader,FileHandle fh,int family)noexcept
:reader_{&reader},fh_{move(fh)},family_{family}{}
[[nodiscard]]static UdpSocket ephemeral(FileReader&reader,int family);
[[nodiscard]]static UdpSocket from_handle(FileReader&reader,FileHandle fh,int family)noexcept{
return UdpSocket{reader,move(fh),family};
}
[[nodiscard]]conflux::work::root::Task<SZ>send_to(
span<u8 const>data,sockaddr_storage dest,socklen_t len,int flags=0){
return reader_->sendto_async(fh_,data.data(),data.size(),flags,dest,len);
}
[[nodiscard]]conflux::work::root::Task<UdpRecvResult>recv_from(span<u8>buf,int flags=0);
[[nodiscard]]conflux::work::root::Task<UdpRecvResult>recv_from(
span<u8>buf,milliseconds timeout,int flags=0);
[[nodiscard]]FileHandle const&handle()const noexcept{return fh_;}
[[nodiscard]]int raw_fd()const noexcept{return fh_.raw_fd();}
[[nodiscard]]int family()const noexcept{return family_;}
[[nodiscard]]bool valid()const noexcept{return reader_!=nullptr&&fh_.valid();}
};
UdpSocket UdpSocket::ephemeral(FileReader&reader,int family){
int const fd=::socket(family,SOCK_DGRAM|SOCK_CLOEXEC,IPPROTO_UDP);
if(fd<0)
throw FileIoError{errno,"udp: socket"};
if(family==AF_INET){
sockaddr_in sa{};
sa.sin_family=AF_INET;
sa.sin_addr.s_addr=htonl(INADDR_ANY);
if(::bind(fd,reinterpret_cast<sockaddr*>(&sa),sizeof(sa))<0){
int const e=errno;
::close(fd);
throw FileIoError{e,"udp: bind"};
}
}else if(family==AF_INET6){
sockaddr_in6 sa{};
sa.sin6_family=AF_INET6;
sa.sin6_addr=in6addr_any;
if(::bind(fd,reinterpret_cast<sockaddr*>(&sa),sizeof(sa))<0){
int const e=errno;
::close(fd);
throw FileIoError{e,"udp: bind"};
}
}else{
::close(fd);
throw FileIoError{EAFNOSUPPORT,"udp: unsupported family"};
}
return UdpSocket{reader,FileHandle::from_fd(fd),family};
}
conflux::work::root::Task<UdpRecvResult>UdpSocket::recv_from(span<u8>buf,int flags){
namespace wroot=conflux::work::root;
auto[task,raw_src]=wroot::make_task_source<UdpRecvResult>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<UdpRecvResult>>(move(raw_src));
int const fd=fh_.is_direct()?fh_.direct_slot():fh_.raw_fd();
auto*sqe=io_uring_get_sqe(reader_->ring());
if(!sqe){
auto _=shared_src->try_set_exception(
make_exception_ptr(FileIoError{ENOSPC,"udp: SQ full"}));
return move(task);
}
auto h=make_shared<MsgHolder>();
h->iov.iov_base=buf.data();
h->iov.iov_len=buf.size();
h->msg.msg_name=&h->from;
h->msg.msg_namelen=sizeof(h->from);
h->msg.msg_iov=&h->iov;
h->msg.msg_iovlen=1;
io_uring_prep_recvmsg(sqe,fd,&h->msg,static_cast<unsigned>(flags));
if(fh_.is_direct())
io_uring_sqe_set_flags(sqe,IOSQE_FIXED_FILE);
auto[slot,gen]=reader_->completions()->reserve([shared_src,h](IoResult r)mutable{
try{
if(r.res<0){
auto _=shared_src->try_set_exception(
make_exception_ptr(FileIoError{-r.res,"udp: recvfrom failed"}));
return;
}
UdpRecvResult result;
result.bytes=static_cast<SZ>(r.res);
result.from=h->from;
result.from_len=h->msg.msg_namelen;
auto _=shared_src->try_set_value(wroot::Success<UdpRecvResult>{move(result)});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
io_uring_sqe_set_data64(sqe,reader_->encode_ud(slot,gen));
return move(task);
}
conflux::work::root::Task<UdpRecvResult>UdpSocket::recv_from(
span<u8>buf,milliseconds timeout,int flags){
namespace wroot=conflux::work::root;
auto[task,raw_src]=wroot::make_task_source<UdpRecvResult>(wroot::SubmitOptions{.enable_cancellation=false});
auto shared_src=make_shared<wroot::TaskSource<UdpRecvResult>>(move(raw_src));
if(io_uring_sq_space_left(reader_->ring())<2){
auto _=shared_src->try_set_exception(
make_exception_ptr(FileIoError{ENOSPC,"udp: SQ full"}));
return move(task);
}
int const fd=fh_.is_direct()?fh_.direct_slot():fh_.raw_fd();
auto*sqe_recv=io_uring_get_sqe(reader_->ring());
auto*sqe_to=io_uring_get_sqe(reader_->ring());
auto h=make_shared<MsgHolder>();
h->iov.iov_base=buf.data();
h->iov.iov_len=buf.size();
h->msg.msg_name=&h->from;
h->msg.msg_namelen=sizeof(h->from);
h->msg.msg_iov=&h->iov;
h->msg.msg_iovlen=1;
auto ts=make_shared<__kernel_timespec>();
auto const sec=chrono::duration_cast<chrono::seconds>(timeout);
ts->tv_sec=sec.count();
ts->tv_nsec=(timeout-sec).count()*1000000LL;
io_uring_prep_recvmsg(sqe_recv,fd,&h->msg,static_cast<unsigned>(flags));
unsigned const recv_flags=IOSQE_IO_LINK|(fh_.is_direct()?IOSQE_FIXED_FILE:0u);
io_uring_sqe_set_flags(sqe_recv,recv_flags);
auto[slot_recv,gen_recv]=reader_->completions()->reserve([shared_src,h](IoResult r)mutable{
try{
if(r.res==-ECANCELED){
auto _=shared_src->try_set_exception(
make_exception_ptr(FileIoError{ETIMEDOUT,"udp: recv timed out"}));
return;
}
if(r.res<0){
auto _=shared_src->try_set_exception(
make_exception_ptr(FileIoError{-r.res,"udp: recvfrom failed"}));
return;
}
UdpRecvResult result;
result.bytes=static_cast<SZ>(r.res);
result.from=h->from;
result.from_len=h->msg.msg_namelen;
auto _=shared_src->try_set_value(wroot::Success<UdpRecvResult>{move(result)});
}catch(...){auto _=shared_src->try_set_exception(current_exception());}
});
io_uring_sqe_set_data64(sqe_recv,reader_->encode_ud(slot_recv,gen_recv));
io_uring_prep_link_timeout(sqe_to,ts.get(),0);
auto[slot_to,gen_to]=reader_->completions()->reserve([ts](IoResult)mutable{auto _=ts;});
io_uring_sqe_set_data64(sqe_to,reader_->encode_ud(slot_to,gen_to));
return move(task);
}
