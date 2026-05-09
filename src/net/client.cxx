module;
#include "client_dns_bridge.hxx"
#include<arpa/inet.h>
#include<cerrno>
#include<climits>
#include<cstring>
#include<fcntl.h>
#include<netdb.h>
#include<netinet/in.h>
#include<poll.h>
#include<sys/socket.h>
#include<unistd.h>
#if CONFLUX_HAS_TLS
#include<openssl/err.h>
#include<openssl/ssl.h>
#include<openssl/x509.h>
#endif

export module conflux.net.client;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.request;
import conflux.utils;
import conflux.work;
import conflux.file_io;
import conflux.socket_io.coro;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
// ─── exported response types ─────────────────────────────────────────────────

export namespace conflux::http{
struct HttpResponseHead{
int status{502};
S status_text{"Bad Gateway"};
HttpFields headers=HttpFields(true);
V<S>set_cookies{};
};
struct HttpResponse{
HttpResponseHead head{};
S body{};
HttpTelemetry telemetry{};

// Phase 2: json() / json_borrowed() accessors.
};
using HttpResult=expected<HttpResponse,HttpError>;
struct HttpClientOptions{
HttpTimeouts default_timeouts{};
bool verify_peer{true};
S ca_bundle_path{};// empty = system default
SZ max_header_bytes{64*1024};
SZ max_body_bytes{16*1024*1024};
SZ max_buffered_bytes{4*1024*1024};
HttpFields default_headers{};
void*resolver{nullptr};// conflux::net::dns::Resolver*; void* avoids exporting dns types
};
}// namespace conflux::http
// ─── legacy free function (kept for existing test compatibility) ──────────────

export[[nodiscard]]Opt<S>decode_chunked_body(SV encoded);
// ─── internal transport ───────────────────────────────────────────────────────

