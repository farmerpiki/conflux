// Plain TU — not a module unit.
#include<arpa/inet.h>
#include<liburing.h>
#include<netinet/in.h>
#include<sys/socket.h>
#include<unistd.h>

#include<catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.socket_io;
import conflux.net.dns;

using namespace conflux::net::dns;
using namespace conflux::net::dns::codec;
namespace{
constexpr u64 pack_ud(
u32 slot,
u32 gen)noexcept{
return(static_cast<u64>(gen)<<32U)|slot;
}
class TempTextFile{
public:
TempTextFile(
SV stem,
SV contents)
:path_{
fs::temp_directory_path()/format("conflux-dns-{}-{}-{}",stem,::getpid(),chrono::steady_clock::now().time_since_epoch().count())}{
std::ofstream out{path_};
out<<contents;
}
~TempTextFile(){
EC ec;
fs::remove(path_,ec);
}
TempTextFile(TempTextFile const&)=delete;
TempTextFile&operator=(TempTextFile const&)=delete;
[[nodiscard]]fs::path const&path()const noexcept{return path_;}
void write(
SV contents)const{
std::ofstream out{path_};
out<<contents;
}
private:
fs::path path_;
};
// ---------------------------------------------------------------------------
// DnsMockServer — thread-based UDP listener on 127.0.0.1:0
// ---------------------------------------------------------------------------

class DnsMockServer{
public:
enum class RespKind:u8{
noerror,
nxdomain,
servfail,
refused,
formerr,
no_response
};
struct MockRR{
V<u8>rdata;// 4 bytes = A, 16 bytes = AAAA
u32 ttl{60};
};
struct Response{
RespKind kind{RespKind::nxdomain};// default: NXDOMAIN
V<MockRR>records;
u16 id_delta{0};
bool wrong_question{false};
bool truncated{false};
};
struct ReceivedQuery{
S name;
u16 qtype{};
chrono::steady_clock::time_point at{};
};
DnsMockServer(){
fd_=::socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,IPPROTO_UDP);
if(fd_<0)
throw RE{"DnsMockServer: socket failed"};
::sockaddr_in sa{};
sa.sin_family=AF_INET;
sa.sin_port=0;
sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
if(::bind(fd_,reinterpret_cast<::sockaddr const*>(&sa),sizeof(sa))<0){
::close(fd_);
throw RE{"DnsMockServer: bind failed"};
}
::sockaddr_in bound{};
socklen_t len=sizeof(bound);
::getsockname(fd_,reinterpret_cast<::sockaddr*>(&bound),&len);
port_=ntohs(bound.sin_port);

::timeval tv{};
tv.tv_usec=50000;// 50 ms receive timeout for clean shutdown
::setsockopt(fd_,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));

thread_=thread{[this]{run();}};
}
~DnsMockServer(){
running_.store(false,memory_order_relaxed);
thread_.join();
::close(fd_);
}
DnsMockServer(DnsMockServer const&)=delete;
DnsMockServer&operator=(DnsMockServer const&)=delete;
[[nodiscard]]u16 port()const noexcept{return port_;}
[[nodiscard]]NameserverEndpoint endpoint()const noexcept{
NameserverEndpoint ns{};
auto*sin=reinterpret_cast<::sockaddr_in*>(&ns.addr);
sin->sin_family=AF_INET;
sin->sin_port=htons(port_);
sin->sin_addr.s_addr=htonl(INADDR_LOOPBACK);
ns.addr_len=sizeof(::sockaddr_in);
ns.port=port_;
return ns;
}
void set_response(
S const&name,
u16 qtype,
Response resp){
SL const lk{mtx_};
responses_[name+':'+to_string(qtype)]=move(resp);
}
[[nodiscard]]V<ReceivedQuery>queries()const{
SL const lk{mtx_};
return received_;
}
void clear_queries(){
SL const lk{mtx_};
received_.clear();
}
[[nodiscard]]SZ query_count(
SV name,
u16 qtype)const{
SL const lk{mtx_};
SZ n=0;
for(auto const&q:received_)
if(q.name==name&&q.qtype==qtype)
++n;
return n;
}
private:
void run(){
A<u8,512>buf{};
::sockaddr_storage src{};
socklen_t src_len{};

while(running_.load(memory_order_relaxed)){
src_len=sizeof(src);
ssize_t const n=
::recvfrom(fd_,buf.data(),buf.size(),0,reinterpret_cast<::sockaddr*>(&src),&src_len);
if(n<=12)
continue;// timeout or malformed
auto const wire=span<u8 const>{buf.data(),static_cast<SZ>(n)};
auto const name=get_qname(wire);
auto const qtype=get_qtype(wire);

{
SL const lk{mtx_};
received_.push_back({name,qtype,chrono::steady_clock::now()});
}

Response resp;
{
SL const lk{mtx_};
auto const key=name+':'+to_string(qtype);
if(auto it=responses_.find(key);it!=responses_.end())
resp=it->second;
}
if(resp.kind==RespKind::no_response)
continue;

auto const reply=build_response(wire,resp);
::sendto(fd_,reply.data(),reply.size(),0,reinterpret_cast<::sockaddr const*>(&src),src_len);
}
}
static S get_qname(
span<u8 const>wire){
S name;
SZ i=12;
while(i<wire.size()&&wire[i]!=0){
u8 const len=wire[i++];
if(!name.empty())
name+='.';
for(u8 j=0;j<len&&i<wire.size();++j){
char c=static_cast<char>(wire[i++]);
if(c>='A'&&c<='Z')
c=static_cast<char>(c+('a'-'A'));
name+=c;
}
}
return name;
}
static u16 get_qtype(
span<u8 const>wire){
SZ i=12;
while(i<wire.size()&&wire[i]!=0)
i+=1U+wire[i];
++i;
if(i+2>wire.size())
return 0;
return static_cast<u16>((static_cast<u16>(wire[i])<<8U)|wire[i+1]);
}
static SZ find_question_end(
span<u8 const>wire){
SZ i=12;
while(i<wire.size()&&wire[i]!=0)
i+=1U+wire[i];
return i+1+4;// null + QTYPE(2) + QCLASS(2)
}
static u8 rcode_for(
RespKind k)noexcept{
switch(k){
case RespKind::noerror:return 0;
case RespKind::nxdomain:return 3;
case RespKind::servfail:return 2;
case RespKind::refused:return 5;
case RespKind::formerr:return 1;
default:return 0;
}
}
static V<u8>build_response(
span<u8 const>query,
Response const&resp){
V<u8>out;
out.reserve(64);

for(int k=0;k<12;++k)
out.push_back(query[static_cast<SZ>(k)]);
if(resp.id_delta!=0){
u16 const id=static_cast<u16>((static_cast<u16>(out[0])<<8U)|out[1]);
u16 const mutated=static_cast<u16>(id+resp.id_delta);
out[0]=static_cast<u8>(mutated>>8U);
out[1]=static_cast<u8>(mutated&0xFFU);
}
out[2]=static_cast<u8>((out[2]&0x01U)|0x80U);// QR=1, keep RD
out[3]=static_cast<u8>(0x80U|rcode_for(resp.kind));// RA=1, RCODE
if(resp.truncated)
out[2]=static_cast<u8>(out[2]|0x02U);// TC=1
u16 const ancount=(resp.kind==RespKind::noerror)?static_cast<u16>(resp.records.size()):u16{0};
out[6]=static_cast<u8>(ancount>>8U);
out[7]=static_cast<u8>(ancount&0xFFU);
out[8]=out[9]=out[10]=out[11]=0;

SZ const qend=find_question_end(query);
for(SZ k=12;k<qend&&k<query.size();++k)
out.push_back(query[k]);
if(resp.wrong_question&&out.size()>=qend){
// Corrupt the echoed QTYPE while leaving the packet otherwise parseable.
out[qend-4]=0x00;
out[qend-3]=0x0F;// MX
}

if(resp.kind==RespKind::noerror){
for(auto const&rr:resp.records){
out.push_back(0xC0);
out.push_back(0x0C);// pointer to question QNAME
u16 const rtype=(rr.rdata.size()==4)?u16{1}:u16{28};
out.push_back(static_cast<u8>(rtype>>8U));
out.push_back(static_cast<u8>(rtype&0xFFU));
out.push_back(0x00);
out.push_back(0x01);// CLASS IN
out.push_back(static_cast<u8>((rr.ttl>>24U)&0xFFU));
out.push_back(static_cast<u8>((rr.ttl>>16U)&0xFFU));
out.push_back(static_cast<u8>((rr.ttl>>8U)&0xFFU));
out.push_back(static_cast<u8>(rr.ttl&0xFFU));
auto const rdlen=static_cast<u16>(rr.rdata.size());
out.push_back(static_cast<u8>(rdlen>>8U));
out.push_back(static_cast<u8>(rdlen&0xFFU));
for(auto b:rr.rdata)
out.push_back(b);
}
}
return out;
}
int fd_{-1};
u16 port_{};
thread thread_;
Atom<bool>running_{true};
mutable mutex mtx_;
UM<S,Response>responses_;
V<ReceivedQuery>received_;
};
ResolveOptions mock_opts(
DnsMockServer const&mock){
ResolveOptions opts;
opts.override_nameservers={mock.endpoint()};
return opts;
}
struct RingGuard{
::io_uring ring{};
CompletionTable ct;
bool ok{false};

RingGuard()=default;
~RingGuard(){
if(ok)
::io_uring_queue_exit(&ring);
}
RingGuard(RingGuard const&)=delete;
RingGuard&operator=(RingGuard const&)=delete;
RingGuard(RingGuard&&)=delete;
RingGuard&operator=(RingGuard&&)=delete;
static UP<RingGuard>make(
unsigned entries=32){
auto g=make_unique<RingGuard>();
g->ok=(::io_uring_queue_init(entries,&g->ring,0)==0);
return g;
}
};
// SocketTaskRing-owning ring guard for caller-ring tests.
struct StrRingGuard{
::io_uring ring{};
CompletionTable ct;
SocketTaskRing str;
bool ring_ok{false};
StrRingGuard()
:str{SocketRawRing{&ring},ct,[](u32 s,u32 g)noexcept->u64{return pack_ud(s,g);}}{}
~StrRingGuard(){
if(ring_ok)::io_uring_queue_exit(&ring);
}
StrRingGuard(StrRingGuard const&)=delete;
StrRingGuard&operator=(StrRingGuard const&)=delete;
static UP<StrRingGuard>make(unsigned entries=64){
auto g=make_unique<StrRingGuard>();
if(::io_uring_queue_init(entries,&g->ring,0)<0)return{};
g->ring_ok=true;
return g;
}
};
template<typename T>
T block_on_str(
StrRingGuard&g,
conflux::work::root::Task<T>task,
chrono::milliseconds budget=chrono::milliseconds{5000}){
using namespace conflux::work::root;
struct Slot{
atomic_flag done{};
EP err{};
[[no_unique_address]]std::conditional_t<std::is_void_v<T>,std::monostate,Opt<T>>value{};
};
auto slot=make_shared<Slot>();
auto jh=make_shared<TaskJoinHandle<T>>(into_join_handle(move(task)));
jh->control().set_on_ready_or_run([slot,jh]()noexcept{
try{
auto outcome=join(move(*jh));
if(outcome.is_failure())
slot->err=move(outcome).failure().error;
else if(outcome.is_cancelled())
slot->err=make_exception_ptr(RE{"task cancelled"});
else if constexpr(!std::is_void_v<T>)
slot->value.emplace(move(outcome).success().value);
}catch(...){slot->err=current_exception();}
slot->done.test_and_set(memory_order_release);
});
auto*raw=&g.ring;
auto*ct=&g.ct;
auto const deadline=chrono::steady_clock::now()+budget;
while(!slot->done.test(memory_order_acquire)){
::io_uring_cqe*cqe=nullptr;
__kernel_timespec ts{.tv_sec=1,.tv_nsec=0};
int const rc=::io_uring_submit_and_wait_timeout(raw,&cqe,1,&ts,nullptr);
if(rc==-ETIME){
if(chrono::steady_clock::now()>deadline)
throw RE{"block_on_str: budget exhausted"};
continue;
}
if(rc==-EINTR)continue;
if(rc>=0&&cqe==nullptr)continue;
A<::io_uring_cqe*,32>batch{};
for(;;){
unsigned const n=::io_uring_peek_batch_cqe(raw,batch.data(),static_cast<unsigned>(batch.size()));
if(n==0)break;
for(unsigned i=0;i<n;++i){
auto const*c=batch[static_cast<SZ>(i)];
auto const ud=c->user_data;
ct->dispatch(static_cast<u32>(ud&0xFFFFFFFFU),static_cast<u32>(ud>>32U),c->res,c->flags);
}
::io_uring_cq_advance(raw,n);
if(slot->done.test(memory_order_acquire))break;
}
}
if(slot->err)rethrow_exception(slot->err);
if constexpr(!std::is_void_v<T>)return move(*slot->value);
}
}// namespace
// ---------------------------------------------------------------------------
// Tests: native_udp backend via resolve_blocking (spins its own temp ring)
// ---------------------------------------------------------------------------

