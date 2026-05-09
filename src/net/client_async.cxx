module;
#include "client_dns_bridge.hxx"
#include<arpa/inet.h>
#include<cstring>
#include<netdb.h>
#include<netinet/in.h>
#include<sys/socket.h>

export module conflux.net.async_client;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.request;
import conflux.utils;
import conflux.work;
import conflux.uring.completion;
import conflux.socket_io;
import conflux.socket_io.coro;
import conflux.net.client;
namespace async_detail{
using namespace conflux::http;
namespace wroot=conflux::work::root;
[[nodiscard]]S build_host_header(Url const&url){
bool const default_port=(url.scheme=="http"&&url.port==80)||(url.scheme=="https"&&url.port==443);
return default_port?url.host:format("{}:{}",url.host,url.port);
}

enum class ChunkedDecodeStatus:u8{complete,
incomplete,
invalid};
ChunkedDecodeStatus decode_chunked_prefix(SV encoded,S&decoded,SZ&consumed){
for(;;){
auto const line_end=encoded.find("\r\n",consumed);
if(line_end==SV::npos)return ChunkedDecodeStatus::incomplete;
auto size_str=trim(encoded.substr(consumed,line_end-consumed));
if(auto const semi=size_str.find(';');semi!=SV::npos)size_str=trim(size_str.substr(0,semi));
if(size_str.empty())return ChunkedDecodeStatus::invalid;
SZ chunk_size=0;
auto const parsed=from_chars(size_str.data(),size_str.data()+size_str.size(),chunk_size,16);
if(parsed.ec!=errc{}||parsed.ptr!=size_str.data()+size_str.size())return ChunkedDecodeStatus::invalid;
consumed=line_end+2;
if(chunk_size==0){
for(;;){
auto const eol=encoded.find("\r\n",consumed);
if(eol==SV::npos)return ChunkedDecodeStatus::incomplete;
bool const empty=(eol==consumed);
consumed=eol+2;
if(empty)return ChunkedDecodeStatus::complete;
}
}
if(encoded.size()<consumed+chunk_size+2)return ChunkedDecodeStatus::incomplete;
decoded.append(encoded.substr(consumed,chunk_size));
consumed+=chunk_size;
if(encoded.substr(consumed,2)!="\r\n")return ChunkedDecodeStatus::invalid;
consumed+=2;
}
}
wroot::Task<S>async_recv_until(TcpStream&stream,SV delim,SZ max){
S buf;
buf.reserve(min<SZ>(4096,max));
A<u8,4096>tmp{};
while(buf.size()<max){
SZ n;
try{
n=co_await stream.read_borrowed(span<u8>{tmp.data(),tmp.size()});
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
n=co_await stream.read_borrowed(span<u8>{tmp.data(),want});
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
n=co_await stream.read_borrowed(span<u8>{tmp.data(),tmp.size()});
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
n=co_await stream.read_borrowed(span<u8>{tmp.data(),tmp.size()});
}catch(...){co_return false;}
if(n==0)co_return false;
encoded.append(reinterpret_cast<char const*>(tmp.data()),n);
}
}
wroot::Task<HttpResult>do_async_request(
SocketTaskRing&ring,
HttpRequest const&req,
HttpClientOptions const&opts){
auto const&url=req.url();
if(url.scheme=="https")
co_return unexpected(HttpError{
.kind=HttpErrorKind::tls,
.phase=HttpPhase::tls,
.message="async TLS not yet implemented; use send_blocking for HTTPS"});
auto const&timeouts=req.timeouts();
HttpTelemetry tel{};
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
for(auto const*ai=res_raw;ai;ai=ai->ai_next){
client_dns_bridge::Endpoint ep{};
memcpy(ep.addr,ai->ai_addr,min(sizeof(ep.addr),static_cast<SZ>(ai->ai_addrlen)));
ep.addr_len=static_cast<unsigned>(ai->ai_addrlen);
ep.family=(ai->ai_family==AF_INET6)?6:4;
endpoints.push_back(ep);
}
::freeaddrinfo(res_raw);
}
}
tel.dns=chrono::steady_clock::now()-t0;
if(endpoints.empty())
co_return unexpected(HttpError{
.kind=HttpErrorKind::dns,
.phase=HttpPhase::resolve,
.message=format("failed to resolve '{}'",url.host)});
Opt<TcpStream>stream;
HttpError conn_err{.kind=HttpErrorKind::connect,.phase=HttpPhase::connect};
auto const t1=chrono::steady_clock::now();
for(auto const&ep:endpoints){
sockaddr_storage ss{};
memcpy(&ss,ep.addr,ep.addr_len);
int const fam=(ep.family==6)?AF_INET6:AF_INET;
try{
stream.emplace(co_await tcp_connect(ring,fam,ss,static_cast<socklen_t>(ep.addr_len)));
break;
}catch(IoError const&e){
conn_err.os_errno=e.code().value();
conn_err.message=format("connect to '{}:{}' failed: {}",url.host,url.port,e.what());
}catch(...){
conn_err.message=format("connect to '{}:{}' failed",url.host,url.port);
}
}
tel.connect=chrono::steady_clock::now()-t1;
if(!stream)co_return unexpected(conn_err);
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
if(lower=="host"||conflux::http::is_hop_by_hop_header(lower))continue;
merged.set(k,v);
}
for(auto const&[k,v]:merged){
auto const lower=ascii_lower(k);
if(lower=="host"||conflux::http::is_hop_by_hop_header(lower))continue;
wire+=format("{}: {}\r\n",k,v);
}
wire+="Connection: close\r\n";
if(!req.body().empty())
wire+=format("Content-Length: {}\r\n",req.body().size());
wire+="\r\n";
try{
co_await stream->write_all_borrowed(span<u8 const>{reinterpret_cast<u8 const*>(wire.data()),wire.size()});
if(!req.body().empty())
co_await stream->write_all_borrowed(span<u8 const>{reinterpret_cast<u8 const*>(req.body().data()),req.body().size()});
}catch(IoError const&e){
co_return unexpected(HttpError{.kind=HttpErrorKind::write,.phase=HttpPhase::write,.os_errno=e.code().value(),.message="failed to send request"});
}
tel.bytes_sent+=wire.size()+req.body().size();
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
}else if(!conflux::http::is_hop_by_hop_header(kl)){
response.head.headers.set(S{k},S{v});
}
}
pos=(end!=SV::npos)?end+2:headers_str.size();
}
if(has_content_length&&content_length>max_body_sz)
co_return unexpected(HttpError{.kind=HttpErrorKind::body_too_large,.message=format("Content-Length {} exceeds limit {}",content_length,max_body_sz)});
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
}// namespace async_detail
export namespace conflux::http{
[[nodiscard]]conflux::work::root::Task<HttpResult>send_async(
HttpClient const&client,
SocketTaskRing&ring,
HttpRequest const&req){
return async_detail::do_async_request(ring,req,client.options());
}
}
