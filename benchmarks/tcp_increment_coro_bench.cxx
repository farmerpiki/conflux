// Benchmark: TCP round-trip (send N, expect N+1).
// fr/* variants use FileReader; str/* variants use SocketTaskRing/TcpStream.
// Phase 1: all four variants run against the same blocking single-connection server.
#include<arpa/inet.h>
#include<charconv>
#include<liburing.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#include<sys/socket.h>
#include<unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.socket_io;
import conflux.socket_io.coro;

using conflux::work::root::Task;
namespace{
constexpr u64 pack_ud(
u32 slot,
u32 gen)noexcept{
return(static_cast<u64>(gen)<<32U)|slot;
}
struct Config{
SZ iterations=100000;
SZ warmup=5000;
bool json_out=false;
};
template<class T,SZ N>
void consume_prefix(
A<T,N>&buf,
SZ&held,
SZ drop){
if(drop>=held){
held=0;
return;
}
SZ const remain=held-drop;
for(SZ i=0;i<remain;++i)
buf[i]=buf[i+drop];
held=remain;
}
namespace{
u64 parse_u64(
char const*s)noexcept{
SV const sv{s};
u64 v{};
from_chars(sv.data(),sv.data()+sv.size(),v);
return v;
}
}// namespace
Config parse_args(
span<char*>args){
Config cfg;
for(SZ i=1;i<args.size();++i){
SV const a=args[i];
if(a=="--iterations"&&i+1<args.size()){
cfg.iterations=parse_u64(args[++i]);
}else if(a=="--warmup"&&i+1<args.size()){
cfg.warmup=parse_u64(args[++i]);
}else if(a=="--json"){
cfg.json_out=true;
}else if(a=="--help"||a=="-h"){
println("Usage: conflux_tcp_increment_coro_bench [--iterations N] [--warmup N] [--json]");
std::exit(0);
}
}
return cfg;
}
// ── server ────────────────────────────────────────────────────────────────────
void serve_one(
int cfd,
atomic_flag&stop){
int one=1;
::setsockopt(cfd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
A<char,64>buf{};
SZ held=0;
while(!stop.test(memory_order_acquire)){
ssize_t const got=::read(cfd,buf.data()+held,buf.size()-held);
if(got<=0)
break;
held+=static_cast<SZ>(got);
SZ scan=0;
while(scan<held){
auto view=span{buf}.subspan(scan,held-scan);
auto it=ranges::find(view,'\n');
if(it==view.end())
break;
SZ const msg_end=scan+static_cast<SZ>(it-view.begin());
u64 n=0;
if(from_chars(buf.data()+scan,buf.data()+msg_end,n).ec!=errc{}){
::close(cfd);
return;
}
++n;
A<char,24>out{};
auto const conv=to_chars(out.data(),out.data()+out.size()-1,n);
if(conv.ec!=errc{}){
::close(cfd);
return;
}
*conv.ptr='\n';
SZ const out_len=static_cast<SZ>(conv.ptr-out.data())+1;
SZ sent=0;
while(sent<out_len){
ssize_t const w=::write(cfd,out.data()+sent,out_len-sent);
if(w<=0){
::close(cfd);
return;
}
sent+=static_cast<SZ>(w);
}
scan=msg_end+1;
}
if(scan>0)
consume_prefix(buf,held,scan);
if(held==buf.size()){
::close(cfd);
return;
}
}
::close(cfd);
}
void run_server(
int listen_fd,
atomic_flag&stop){
timeval tv{.tv_sec=0,.tv_usec=100000};
::setsockopt(listen_fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
while(!stop.test(memory_order_acquire)){
int const cfd=::accept4(listen_fd,nullptr,nullptr,SOCK_CLOEXEC);
if(cfd<0){
if(errno==EAGAIN||errno==EINTR)continue;
break;
}
serve_one(cfd,stop);
}
::close(listen_fd);
}
int start_listener(
u16&port_out){
int const fd=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
if(fd<0)
throw RE{"socket"};
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
int connect_to(
u16 port){
int const fd=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
if(fd<0)
throw RE{"socket"};
int one=1;
::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
sockaddr_in addr{};
addr.sin_family=AF_INET;
addr.sin_addr.s_addr=::htonl(INADDR_LOOPBACK);
addr.sin_port=::htons(port);
if(::connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0){
::close(fd);
throw RE{"connect"};
}
return fd;
}
sockaddr_storage loopback_addr(
u16 port)noexcept{
sockaddr_storage ss{};
auto*sin=reinterpret_cast<sockaddr_in*>(&ss);
sin->sin_family=AF_INET;
sin->sin_addr.s_addr=::htonl(INADDR_LOOPBACK);
sin->sin_port=::htons(port);
return ss;
}
SZ encode_line(
span<char>out,
u64 n){
auto const r=to_chars(out.data(),out.data()+out.size()-1,n);
if(r.ec!=errc{})
throw RE{"to_chars"};
*r.ptr='\n';
return static_cast<SZ>(r.ptr-out.data())+1;
}
u64 decode_line(
SV line){
u64 n=0;
if(from_chars(line.data(),line.data()+line.size(),n).ec!=errc{})
throw RE{"from_chars"};
return n;
}
// ── fr/* (FileReader) variants ────────────────────────────────────────────────
struct FrLineReader{
FileReader&files;
FileHandle const&handle;
A<byte,128>buf{};
SZ held=0;
Task<SV>read_line(){
for(;;){
auto view=span{buf}.first(held);
auto it=ranges::find(view,static_cast<byte>('\n'));
if(it!=view.end()){
auto const end=static_cast<SZ>(it-view.begin());
co_return SV{reinterpret_cast<char const*>(buf.data()),end};
}
auto got=co_await files.read_into(handle,0,span{buf.data()+held,buf.size()-held});
if(got==0)
throw RE{"eof"};
held+=got;
}
}
void consume_line(
SZ line_len){
consume_prefix(buf,held,line_len+1);
}
};
u64 run_fr_callback(
FileReader&files,
FileHandle const&sock,
SZ iters,
u64 start){
FrLineReader reader{.files=files,.handle=sock};
A<char,24>out{};
u64 n=start;
auto const t0=chrono::steady_clock::now();
for(SZ i=0;i<iters;++i){
SZ const len=encode_line(out,n);
block_on(files,files.write_into(sock,0,as_bytes(span{out.data(),len})));
auto line=block_on(files,reader.read_line());
u64 const got=decode_line(line);
reader.consume_line(line.size());
if(got!=n+1)
throw RE{format("expected {} got {}",n+1,got)};
n=got;
}
auto const t1=chrono::steady_clock::now();
return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1-t0).count());
}
Task<u64>fr_coro_loop(
FileReader&files,
FileHandle const&sock,
SZ iters,
u64 start){
FrLineReader reader{.files=files,.handle=sock};
A<char,24>out{};
u64 n=start;
for(SZ i=0;i<iters;++i){
SZ const len=encode_line(out,n);
co_await files.write_into(sock,0,as_bytes(span{out.data(),len}));
auto line=co_await reader.read_line();
u64 const got=decode_line(line);
reader.consume_line(line.size());
if(got!=n+1)
throw RE{format("expected {} got {}",n+1,got)};
n=got;
}
co_return n;
}
u64 run_fr_coroutine(
FileReader&files,
FileHandle const&sock,
SZ iters,
u64 start){
auto const t0=chrono::steady_clock::now();
auto _=block_on(files,fr_coro_loop(files,sock,iters,start));
auto const t1=chrono::steady_clock::now();
return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1-t0).count());
}
// ── str/* (SocketTaskRing) variants ───────────────────────────────────────────
struct StrLineReader{
TcpStream&stream;
SocketTaskRing&ring;
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
SZ const got=block_on_socket_task(ring,stream.recv_borrowed(span<u8>{buf.data()+held,buf.size()-held}));
if(got==0)throw RE{"eof"};
held+=got;
}
}
void consume_line(
SZ line_len){
consume_prefix(buf,held,line_len+1);
}
};
u64 run_str_callback(
SocketTaskRing&ring,
TcpStream&stream,
SZ iters,
u64 start){
StrLineReader reader{.stream=stream,.ring=ring};
A<char,24>out{};
u64 n=start;
auto const t0=chrono::steady_clock::now();
for(SZ i=0;i<iters;++i){
SZ const len=encode_line(out,n);
block_on_socket_task(ring,stream.write_all_borrowed(span<u8 const>{reinterpret_cast<u8 const*>(out.data()),len}));
auto line=reader.read_line();
u64 const got=decode_line(line);
reader.consume_line(line.size());
if(got!=n+1)
throw RE{format("expected {} got {}",n+1,got)};
n=got;
}
auto const t1=chrono::steady_clock::now();
return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1-t0).count());
}
Task<u64>str_coro_loop(
TcpStream&stream,
SZ iters,
u64 start){
A<u8,128>rbuf{};
SZ held=0;
A<char,24>out{};
u64 n=start;
for(SZ i=0;i<iters;++i){
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
break;
}
SZ const r=co_await stream.recv_borrowed(span<u8>{rbuf.data()+held,rbuf.size()-held});
if(r==0)throw RE{"eof"};
held+=r;
}
}
co_return n;
}
u64 run_str_coroutine(
SocketTaskRing&ring,
TcpStream&stream,
SZ iters,
u64 start){
auto const t0=chrono::steady_clock::now();
auto _=block_on_socket_task(ring,str_coro_loop(stream,iters,start));
auto const t1=chrono::steady_clock::now();
return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1-t0).count());
}
}// namespace
int main(
int argc,
char**argv){
if(argc>=2&&SV{argv[1]}=="--bench-info"){
std::print(
"{}\n",
R"({"name":"tcp_increment","parser":"standard","configs":[{"name":"default","extra":{},"args":["--iterations","200","--warmup","50"]}]})");
return 0;
}
auto cfg=parse_args(span{argv,static_cast<SZ>(argc)});
// 0=fr/callback  1=fr/coroutine  2=str/callback  3=str/coroutine
static constexpr A<SV,4>labels{"fr/callback","fr/coroutine","str/callback","str/coroutine"};
auto const lbl=[&](int w)noexcept->SV{return labels[static_cast<SZ>(w)];};
for(int which=0;which<4;++which){
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
try{
if(which<2){
FileReader files{&raw,&ct,pack_ud};
int const csock=connect_to(port);
FileHandle sock=FileHandle::from_fd(csock);
(void)run_fr_callback(files,sock,cfg.warmup,0);
u64 const ns=(which==0)?
run_fr_callback(files,sock,cfg.iterations,cfg.warmup):
run_fr_coroutine(files,sock,cfg.iterations,cfg.warmup);
double const per=static_cast<double>(ns)/static_cast<double>(cfg.iterations);
if(cfg.json_out){
println("{{\"config\":\"default\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f}}}",lbl(which),cfg.iterations,ns,per);
}else{
if(which==0)println("iterations: {}, warmup: {}",cfg.iterations,cfg.warmup);
println("  {:<18} {:>8.1f} ns/iter ({} ns total)",lbl(which),per,ns);
}
server_stop.test_and_set(memory_order_release);
::shutdown(sock.raw_fd(),SHUT_RDWR);
(void)sock.release_fd();
::close(csock);
}else{
SocketTaskRing task_ring{
SocketRawRing{&raw},ct,
[](u32 s,u32 g)noexcept->u64{return pack_ud(s,g);}};
auto ss=loopback_addr(port);
TcpStream stream=block_on_socket_task(task_ring,
tcp_connect(task_ring,AF_INET,ss,sizeof(sockaddr_in)));
(void)run_str_callback(task_ring,stream,cfg.warmup,0);
u64 const ns=(which==2)?
run_str_callback(task_ring,stream,cfg.iterations,cfg.warmup):
run_str_coroutine(task_ring,stream,cfg.iterations,cfg.warmup);
double const per=static_cast<double>(ns)/static_cast<double>(cfg.iterations);
if(cfg.json_out)
println("{{\"config\":\"default\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f}}}",lbl(which),cfg.iterations,ns,per);
else
println("  {:<18} {:>8.1f} ns/iter ({} ns total)",lbl(which),per,ns);
server_stop.test_and_set(memory_order_release);
// stream dtor closes fd → unblocks server's ::read → server sees stop flag
}
}catch(exception const&e){println(cerr,"error: {}",e.what());}
::io_uring_queue_exit(&raw);
server.join();
}
}