TEST_CASE(
"dns: resolve A record via native_udp backend",
"[dns][resolver][native]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response(
"a.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={192,168,1,1},.ttl=300}},
});

auto result=r.resolve_blocking("a.test",80,mock_opts(mock));
REQUIRE(result.has_value());
CHECK_FALSE(result->endpoints.empty());
CHECK(result->endpoints[0].family==AddressFamily::v4);
auto const&sin=*reinterpret_cast<::sockaddr_in const*>(&result->endpoints[0].addr);
CHECK(sin.sin_addr.s_addr==htonl(0xC0A80101U));// 192.168.1.1
CHECK(ntohs(sin.sin_port)==80);
}
TEST_CASE(
"dns: hosts file shortcut returns endpoint without querying nameserver",
"[dns][resolver][hosts]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
TempTextFile const hosts{
"hosts",
"203.0.113.9 HostAlias\n" "2001:db8::9 HostAlias\n"};
ResolverOptions resolver_opts;
resolver_opts.hosts_file=hosts.path();
Resolver r{&g->ring,&g->ct,pack_ud,move(resolver_opts)};

DnsMockServer const mock;
auto opts=mock_opts(mock);
auto result=r.resolve_blocking("HostAlias",2525,opts);

REQUIRE(result.has_value());
CHECK(result->from_hosts_file);
REQUIRE(result->endpoints.size()==2);
CHECK(mock.queries().empty());
for(auto const&ep:result->endpoints){
if(ep.family==AddressFamily::v4){
auto const&sin=*reinterpret_cast<::sockaddr_in const*>(&ep.addr);
CHECK(ntohs(sin.sin_port)==2525);
}else{
auto const&sin6=*reinterpret_cast<::sockaddr_in6 const*>(&ep.addr);
CHECK(ntohs(sin6.sin6_port)==2525);
}
}
}
TEST_CASE(
"dns: resolv.conf nameserver is used when no override is supplied",
"[dns][resolver][resolv-conf]"){
auto g=RingGuard::make();
REQUIRE(g->ok);

DnsMockServer mock;
mock.set_response(
"resolv-conf.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,7,0,1},.ttl=60}},
});

