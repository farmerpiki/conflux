// Benchmark: TCP round-trip via SocketTaskRing (P1-09).
// Mirrors tcp_increment_coro_bench variants but drives I/O through
// TcpStream (socket_io.coro) instead of FileReader.
// Variants: socket_callback (block_on_ring per op) and socket_coroutine
// (single Task<u64> co_awaiting each send/recv).
#include<arpa/inet.h>
#include<liburing.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#include<sys/socket.h>
#include<unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.uring.completion;
import conflux.socket_io;
import conflux.socket_io.coro;

using namespace std;
using conflux::work::root::into_join_handle;
using conflux::work::root::join;
using conflux::work::root::Task;
using conflux::work::root::TaskJoinHandle;
namespace{
constexpr u64 pack_ud(u32 s,u32 g)noexcept{
return(static_cast<u64>(g)<<32U)|s;
}
struct Config{
SZ iterations=100000;
SZ warmup=5000;
bool json_out=false;
};
Config parse_args(span<char*>args){
Config cfg;
for(SZ i=1;i<args.size();++i){
SV const a=args[i];
if(a=="--iterations"&&i+1<args.size()){
SZ v{};
from_chars(args[i+1],args[i+1]+strlen(args[i+1]),v);
cfg.iterations=v;
++i;
}else if(a=="--warmup"&&i+1<args.size()){
SZ v{};
from_chars(args[i+1],args[i+1]+strlen(args[i+1]),v);
cfg.warmup=v;
++i;
}else if(a=="--json"){
cfg.json_out=true;
}
}
return cfg;
}
// ── server ────────────────────────────────────────────────────────────────
// Accepts a single connection, echoes n → n+1 lines until close.
template<class T,SZ N>
void consume_prefix(A<T,N>&buf,SZ&held,SZ drop){
if(drop>=held){
held=0;
return;
}
SZ const rem=held-drop;
for(SZ i=0;i<rem;++i)
buf[i]=buf[i+drop];
held=rem;
}
void run_server(int lfd,atomic_flag&stop){
int const cfd=::accept4(lfd,nullptr,nullptr,SOCK_CLOEXEC);
::close(lfd);
if(cfd<0)return;
int one=1;
::setsockopt(cfd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
A<char,64>buf{};
SZ held=0;
while(!stop.test(memory_order_acquire)){
ssize_t const got=::read(cfd,buf.data()+held,buf.size()-held);
if(got<=0)break;
held+=static_cast<SZ>(got);
SZ scan=0;
while(scan<held){
auto view=span{buf}.subspan(scan,held-scan);
auto it=ranges::find(view,'\n');
if(it==view.end())break;
SZ const msg_end=scan+static_cast<SZ>(it-view.begin());
u64 n=0;
if(from_chars(buf.data()+scan,buf.data()+msg_end,n).ec!=errc{}){
::close(cfd);
return;
}
++n;
A<char,24>out{};
auto conv=to_chars(out.data(),out.data()+out.size()-1,n);
if(conv.ec!=errc{}){
::close(cfd);
return;
}
*conv.ptr='\n';
SZ const outlen=static_cast<SZ>(conv.ptr-out.data())+1;
SZ sent=0;
while(sent<outlen){
ssize_t w=::write(cfd,out.data()+sent,outlen-sent);
if(w<=0){
::close(cfd);
return;
}
sent+=static_cast<SZ>(w);
}
scan=msg_end+1;
}
if(scan>0)consume_prefix(buf,held,scan);
if(held==buf.size()){
::close(cfd);
return;
}
}
::close(cfd);
}
int start_listener(u16&port_out){
int const fd=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
if(fd<0)throw RE{"socket"};
int one=1;
::setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
sockaddr_in addr{};
addr.sin_family=AF_INET;
addr.sin_addr.s_addr=::htonl(INADDR_LOOPBACK);
addr.sin_port=0;
if(::bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0){
::close(fd);
throw RE{"bind"};
}
socklen_t slen=sizeof(addr);
if(::getsockname(fd,reinterpret_cast<sockaddr*>(&addr),&slen)<0){
::close(fd);
throw RE{"getsockname"};
}
port_out=::ntohs(addr.sin_port);
if(::listen(fd,16)<0){
::close(fd);
throw RE{"listen"};
}
return fd;
}
sockaddr_storage loopback_addr(u16 port)noexcept{
sockaddr_storage ss{};
auto*sin=reinterpret_cast<sockaddr_in*>(&ss);
sin->sin_family=AF_INET;
sin->sin_addr.s_addr=::htonl(INADDR_LOOPBACK);
sin->sin_port=::htons(port);
return ss;
}
// ── line codec ───────────────────────────────────────────────────────────
SZ encode_line(span<char>out,u64 n){
auto r=to_chars(out.data(),out.data()+out.size()-1,n);
if(r.ec!=errc{})throw RE{"to_chars"};
*r.ptr='\n';
return static_cast<SZ>(r.ptr-out.data())+1;
}
u64 decode_line(SV line){
u64 n=0;
if(from_chars(line.data(),line.data()+line.size(),n).ec!=errc{})
throw RE{"from_chars"};
return n;
}
// ── block_on_ring ─────────────────────────────────────────────────────────
template<typename T>
T block_on_ring(
::io_uring*ring,
CompletionTable&completions,
Task<T>task){
struct Slot{
atomic_flag done{};
exception_ptr err{};
conditional_t<is_void_v<T>,monostate,optional<T>>value{};
};
auto sl=make_shared<Slot>();
auto jh=make_shared<TaskJoinHandle<T>>(into_join_handle(move(task)));
jh->control().set_on_ready_or_run([sl,jh]()noexcept{
try{
auto outcome=join(move(*jh));
if(outcome.is_failure())sl->err=move(outcome).failure().error;
else if(outcome.is_cancelled())sl->err=make_exception_ptr(RE{"task cancelled"});
else if constexpr(!is_void_v<T>)sl->value.emplace(move(outcome).success().value);
}catch(...){sl->err=current_exception();}
sl->done.test_and_set(memory_order_release);
});
while(!sl->done.test(memory_order_acquire)){
::io_uring_cqe*cqe=nullptr;
__kernel_timespec ts{.tv_sec=5,.tv_nsec=0};
int const rc=::io_uring_submit_and_wait_timeout(ring,&cqe,1,&ts,nullptr);
if(rc==-ETIME)throw RE{"block_on_ring: timed out"};
if(rc==-EINTR)continue;
if(rc>=0&&cqe==nullptr)continue;
A<::io_uring_cqe*,32>batch{};
for(;;){
unsigned const n=::io_uring_peek_batch_cqe(ring,batch.data(),32u);
if(n==0)break;
for(unsigned i=0;i<n;++i){
auto const*c=batch[static_cast<SZ>(i)];
u64 const ud=c->user_data;
completions.dispatch(
static_cast<u32>(ud&0xFFFFFFFFU),
static_cast<u32>(ud>>32U),
c->res,c->flags);
}
::io_uring_cq_advance(ring,n);
if(sl->done.test(memory_order_acquire))break;
}
}
if(sl->err)rethrow_exception(sl->err);
if constexpr(!is_void_v<T>)return move(*sl->value);
}
// ── socket_callback variant ───────────────────────────────────────────────
// Connects once; warmup + measurement share the same TcpStream.
struct SockLineReader{
TcpStream&stream;
::io_uring*ring;
CompletionTable&ct;
A<u8,128>buf{};
SZ held=0;
SV read_line(){
for(;;){
auto view=span{buf}.first(held);
auto it=ranges::find(view,u8('\n'));
if(it!=view.end()){
SZ const end=static_cast<SZ>(it-view.begin());
return SV{reinterpret_cast<char const*>(buf.data()),end};
}
SZ const got=block_on_ring(ring,ct,stream.recv_borrowed(span<u8>{buf.data()+held,buf.size()-held}));
if(got==0)throw RE{"eof"};
held+=got;
}
}
void consume_line(SZ line_len){consume_prefix(buf,held,line_len+1);}
};
u64 run_socket_callback(
SocketTaskRing&task_ring,
::io_uring*raw,
CompletionTable&ct,
u16 port,
SZ warmup,
SZ iters){
auto ss=loopback_addr(port);
TcpStream stream=block_on_ring(raw,ct,tcp_connect(task_ring,AF_INET,ss,sizeof(sockaddr_in)));
SockLineReader reader{.stream=stream,.ring=raw,.ct=ct};
A<char,24>out{};
u64 n=0;
auto round=[&]{
SZ const len=encode_line(out,n);
block_on_ring(raw,ct,stream.write_all_borrowed(span<u8 const>{reinterpret_cast<u8 const*>(out.data()),len}));
auto line=reader.read_line();
u64 const got=decode_line(line);
reader.consume_line(line.size());
if(got!=n+1)throw RE{format("expected {} got {}",n+1,got)};
n=got;
};
for(SZ i=0;i<warmup;++i)
round();
auto const t0=chrono::steady_clock::now();
for(SZ i=0;i<iters;++i)
round();
auto const t1=chrono::steady_clock::now();
return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1-t0).count());
}
// ── socket_coroutine variant ──────────────────────────────────────────────
// Single coroutine: connect, warmup, measure.
Task<u64>socket_coro_loop(
SocketTaskRing&ring,
u16 port,
SZ warmup,
SZ iters){
auto ss=loopback_addr(port);
TcpStream stream=co_await tcp_connect(ring,AF_INET,ss,sizeof(sockaddr_in));
A<u8,128>rbuf{};
SZ held=0;
A<char,24>out{};
u64 n=0;
auto round=[&]()->Task<void>{
SZ const len=encode_line(out,n);
co_await stream.write_all_borrowed(span<u8 const>{reinterpret_cast<u8 const*>(out.data()),len});
for(;;){
auto view=span{rbuf}.first(held);
auto it=ranges::find(view,u8('\n'));
if(it!=view.end()){
SZ const end=static_cast<SZ>(it-view.begin());
SV const line{reinterpret_cast<char const*>(rbuf.data()),end};
u64 const got=decode_line(line);
consume_prefix(rbuf,held,end+1);
if(got!=n+1)throw RE{format("expected {} got {}",n+1,got)};
n=got;
co_return;
}
SZ const r=co_await stream.recv_borrowed(span<u8>{rbuf.data()+held,rbuf.size()-held});
if(r==0)throw RE{"eof"};
held+=r;
}
};
for(SZ i=0;i<warmup;++i)
co_await round();
auto const t0=chrono::steady_clock::now();
for(SZ i=0;i<iters;++i)
co_await round();
auto const t1=chrono::steady_clock::now();
co_return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1-t0).count());
}
u64 run_socket_coroutine(
SocketTaskRing&task_ring,
::io_uring*raw,
CompletionTable&ct,
u16 port,
SZ warmup,
SZ iters){
return block_on_ring(raw,ct,socket_coro_loop(task_ring,port,warmup,iters));
}
}// namespace
int main(int argc,char**argv){
if(argc>=2&&SV{argv[1]}=="--bench-info"){
println("{{\"name\":\"tcp_socket_task\",\"parser\":\"standard\",\"configs\":[{{\"name\":\"default\",\"extra\":{{}},\"args\":[\"--iterations\",\"200\",\"--warmup\",\"50\"]}}]}}");
return 0;
}
auto cfg=parse_args(span{argv,static_cast<SZ>(argc)});
for(int which=0;which<2;++which){
u16 port=0;
int const lfd=start_listener(port);
atomic_flag server_stop{};
thread server{[lfd,&server_stop]{run_server(lfd,server_stop);}};
::io_uring raw{};
if(::io_uring_queue_init(64,&raw,0)<0){
server_stop.test_and_set(memory_order_release);
server.join();
println(cerr,"io_uring_queue_init failed");
return 1;
}
CompletionTable ct;
SocketTaskRing task_ring{
SocketRawRing{&raw},ct,
[](u32 s,u32 g)noexcept->u64{return pack_ud(s,g);}};
try{
u64 const ns=(which==0)?
run_socket_callback(task_ring,&raw,ct,port,cfg.warmup,cfg.iterations):
run_socket_coroutine(task_ring,&raw,ct,port,cfg.warmup,cfg.iterations);
double const per=static_cast<double>(ns)/static_cast<double>(cfg.iterations);
SV const label=(which==0)?"socket_callback":"socket_coroutine";
if(cfg.json_out){
println("{{\"config\":\"default\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f}}}",label,cfg.iterations,ns,per);
}else{
if(which==0)println("iterations: {}, warmup: {}",cfg.iterations,cfg.warmup);
println("  {:<18} {:>8.1f} ns/iter ({} ns total)",label,per,ns);
}
}catch(exception const&e){println(cerr,"error: {}",e.what());}
::io_uring_queue_exit(&raw);
server_stop.test_and_set(memory_order_release);
server.join();
}
}