namespace client_detail{
using namespace conflux::http;
struct Connection{
int fd{-1};
bool use_tls{false};
#if CONFLUX_HAS_TLS
Opt<TlsContext>tls_ctx;
Opt<TlsStream>tls_stream;
#endif
};
constexpr A<SV,8>kHopByHop{
"connection",
"keep-alive",
"proxy-authenticate",
"proxy-authorization",
"te",
"trailers",
"transfer-encoding",
"upgrade",
};
[[nodiscard]]bool is_hop_by_hop(
SV name){
return ranges::contains(kHopByHop,name);
}
// Convert chrono::milliseconds to seconds for wait_fd (ceiling, ≥1 if ms>0).
[[nodiscard]]int to_sec(
chrono::milliseconds ms)noexcept{
if(ms.count()<=0)
return-1;// indefinite
long long const s=(ms.count()+999)/1000;
return static_cast<int>(min<long long>(s,INT_MAX));
}
void close_conn(
Connection&conn)noexcept{
#if CONFLUX_HAS_TLS
if(conn.tls_stream){
conn.tls_stream->shutdown_safe();
conn.tls_stream.reset();
}
conn.tls_ctx.reset();
#endif
if(conn.fd>=0){
::close(conn.fd);
conn.fd=-1;
}
}
bool connect_with_timeout(
int fd,
sockaddr const*addr,
socklen_t addrlen,
int timeout_sec){
if(timeout_sec<=0)
return::connect(fd,addr,addrlen)==0;
int const flags=::fcntl(fd,F_GETFL,0);
if(flags<0)
return false;
(void)::fcntl(fd,F_SETFL,flags|O_NONBLOCK);
int const rc=::connect(fd,addr,addrlen);
if(rc==0){
(void)::fcntl(fd,F_SETFL,flags);
return true;
}
if(errno!=EINPROGRESS){
(void)::fcntl(fd,F_SETFL,flags);
return false;
}
bool const ready=wait_fd(fd,POLLOUT,timeout_sec);
(void)::fcntl(fd,F_SETFL,flags);
if(!ready)
return false;
int so_error=0;
socklen_t len=sizeof(so_error);
return::getsockopt(fd,SOL_SOCKET,SO_ERROR,&so_error,&len)==0&&so_error==0;
}

enum class ConnectFailure{
dns,
connect
};
[[nodiscard]]int try_connect_endpoints(
span<client_dns_bridge::Endpoint const>endpoints,
u16 port,
int timeout_sec,
HttpTelemetry&tel){
constexpr auto kConnectAttemptDelay=chrono::milliseconds{250};
auto const t1=chrono::steady_clock::now();
int fd=-1;
int prev_family=-1;
for(auto const&ep:endpoints){
if(fd!=-1){
::close(fd);
fd=-1;
}
int const fam=(ep.family==4)?AF_INET:AF_INET6;
if(prev_family!=-1&&fam!=prev_family)
std::this_thread::sleep_for(kConnectAttemptDelay);
prev_family=fam;
fd=::socket(fam,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP);
if(fd<0)
continue;
sockaddr_storage addr{};
std::memcpy(&addr,ep.addr,min(sizeof(addr),sizeof(ep.addr)));
if(connect_with_timeout(
fd,
reinterpret_cast<sockaddr const*>(&addr),
static_cast<socklen_t>(ep.addr_len),
timeout_sec)){
char buf[INET6_ADDRSTRLEN]{};
if(fam==AF_INET){
auto const*sa4=reinterpret_cast<sockaddr_in const*>(&addr);
inet_ntop(AF_INET,&sa4->sin_addr,buf,sizeof(buf));
tel.peer_addr=format("{}:{}",buf,port);
}else{
auto const*sa6=reinterpret_cast<sockaddr_in6 const*>(&addr);
inet_ntop(AF_INET6,&sa6->sin6_addr,buf,sizeof(buf));
tel.peer_addr=format("[{}]:{}",buf,port);
}
break;
}
::close(fd);
fd=-1;
}
tel.connect=chrono::steady_clock::now()-t1;
return fd;
}
struct EndpointCollector{
V<client_dns_bridge::Endpoint>endpoints;
};
bool collect_endpoint(
void*ctx,
client_dns_bridge::Endpoint const&endpoint)noexcept{
try{
static_cast<EndpointCollector*>(ctx)->endpoints.push_back(endpoint);
return true;
}catch(...){
return false;
}
}
[[nodiscard]]int resolve_and_connect(
SV host,
u16 port,
int timeout_sec,
chrono::milliseconds resolve_timeout,
HttpTelemetry&tel,
ConnectFailure&failure,
S&failure_message,
int&failure_errno,
void*resolver_ptr=nullptr){
if(resolver_ptr){
EndpointCollector collector;
A<char,256>error_buf{};
auto const t0=chrono::steady_clock::now();
bool const resolved=client_dns_bridge::resolve(
resolver_ptr,
host.data(),
host.size(),
port,
resolve_timeout.count(),
&collector,
collect_endpoint,
error_buf.data(),
error_buf.size());
tel.dns=chrono::steady_clock::now()-t0;
if(!resolved){
failure=ConnectFailure::dns;
failure_errno=0;
failure_message=format("DNS resolution failed for '{}': {}",host,error_buf.data());
return-1;
}
int const fd=try_connect_endpoints(collector.endpoints,port,timeout_sec,tel);
if(fd<0){
failure=ConnectFailure::connect;
failure_errno=errno;
failure_message=format("failed to connect to '{}:{}'",host,port);
}
return fd;
}
S const host_str{host};
S const port_str=to_string(port);
addrinfo hints{};
hints.ai_family=AF_UNSPEC;
hints.ai_socktype=SOCK_STREAM;
hints.ai_protocol=IPPROTO_TCP;
hints.ai_flags=AI_ADDRCONFIG;
auto const t0=chrono::steady_clock::now();
addrinfo*res_raw=nullptr;
int const gai_err=::getaddrinfo(host_str.c_str(),port_str.c_str(),&hints,&res_raw);
tel.dns=chrono::steady_clock::now()-t0;
if(gai_err!=0){
failure=ConnectFailure::dns;
failure_errno=0;
failure_message=format("DNS resolution failed for '{}': {}",host,::gai_strerror(gai_err));
return-1;
}
struct AddrInfoDeleter{
void operator()(addrinfo*p)const noexcept{::freeaddrinfo(p);}
};
UPD<addrinfo,AddrInfoDeleter>const res{res_raw};
auto const t1=chrono::steady_clock::now();
int fd=-1;
int prev_family=-1;
for(addrinfo const*ai=res.get();ai!=nullptr;ai=ai->ai_next){
if(fd!=-1){
::close(fd);
fd=-1;
}
if(prev_family!=-1&&ai->ai_family!=prev_family)
std::this_thread::sleep_for(chrono::milliseconds{250});
prev_family=ai->ai_family;
fd=::socket(ai->ai_family,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP);
if(fd<0)
continue;
if(connect_with_timeout(fd,ai->ai_addr,static_cast<socklen_t>(ai->ai_addrlen),timeout_sec)){
char buf[INET6_ADDRSTRLEN]{};
if(ai->ai_family==AF_INET){
auto const*sa4=reinterpret_cast<sockaddr_in const*>(ai->ai_addr);
inet_ntop(AF_INET,&sa4->sin_addr,buf,sizeof(buf));
tel.peer_addr=format("{}:{}",buf,port);
}else{
auto const*sa6=reinterpret_cast<sockaddr_in6 const*>(ai->ai_addr);
inet_ntop(AF_INET6,&sa6->sin6_addr,buf,sizeof(buf));
tel.peer_addr=format("[{}]:{}",buf,port);
}
break;
}
::close(fd);
fd=-1;
}
tel.connect=chrono::steady_clock::now()-t1;
if(fd<0){
failure=ConnectFailure::connect;
failure_errno=errno;
failure_message=format("failed to connect to '{}:{}'",host,port);
}
return fd;
}
bool send_all(
Connection&conn,
SV data,
int timeout_sec){
#if CONFLUX_HAS_TLS
if(conn.use_tls)
return conn.tls_stream->write_all(data,timeout_sec);
#endif
SZ sent=0;
while(sent<data.size()){
if(!wait_fd(conn.fd,POLLOUT,timeout_sec))
return false;
auto const n=::send(conn.fd,data.data()+sent,data.size()-sent,MSG_NOSIGNAL);
if(n<=0)
return false;
sent+=static_cast<SZ>(n);
}
return true;
}
bool recv_some(
Connection&conn,
S&out,
int timeout_sec){
#if CONFLUX_HAS_TLS
if(conn.use_tls)
return conn.tls_stream->read_some(out,timeout_sec);
#endif
A<char,4096>tmp{};
if(!wait_fd(conn.fd,POLLIN,timeout_sec))
return false;
auto const n=::recv(conn.fd,tmp.data(),tmp.size(),0);
if(n<=0)
return false;
out.append(tmp.data(),static_cast<SZ>(n));
return true;
}
// Receive until delimiter or max bytes. Returns accumulated bytes (may contain
// data past the delimiter if overread from the socket).
S recv_until(
Connection&conn,
SV delim,
int timeout_sec,
SZ max){
S buf;
buf.reserve(min<SZ>(4096,max));
while(buf.size()<max){
if(!recv_some(conn,buf,timeout_sec))
break;
if(buf.find(delim)!=S::npos)
break;
}
return buf;
}
bool recv_exact(
Connection&conn,
S&out,
int timeout_sec,
SZ target,
SZ cap){
while(out.size()<target){
if(out.size()>=cap)
return false;// body_too_large
if(!recv_some(conn,out,timeout_sec))
return false;
}
return true;
}
void recv_to_eof(
Connection&conn,
S&out,
int timeout_sec,
SZ cap,
bool&too_large){
too_large=false;
while(recv_some(conn,out,timeout_sec)){
if(out.size()>cap){
too_large=true;
return;
}
}
}

enum class ChunkedDecodeStatus:u8{
complete,
incomplete,
invalid
};
ChunkedDecodeStatus decode_chunked_prefix(
SV encoded,
S&decoded,
SZ&consumed){
for(;;){
auto const line_end=encoded.find("\r\n",consumed);
if(line_end==SV::npos)
return ChunkedDecodeStatus::incomplete;
auto size_str=trim(encoded.substr(consumed,line_end-consumed));
if(auto const semi=size_str.find(';');semi!=SV::npos)
size_str=trim(size_str.substr(0,semi));
if(size_str.empty())
return ChunkedDecodeStatus::invalid;
SZ chunk_size=0;
auto const parsed=from_chars(size_str.data(),size_str.data()+size_str.size(),chunk_size,16);
if(parsed.ec!=errc{}||parsed.ptr!=size_str.data()+size_str.size())
return ChunkedDecodeStatus::invalid;
consumed=line_end+2;
if(chunk_size==0){
for(;;){
auto const eol=encoded.find("\r\n",consumed);
if(eol==SV::npos)
return ChunkedDecodeStatus::incomplete;
bool const empty=(eol==consumed);
consumed=eol+2;
if(empty)
return ChunkedDecodeStatus::complete;
}
}
if(encoded.size()<consumed+chunk_size+2)
return ChunkedDecodeStatus::incomplete;
decoded.append(encoded.substr(consumed,chunk_size));
consumed+=chunk_size;
if(encoded.substr(consumed,2)!="\r\n")
return ChunkedDecodeStatus::invalid;
consumed+=2;
}
}
bool recv_chunked(
Connection&conn,
S&encoded,
S&decoded,
int timeout_sec,
SZ cap,
SZ buf_cap,
bool&too_large){
too_large=false;
decoded.clear();
SZ consumed=0;
for(;;){
switch(decode_chunked_prefix(encoded,decoded,consumed)){
case ChunkedDecodeStatus::complete:return true;
case ChunkedDecodeStatus::invalid:return false;
case ChunkedDecodeStatus::incomplete:
if(decoded.size()>cap||encoded.size()>buf_cap){
too_large=true;
return false;
}
if(!recv_some(conn,encoded,timeout_sec))
return false;
break;
}
}
}
[[nodiscard]]S build_host_header(
Url const&url){
bool const default_port=(url.scheme=="http"&&url.port==80)||(url.scheme=="https"&&url.port==443);
return default_port?url.host:format("{}:{}",url.host,url.port);
}
// ─── async transport helpers ─────────────────────────────────────────────────
namespace wroot=conflux::work::root;
wroot::Task<S>async_recv_until(TcpStream&stream,SV delim,SZ max){
S buf;
buf.reserve(min<SZ>(4096,max));
A<u8,4096>tmp{};
while(buf.size()<max){
SZ n;
try{
n=co_await stream.recv(span<u8>{tmp.data(),tmp.size()});
}catch(...){break;}
if(n==0)break;
buf.append(reinterpret_cast<char const*>(tmp.data()),n);
if(buf.find(delim)!=S::npos)break;
}
co_return buf;
}
wroot::Task<bool>async_recv_exact(TcpStream&stream,S&out,SZ target,SZ cap){
A<u8,4096>tmp{};
while(out.size()<target){
if(out.size()>=cap)co_return false;
auto const want=min(tmp.size(),target-out.size());
SZ n;
try{
n=co_await stream.recv(span<u8>{tmp.data(),want});
}catch(...){co_return false;}
if(n==0)co_return false;
out.append(reinterpret_cast<char const*>(tmp.data()),n);
}
co_return true;
}
wroot::Task<void>async_recv_to_eof(TcpStream&stream,S&out,SZ cap,bool&too_large){
too_large=false;
A<u8,4096>tmp{};
for(;;){
SZ n;
try{
n=co_await stream.recv(span<u8>{tmp.data(),tmp.size()});
}catch(...){break;}
if(n==0)break;
out.append(reinterpret_cast<char const*>(tmp.data()),n);
if(out.size()>cap){
too_large=true;
co_return;
}
}
}
wroot::Task<bool>async_recv_chunked(TcpStream&stream,S&encoded,S&decoded,SZ cap,SZ buf_cap,bool&too_large){
too_large=false;
decoded.clear();
SZ consumed=0;
A<u8,4096>tmp{};
for(;;){
auto const st=decode_chunked_prefix(encoded,decoded,consumed);
if(st==ChunkedDecodeStatus::complete)co_return true;
if(st==ChunkedDecodeStatus::invalid)co_return false;
if(decoded.size()>cap||encoded.size()>buf_cap){
too_large=true;
co_return false;
}
SZ n;
try{
n=co_await stream.recv(span<u8>{tmp.data(),tmp.size()});
}catch(...){co_return false;}
if(n==0)co_return false;
encoded.append(reinterpret_cast<char const*>(tmp.data()),n);
}
}
// Core async transport — plaintext HTTP only (TLS stays blocking for now).
wroot::Task<conflux::http::HttpResult>do_async_request(
FileReader&reader,
conflux::http::HttpRequest const&req,
conflux::http::HttpClientOptions const&opts){
using namespace conflux::http;
auto const&url=req.url();
if(url.scheme=="https")
co_return unexpected(HttpError{
.kind=HttpErrorKind::tls,
.phase=HttpPhase::tls,
.message="async TLS not yet implemented; use send_blocking for HTTPS"});
auto const&timeouts=req.timeouts();
HttpTelemetry tel{};
// Resolve endpoints (blocking; async DNS in Task 5).
V<client_dns_bridge::Endpoint>endpoints;
auto const t0=chrono::steady_clock::now();
if(opts.resolver){
A<char,256>errbuf{};
auto*ctx=&endpoints;
client_dns_bridge::resolve(
opts.resolver,
url.host.data(),url.host.size(),
static_cast<u16>(url.port),
timeouts.resolve.count()>0?timeouts.resolve.count():30000LL,
ctx,
[](void*c,client_dns_bridge::Endpoint const&ep)noexcept{
static_cast<V<client_dns_bridge::Endpoint>*>(c)->push_back(ep);
return true;
},
errbuf.data(),errbuf.size());
}
if(endpoints.empty()){
S const host_str{url.host};
S const port_str=to_string(url.port);
addrinfo hints{};
hints.ai_family=AF_UNSPEC;
hints.ai_socktype=SOCK_STREAM;
hints.ai_protocol=IPPROTO_TCP;
hints.ai_flags=AI_ADDRCONFIG;
addrinfo*res_raw=nullptr;
if(::getaddrinfo(host_str.c_str(),port_str.c_str(),&hints,&res_raw)==0){
struct AddrInfoDeleter{
void operator()(addrinfo*p)const noexcept{::freeaddrinfo(p);}
};
UPD<addrinfo,AddrInfoDeleter>const res{res_raw};
for(auto const*ai=res.get();ai;ai=ai->ai_next){
client_dns_bridge::Endpoint ep{};
memcpy(ep.addr,ai->ai_addr,min(sizeof(ep.addr),static_cast<SZ>(ai->ai_addrlen)));
ep.addr_len=static_cast<unsigned>(ai->ai_addrlen);
ep.family=(ai->ai_family==AF_INET6)?6:4;
endpoints.push_back(ep);
}
}
}
tel.dns=chrono::steady_clock::now()-t0;
if(endpoints.empty())
co_return unexpected(HttpError{
.kind=HttpErrorKind::dns,
.phase=HttpPhase::resolve,
.message=format("failed to resolve '{}'",url.host)});
// Async connect — try endpoints in order.
Opt<TcpStream>stream;
HttpError conn_err{.kind=HttpErrorKind::connect,.phase=HttpPhase::connect};
auto const t1=chrono::steady_clock::now();
for(auto const&ep:endpoints){
sockaddr_storage ss{};
memcpy(&ss,ep.addr,ep.addr_len);
int const fam=(ep.family==6)?AF_INET6:AF_INET;
try{
stream.emplace(co_await tcp_connect(reader,fam,ss,static_cast<socklen_t>(ep.addr_len)));
break;
}catch(FileIoError const&e){
conn_err.os_errno=e.code().value();
conn_err.message=format("connect to '{}:{}' failed: {}",url.host,url.port,e.what());
}catch(...){
conn_err.message=format("connect to '{}:{}' failed",url.host,url.port);
}
}
tel.connect=chrono::steady_clock::now()-t1;
if(!stream)co_return unexpected(conn_err);
// Build wire.
S path=url.path;
if(!url.query.empty()){
path+='?';
path+=url.query;
}
S wire;
wire.reserve(256);
auto const caller_host=req.headers()["host"];
S const host_hdr=caller_host.empty()?build_host_header(url):S{caller_host};
wire+=format("{} {} HTTP/1.1\r\nHost: {}\r\n",req.method(),path,host_hdr);
HttpFields merged=opts.default_headers;
for(auto const&[k,v]:req.headers()){
auto const lower=ascii_lower(k);
if(lower=="host"||is_hop_by_hop(lower))continue;
merged.set(k,v);
}
for(auto const&[k,v]:merged){
auto const lower=ascii_lower(k);
if(lower=="host"||is_hop_by_hop(lower))continue;
wire+=format("{}: {}\r\n",k,v);
}
wire+="Connection: close\r\n";
if(!req.body().empty())
wire+=format("Content-Length: {}\r\n",req.body().size());
wire+="\r\n";
// Send.
try{
co_await stream->send_all(span<u8 const>{reinterpret_cast<u8 const*>(wire.data()),wire.size()});
if(!req.body().empty())
co_await stream->send_all(span<u8 const>{reinterpret_cast<u8 const*>(req.body().data()),req.body().size()});
}catch(FileIoError const&e){
co_return unexpected(HttpError{.kind=HttpErrorKind::write,.phase=HttpPhase::write,.os_errno=e.code().value(),.message="failed to send request"});
}
tel.bytes_sent+=wire.size()+req.body().size();
// Recv headers.
SZ const max_hdr=opts.max_header_bytes;
SZ const max_body_sz=opts.max_body_bytes;
SZ const max_buf=opts.max_buffered_bytes;
S raw=co_await async_recv_until(*stream,"\r\n\r\n",max_hdr+4096);
auto const header_end=raw.find("\r\n\r\n");
if(header_end==S::npos){
if(raw.size()>=max_hdr)
co_return unexpected(HttpError{.kind=HttpErrorKind::header_too_large,.message=format("response headers exceed {} bytes",max_hdr)});
co_return unexpected(HttpError{.kind=HttpErrorKind::protocol,.message="response headers missing CRLFCRLF"});
}
if(header_end>max_hdr)
co_return unexpected(HttpError{.kind=HttpErrorKind::header_too_large,.message=format("response headers exceed {} bytes",max_hdr)});
tel.bytes_received+=raw.size();
// Parse status + headers.
auto const headers_str=SV{raw}.substr(0,header_end);
HttpResponse response;
auto const nl=headers_str.find("\r\n");
auto const status_line=(nl!=SV::npos)?headers_str.substr(0,nl):headers_str;
auto const sp1=status_line.find(' ');
if(sp1==SV::npos)
co_return unexpected(HttpError{.kind=HttpErrorKind::protocol,.message="malformed status line"});
auto const rest=status_line.substr(sp1+1);
auto const sp2=rest.find(' ');
auto const code_sv=(sp2!=SV::npos)?rest.substr(0,sp2):rest;
int status=0;
auto const[ptr,ec]=from_chars(code_sv.data(),code_sv.data()+code_sv.size(),status);
if(ec!=errc{}||status<100||status>999)
co_return unexpected(HttpError{.kind=HttpErrorKind::protocol,.message=format("invalid status code '{}'",code_sv)});
response.head.status=status;
if(sp2!=SV::npos)response.head.status_text=S{rest.substr(sp2+1)};
SZ content_length=0;
bool has_content_length=false;
bool chunked=false;
SZ pos=(nl!=SV::npos)?nl+2:headers_str.size();
while(pos<headers_str.size()){
auto const end=headers_str.find("\r\n",pos);
auto const hdr=(end!=SV::npos)?headers_str.substr(pos,end-pos):headers_str.substr(pos);
auto const colon=hdr.find(':');
if(colon!=SV::npos){
auto k=hdr.substr(0,colon);
auto v=hdr.substr(colon+1);
while(!v.empty()&&(v[0]==' '||v[0]=='\t'))
v.remove_prefix(1);
auto const kl=ascii_lower(k);
auto const vl=ascii_lower(v);
if(kl=="content-length"){
from_chars(v.data(),v.data()+v.size(),content_length);
has_content_length=true;
}else if(kl=="transfer-encoding"&&vl.find("chunked")!=S::npos){
chunked=true;
}else if(kl=="set-cookie"){
response.head.set_cookies.push_back(S{v});
}else if(!is_hop_by_hop(kl)){
response.head.headers.set(S{k},S{v});
}
}
pos=(end!=SV::npos)?end+2:headers_str.size();
}
if(has_content_length&&content_length>max_body_sz)
co_return unexpected(HttpError{.kind=HttpErrorKind::body_too_large,.message=format("Content-Length {} exceeds limit {}",content_length,max_body_sz)});
// Recv body.
response.body=raw.substr(header_end+4);
if(req.method()=="HEAD"){
response.body.clear();
}else if(chunked){
S decoded;
bool too_large=false;
if(!co_await async_recv_chunked(*stream,response.body,decoded,max_body_sz,max_buf,too_large)){
if(too_large)
co_return unexpected(HttpError{.kind=HttpErrorKind::body_too_large,.message=format("chunked body exceeds limit {}",max_body_sz)});
co_return unexpected(HttpError{.kind=HttpErrorKind::read,.phase=HttpPhase::between_bytes,.message="failed to receive chunked body"});
}
tel.bytes_received+=decoded.size();
response.body=move(decoded);
}else if(has_content_length&&content_length>response.body.size()){
if(!co_await async_recv_exact(*stream,response.body,content_length,max_body_sz)){
if(response.body.size()>=max_body_sz)
co_return unexpected(HttpError{.kind=HttpErrorKind::body_too_large,.message=format("body exceeds limit {}",max_body_sz)});
co_return unexpected(HttpError{.kind=HttpErrorKind::read,.phase=HttpPhase::between_bytes,.message="failed to receive body"});
}
tel.bytes_received+=content_length-(raw.size()-(header_end+4));
}else if(!has_content_length&&!chunked){
bool too_large=false;
co_await async_recv_to_eof(*stream,response.body,max_body_sz,too_large);
if(too_large)
co_return unexpected(HttpError{.kind=HttpErrorKind::body_too_large,.message=format("EOF-delimited body exceeds limit {}",max_body_sz)});
tel.bytes_received+=response.body.size();
}
co_await stream->close();
response.telemetry=tel;
co_return response;
}
// Core blocking transport — returns HttpResult.
HttpResult do_blocking_request(
conflux::http::HttpRequest const&req,
HttpClientOptions const&opts){
auto const&url=req.url();
bool const use_tls=(url.scheme=="https");
auto const&timeouts=req.timeouts();
HttpTelemetry tel{};

// DNS + connect.
int const connect_sec=to_sec(timeouts.connect);
ConnectFailure conn_fail{};
S conn_fail_message{};
int conn_fail_errno{0};
int const fd=resolve_and_connect(
url.host,
url.port,
connect_sec,
timeouts.resolve,
tel,
conn_fail,
conn_fail_message,
conn_fail_errno,
opts.resolver);
if(fd<0){
bool const is_dns=conn_fail==ConnectFailure::dns;
return unexpected(
HttpError{
.kind=is_dns?HttpErrorKind::dns:HttpErrorKind::connect,
.phase=is_dns?HttpPhase::resolve:HttpPhase::connect,
.os_errno=conn_fail_errno,
.message=conn_fail_message.empty()?
format("failed to {} '{}:{}'",is_dns?"resolve":"connect",url.host,url.port):
conn_fail_message,
});
}

#if CONFLUX_HAS_TLS
Opt<TlsContext>tls_ctx;
Opt<TlsStream>tls_stream;
#endif

if(use_tls){
#if CONFLUX_HAS_TLS
bool const verify=req.verify_peer()&&opts.verify_peer;
auto const sni_sv=req.server_name().empty()?SV{url.host}:req.server_name();
int const tls_sec=to_sec(timeouts.tls);

try{
tls_ctx.emplace();
}catch(TlsError const&e){
::close(fd);
return unexpected(
HttpError{
.kind=HttpErrorKind::tls,
.phase=HttpPhase::tls,
.message=e.what(),
});
}
tls_ctx->set_verify_peer(verify);
if(verify){
if(!opts.ca_bundle_path.empty())
(void)SSL_CTX_load_verify_locations(tls_ctx->native_handle(),opts.ca_bundle_path.c_str(),nullptr);
else
(void)tls_ctx->set_default_verify_paths();
}

try{
tls_stream.emplace(*tls_ctx,fd);
}catch(TlsError const&e){
::close(fd);
return unexpected(
HttpError{
.kind=HttpErrorKind::tls,
.phase=HttpPhase::tls,
.message=e.what(),
});
}
if(!tls_stream->set_server_name(sni_sv)){
tls_stream->shutdown_safe();
::close(fd);
return unexpected(
HttpError{.kind=HttpErrorKind::tls,.phase=HttpPhase::tls,.message="SNI setup failed"});
}
if(verify&&!tls_stream->set_verify_hostname(sni_sv)){
tls_stream->shutdown_safe();
::close(fd);
return unexpected(
HttpError{
.kind=HttpErrorKind::tls,
.phase=HttpPhase::tls,
.message="hostname verification setup failed"});
}

auto t_tls=chrono::steady_clock::now();
if(!tls_stream->handshake_connect(tls_sec)){
// Capture TLS error details.
long const vr=SSL_get_verify_result(tls_stream->native_handle());
int const alert=ERR_GET_REASON(ERR_get_error());
S verify_reason;
if(verify&&vr!=X509_V_OK){
if(auto const*s=X509_verify_cert_error_string(vr);s!=nullptr)
verify_reason=s;
}
tls_stream->shutdown_safe();
::close(fd);
return unexpected(
HttpError{
.kind=HttpErrorKind::tls,
.phase=HttpPhase::tls,
.tls_alert=alert,
.verify_reason=move(verify_reason),
.message="TLS handshake failed",
});
}
tel.tls=chrono::steady_clock::now()-t_tls;
tel.tls_verified=verify;
tel.negotiated_protocol="https/1.1";// Phase 2: ALPN negotiation

// Capture cipher/version for telemetry.
if(auto const*ssl=tls_stream->native_handle();ssl!=nullptr){
if(auto const*cipher=SSL_get_current_cipher(ssl)){
tel.tls_cipher=SSL_CIPHER_get_name(cipher);
tel.tls_version=SSL_CIPHER_get_version(cipher);
}
}
#else
::close(fd);
return unexpected(
HttpError{
.kind=HttpErrorKind::tls,
.phase=HttpPhase::tls,
.message="TLS not available (built without TLS)"});
#endif
}

// Helper wrappers.
Connection conn;
conn.fd=fd;
#if CONFLUX_HAS_TLS
conn.use_tls=use_tls;
conn.tls_ctx=move(tls_ctx);
conn.tls_stream=move(tls_stream);
#endif

// Build request line + headers.
S path=url.path;
if(!url.query.empty()){
path+='?';
path+=url.query;
}
S wire;
wire.reserve(256);
// Caller-supplied Host overrides URL-derived value (needed for preserve_host).
auto const caller_host=req.headers()["host"];
S const host_hdr=caller_host.empty()?build_host_header(url):S{caller_host};
wire+=format("{} {} HTTP/1.1\r\nHost: {}\r\n",req.method(),path,host_hdr);

// Merge default headers first, then per-request headers override.
HttpFields merged_headers=opts.default_headers;
for(auto const&[k,v]:req.headers()){
auto const lower=ascii_lower(k);
if(lower=="host"||is_hop_by_hop(lower))
continue;
merged_headers.set(k,v);
}
for(auto const&[k,v]:merged_headers){
auto const lower=ascii_lower(k);
if(lower=="host"||is_hop_by_hop(lower))
continue;
wire+=format("{}: {}\r\n",k,v);
}
wire+="Connection: close\r\n";
if(!req.body().empty())
wire+=format("Content-Length: {}\r\n",req.body().size());
wire+="\r\n";

// Send headers.
int const write_sec=to_sec(timeouts.write);
if(!send_all(conn,wire,write_sec)){
close_conn(conn);
return unexpected(
HttpError{
.kind=HttpErrorKind::write,
.phase=HttpPhase::write,
.os_errno=errno,
.message="failed to send request headers"});
}
tel.bytes_sent+=wire.size();

// Send body.
if(!req.body().empty()){
if(!send_all(conn,req.body(),write_sec)){
close_conn(conn);
return unexpected(
HttpError{
.kind=HttpErrorKind::write,
.phase=HttpPhase::write,
.os_errno=errno,
.message="failed to send request body"});
}
tel.bytes_sent+=req.body().size();
}

// Receive response headers.
int const first_byte_sec=to_sec(timeouts.first_byte);
int const between_sec=to_sec(timeouts.between_bytes);
SZ const max_hdr=opts.max_header_bytes;
SZ const max_body=opts.max_body_bytes;
SZ const max_buf=opts.max_buffered_bytes;

auto t_ttfb=chrono::steady_clock::now();
auto raw=recv_until(conn,"\r\n\r\n",first_byte_sec,max_hdr+4096);
auto const header_end=raw.find("\r\n\r\n");

if(header_end==S::npos){
close_conn(conn);
if(raw.size()>=max_hdr)
return unexpected(
HttpError{
.kind=HttpErrorKind::header_too_large,
.message=format("response headers exceed {} bytes",max_hdr)});
return unexpected(HttpError{.kind=HttpErrorKind::protocol,.message="response headers missing CRLFCRLF"});
}
if(header_end>max_hdr){
close_conn(conn);
return unexpected(
HttpError{
.kind=HttpErrorKind::header_too_large,
.message=format("response headers exceed {} bytes",max_hdr)});
}
tel.ttfb=chrono::steady_clock::now()-t_ttfb;

// Parse status line + headers.
auto const headers_str=SV{raw}.substr(0,header_end);
HttpResponse response;
auto const nl=headers_str.find("\r\n");
auto const status_line=(nl!=SV::npos)?headers_str.substr(0,nl):headers_str;
auto const sp1=status_line.find(' ');
if(sp1==SV::npos){
close_conn(conn);
return unexpected(HttpError{.kind=HttpErrorKind::protocol,.message="malformed status line"});
}
auto const rest=status_line.substr(sp1+1);
auto const sp2=rest.find(' ');
auto const code_sv=(sp2!=SV::npos)?rest.substr(0,sp2):rest;
int status=0;
auto const[ptr,ec]=from_chars(code_sv.data(),code_sv.data()+code_sv.size(),status);
if(ec!=errc{}||status<100||status>999){
close_conn(conn);
return unexpected(
HttpError{.kind=HttpErrorKind::protocol,.message=format("invalid status code '{}'",code_sv)});
}
response.head.status=status;
if(sp2!=SV::npos)
response.head.status_text=S{rest.substr(sp2+1)};

SZ content_length=0;
bool has_content_length=false;
bool chunked=false;
SZ pos=(nl!=SV::npos)?nl+2:headers_str.size();
while(pos<headers_str.size()){
auto const end=headers_str.find("\r\n",pos);
auto const hdr=(end!=SV::npos)?headers_str.substr(pos,end-pos):headers_str.substr(pos);
auto const colon=hdr.find(':');
if(colon!=SV::npos){
auto k=hdr.substr(0,colon);
auto v=hdr.substr(colon+1);
while(!v.empty()&&(v[0]==' '||v[0]=='\t'))
v.remove_prefix(1);
auto const kl=ascii_lower(k);
auto const vl=ascii_lower(v);
if(kl=="content-length"){
from_chars(v.data(),v.data()+v.size(),content_length);
has_content_length=true;
}else if(kl=="transfer-encoding"&&vl.find("chunked")!=S::npos){
chunked=true;
}else if(kl=="set-cookie"){
response.head.set_cookies.push_back(S{v});
}else if(!is_hop_by_hop(kl)){
response.head.headers.set(S{k},S{v});
}
}
pos=(end!=SV::npos)?end+2:headers_str.size();
}

// Validate content-length against cap.
if(has_content_length&&content_length>max_body){
close_conn(conn);
return unexpected(
HttpError{
.kind=HttpErrorKind::body_too_large,
.message=format("Content-Length {} exceeds limit {}",content_length,max_body)});
}

// Receive body.
response.body=raw.substr(header_end+4);
tel.bytes_received+=raw.size();

auto t_body=chrono::steady_clock::now();
if(req.method()=="HEAD"){
response.body.clear();
}else if(chunked){
S decoded;
bool too_large=false;
if(!recv_chunked(conn,response.body,decoded,between_sec,max_body,max_buf,too_large)){
close_conn(conn);
if(too_large)
return unexpected(
HttpError{
.kind=HttpErrorKind::body_too_large,
.message=format("chunked body exceeds limit {}",max_body)});
return unexpected(
HttpError{
.kind=HttpErrorKind::read,
.phase=HttpPhase::between_bytes,
.os_errno=errno,
.message="failed to receive chunked body"});
}
tel.bytes_received+=decoded.size();
response.body=move(decoded);
}else if(has_content_length&&content_length>response.body.size()){
if(!recv_exact(conn,response.body,between_sec,content_length,max_body)){
close_conn(conn);
if(response.body.size()>=max_body)
return unexpected(
HttpError{
.kind=HttpErrorKind::body_too_large,
.message=format("body exceeds limit {}",max_body)});
return unexpected(
HttpError{
.kind=HttpErrorKind::read,
.phase=HttpPhase::between_bytes,
.os_errno=errno,
.message="failed to receive body"});
}
tel.bytes_received+=content_length-(raw.size()-(header_end+4));
}else if(!has_content_length&&!chunked){
bool too_large=false;
recv_to_eof(conn,response.body,between_sec,max_body,too_large);
if(too_large){
close_conn(conn);
return unexpected(
HttpError{
.kind=HttpErrorKind::body_too_large,
.message=format("EOF-delimited body exceeds limit {}",max_body)});
}
tel.bytes_received+=response.body.size();
}
tel.body=chrono::steady_clock::now()-t_body;

close_conn(conn);
response.telemetry=tel;
return response;
}
}// namespace client_detail
// ─── decode_chunked_body (legacy free function) ───────────────────────────────