TempTextFile const resolv{"resolv",format("nameserver 127.0.0.1:{}\n",mock.port())};
ResolverOptions resolver_opts;
resolver_opts.resolv_conf=resolv.path();
Resolver r{&g->ring,&g->ct,pack_ud,move(resolver_opts)};

ResolveOptions opts;
opts.allow_v6=false;
auto result=r.resolve_blocking("resolv-conf.test",80,opts);

REQUIRE(result.has_value());
REQUIRE(result->endpoints.size()==1);
CHECK(mock.query_count("resolv-conf.test",1)==1);
}
TEST_CASE(
"dns: resolv.conf search domain is applied for short blocking lookup",
"[dns][resolver][resolv-conf]"){
auto g=RingGuard::make();
REQUIRE(g->ok);

DnsMockServer mock;
mock.set_response(
"www.example.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,7,0,2},.ttl=60}},
});

TempTextFile const resolv{
"resolv-search",
format(
"nameserver 127.0.0.1:{}\n" "search example.test\n" "options ndots:2\n",
mock.port())};
ResolverOptions resolver_opts;
resolver_opts.resolv_conf=resolv.path();
Resolver r{&g->ring,&g->ct,pack_ud,move(resolver_opts)};

ResolveOptions opts;
opts.allow_v6=false;
opts.query_timeout=chrono::milliseconds{50};
auto result=r.resolve_blocking("www",80,opts);

