// Plain TU — not a module unit.
#include<arpa/inet.h>
#include<liburing.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#include<sys/socket.h>
#include<unistd.h>

#include<catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.uring.completion;
import conflux.work;
import conflux.socket_io;
import conflux.socket_io.coro;

using namespace std;
namespace{
constexpr uint64_t pack_ud(
uint32_t slot,
uint32_t gen)noexcept{
return(static_cast<uint64_t>(gen)<<32U)|slot;
}
template<typename T>
T block_on_ring(
::io_uring*ring,
CompletionTable&completions,
conflux::work::root::Task<T>task,
chrono::milliseconds budget=chrono::seconds{5}){
using namespace conflux::work::root;
struct Slot{
atomic_flag done{};
exception_ptr err{};
[[no_unique_address]]conditional_t<is_void_v<T>,monostate,optional<T>>value{};
};
auto slot=make_shared<Slot>();
auto jh=make_shared<TaskJoinHandle<T>>(into_join_handle(move(task)));
jh->control().set_on_ready_or_run([slot,jh]()noexcept{
try{
auto outcome=join(move(*jh));
if(outcome.is_failure())
slot->err=move(outcome).failure().error;
else if(outcome.is_cancelled())
slot->err=make_exception_ptr(runtime_error{"task cancelled"});
else if constexpr(!is_void_v<T>)
slot->value.emplace(move(outcome).success().value);
}catch(...){slot->err=current_exception();}
slot->done.test_and_set(memory_order_release);
});
auto const deadline=chrono::steady_clock::now()+budget;
while(!slot->done.test(memory_order_acquire)){
::io_uring_cqe*cqe=nullptr;
__kernel_timespec ts{.tv_sec=1,.tv_nsec=0};
int const rc=::io_uring_submit_and_wait_timeout(ring,&cqe,1,&ts,nullptr);
if(rc==-ETIME){
if(chrono::steady_clock::now()>deadline)
throw runtime_error{"block_on_ring: budget exhausted"};
continue;
}
if(rc==-EINTR)continue;
if(rc>=0&&cqe==nullptr)continue;
array<::io_uring_cqe*,32>batch{};
for(;;){
unsigned const n=::io_uring_peek_batch_cqe(ring,batch.data(),32u);
if(n==0)break;
for(unsigned i=0;i<n;++i){
auto const*c=batch[static_cast<size_t>(i)];
auto ud=c->user_data;
completions.dispatch(
static_cast<uint32_t>(ud&0xFFFFFFFFU),
static_cast<uint32_t>(ud>>32U),
c->res,c->flags);
}
::io_uring_cq_advance(ring,n);
if(slot->done.test(memory_order_acquire))break;
}
}
if(slot->err)rethrow_exception(slot->err);
if constexpr(!is_void_v<T>)return move(*slot->value);
}
struct RingFixture{
::io_uring ring{};
CompletionTable completions{};
SocketTaskRing task_ring;
bool ring_ok{false};
RingFixture()
:task_ring{SocketRawRing{&ring},completions,[](uint32_t s,uint32_t g)noexcept->uint64_t{return pack_ud(s,g);}}{}
static unique_ptr<RingFixture>make(unsigned entries=64){
auto fx=make_unique<RingFixture>();
if(::io_uring_queue_init(entries,&fx->ring,0)<0)
return{};
fx->ring_ok=true;
return fx;
}
~RingFixture(){
if(ring_ok)::io_uring_queue_exit(&ring);
}
RingFixture(RingFixture const&)=delete;
RingFixture&operator=(RingFixture const&)=delete;
RingFixture(RingFixture&&)=delete;
RingFixture&operator=(RingFixture&&)=delete;
template<typename T>
T run(conflux::work::root::Task<T>task,chrono::milliseconds budget=chrono::seconds{5}){
return block_on_ring(&ring,completions,move(task),budget);
}
};
unique_ptr<RingFixture>require_ring_fixture(unsigned entries=64){
auto fx=RingFixture::make(entries);
INFO("conflux requires a host that permits io_uring_queue_init");
REQUIRE(fx!=nullptr);
return fx;
}
sockaddr_storage loopback_addr(uint16_t port)noexcept{
sockaddr_storage ss{};
auto*sin=reinterpret_cast<sockaddr_in*>(&ss);
sin->sin_family=AF_INET;
sin->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
sin->sin_port=htons(port);
return ss;
}
// Minimal TCP echo server running in a jthread.
// accept → read → write (echo) in a loop until client closes.
class TcpEchoServer{
int listen_fd_{-1};
uint16_t port_{0};
jthread thread_;
public:
TcpEchoServer(){
listen_fd_=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP);
if(listen_fd_<0)return;
int const one=1;
::setsockopt(listen_fd_,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
sockaddr_in sa{};
sa.sin_family=AF_INET;
sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
sa.sin_port=0;
if(::bind(listen_fd_,reinterpret_cast<sockaddr*>(&sa),sizeof(sa))<0){
::close(listen_fd_);
listen_fd_=-1;
return;
}
if(::listen(listen_fd_,8)<0){
::close(listen_fd_);
listen_fd_=-1;
return;
}
sockaddr_in bound{};
socklen_t len=sizeof(bound);
::getsockname(listen_fd_,reinterpret_cast<sockaddr*>(&bound),&len);
port_=ntohs(bound.sin_port);
thread_=jthread{[this](stop_token st){
while(!st.stop_requested()){
struct timeval tv{0,20000};// 20ms poll
::setsockopt(listen_fd_,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
int const client=::accept(listen_fd_,nullptr,nullptr);
if(client<0)continue;
A<char,4096>buf{};
for(;;){
ssize_t const n=::recv(client,buf.data(),buf.size(),0);
if(n<=0)break;
ssize_t off=0;
while(off<n){
ssize_t const w=::send(client,buf.data()+off,static_cast<SZ>(n)-static_cast<SZ>(off),MSG_NOSIGNAL);
if(w<=0)goto done;
off+=w;
}
}
done:
::close(client);
}
}};
}
~TcpEchoServer(){
thread_.request_stop();
if(listen_fd_>=0)::close(listen_fd_);
}
[[nodiscard]]bool ok()const noexcept{return listen_fd_>=0&&port_>0;}
[[nodiscard]]uint16_t port()const noexcept{return port_;}
};
}// namespace
// ---------------------------------------------------------------------------
// CompletionTable — pure unit tests, no io_uring
// ---------------------------------------------------------------------------

TEST_CASE(
"completion_table: reserve+dispatch single-shot fires callback once",
"[completion_table]"){
CompletionTable ct;
int calls=0;
int res_seen=-1;
auto[slot,gen]=ct.reserve([&calls,&res_seen](IoResult r)noexcept{
++calls;
res_seen=r.res;
});
CHECK(ct.pending()==1);
ct.dispatch(slot,gen,42,0);
CHECK(calls==1);
CHECK(res_seen==42);
CHECK(ct.pending()==0);
// second dispatch with same slot+gen is stale (gen bumped)
ct.dispatch(slot,gen,99,0);
CHECK(calls==1);
}
TEST_CASE(
"completion_table: stale gen is silently ignored",
"[completion_table]"){
CompletionTable ct;
int calls=0;
auto[slot,gen]=ct.reserve([&calls](IoResult)noexcept{++calls;});
ct.dispatch(slot,gen+1,0,0);// wrong gen
CHECK(calls==0);
CHECK(ct.pending()==1);
ct.dispatch(slot,gen,0,0);// correct
CHECK(calls==1);
}
TEST_CASE(
"completion_table: out-of-range slot is ignored",
"[completion_table]"){
CompletionTable ct;
ct.dispatch(9999,0,0,0);// no crash, no effect
CHECK(ct.pending()==0);
}
TEST_CASE(
"completion_table: cancel_all fires all pending with -ECANCELED",
"[completion_table]"){
CompletionTable ct;
int a=0,b=0,c=0;
[[maybe_unused]]auto _a=ct.reserve([&a](IoResult r)noexcept{a=r.res;});
[[maybe_unused]]auto _b=ct.reserve([&b](IoResult r)noexcept{b=r.res;});
[[maybe_unused]]auto _c=ct.reserve([&c](IoResult r)noexcept{c=r.res;});
CHECK(ct.pending()==3);
auto const ok=ct.cancel_all();
CHECK(ok);
CHECK(ct.pending()==0);
CHECK(a==-ECANCELED);
CHECK(b==-ECANCELED);
CHECK(c==-ECANCELED);
// After cancel, can reserve again cleanly.
int d=0;
auto[s3,g3]=ct.reserve([&d](IoResult r)noexcept{d=r.res;});
ct.dispatch(s3,g3,7,0);
CHECK(d==7);
}
TEST_CASE(
"completion_table: cancel_all does not visit slots reserved inside a callback",
"[completion_table]"){
CompletionTable ct;
int visited=0;
int extra_fired=0;
// callback reserves a new slot — that new slot must NOT be visited by cancel_all
[[maybe_unused]]auto _=ct.reserve([&ct,&visited,&extra_fired](IoResult)noexcept{
++visited;
[[maybe_unused]]auto _inner=ct.reserve([&extra_fired](IoResult r)noexcept{
if(r.res==-ECANCELED)++extra_fired;
});
});
[[maybe_unused]]auto _ok=ct.cancel_all();
CHECK(visited==1);
CHECK(extra_fired==0);// the slot reserved inside the callback was NOT visited
CHECK(ct.pending()==1);// the extra slot is still pending
}
TEST_CASE(
"completion_table: reserve_multishot stays alive on IORING_CQE_F_MORE",
"[completion_table]"){
CompletionTable ct;
int calls=0;
auto[slot,gen]=ct.reserve_multishot([&calls](IoResult r)noexcept{
if(r.res>=0)++calls;
});
// Fire with MORE flag set twice, then terminate
ct.dispatch(slot,gen,1,IORING_CQE_F_MORE);
ct.dispatch(slot,gen,2,IORING_CQE_F_MORE);
CHECK(calls==2);
CHECK(ct.pending()==1);// still alive
// Final delivery (no MORE)
ct.dispatch(slot,gen,3,0);
CHECK(calls==3);
CHECK(ct.pending()==0);
}
// ---------------------------------------------------------------------------
// SocketTaskRing — construction / encode
// ---------------------------------------------------------------------------

TEST_CASE(
"socket_task_ring: construction and encode delegate to UserDataFn",
"[socket_task_ring]"){
auto fx=require_ring_fixture();
// encode(slot=5, gen=7) should produce (7<<32)|5
uint64_t const encoded=fx->task_ring.encode(5,7);
CHECK(encoded==((static_cast<uint64_t>(7)<<32U)|5U));
}
// ---------------------------------------------------------------------------
// TcpStream — tcp_connect
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_connect: connects to a listening server",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,
AF_INET,
addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
CHECK(stream.valid());
fx->run(stream.close());
}
TEST_CASE(
"tcp_connect: throws IoError on connection refused",
"[tcp][uring]"){
// Bind and immediately close — port is guaranteed to refuse.
int tmp=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
REQUIRE(tmp>=0);
sockaddr_in sa{};
sa.sin_family=AF_INET;
sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
sa.sin_port=0;
::bind(tmp,reinterpret_cast<sockaddr*>(&sa),sizeof(sa));
socklen_t len=sizeof(sa);
::getsockname(tmp,reinterpret_cast<sockaddr*>(&sa),&len);
uint16_t const port=ntohs(sa.sin_port);
::close(tmp);// now the port refuses

auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(port);
int err_code=0;
bool got=false;
try{
fx->run(tcp_connect(
fx->task_ring,
AF_INET,
addr,
static_cast<socklen_t>(sizeof(sockaddr_in)),
ConnectOptions{.timeout=chrono::seconds{5}}));
got=true;
}catch(IoError const&e){err_code=e.code().value();}
CHECK_FALSE(got);
CHECK(err_code==ECONNREFUSED);
}
// ---------------------------------------------------------------------------
// TcpStream — write_all_borrowed + recv_borrowed echo
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: write_all_borrowed + recv_borrowed echo round-trip",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();

sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,
AF_INET,
addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());

A<uint8_t,13>msg{};
for(uint8_t i=0;i<13;++i)
msg[i]=i;

fx->run(stream.write_all_borrowed(span<uint8_t const>{msg.data(),msg.size()}));

A<uint8_t,13>rx{};
SZ received=0;
while(received<msg.size()){
SZ const n=fx->run(stream.recv_borrowed(
span<uint8_t>{rx.data()+received,msg.size()-received}));
REQUIRE(n>0);
received+=n;
}
CHECK(received==msg.size());
CHECK(memcmp(msg.data(),rx.data(),msg.size())==0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// TcpStream — EOF on closed connection
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: recv_borrowed returns 0 after server closes",
"[tcp][uring]"){
// Server that accepts and immediately closes.
int listen_fd=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
REQUIRE(listen_fd>=0);
int const one=1;
::setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
sockaddr_in sa{};
sa.sin_family=AF_INET;
sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
sa.sin_port=0;
::bind(listen_fd,reinterpret_cast<sockaddr*>(&sa),sizeof(sa));
::listen(listen_fd,4);
socklen_t len=sizeof(sa);
::getsockname(listen_fd,reinterpret_cast<sockaddr*>(&sa),&len);
uint16_t const port=ntohs(sa.sin_port);

jthread closer{[listen_fd]{
int const c=::accept(listen_fd,nullptr,nullptr);
if(c>=0)::close(c);// immediately close
}};

auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(port);
auto stream=fx->run(tcp_connect(
fx->task_ring,
AF_INET,
addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());

closer.join();
::close(listen_fd);

// read should return 0 (EOF)
A<uint8_t,64>buf{};
SZ const n=fx->run(stream.recv_borrowed(span<uint8_t>{buf.data(),buf.size()}));
CHECK(n==0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// TcpStream — connect with timeout (SQ-size guard)
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_connect: connect with timeout succeeds (happy path)",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
sockaddr_storage addr=loopback_addr(server.port());
auto fx=require_ring_fixture();
auto stream=fx->run(tcp_connect(
fx->task_ring,
AF_INET,
addr,
static_cast<socklen_t>(sizeof(sockaddr_in)),
ConnectOptions{.timeout=chrono::milliseconds{500}}));
CHECK(stream.valid());
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// TcpStream — write_copy round-trip
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: write_copy round-trip",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
A<uint8_t,8>msg{1,2,3,4,5,6,7,8};
fx->run(stream.write_all_copy(span<uint8_t const>{msg.data(),msg.size()}));
A<uint8_t,8>rx{};
SZ received=0;
while(received<msg.size()){
SZ const n=fx->run(stream.recv_borrowed(
span<uint8_t>{rx.data()+received,msg.size()-received}));
REQUIRE(n>0);
received+=n;
}
CHECK(memcmp(msg.data(),rx.data(),msg.size())==0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// TcpStream — close() makes valid() false
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: close() makes valid() return false",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
fx->run(stream.close());
CHECK_FALSE(stream.valid());
}
// ---------------------------------------------------------------------------
// SocketFdMode — direct_required throws ENOTSUP
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_connect: direct_required throws ENOTSUP",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
SocketTaskRingOptions ropts;
ropts.fd_mode=SocketFdMode::direct_required;
SocketTaskRing direct_ring{SocketRawRing{&fx->ring},fx->completions,
[](uint32_t s,uint32_t g)noexcept->uint64_t{return pack_ud(s,g);},ropts};
int err_code=0;
bool got=false;
try{
fx->run(tcp_connect(direct_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
got=true;
}catch(IoError const&e){err_code=e.code().value();}
CHECK_FALSE(got);
CHECK(err_code==ENOTSUP);
}
// ---------------------------------------------------------------------------
// SocketFdMode — direct_if_available falls back to os_fd
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_connect: direct_if_available falls back to os_fd",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
SocketTaskRingOptions ropts;
ropts.fd_mode=SocketFdMode::direct_if_available;
SocketTaskRing direct_ring{SocketRawRing{&fx->ring},fx->completions,
[](uint32_t s,uint32_t g)noexcept->uint64_t{return pack_ud(s,g);},ropts};
auto stream=fx->run(tcp_connect(direct_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
CHECK(stream.valid());
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// tcp_connect — negative timeout
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_connect: negative timeout throws EINVAL",
"[tcp][uring]"){
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(12345);
int err_code=0;
try{
fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in)),
ConnectOptions{.timeout=chrono::milliseconds{-1}}));
}catch(IoError const&e){err_code=e.code().value();}
CHECK(err_code==EINVAL);
}
// ---------------------------------------------------------------------------
// TcpStream — ops after close() throw EBADF
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: recv_borrowed after close() throws EBADF",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
fx->run(stream.close());
int err=0;
array<uint8_t,16>buf{};
try{
fx->run(stream.recv_borrowed(span<uint8_t>{buf.data(),buf.size()}));
}catch(IoError const&e){err=e.code().value();}
CHECK(err==EBADF);
}
// ---------------------------------------------------------------------------
// Cancellation — cancel in-flight read by user_data
// ---------------------------------------------------------------------------