Opt<S>decode_chunked_body(
SV encoded){
S decoded;
SZ consumed=0;
if(client_detail::decode_chunked_prefix(encoded,decoded,consumed)!=client_detail::ChunkedDecodeStatus::complete)
return nullopt;
if(consumed!=encoded.size())
return nullopt;
return decoded;
}
// ─── HttpClient ───────────────────────────────────────────────────────────────

export namespace conflux::http{
class HttpClient{
HttpClientOptions opts_;
[[nodiscard]]static HttpTimeouts resolve_timeouts(
HttpRequest const&req,
HttpClientOptions const&opts){
constexpr HttpTimeouts def{};
auto const&rt=req.timeouts();
auto const&ct=opts.default_timeouts;
return{
.resolve=rt.resolve!=def.resolve?rt.resolve:ct.resolve,
.connect=rt.connect!=def.connect?rt.connect:ct.connect,
.tls=rt.tls!=def.tls?rt.tls:ct.tls,
.write=rt.write!=def.write?rt.write:ct.write,
.first_byte=rt.first_byte!=def.first_byte?rt.first_byte:ct.first_byte,
.between_bytes=rt.between_bytes!=def.between_bytes?rt.between_bytes:ct.between_bytes,
};
}
public:
explicit HttpClient(
HttpClientOptions opts={})
:opts_{move(opts)}{}
[[nodiscard]]HttpClientOptions const&options()const noexcept{return opts_;}
[[nodiscard]]HttpResult send_blocking(
HttpRequest const&req)const{
auto effective_opts=opts_;
return client_detail::do_blocking_request(req,effective_opts);
}
[[nodiscard]]conflux::work::root::Task<HttpResult>send_async(
FileReader&reader,
HttpRequest const&req)const{
return client_detail::do_async_request(reader,req,opts_);
}
};
}// namespace conflux::http