REQUIRE(result.has_value());
REQUIRE(result->endpoints.size()==1);
CHECK(mock.query_count("www.example.test",1)==1);
CHECK(mock.query_count("www",1)==0);
}
TEST_CASE(
"dns: resolv.conf attempts retries native nameserver",
"[dns][resolver][resolv-conf]"){
auto g=RingGuard::make();
REQUIRE(g->ok);

DnsMockServer mock;
mock.set_response(
"attempts.test",
1,
{
.kind=DnsMockServer::RespKind::no_response,
.records={},
});

TempTextFile const resolv{
"resolv-attempts",
format(
"nameserver 127.0.0.1:{}\n" "options attempts:2\n",
mock.port())};
ResolverOptions resolver_opts;
resolver_opts.resolv_conf=resolv.path();
Resolver r{&g->ring,&g->ct,pack_ud,move(resolver_opts)};

ResolveOptions opts;
opts.allow_v6=false;
opts.query_timeout=chrono::milliseconds{50};
auto result=r.resolve_blocking("attempts.test",80,opts);

REQUIRE(result.has_value());
CHECK(result->endpoints.empty());
CHECK(mock.query_count("attempts.test",1)==2);
}
TEST_CASE(
"dns: reload refreshes resolv.conf nameservers",
"[dns][resolver][resolv-conf]"){
auto g=RingGuard::make();
REQUIRE(g->ok);

DnsMockServer first;
first.set_response(
"before-reload.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,7,0,3},.ttl=60}},
});
DnsMockServer second;
second.set_response(
"after-reload.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,7,0,4},.ttl=60}},
});

TempTextFile const resolv{"resolv-reload",format("nameserver 127.0.0.1:{}\n",first.port())};
ResolverOptions resolver_opts;
resolver_opts.resolv_conf=resolv.path();
resolver_opts.cache_capacity=0;
Resolver r{&g->ring,&g->ct,pack_ud,move(resolver_opts)};

ResolveOptions opts;
opts.allow_v6=false;
auto before=r.resolve_blocking("before-reload.test",80,opts);
REQUIRE(before.has_value());
CHECK(first.query_count("before-reload.test",1)==1);