TEST_CASE(
"cancellation: cancel read by user_data",
"[tcp][cancel][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
// echo server only sends when we send; no data pending → read will block
array<uint8_t,64>buf{};
// coroutine starts eagerly — SQE added to ring immediately
auto read_task=stream.recv_borrowed(span<uint8_t>{buf.data(),buf.size()});
// cancel from ring owner thread (inline path): fires cancel hook →
// submits read+cancel SQEs together via ring.raw().submit()
read_task.cancel();
bool got_cancel=false;
int err=0;
try{
fx->run(move(read_task));
}catch(IoError const&e){err=e.code().value();}catch(exception const&){
got_cancel=true;
}
CHECK(got_cancel||err==ECANCELED);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// Cancellation — submit_on_ring_owner false → try_set_cancelled
// ---------------------------------------------------------------------------

TEST_CASE(
"cancellation: submit_on_ring_owner false triggers try_set_cancelled",
"[tcp][cancel]"){
// Verifies Finding 9 fix: when submit_on_owner returns false,
// try_set_cancelled is called rather than silently hanging.
TcpEchoServer server;
REQUIRE(server.ok());
bool owner_called=false;
SocketTaskRingOptions opts;
// simulate cross-thread cancel handler that can't post (returns false)
opts.submit_on_ring_owner=[&owner_called](RingOpFn)->bool{
owner_called=true;
return false;
};
auto fx=require_ring_fixture();
SocketTaskRing ring2{SocketRawRing{&fx->ring},fx->completions,
[](uint32_t s,uint32_t g)noexcept->uint64_t{return pack_ud(s,g);},opts};
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(ring2,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
array<uint8_t,64>buf{};
auto read_task=stream.recv_borrowed(span<uint8_t>{buf.data(),buf.size()});
// cancel → cancel hook → submit_on_owner → lambda returns false
// with fix: try_set_cancelled() fires immediately
read_task.cancel();
bool got_cancel=false;
try{
fx->run(move(read_task));
}catch(exception const&){got_cancel=true;}
CHECK(owner_called);
CHECK(got_cancel);
// close drains the unsubmitted read SQE
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// tcp_connect — linked timeout fires ETIMEDOUT
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_connect: connect timeout fires ETIMEDOUT",
"[tcp][uring]"){
// 192.0.2.1 is RFC 5737 TEST-NET-1 — non-routable; SYN is dropped.
// The io_uring linked timeout fires at 300ms → ECANCELED → ETIMEDOUT.
auto fx=require_ring_fixture();
sockaddr_storage addr{};
auto*sin=reinterpret_cast<sockaddr_in*>(&addr);
sin->sin_family=AF_INET;
sin->sin_addr.s_addr=htonl(0xC0000201U);// 192.0.2.1
sin->sin_port=htons(1);
int err=0;
try{
fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in)),
ConnectOptions{.timeout=chrono::milliseconds{300}}),
chrono::seconds{10});
}catch(IoError const&e){err=e.code().value();}
// ETIMEDOUT when linked timeout fires; EHOSTUNREACH/ENETUNREACH if unroutable
CHECK(err==ETIMEDOUT||err==EHOSTUNREACH||err==ENETUNREACH);
}
// ---------------------------------------------------------------------------
// TcpStream — write_copy safe after source span is destroyed
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: write_copy safe after source span destroyed",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
conflux::work::root::Task<void>copy_task{};
{
// source lives in this inner scope only
array<uint8_t,8>msg{1,2,3,4,5,6,7,8};
// write_all_copy copies into an internal holder immediately
copy_task=stream.write_all_copy(span<uint8_t const>{msg.data(),msg.size()});
// msg destroyed here
}
fx->run(move(copy_task));// must not UB — holder owns the data
array<uint8_t,8>rx{};
SZ received=0;
while(received<8){
SZ const n=fx->run(stream.recv_borrowed(span<uint8_t>{rx.data()+received,8u-received}));
REQUIRE(n>0);
received+=n;
}
array<uint8_t,8>expected{1,2,3,4,5,6,7,8};
CHECK(memcmp(rx.data(),expected.data(),8)==0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// TcpStream — TCP_NODELAY set by default
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: TCP_NODELAY set by default after connect",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
int nodelay=0;
socklen_t optlen=sizeof(nodelay);
int const rc=::getsockopt(stream.raw_fd(),IPPROTO_TCP,TCP_NODELAY,&nodelay,&optlen);
CHECK(rc==0);
CHECK(nodelay!=0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// tcp_connect — cancel during socket_pending (before socket CQE)
// ---------------------------------------------------------------------------

static int count_proc_fds()noexcept{
int n=0;
namespace fs=std::filesystem;
try{
for([[maybe_unused]]auto const&_:fs::directory_iterator{"/proc/self/fd"})
++n;
}catch(...){}
return n;
}
TEST_CASE(
"tcp_connect: cancel during socket_pending completes cancelled",
"[tcp][cancel][uring]"){
auto fx=require_ring_fixture();
// blackhole — SYN never answered; connect SQE stays pending indefinitely
sockaddr_storage addr{};
auto*sin=reinterpret_cast<sockaddr_in*>(&addr);
sin->sin_family=AF_INET;
sin->sin_addr.s_addr=htonl(0xC0000201U);// 192.0.2.1
sin->sin_port=htons(1);

int const fd_before=count_proc_fds();

auto task=tcp_connect(fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in)));
// cancel before any CQE pumped — fires cancel hook inline (single-thread ring)
task.cancel();
bool got_cancel=false;
int err_code=0;
try{
fx->run(move(task),chrono::seconds{5});
}catch(IoError const&e){err_code=e.code().value();}catch(exception const&){
got_cancel=true;
}
CHECK(got_cancel);
CHECK(err_code==0);

int const fd_after=count_proc_fds();
// no permanent fd leak — allow small slack for test infra
CHECK(fd_after<=fd_before+4);
}
// ---------------------------------------------------------------------------
// tcp_connect — cancel during connect_pending (after socket CQE, before connect CQE)
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_connect: cancel during connect_pending completes cancelled",
"[tcp][cancel][uring]"){
auto fx=require_ring_fixture();
// blackhole address — connect SQE stays pending until cancelled
sockaddr_storage addr{};
auto*sin=reinterpret_cast<sockaddr_in*>(&addr);
sin->sin_family=AF_INET;
sin->sin_addr.s_addr=htonl(0xC0000201U);
sin->sin_port=htons(1);

auto task=tcp_connect(fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in)));

