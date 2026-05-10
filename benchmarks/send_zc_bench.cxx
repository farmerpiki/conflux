#include<arpa/inet.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#include<signal.h>
#include<sys/socket.h>
#include<unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import conflux.work;
import bench_common;

using namespace std::literals;
namespace{
struct BenchClient{
int fd=-1;
explicit BenchClient(u16 port){
fd=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
if(fd<0)throw RE{"socket failed"};
static constexpr int one=1;
::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
::setsockopt(fd,IPPROTO_TCP,TCP_QUICKACK,&one,sizeof one);
sockaddr_in addr{};
addr.sin_family=AF_INET;
addr.sin_port=htons(port);
::inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
if(::connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0){
::close(fd);
throw RE{"connect failed"};
}
}
~BenchClient(){
if(fd>=0)::close(fd);
}
BenchClient(BenchClient const&)=delete;
BenchClient&operator=(BenchClient const&)=delete;
BenchClient(BenchClient&&o)noexcept:fd(exchange(o.fd,-1)){}
BenchClient&operator=(BenchClient&&o)noexcept{
if(this!=&o){
if(fd>=0)::close(fd);
fd=exchange(o.fd,-1);
}
return*this;
}
void send_all(SV data)const{
auto const*p=data.data();
auto remaining=data.size();
while(remaining>0){
auto n=::send(fd,p,remaining,MSG_NOSIGNAL);
if(n<=0)throw RE{"send failed"};
p+=n;
remaining-=static_cast<SZ>(n);
}
}
SZ recv_response(span<char>buf)const{
SZ total=0;
SZ hdr_end_pos=SV::npos;
SZ body_len=0;
bool have_cl=false;
for(;;){
auto n=::recv(fd,buf.data()+total,buf.size()-total,0);
if(n<=0)break;
total+=static_cast<SZ>(n);
if(hdr_end_pos==SV::npos){
SV sofar{buf.data(),total};
hdr_end_pos=sofar.find("\r\n\r\n");
if(hdr_end_pos==SV::npos)continue;
hdr_end_pos+=4;
SV hdrs{buf.data(),hdr_end_pos};
auto cl=hdrs.find("Content-Length: ");
if(cl!=SV::npos){
cl+=16;
auto end=hdrs.find("\r\n",cl);
from_chars(buf.data()+cl,buf.data()+end,body_len);
have_cl=true;
}
if(hdrs.starts_with("HTTP/1.1 304")||hdrs.starts_with("HTTP/1.1 204"))
return total;
}
if(have_cl&&total>=hdr_end_pos+body_len)
return total;
if(!have_cl&&hdr_end_pos!=SV::npos)
return total;
}
return total;
}
void reconnect(u16 port){
if(fd>=0)::close(fd);
fd=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
if(fd<0)throw RE{"socket failed"};
static constexpr int one=1;
::setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
::setsockopt(fd,IPPROTO_TCP,TCP_QUICKACK,&one,sizeof one);
sockaddr_in addr{};
addr.sin_family=AF_INET;
addr.sin_port=htons(port);
::inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
if(::connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0){
::close(fd);
fd=-1;
throw RE{"reconnect failed"};
}
}
};
void wait_for_server(u16 port){
for(int i=0;i<200;++i){
int const s=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
sockaddr_in addr{};
addr.sin_family=AF_INET;
addr.sin_port=htons(port);
::inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
bool const up=::connect(s,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0;
::close(s);
if(up)return;
std::this_thread::sleep_for(chrono::milliseconds(10));
}
throw RE{"server did not start in time"};
}
struct ServerHandle{
SP<HttpServer>server;
thread thr;
u16 port{};
};
ServerHandle start_server(Config cfg,Router router){
(void)::signal(SIGPIPE,SIG_IGN);
cfg.startup_banner=false;
auto srv=make_shared<HttpServer>(cfg,move(router));
thread t{[srv]{
try{
auto _=srv->run();
}catch(exception const&e){println(cerr,"bench server: {}",e.what());}
}};
auto p=srv->port();
wait_for_server(p);
return{.server=srv,.thr=move(t),.port=p};
}
Config bench_config_zc(SV mode){
Config cfg{};
cfg.port=0;
cfg.rings=1;
cfg.ring_entries=256;
cfg.single_issuer=true;
cfg.defer_taskrun=true;
cfg.coop_taskrun=true;
cfg.taskrun_flag=true;
cfg.startup_banner=false;
cfg.fixed_buffer_slabs=0;
cfg.splice_pipe_pairs=0;
cfg.send_zc=S{mode};
cfg.send_zc_threshold=16384;
cfg.send_zc_report_usage=true;
return cfg;
}
using RunFn=Fn<void()>;
struct Variant{
S name;
RunFn setup;
RunFn run;
RunFn teardown;
SZ ops_per_iter=1;
SZ iters_override=0;
};
struct BenchStats{
S config;
S variant;
SZ iterations{};
u64 total_ns{};
double ns_per_iter{};
};
BenchStats run_variant(
Variant const&v,
SZ iterations,
SZ warmup,
SV config_name){
if(v.iters_override){
iterations=v.iters_override;
warmup=max(SZ{2},v.iters_override/10);
}
if(v.setup)v.setup();
for(SZ i=0;i<warmup;++i)
v.run();
auto const t0=bench_now_ns();
for(SZ i=0;i<iterations;++i)
v.run();
auto const t1=bench_now_ns();
if(v.teardown)v.teardown();
auto const total=t1-t0;
auto const ns_pi=static_cast<double>(total)/static_cast<double>(iterations);
return BenchStats{
.config=S{config_name},
.variant=v.name,
.iterations=iterations,
.total_ns=total,
.ns_per_iter=ns_pi,
};
}
void print_variant(
BenchStats const&s,
bool json,
SZ ops_per_iter){
if(json){
if(ops_per_iter>1){
auto const total_ops=s.iterations*ops_per_iter;
auto const ns_per_op=s.ns_per_iter/static_cast<double>(ops_per_iter);
println(
"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},\"ops_per_iter\":{},\"total_ops\":{},\"ns_per_op\":{:.2f}}}",
s.config,s.variant,s.iterations,s.total_ns,s.ns_per_iter,ops_per_iter,total_ops,ns_per_op);
}else{
println(
"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f}}}",
s.config,s.variant,s.iterations,s.total_ns,s.ns_per_iter);
}
}else if(ops_per_iter>1){
auto const ns_per_op=s.ns_per_iter/static_cast<double>(ops_per_iter);
println("{:<40} {:>8} iters  {:>10.2f} ns/iter  {:>10.2f} ns/op (x{})",
s.variant,s.iterations,s.ns_per_iter,ns_per_op,ops_per_iter);
}else{
println("{:<40} {:>8} iters  {:>10.2f} ns/iter",
s.variant,s.iterations,s.ns_per_iter);
}
}
}// namespace
int main(int argc,char**argv){
bench_info_if_requested(argc,argv,
R"({"name":"send_zc","parser":"standard","configs":[{"name":"default","extra":{},"args":["--iterations","2000","--warmup","200"]}]})");

auto const args=bench_parse_args(span{argv,static_cast<SZ>(argc)});
auto const iters=args.iterations;
auto const warmup=args.warmup;
auto const json_out=args.json_out;
auto const config_name=args.config_name.empty()?"default"sv:SV{args.config_name};
struct BodySpec{
SV label;
SZ size;
};
static constexpr A<BodySpec,7>kBodies{{{"512B",512},{"1K",1024},{"4K",4096},{"16K",16384},
{"64K",65536},{"256K",262144},{"1M",1048576}}};

M<S,S>body_map;
for(auto const&[label,size]:kBodies)
body_map.emplace(S{label},S(size,'X'));

auto make_router=[&]{
Router r;
for(auto const&[label,body]:body_map)
r.get(format("/body/{}",label),[&body](HttpRequest const&){
return HttpResponse::text(body);
});
return r;
};

auto off_srv=start_server(bench_config_zc("off"),make_router());
auto auto_srv=start_server(bench_config_zc("auto"),make_router());

auto const static_dir=fs::temp_directory_path()/"conflux_send_zc_bench_static";
fs::create_directories(static_dir);
for(auto const&[label,size]:kBodies){
auto path=static_dir/format("{}.bin",label);
std::ofstream out{path,std::ios::binary};
S data(size,'Y');
out.write(data.data(),static_cast<std::streamsize>(data.size()));
}
Router static_off_router;
static_off_router.serve_static("/",S{static_dir.string()});
auto static_off_srv=start_server(bench_config_zc("off"),move(static_off_router));

Router static_auto_router;
static_auto_router.serve_static("/",S{static_dir.string()});
auto static_auto_srv=start_server(bench_config_zc("auto"),move(static_auto_router));

V<char>recv_buf(1200000);
auto rb=span<char>{recv_buf};
UP<BenchClient>client;

V<Variant>variants;

for(auto const&[label,size]:kBodies){
auto const req=format("GET /body/{} HTTP/1.1\r\nHost: localhost\r\n\r\n",label);
auto const req_s=S{req};
auto label_s=S{label};

variants.push_back({.name=format("plain/{}/off",label_s),
.setup=[&,p=off_srv.port]{client=make_unique<BenchClient>(p);},
.run=[&,r=req_s]{client->send_all(r);(void)client->recv_response(rb);},
.teardown=[&]{client.reset();}});

variants.push_back({.name=format("plain/{}/zc_auto",label_s),
.setup=[&,p=auto_srv.port]{client=make_unique<BenchClient>(p);},
.run=[&,r=req_s]{client->send_all(r);(void)client->recv_response(rb);},
.teardown=[&]{client.reset();}});
}

for(auto const&[label,size]:kBodies){
auto const req=format("GET /{}.bin HTTP/1.1\r\nHost: localhost\r\n\r\n",label);
auto const req_s=S{req};
auto label_s=S{label};

variants.push_back({.name=format("mapped/{}/off",label_s),
.setup=[&,p=static_off_srv.port]{client=make_unique<BenchClient>(p);},
.run=[&,r=req_s]{client->send_all(r);(void)client->recv_response(rb);},
.teardown=[&]{client.reset();}});

variants.push_back({.name=format("mapped/{}/zc_auto",label_s),
.setup=[&,p=static_auto_srv.port]{client=make_unique<BenchClient>(p);},
.run=[&,r=req_s]{client->send_all(r);(void)client->recv_response(rb);},
.teardown=[&]{client.reset();}});
}

if(!json_out)
println("send_zc_bench: {} iterations, {} warmup\n",iters,warmup);

for(auto const&v:variants){
auto s=run_variant(v,iters,warmup,config_name);
print_variant(s,json_out,v.ops_per_iter);
}

off_srv.server->shutdown();
auto_srv.server->shutdown();
static_off_srv.server->shutdown();
static_auto_srv.server->shutdown();
off_srv.thr.join();
auto_srv.thr.join();
static_off_srv.thr.join();
static_auto_srv.thr.join();
fs::remove_all(static_dir);
}