resolv.write(format("nameserver 127.0.0.1:{}\n",second.port()));
r.reload();

auto after=r.resolve_blocking("after-reload.test",80,opts);
REQUIRE(after.has_value());
CHECK(second.query_count("after-reload.test",1)==1);
CHECK(first.query_count("after-reload.test",1)==0);
}
TEST_CASE(
"dns: AAAA record returned first (v6 preference)",
"[dns][resolver][native]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response(
"dual.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,0,0,1},.ttl=60}},
});
mock.set_response(
"dual.test",
28,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1},.ttl=60}},
});

auto result=r.resolve_blocking("dual.test",443,mock_opts(mock));
REQUIRE(result.has_value());
REQUIRE(result->endpoints.size()==2);
CHECK(result->endpoints[0].family==AddressFamily::v6);
CHECK(result->endpoints[1].family==AddressFamily::v4);
}
TEST_CASE(
"dns: A record returned first when v4 is preferred",
"[dns][resolver][native]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response(
"prefer4.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,0,0,4},.ttl=60}},
});
mock.set_response(
"prefer4.test",
28,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,4},.ttl=60}},
});

auto opts=mock_opts(mock);
opts.prefer=AddressFamily::v4;
auto result=r.resolve_blocking("prefer4.test",443,opts);
REQUIRE(result.has_value());
REQUIRE(result->endpoints.size()==2);
CHECK(result->endpoints[0].family==AddressFamily::v4);
CHECK(result->endpoints[1].family==AddressFamily::v6);
}
TEST_CASE(
"dns: A+AAAA both queried in parallel",
"[dns][resolver][native][happy-eyeballs]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response(
"parallel.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,1,1,1},.ttl=60}},
});
mock.set_response(
"parallel.test",
28,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,2},.ttl=60}},
});

auto result=r.resolve_blocking("parallel.test",80,mock_opts(mock));
REQUIRE(result.has_value());
CHECK(mock.query_count("parallel.test",1)==1);
CHECK(mock.query_count("parallel.test",28)==1);
}
TEST_CASE(
"dns: AAAA nxdomain with A success returns v4",
"[dns][resolver][native][happy-eyeballs]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response(
"v4only.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={172,16,0,1},.ttl=60}},
});

auto result=r.resolve_blocking("v4only.test",80,mock_opts(mock));
REQUIRE(result.has_value());
REQUIRE(result->endpoints.size()==1);
CHECK(result->endpoints[0].family==AddressFamily::v4);
}
TEST_CASE(
"dns: both A and AAAA nxdomain returns nxdomain error",
"[dns][resolver][native]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer const mock;// default: NXDOMAIN for everything

auto result=r.resolve_blocking("no.such.domain",80,mock_opts(mock));
// For native_udp, resolve_blocking catches DnsError thrown from block_on
// and returns unexpected{} with DnsErrorKind::nxdomain.
REQUIRE_FALSE(result.has_value());
CHECK(result.error().kind==DnsErrorKind::nxdomain);
}
TEST_CASE(
"dns: cache hit on second lookup sends no new queries",
"[dns][resolver][native][cache]"){
auto g=RingGuard::make();
REQUIRE(g);
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud,ResolverOptions{.cache_capacity=16}};

DnsMockServer mock;
mock.set_response(
"cached.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,0,0,2},.ttl=300}},
});

auto opts=mock_opts(mock);

auto r1=r.resolve_blocking("cached.test",80,opts);
REQUIRE(r1.has_value());
CHECK_FALSE(r1->from_cache);
SZ const after_first=mock.queries().size();

auto r2=r.resolve_blocking("cached.test",80,opts);
REQUIRE(r2.has_value());
CHECK(r2->from_cache);
CHECK(mock.queries().size()==after_first);// no new queries
CHECK_FALSE(r2->endpoints.empty());
}
TEST_CASE(
"dns: TTL in cache entry matches server response",
"[dns][resolver][native][cache]"){
auto g=RingGuard::make();
REQUIRE(g);
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud,ResolverOptions{.cache_capacity=16}};

DnsMockServer mock;
mock.set_response(
"ttl.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,0,0,3},.ttl=42}},
});

auto r1=r.resolve_blocking("ttl.test",80,mock_opts(mock));
REQUIRE(r1.has_value());
CHECK(r1->suggested_ttl==chrono::seconds{42});
}
TEST_CASE(
"dns: deadlock detection — resolve_blocking on owned ring",
"[dns][resolver][deadlock]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

CurrentResolverScope const scope{&r};
auto result=r.resolve_blocking("example.com",80);
REQUIRE_FALSE(result.has_value());
CHECK(result.error().kind==DnsErrorKind::cannot_block_on_owned_ring);
}
TEST_CASE(
"dns: servfail does not propagate as nxdomain",
"[dns][resolver][native]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
DnsMockServer::Response const sf{DnsMockServer::RespKind::servfail,{}};
mock.set_response("srv.test",1,sf);
mock.set_response("srv.test",28,sf);

auto result=r.resolve_blocking("srv.test",80,mock_opts(mock));
// SERVFAIL maps to BatchFailReason::network; both-fail → empty result, no nxdomain thrown.
if(result.has_value())
CHECK(result->endpoints.empty());
else
CHECK(result.error().kind!=DnsErrorKind::nxdomain);
}
TEST_CASE(
"dns: truncated UDP with failed TCP fallback returns truncated error",
"[dns][resolver][native][tcp]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response(
"tcp-fallback.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={},
.truncated=true,
});