// Pump exactly one CQE (socket creation) so ConnectOp enters connect_pending.
// SINGLE_ISSUER ring on ring-owner thread — safe to peek/submit here.
{
::io_uring_cqe*cqe=nullptr;
__kernel_timespec ts{.tv_sec=5,.tv_nsec=0};
::io_uring_submit_and_wait_timeout(&fx->ring,&cqe,1,&ts,nullptr);
array<::io_uring_cqe*,1>batch{};
unsigned const n=::io_uring_peek_batch_cqe(&fx->ring,batch.data(),1u);
if(n>0){
auto const*c=batch[0];
fx->completions.dispatch(
static_cast<uint32_t>(c->user_data&0xFFFFFFFFU),
static_cast<uint32_t>(c->user_data>>32U),
c->res,c->flags);
::io_uring_cq_advance(&fx->ring,1);
}
}
// Now in connect_pending — cancel fires cancel_on_owner inline (single-thread ring)
task.cancel();
bool got_cancel=false;
int err_code=0;
try{
fx->run(move(task),chrono::seconds{5});
}catch(IoError const&e){err_code=e.code().value();}catch(exception const&){
got_cancel=true;
}
CHECK(got_cancel);
CHECK(err_code==0);
}
// ---------------------------------------------------------------------------
// tcp_connect — submit_on_ring_owner false → immediate complete_cancelled
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_connect: submit_on_ring_owner false triggers complete_cancelled",
"[tcp][cancel][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
bool owner_called=false;
SocketTaskRingOptions opts;
opts.submit_on_ring_owner=[&owner_called](RingOpFn)->bool{
owner_called=true;
return false;
};
auto fx=require_ring_fixture();
SocketTaskRing ring2{SocketRawRing{&fx->ring},fx->completions,
[](uint32_t s,uint32_t g)noexcept->uint64_t{return pack_ud(s,g);},opts};
sockaddr_storage addr=loopback_addr(server.port());
auto task=tcp_connect(ring2,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in)));
// cancel → hook → submit_on_owner returns false → complete_cancelled
task.cancel();
bool got_cancel=false;
int err_code=0;
try{
fx->run(move(task));
}catch(IoError const&e){err_code=e.code().value();}catch(exception const&){
got_cancel=true;
}
CHECK(owner_called);
CHECK(got_cancel);
CHECK(err_code==0);
}
// ---------------------------------------------------------------------------
// tcp_connect — cancel with timeout armed: cancel_requested beats stop_cause==timeout
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_connect: cancel with timeout armed reports cancelled not ETIMEDOUT",
"[tcp][cancel][uring]"){
// Verifies that cancel_requested==true in on_connect_cqe beats stop_cause==timeout.
// With a 5s timeout armed and immediate cancel (ring-owner inline), connect CQE
// arrives with -ECANCELED; cancel_requested is true → complete_cancelled, not ETIMEDOUT.
auto fx=require_ring_fixture();
sockaddr_storage addr{};
auto*sin=reinterpret_cast<sockaddr_in*>(&addr);
sin->sin_family=AF_INET;
sin->sin_addr.s_addr=htonl(0xC0000201U);// 192.0.2.1 — blackhole
sin->sin_port=htons(1);

auto task=tcp_connect(fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in)),
ConnectOptions{.timeout=chrono::seconds{5}});
task.cancel();// fires inline on ring-owner; sets cancel_requested before any CQE
bool got_cancel=false;
int err_code=0;
try{
fx->run(move(task),chrono::seconds{10});
}catch(IoError const&e){err_code=e.code().value();}catch(exception const&){
got_cancel=true;
}
CHECK(got_cancel);
CHECK(err_code==0);
}
// ---------------------------------------------------------------------------
// TcpStream — shutdown(SHUT_WR) then read returns EOF
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: shutdown(SHUT_WR) then read returns EOF",
"[tcp][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
// after SHUT_WR the server sees EOF, closes its side → our read returns 0
fx->run(stream.shutdown(SHUT_WR));
array<uint8_t,64>buf{};
SZ const n=fx->run(stream.recv_borrowed(span<uint8_t>{buf.data(),buf.size()}));
CHECK(n==0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// TcpStream — write_all_copy copies once; safe after source mutated
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: write_all_copy safe after source mutated before run",
"[tcp][lifetime][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
array<uint8_t,8>src{1,2,3,4,5,6,7,8};
auto task=stream.write_all_copy(span<uint8_t const>{src.data(),src.size()});
// mutate source — task must have copied it already
src.fill(0xCC);
fx->run(move(task));
array<uint8_t,8>rx{};
SZ received=0;
while(received<8){
SZ const n=fx->run(stream.recv_borrowed(span<uint8_t>{rx.data()+received,8u-received}));
REQUIRE(n>0);
received+=n;
}
array<uint8_t,8>const expected{1,2,3,4,5,6,7,8};
CHECK(memcmp(rx.data(),expected.data(),8)==0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// TcpStream — write_all_owned(V<u8>) round-trip
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: write_all_owned vector round-trip",
"[tcp][lifetime][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
V<uint8_t>payload{10,20,30,40,50,60};
fx->run(stream.write_all_owned(move(payload)));
array<uint8_t,6>rx{};
SZ received=0;
while(received<6){
SZ const n=fx->run(stream.recv_borrowed(span<uint8_t>{rx.data()+received,6u-received}));
REQUIRE(n>0);
received+=n;
}
array<uint8_t,6>const expected{10,20,30,40,50,60};
CHECK(memcmp(rx.data(),expected.data(),6)==0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// TcpStream — write_all_owned(S) round-trip
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: write_all_owned string round-trip",
"[tcp][lifetime][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
S msg="hello";
fx->run(stream.write_all_owned(move(msg)));
array<uint8_t,5>rx{};
SZ received=0;
while(received<5){
SZ const n=fx->run(stream.recv_borrowed(span<uint8_t>{rx.data()+received,5u-received}));
REQUIRE(n>0);
received+=n;
}
CHECK(memcmp(rx.data(),"hello",5)==0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// TcpStream — recv_owned round-trip
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: recv_owned returns owned buffer shrunk to actual bytes",
"[tcp][lifetime][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
array<uint8_t,4>msg{0xDE,0xAD,0xBE,0xEF};
fx->run(stream.write_all_borrowed(span<uint8_t const>{msg.data(),msg.size()}));
// recv_owned with a generous max — result should be exactly what echo returned
auto buf=fx->run(stream.recv_owned(256));
REQUIRE(!buf.empty());
CHECK(buf.size()==msg.size());// must shrink to actual bytes, not max_bytes
CHECK(memcmp(buf.data(),msg.data(),msg.size())==0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// UdpSocket — send_to_copy safe after source mutated before run
// ---------------------------------------------------------------------------

TEST_CASE(
"udp: send_to_copy safe after source mutated before run",
"[udp][lifetime][uring]"){
auto fx=require_ring_fixture();
// Bind a UDP echo server on loopback.
int srv=::socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,IPPROTO_UDP);
REQUIRE(srv>=0);
sockaddr_in sa{};
sa.sin_family=AF_INET;
sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
sa.sin_port=0;
::bind(srv,reinterpret_cast<sockaddr*>(&sa),sizeof(sa));
socklen_t slen=sizeof(sa);
::getsockname(srv,reinterpret_cast<sockaddr*>(&sa),&slen);
uint16_t const srv_port=ntohs(sa.sin_port);

sockaddr_storage dst{};
auto*dsin=reinterpret_cast<sockaddr_in*>(&dst);
dsin->sin_family=AF_INET;
dsin->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
dsin->sin_port=htons(srv_port);

UdpSocket sock=UdpSocket::ephemeral(fx->task_ring,AF_INET);
array<uint8_t,4>payload{1,2,3,4};
auto task=sock.send_to_copy(
span<uint8_t const>{payload.data(),payload.size()},
dst,static_cast<socklen_t>(sizeof(sockaddr_in)));
// mutate source — send_to_copy must have copied
payload.fill(0xCC);
SZ const sent=fx->run(move(task));
CHECK(sent==4);
// verify server received the original bytes, not the mutated 0xCC
array<uint8_t,16>rbuf{};
ssize_t const got=::recv(srv,rbuf.data(),rbuf.size(),0);
REQUIRE(got==4);
array<uint8_t,4>const orig{1,2,3,4};
CHECK(memcmp(rbuf.data(),orig.data(),4)==0);
::close(srv);
}
// ---------------------------------------------------------------------------
// TcpStream — write_owned(V<u8>) single-shot round-trip
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: write_owned vector single-shot round-trip",
"[tcp][lifetime][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
V<uint8_t>payload{0xAA,0xBB,0xCC};
SZ const sent=fx->run(stream.write_owned(move(payload)));
CHECK(sent==3);
array<uint8_t,3>rx{};
SZ received=0;
while(received<3){
SZ const n=fx->run(stream.recv_borrowed(span<uint8_t>{rx.data()+received,3u-received}));
REQUIRE(n>0);
received+=n;
}
array<uint8_t,3>const expected{0xAA,0xBB,0xCC};
CHECK(memcmp(rx.data(),expected.data(),3)==0);
fx->run(stream.close());
}
// ---------------------------------------------------------------------------
// TcpStream — write_owned(S) single-shot round-trip
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp: write_owned string single-shot round-trip",
"[tcp][lifetime][uring]"){
TcpEchoServer server;
REQUIRE(server.ok());
auto fx=require_ring_fixture();
sockaddr_storage addr=loopback_addr(server.port());
auto stream=fx->run(tcp_connect(
fx->task_ring,AF_INET,addr,
static_cast<socklen_t>(sizeof(sockaddr_in))));
REQUIRE(stream.valid());
S msg="XY";
SZ const sent=fx->run(stream.write_owned(move(msg)));
CHECK(sent==2);
array<uint8_t,2>rx{};
SZ received=0;
while(received<2){
SZ const n=fx->run(stream.recv_borrowed(span<uint8_t>{rx.data()+received,2u-received}));
REQUIRE(n>0);
received+=n;
}
CHECK(memcmp(rx.data(),"XY",2)==0);
fx->run(stream.close());
}
// ─────────────────────────────────────────────────────────────────────────────
// Acceptance criteria — tcp_accept / tcp_accept_multishot
// ─────────────────────────────────────────────────────────────────────────────

namespace{
// Blocking IPv4 connect to loopback:port. Returns connected fd.
int connect_v4_blocking(uint16_t port)noexcept{
int fd=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
if(fd<0)return-1;
sockaddr_in sa{};
sa.sin_family=AF_INET;
sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
sa.sin_port=htons(port);
if(::connect(fd,reinterpret_cast<sockaddr*>(&sa),sizeof(sa))<0){
::close(fd);
return-1;
}
return fd;
}
}// namespace
// ---------------------------------------------------------------------------
// AC-5: tcp_accept single-shot cancel while blocked, fd baseline restored
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_accept: cancel while blocked resolves cancelled",
"[tcp_accept][cancel][uring]"){
auto fx=require_ring_fixture();
TcpListener l{TcpListenerOptions{.bind=TcpBindAddress::loopback_v4}};
int const fd_before=count_proc_fds();
auto task=tcp_accept(l,fx->task_ring);
// cancel before any client connects — fires inline on ring owner
task.cancel();
bool got_cancel=false;
int err_code=0;
try{
fx->run(move(task),chrono::seconds{5});
}catch(IoError const&e){err_code=e.code().value();}catch(exception const&){
got_cancel=true;
}
CHECK(got_cancel||err_code==ECANCELED);
int const fd_after=count_proc_fds();
CHECK(fd_after<=fd_before+2);
}
// ---------------------------------------------------------------------------
// AC-5b: tcp_accept direct_required throws ENOTSUP
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_accept: direct_required throws ENOTSUP",
"[tcp_accept][uring]"){
auto fx=require_ring_fixture();
TcpListener l{TcpListenerOptions{.bind=TcpBindAddress::loopback_v4}};
SocketTaskRingOptions ropts;
ropts.fd_mode=SocketFdMode::direct_required;
SocketTaskRing dr{SocketRawRing{&fx->ring},fx->completions,
[](uint32_t s,uint32_t g)noexcept->uint64_t{return(static_cast<uint64_t>(g)<<32U)|s;},ropts};
int err_code=0;
try{
fx->run(tcp_accept(l,dr),chrono::seconds{5});
}catch(IoError const&e){err_code=e.code().value();}
CHECK(err_code==ENOTSUP);
}
// ---------------------------------------------------------------------------
// AC-4: tcp_accept single-shot E2E — 100 sequential connections
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_accept: 100 sequential connections E2E",
"[tcp_accept][uring]"){
auto fx=require_ring_fixture();
TcpListener l{TcpListenerOptions{.bind=TcpBindAddress::loopback_v4}};
int const fd_before=count_proc_fds();
uint32_t const pending_before=fx->completions.pending();
for(int i=0;i<100;++i){
// start accept task (no connect yet — accept SQE in SQ, not submitted)
auto task=tcp_accept(l,fx->task_ring);
// connect from a jthread so ring pump can run concurrently
jthread t{[&l]{
int fd=connect_v4_blocking(l.port());
if(fd>=0)::close(fd);
}};
TcpStream s=fx->run(move(task),chrono::seconds{5});
CHECK(s.valid());
fx->run(s.close(),chrono::seconds{5});
t.join();
}
CHECK(fx->completions.pending()==pending_before);
int const fd_after=count_proc_fds();
CHECK(fd_after<=fd_before+2);
}
// ---------------------------------------------------------------------------
// AC-4b: tcp_accept_multishot E2E — 20 connections then cancel
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_accept_multishot: 20 connections then cancel, no fd leak",
"[tcp_accept_multishot][uring]"){
auto fx=require_ring_fixture();
TcpListener l{TcpListenerOptions{.bind=TcpBindAddress::loopback_v4}};
int const fd_before=count_proc_fds();
uint32_t const pending_before=fx->completions.pending();
auto counter=make_shared<atomic<int>>(0);
using Task_v=conflux::work::root::Task<void>;
Fn<Task_v(TcpStream)>handler=[counter](TcpStream s)->Task_v{
counter->fetch_add(1,memory_order_relaxed);
co_await s.close();
};
auto task=tcp_accept_multishot(l,fx->task_ring,{},move(handler));
// connect 20 clients then cancel — all backlogged before pump runs
jthread t{[&l]{
for(int i=0;i<20;++i){
int fd=connect_v4_blocking(l.port());
if(fd>=0)::close(fd);
}
}};
t.join();
// cancel after all connections queued
task.cancel();
// task resolves (cancelled or error) — not hung; if it hangs, timeout throws
try{
fx->run(move(task),chrono::seconds{30});
}catch(...){};
CHECK(fx->completions.pending()==pending_before);
int const fd_after=count_proc_fds();
CHECK(fd_after<=fd_before+2);
}
// ---------------------------------------------------------------------------
// AC-6: tcp_accept_multishot cancel mid-stream — resolves cancelled, fd clean
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_accept_multishot: cancel resolves cancelled, no fd leak",
"[tcp_accept_multishot][cancel][uring]"){
auto fx=require_ring_fixture();
TcpListener l{TcpListenerOptions{.bind=TcpBindAddress::loopback_v4}};
int const fd_before=count_proc_fds();
using Task_v2=conflux::work::root::Task<void>;
Fn<Task_v2(TcpStream)>handler2=[](TcpStream s)->Task_v2{
co_await s.close();
};
auto task=tcp_accept_multishot(l,fx->task_ring,{},move(handler2));
// 3 connections then cancel
for(int i=0;i<3;++i){
int fd=connect_v4_blocking(l.port());
if(fd>=0)::close(fd);
}
task.cancel();
bool got_cancel=false;
int err_code=0;
try{
fx->run(move(task),chrono::seconds{10});
}catch(IoError const&e){err_code=e.code().value();}catch(exception const&){
got_cancel=true;
}
CHECK(got_cancel||err_code==ECANCELED);
int const fd_after=count_proc_fds();
CHECK(fd_after<=fd_before+2);
}
// ---------------------------------------------------------------------------
// AC-7: TcpListener outlives Task — destroy listener after task resolves, ASAN clean
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_accept_multishot: listener destroyed after task resolves, no UAF",
"[tcp_accept_multishot][lifetime][uring]"){
auto fx=require_ring_fixture();
{
TcpListener l{TcpListenerOptions{.bind=TcpBindAddress::loopback_v4}};
using Task_v3=conflux::work::root::Task<void>;
Fn<Task_v3(TcpStream)>handler3=[](TcpStream s)->Task_v3{
co_await s.close();
};
auto task=tcp_accept_multishot(l,fx->task_ring,{},move(handler3));
int fd=connect_v4_blocking(l.port());
if(fd>=0)::close(fd);
task.cancel();
try{
fx->run(move(task),chrono::seconds{10});
}catch(...){}
// l destroyed at end of scope — task already resolved, no UAF
}
// no crash = ASAN would have fired if UAF occurred
CHECK(true);
}
// ---------------------------------------------------------------------------
// AC-8: tcp_accept cancel with SQ full — retry path, no false completion
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_accept: SQ-full cancel retry, no false completion",
"[tcp_accept][cancel][uring]"){
// small ring so we can fill the SQ precisely
auto fx=require_ring_fixture(16);
TcpListener l{TcpListenerOptions{.bind=TcpBindAddress::loopback_v4}};
int const fd_before=count_proc_fds();
uint32_t const pending_before=fx->completions.pending();
auto task=tcp_accept(l,fx->task_ring);
// fill remaining 15 SQ slots with nops (sentinel user_data — out of range → ignored on CQE)
int nops_added=0;
for(int i=0;i<15;++i){
auto*sqe=::io_uring_get_sqe(&fx->ring);
if(!sqe)break;
::io_uring_prep_nop(sqe);
sqe->user_data=0xDEADBEEFU;
++nops_added;
}
REQUIRE(nops_added==15);// ring must have been exactly 1 slot used (accept)
// cancel: SQ full → submit() flushes nops+accept → retry → cancel SQE submitted
task.cancel();
bool got_cancel=false;
int err_code=0;
try{
fx->run(move(task),chrono::seconds{5});
}catch(IoError const&e){err_code=e.code().value();}catch(exception const&){
got_cancel=true;
}
CHECK(got_cancel||err_code==ECANCELED);
// no CompletionTable leak
CHECK(fx->completions.pending()==pending_before);
int const fd_after=count_proc_fds();
CHECK(fd_after<=fd_before+2);
}
// ---------------------------------------------------------------------------
// AC-9: submit_on_owner failure — cancel_requested set, drain via accept CQE
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_accept: submit_on_owner failure drains via accept CQE, no false completion",
"[tcp_accept][cancel][uring]"){
auto fx=require_ring_fixture();
bool owner_called=false;
SocketTaskRingOptions opts;
opts.submit_on_ring_owner=[&owner_called](RingOpFn)->bool{
owner_called=true;
return false;
};
SocketTaskRing ring2{SocketRawRing{&fx->ring},fx->completions,
[](uint32_t s,uint32_t g)noexcept->uint64_t{return(static_cast<uint64_t>(g)<<32U)|s;},opts};
TcpListener l{TcpListenerOptions{.bind=TcpBindAddress::loopback_v4}};
int const fd_before=count_proc_fds();
auto task=tcp_accept(l,ring2);
// cancel: submit_on_owner returns false → cancel_requested=true, no cancel SQE submitted
task.cancel();
CHECK(owner_called);
// task must NOT have resolved yet (cancel_requested=true but no CQE arrived)
// connect a client to provide the accept CQE → on_accept_cqe sees cancel_requested → complete_cancelled
int client_fd=connect_v4_blocking(l.port());
bool got_cancel=false;
int err_code=0;
try{
fx->run(move(task),chrono::seconds{5});
}catch(IoError const&e){err_code=e.code().value();}catch(exception const&){
got_cancel=true;
}
CHECK(got_cancel||err_code==ECANCELED);
if(client_fd>=0)::close(client_fd);
int const fd_after=count_proc_fds();
CHECK(fd_after<=fd_before+2);
}
// ---------------------------------------------------------------------------
// AC-9b: tcp_accept_multishot submit_on_owner failure — drain via listener close
// ---------------------------------------------------------------------------

TEST_CASE(
"tcp_accept_multishot: submit_on_owner failure drains when listener closed",
"[tcp_accept_multishot][cancel][uring]"){
auto fx=require_ring_fixture();
bool owner_called=false;
SocketTaskRingOptions opts;
opts.submit_on_ring_owner=[&owner_called](RingOpFn)->bool{
owner_called=true;
return false;
};
SocketTaskRing ring2{SocketRawRing{&fx->ring},fx->completions,
[](uint32_t s,uint32_t g)noexcept->uint64_t{return(static_cast<uint64_t>(g)<<32U)|s;},opts};
auto l=make_unique<TcpListener>(TcpListenerOptions{.bind=TcpBindAddress::loopback_v4});
int const fd_before=count_proc_fds();
using Task_v4=conflux::work::root::Task<void>;
Fn<Task_v4(TcpStream)>handler4=[](TcpStream s)->Task_v4{
co_await s.close();
};
auto task=tcp_accept_multishot(*l,ring2,{},move(handler4));
task.cancel();
CHECK(owner_called);
// destroy listener — kernel delivers terminal error CQE for the multishot op
l.reset();
bool got_cancel=false;
int err_code=0;
try{
fx->run(move(task),chrono::seconds{5});
}catch(IoError const&e){err_code=e.code().value();}catch(exception const&){
got_cancel=true;
}
// accepted either as cancelled (cancel_requested=true→complete_cancelled)
// or as error (listener destroyed → error CQE path)
CHECK(got_cancel||err_code!=0);
int const fd_after=count_proc_fds();
CHECK(fd_after<=fd_before+2);
}