auto opts=mock_opts(mock);
opts.allow_v6=false;
opts.query_timeout=chrono::milliseconds{50};
auto result=r.resolve_blocking("tcp-fallback.test",80,opts);
REQUIRE_FALSE(result.has_value());
CHECK(result.error().kind==DnsErrorKind::truncated);
}
TEST_CASE(
"dns: response with mismatched id is ignored",
"[dns][resolver][native][validation]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response(
"bad-id.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,9,0,1},.ttl=60}},
.id_delta=1,
});

auto opts=mock_opts(mock);
opts.allow_v6=false;
opts.query_timeout=chrono::milliseconds{50};

auto result=r.resolve_blocking("bad-id.test",80,opts);
REQUIRE(result.has_value());
CHECK(result->endpoints.empty());
CHECK(mock.query_count("bad-id.test",1)==1);
}
TEST_CASE(
"dns: response with mismatched question is ignored",
"[dns][resolver][native][validation]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response(
"bad-question.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,9,0,2},.ttl=60}},
.wrong_question=true,
});

auto opts=mock_opts(mock);
opts.allow_v6=false;
opts.query_timeout=chrono::milliseconds{50};

auto result=r.resolve_blocking("bad-question.test",80,opts);
REQUIRE(result.has_value());
CHECK(result->endpoints.empty());
CHECK(mock.query_count("bad-question.test",1)==1);
}
TEST_CASE(
"dns: total timeout caps native query wait",
"[dns][resolver][native][timeout]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response(
"silent.test",
1,
{
.kind=DnsMockServer::RespKind::no_response,
.records={},
});

auto opts=mock_opts(mock);
opts.allow_v6=false;
opts.query_timeout=chrono::milliseconds{1000};
opts.total_timeout=chrono::milliseconds{50};

auto const start=chrono::steady_clock::now();
auto result=r.resolve_blocking("silent.test",80,opts);
auto const elapsed=chrono::steady_clock::now()-start;

REQUIRE(result.has_value());
CHECK(result->endpoints.empty());
CHECK(elapsed<chrono::milliseconds{500});
}
TEST_CASE(
"dns: native resolver tries next nameserver after empty response",
"[dns][resolver][native][nameserver]"){
auto g=RingGuard::make();
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer silent;
silent.set_response(
"fallback.test",
1,
{
.kind=DnsMockServer::RespKind::no_response,
.records={},
});
DnsMockServer good;
good.set_response(
"fallback.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,8,0,1},.ttl=60}},
});

ResolveOptions opts;
opts.override_nameservers={silent.endpoint(),good.endpoint()};
opts.allow_v6=false;
opts.query_timeout=chrono::milliseconds{50};

auto result=r.resolve_blocking("fallback.test",80,opts);
REQUIRE(result.has_value());
REQUIRE(result->endpoints.size()==1);
CHECK(result->endpoints[0].family==AddressFamily::v4);
CHECK(silent.query_count("fallback.test",1)==1);
CHECK(good.query_count("fallback.test",1)==1);
}
// decode for block_on — matches pack_ud: gen in upper 32, slot in lower 32.
struct PackUdDecode{
P<u32,u32>operator()(
u64 ud)const noexcept{
return{static_cast<u32>(ud&0xFFFFFFFFU),static_cast<u32>(ud>>32U)};
}
};
TEST_CASE(
"dns: in-flight coalescing — two concurrent resolves send one query",
"[dns][resolver][native][coalesce]"){
auto g=RingGuard::make();
REQUIRE(g);
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud,ResolverOptions{.cache_capacity=16}};

DnsMockServer mock;
mock.set_response(
"coalesce.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,0,0,5},.ttl=60}},
});
mock.set_response(
"coalesce.test",
28,
{
.kind=DnsMockServer::RespKind::nxdomain,
.records={},
});

ResolveOptions const opts=mock_opts(mock);

// Both calls happen before the ring is pumped → second attaches as waiter.
using RR=ResolveResult;
auto first=r.resolve("coalesce.test",80,opts);
auto second=r.resolve("coalesce.test",80,opts);
auto[res1,res2]=block_on<Tup<RR,RR>>(
*r.file_reader(),
join_all(move(first),move(second)),
std::make_optional(chrono::milliseconds{5000}),
PackUdDecode{});

CHECK_FALSE(res1.endpoints.empty());
CHECK_FALSE(res2.endpoints.empty());
CHECK_FALSE(res1.from_coalesced);
CHECK(res2.from_coalesced);

// Mock server should have received only one A query and one AAAA query —
// not doubled.
CHECK(mock.query_count("coalesce.test",1)==1);
CHECK(mock.query_count("coalesce.test",28)==1);
}
TEST_CASE(
"dns: async resolve applies resolv.conf search domain",
"[dns][resolver][native][resolv-conf][async]"){
auto g=RingGuard::make();
REQUIRE(g);
REQUIRE(g->ok);

DnsMockServer mock;
mock.set_response(
"www.example.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,8,0,9},.ttl=60}},
});

TempTextFile const resolv{
"resolv-async-search",
format(
"nameserver 127.0.0.1:{}\n" "search example.test\n" "options ndots:2\n",
mock.port())};
ResolverOptions resolver_opts;
resolver_opts.resolv_conf=resolv.path();
Resolver r{&g->ring,&g->ct,pack_ud,move(resolver_opts)};

ResolveOptions opts;
opts.allow_v6=false;
opts.query_timeout=chrono::milliseconds{100};
auto result=block_on<ResolveResult>(
*r.file_reader(),
r.resolve("www",80,opts),
std::make_optional(chrono::milliseconds{5000}),
PackUdDecode{});

REQUIRE(result.endpoints.size()==1);
CHECK(mock.query_count("www.example.test",1)==1);
CHECK(mock.query_count("www",1)==0);
}
TEST_CASE(
"dns: NXDOMAIN is cached with negative TTL",
"[dns][resolver][native][cache]"){
auto g=RingGuard::make();
REQUIRE(g);
REQUIRE(g->ok);
Resolver r{
&g->ring,
&g->ct,
pack_ud,
ResolverOptions{.cache_capacity=16,.cache_negative_ttl=chrono::seconds{30}}};

DnsMockServer const mock;// default: NXDOMAIN for everything

auto const opts=mock_opts(mock);

auto r1=r.resolve_blocking("negcache.test",80,opts);
REQUIRE_FALSE(r1.has_value());
CHECK(r1.error().kind==DnsErrorKind::nxdomain);

SZ const queries_after_first=mock.queries().size();

// Second call must hit the negative cache — no new queries sent.
auto r2=r.resolve_blocking("negcache.test",80,opts);
REQUIRE_FALSE(r2.has_value());
CHECK(r2.error().kind==DnsErrorKind::nxdomain);
CHECK(mock.queries().size()==queries_after_first);
}
TEST_CASE(
"dns: LRU eviction at capacity",
"[dns][resolver][native][cache]"){
auto g=RingGuard::make();
REQUIRE(g);
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud,ResolverOptions{.cache_capacity=1}};

DnsMockServer mock;
mock.set_response(
"lru1.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,0,1,1},.ttl=300}},
});
mock.set_response(
"lru2.test",
1,
{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,0,1,2},.ttl=300}},
});

auto opts=mock_opts(mock);

// lru1 fills the single-slot cache.
auto r1a=r.resolve_blocking("lru1.test",80,opts);
REQUIRE(r1a.has_value());
CHECK_FALSE(r1a->from_cache);

// lru2 evicts lru1.
auto r2=r.resolve_blocking("lru2.test",80,opts);
REQUIRE(r2.has_value());
CHECK_FALSE(r2->from_cache);

SZ const queries_before=mock.queries().size();

// lru1 must be re-queried (evicted), not served from cache.
auto r1b=r.resolve_blocking("lru1.test",80,opts);
REQUIRE(r1b.has_value());
CHECK_FALSE(r1b->from_cache);
CHECK(mock.queries().size()>queries_before);
}
// ---------------------------------------------------------------------------
// Tests: resolve(SocketTaskRing&, ...) — caller-provided ring
// ---------------------------------------------------------------------------

TEST_CASE(
"dns: resolve(ring) drives query on caller-supplied ring, not resolver-owned ring",
"[dns][resolver][native][async_ring]"){
auto g=RingGuard::make();
REQUIRE(g);
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response("ext-ring.test",1,{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,1,2,3},.ttl=60}},
});

auto gb=StrRingGuard::make();
REQUIRE(gb);
REQUIRE(gb->ring_ok);

ResolveOptions opts=mock_opts(mock);
opts.allow_v6=false;
auto result=block_on_str<ResolveResult>(
*gb,
r.resolve(gb->str,"ext-ring.test",80,opts),
chrono::milliseconds{5000});

REQUIRE_FALSE(result.endpoints.empty());
CHECK(result.endpoints[0].family==AddressFamily::v4);
CHECK(mock.query_count("ext-ring.test",1)==1);
}
TEST_CASE(
"dns: resolve(ring) does not coalesce with in-flight query on a different ring",
"[dns][resolver][native][async_ring][coalesce]"){
auto g=RingGuard::make();
REQUIRE(g);
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response("anti-coalesce.test",1,{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,4,5,6},.ttl=60}},
});

auto gb=StrRingGuard::make();
REQUIRE(gb);
REQUIRE(gb->ring_ok);

ResolveOptions opts=mock_opts(mock);
opts.allow_v6=false;

// Start a query on the resolver-owned ring (pumped later).
auto first=r.resolve("anti-coalesce.test",80,opts);
// Start a query on ring B — different InFlightKey, must not coalesce with first.
auto second=block_on_str<ResolveResult>(
*gb,
r.resolve(gb->str,"anti-coalesce.test",80,opts),
chrono::milliseconds{5000});

REQUIRE_FALSE(second.endpoints.empty());
CHECK_FALSE(second.from_coalesced);

// Now pump the resolver-owned ring so first completes cleanly.
auto first_result=block_on<ResolveResult>(
*r.file_reader(),
move(first),
std::make_optional(chrono::milliseconds{5000}),
PackUdDecode{});
REQUIRE_FALSE(first_result.endpoints.empty());
CHECK_FALSE(first_result.from_coalesced);

// Both rings sent independent queries — mock received at least 2.
CHECK(mock.query_count("anti-coalesce.test",1)>=2);
}
TEST_CASE(
"dns: two resolve(ring) calls for same host coalesce on same ring",
"[dns][resolver][native][async_ring][coalesce]"){
auto g=RingGuard::make();
REQUIRE(g);
REQUIRE(g->ok);
Resolver r{&g->ring,&g->ct,pack_ud};

DnsMockServer mock;
mock.set_response("coalesce-b.test",1,{
.kind=DnsMockServer::RespKind::noerror,
.records={{.rdata={10,7,8,9},.ttl=60}},
});
mock.set_response("coalesce-b.test",28,{.kind=DnsMockServer::RespKind::nxdomain});

auto gb=StrRingGuard::make();
REQUIRE(gb);
REQUIRE(gb->ring_ok);

ResolveOptions const opts=mock_opts(mock);

// Both calls before ring B is pumped — second attaches as waiter.
using RR=ResolveResult;
auto first=r.resolve(gb->str,"coalesce-b.test",80,opts);
auto second=r.resolve(gb->str,"coalesce-b.test",80,opts);
auto[res1,res2]=block_on_str<Tup<RR,RR>>(
*gb,
join_all(move(first),move(second)),
chrono::milliseconds{5000});

CHECK_FALSE(res1.endpoints.empty());
CHECK_FALSE(res2.endpoints.empty());
CHECK_FALSE(res1.from_coalesced);
CHECK(res2.from_coalesced);
// Only one A and one AAAA query should have been sent.
CHECK(mock.query_count("coalesce-b.test",1)==1);
CHECK(mock.query_count("coalesce-b.test",28)==1);
}
TEST_CASE(
"dns: resolve(ring) on nss_thread resolver rejects non-literal host, passes early exits",
"[dns][resolver][nss_thread][async_ring]"){
using namespace conflux;
WorkPool pool{WorkPoolOptions{.threads=1}};

TempTextFile const hosts{"hosts-ext-ring","127.0.0.1 hosts-hit.test\n"};
ResolverOptions ropts;
ropts.enable_etc_hosts=true;
ropts.hosts_file=hosts.path();
Resolver r{pool,move(ropts)};

auto gb=StrRingGuard::make();
REQUIRE(gb);
REQUIRE(gb->ring_ok);

// IP literal always succeeds regardless of backend.
auto lit=block_on_str<ResolveResult>(
*gb,
r.resolve(gb->str,"192.168.0.1",80),
chrono::milliseconds{1000});
REQUIRE(lit.endpoints.size()==1);
CHECK(lit.endpoints[0].family==AddressFamily::v4);

// /etc/hosts hit succeeds without hitting the backend.
auto hosts_hit=block_on_str<ResolveResult>(
*gb,
r.resolve(gb->str,"hosts-hit.test",80),
chrono::milliseconds{1000});
REQUIRE_FALSE(hosts_hit.endpoints.empty());
CHECK(hosts_hit.from_hosts_file);

// Non-literal, non-cached host must return not_implemented.
try{
block_on_str<ResolveResult>(
*gb,
r.resolve(gb->str,"example.test",80),
chrono::milliseconds{1000});
FAIL("expected DnsError");
}catch(DnsError const&e){
CHECK(e.kind==DnsErrorKind::not_implemented);
}
}
