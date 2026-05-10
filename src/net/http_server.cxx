module;
#include<cstddef>// must precede openssl/nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" blocks re-include c++config.h

#include<arpa/inet.h>
#include<liburing.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#if CONFLUX_HAS_TLS
#include<openssl/err.h>
#include<openssl/ssl.h>
#endif
#if CONFLUX_HAS_HTTP2
#include<nghttp2/nghttp2.h>
#endif
#include<sys/eventfd.h>
#include<sys/mman.h>
#include<sys/socket.h>
#include<unistd.h>

export module conflux.net.http_server;

#ifdef CONFLUX_BUILD_FUZZ
#define CONFLUX_FUZZ_EXPORT export
#else
#define CONFLUX_FUZZ_EXPORT
#endif

import std;
import conflux.types;
import std.compat;

import conflux.net.http.types;
import conflux.net.router;
import conflux.net.vhost;
import conflux.net.config;
import conflux.net.http1_parser;
import conflux.uring;
import conflux.work;
import conflux.file_io;
import conflux.socket_io;
import conflux.utils;
#if CONFLUX_HAS_HTTP2
import conflux.net.http2;
#endif
#if CONFLUX_HAS_HTTP3
import conflux.net.http3;
#endif
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif

enum class Op:u8{
Accept,
Recv,
Send,
Close,
SsePoll,
DeferredPoll,
Shutdown,
Timer,
FileIo,
WsCancel,
FixedFdInstall,
DirectSlotClose,
Nop

};

export enum class RunStatus:u8{
stopped_normally,
fatal_cq_overflow,
fatal_cq_overflow_no_nodrop,
fatal_submit_wait_ebadr,
fatal_internal_exception

};

enum class ServerFatalReason:u8{
none,
cq_overflow,
cq_overflow_no_nodrop,
submit_wait_ebadr,
internal_exception

};

constexpr u32 OP_SHIFT=56U;
constexpr u32 GEN_SHIFT=24U;
constexpr u64 GEN_MASK=0xFFFFFFFFULL;
constexpr u64 FD_MASK=0x00FFFFFFULL;
constexpr u64 pack(
Op op,
u32 gen,
int fd)noexcept{
return(static_cast<u64>(static_cast<u8>(op))<<OP_SHIFT)|((static_cast<u64>(gen)&GEN_MASK)<<GEN_SHIFT)|(static_cast<u64>(static_cast<u32>(fd))&FD_MASK);
}
constexpr Tup<Op,u32,int>unpack(
u64 ud)noexcept{
return{
static_cast<Op>(ud>>OP_SHIFT),
static_cast<u32>((ud>>GEN_SHIFT)&GEN_MASK),
static_cast<int>(ud&FD_MASK)};
}
#include "direct_slot_pool.hxx"
SV http_date_now(){
static thread_local S cached;
static thread_local time_t cached_epoch=0;
auto const now=::time(nullptr);
if(now!=cached_epoch){
cached_epoch=now;
tm gmt{};
::gmtime_r(&now,&gmt);
A<char,32>buf{};
strftime(buf.data(),buf.size(),"%a, %d %b %Y %H:%M:%S GMT",&gmt);
cached=S{buf.data()};
}
return cached;
}
bool is_valid_header_name(
SV name)noexcept{
if(name.empty())
return false;
for(auto c:name){
auto const u=static_cast<unsigned char>(c);
if(u>=0x80U)
return false;
bool const ok=(c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='!'||c=='#'||c=='$'||c=='%'||c=='&'||c=='\''||c=='*'||c=='+'||c=='-'||c=='.'||c=='^'||c=='_'||c=='`'||c=='|'||c=='~';
if(!ok)
return false;
}
return true;
}
bool is_valid_header_value(
SV value)noexcept{
for(auto c:value){
auto const u=static_cast<unsigned char>(c);
if(u<0x20&&u!='\t')
return false;
if(u==0x7F)
return false;
}
return true;
}
bool is_framing_header(
SV name)noexcept{
auto lower_eq=[](SV a,SV b){
if(a.size()!=b.size())
return false;
for(SZ i=0;i<a.size();++i){
auto ca=static_cast<unsigned char>(a[i]);
auto cb=static_cast<unsigned char>(b[i]);
if(ca>='A'&&ca<='Z')
ca+=32;
if(ca!=cb)
return false;
}
return true;
};
return lower_eq(name,"content-length")||lower_eq(name,"transfer-encoding")||lower_eq(name,"connection")||lower_eq(name,"upgrade")||lower_eq(name,"keep-alive")||lower_eq(name,"te")||lower_eq(name,"trailer");
}
bool must_not_have_body(
int status)noexcept{
return(status>=100&&status<200)||status==204||status==304;
}
bool is_valid_reason_phrase(
SV value)noexcept{
for(auto c:value){
auto const u=static_cast<unsigned char>(c);
if(u<0x20&&u!='\t')
return false;
if(u==0x7F)
return false;
}
return true;
}
S format_response(
HttpResponse const&r,
SV alt_svc={},
bool close=false){
if(r.is_ws_upgrade()&&r.ws_upgrade_ptr())
return format(
"HTTP/1.1 101 Switching Protocols\r\n" "Upgrade: websocket\r\n" "Connection: Upgrade\r\n" "Sec-WebSocket-Accept: {}\r\n\r\n",
r.ws_upgrade_ptr()->accept_key);
bool const status_no_body=must_not_have_body(r.status);
bool const suppress_body=status_no_body||r.head_only;
S out=format(
"HTTP/1.1 {} {}\r\n" "Date: {}\r\n",
r.status,
is_valid_reason_phrase(r.status_text)?SV{r.status_text}:SV{},
http_date_now());
if(!r.content_type.empty())
out+=format("Content-Type: {}\r\n",r.content_type);
if(r.status==204){
// 204 must not have Content-Length per RFC 9110 §15.3.5 (omit it).
}else if(r.status==304){
if(r.content_length_hint!=0)
out+=format("Content-Length: {}\r\n",r.content_length_hint);
}else if(status_no_body){
// 1xx: no Content-Length
}else{
out+=format("Content-Length: {}\r\n",r.content_length());
}
for(auto const&[k,v]:r.headers){
if(is_framing_header(k))
continue;
if(!is_valid_header_name(k)||!is_valid_header_value(v))
continue;
out+=format("{}: {}\r\n",k,v);
}
for(auto const&sc:r.set_cookies){
if(!is_valid_header_value(sc))
continue;
out+=format("Set-Cookie: {}\r\n",sc);
}
if(!alt_svc.empty())
out+=format("Alt-Svc: {}\r\n",alt_svc);
out+=close?"Connection: close\r\n\r\n":"Connection: keep-alive\r\n\r\n";
if(!suppress_body&&!r.is_mapped_file()&&!r.is_streamed_file())
out+=r.text_body();
return out;
}
SV format_sse_headers(
bool close){
static constexpr SV kKeepAlive=
"HTTP/1.1 200 OK\r\n" "Content-Type: text/event-stream\r\n" "Cache-Control: no-cache\r\n" "Connection: keep-alive\r\n" "\r\n";
static constexpr SV kClose=
"HTTP/1.1 200 OK\r\n" "Content-Type: text/event-stream\r\n" "Cache-Control: no-cache\r\n" "Connection: close\r\n" "\r\n";
return close?kClose:kKeepAlive;
}
#if CONFLUX_HAS_HTTP2
struct Ring;// forward-declared so H2ConnCtx can hold Ring* while Conn precedes Ring
static constexpr SZ kH2PendingSendCap=64U*1024U;
struct H2Stream{
S method{};
S path{};
S scheme{"https"};
S authority{};
HttpFields headers{};
S body{};
SZ expected_body_size{};
bool body_reserved{};
bool end_stream_seen{};
bool rejected{};
bool regular_header_seen{};
bool seen_method{};
bool seen_path{};
bool seen_scheme{};
bool seen_authority{};
bool seen_content_length{};
SZ header_count{};
SZ header_list_size{};
// Response state for the data provider callback:
S response_body{};
SZ response_off{};
HttpFields response_trailers{};
int deferred_efd{-1};
// SSE streaming state (non-null → H2 SSE stream):
SP<SseChannel>sse_channel{};
S h2_sse_buf{};// overflow: drained SSE data not yet framed
};
struct H2ConnCtx{
Ring*ring;
int fd;
};
#endif// CONFLUX_HAS_HTTP2

struct Ring;
struct Conn;

static constexpr SZ kMaxChunkHexDigits=16;
static constexpr SZ kMaxChunkSizeLineBytes=256;
static constexpr SZ kMaxChunkTrailerLines=64;
static constexpr SZ kMaxChunkTrailerBytes=8192;

enum class ChunkedDecodePhase:u8{
SizeLine,
Data,
DataCrlf,
Trailers,
Complete

};
struct ChunkedDecodeState{
bool active{};
SZ body_start{};
SZ pos{};
SZ chunks_seen{};
SZ current_chunk_size{};
SZ remaining{};
SZ trailer_lines{};
SZ trailer_bytes{};
ChunkedDecodePhase phase{ChunkedDecodePhase::SizeLine};
S body{};
void reset(){
active=false;
body_start=0;
pos=0;
chunks_seen=0;
current_chunk_size=0;
remaining=0;
trailer_lines=0;
trailer_bytes=0;
phase=ChunkedDecodePhase::SizeLine;
body.clear();
}
};
void dispatch_request(
Conn&conn,
SV raw,
Ring const&ring,
SZ max_body_size,
bool http_redirect_to_https,
V<S>const&https_redirect_hosts,
ParserLimits const&limits);
struct PartialBuf{
S buf{};
SZ pos{0};
[[nodiscard]]inline bool empty()const noexcept{return pos>=buf.size();}
[[nodiscard]]inline SZ size()const noexcept{return buf.size()-pos;}
[[nodiscard]]inline char const*data()const noexcept{return buf.data()+pos;}
[[nodiscard]]inline char front()const noexcept{return buf[pos];}
[[nodiscard]]inline SV view()const noexcept{return{buf.data()+pos,buf.size()-pos};}
inline void append(
char const*p,
SZ n){
buf.append(p,n);
}
inline void consume(
SZ n)noexcept{
pos+=n;
if(pos>=buf.size())
clear();
}
inline void clear()noexcept{
buf.clear();
pos=0;
}
[[nodiscard]]inline S take(){
if(pos>0)
buf.erase(0,pos);
pos=0;
return move(buf);
}
};
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): field order mirrors connection state-machine phases.
struct alignas(
64)Conn{
int fd=-1;
u32 gen=0;
bool recv_armed=false;
u16 incremental_buf_id{};
bool have_incremental_buf_id{};
u32 last_recv_cqe_flags{};
bool have_last_recv_cqe_flags{};
bool send_queued=false;
bool is_sse=false;
bool sse_headers_sent=false;
bool is_ws=false;// true → WebSocket upgrade; hand off fd to WS thread after send
bool is_deferred=false;
bool deferred_head_only=false;// HEAD on deferred route → strip body when ready
bool closing=false;// close SQE already submitted for this generation
bool close_after_send=false;// true → close instead of re-arming recv
int sse_efd=-1;
SP<SseChannel>sse_channel{};
int deferred_efd=-1;
SP<DeferredResponse>deferred_response{};
SP<WsUpgrade>ws_upgrade{};// set when 101 pending; cleared after handoff
SP<WorkPool>ws_work_pool{};
HttpRequest saved_req{};// copy of request saved for WS handler thread
bool is_tls=false;// set after first-byte sniff; used by dispatch_request
#if CONFLUX_HAS_TLS
// TLS state (null → plaintext connection)
UniqueSsl ssl;
S tls_send_buf{};// encrypted bytes waiting to be sent via io_uring
SZ tls_send_off{};// bytes of tls_send_buf already sent
S tls_recv_buf{};// unconsumed ciphertext; rbio (BIO_new_mem_buf) points here
bool tls_hs_done=false;// TLS handshake completed; also used as undecided sentinel
bool tls_sending_response=false;// true → current tls_send_buf carries HTTP response data
bool ktls_send=false;// kTLS send offload active; splice_to_fd usable for TLS file body
#endif
bool has_response=false;
S own_response{};
PartialBuf partial{};
SZ written=0;
SZ request_bytes=0;// bytes consumed by current dispatched request
chrono::steady_clock::time_point last_activity;// updated on accept and recv
chrono::steady_clock::time_point request_started{};
bool request_in_progress=false;
bool expect_continue_sent=false;
S remote_addr{};// peer IP, set on accept
ChunkedDecodeState chunked_decode{};
// mmap path: non-null when current response has a zero-copy file region
SP<MappedFile>mapped_file{};
SZ mapped_total{};// own_response.size() + mapped_file->size
u64 mapped_delivered{};
A<iovec,2>writev_iov{};// iovecs rebuilt per-send in queue_send_mapped

// file_io streaming path: non-null when current response streams via splice
// (plain HTTP) or read_fixed+SSL_write (TLS). Phase tracks whether headers
// have been flushed to the socket.
SP<StreamedFile>streamed_file{};
bool streamed_headers_sent=false;
u64 streamed_delivered=0;
bool streamed_splice_in_flight=false;
#if CONFLUX_HAS_HTTP2
bool is_h2{};
nghttp2_session*h2_session=nullptr;
UP<H2ConnCtx>h2_ctx;
M<i32,H2Stream>h2_streams{};
S h2_pending_send{};
i32 h2_sse_stream_id{-1};// stream_id of active H2 SSE stream (-1 = none)
bool h2_sse_pending_wait{};// set by on_frame_recv_cb to trigger queue_sse_wait after h2_do_send
#endif
};
// Extract a named parameter from a header value (e.g. boundary= from Content-Type).
// Returns unquoted value, empty if not found.
SV extract_param(
SV header,
SV param_name){
auto pos=header.find(param_name);
if(pos==SV::npos)
return{};
pos+=param_name.size();
if(pos>=header.size()||header[pos]!='=')
return{};
++pos;
if(pos>=header.size())
return{};
if(header[pos]=='"'){
++pos;
auto end=header.find('"',pos);
return end==SV::npos?header.substr(pos):header.substr(pos,end-pos);
}
auto end=header.find_first_of(";\r\n ",pos);
return end==SV::npos?header.substr(pos):header.substr(pos,end-pos);
}
// Parse Cookie header into out: "name1=val1; name2=val2".
void parse_cookies(
SV cookie_header,
HttpFieldsView&out){
SZ pos=0;
while(pos<cookie_header.size()){
while(pos<cookie_header.size()&&cookie_header[pos]==' ')
++pos;
auto sep=cookie_header.find(';',pos);
auto P=sep==SV::npos?cookie_header.substr(pos):cookie_header.substr(pos,sep-pos);
while(!P.empty()&&P.back()==' ')
P.remove_suffix(1);
if(auto eq=P.find('=');eq!=SV::npos)
out.emplace_back(P.substr(0,eq),P.substr(eq+1));
else if(!P.empty())
out.emplace_back(P,{});
if(sep==SV::npos)
break;
pos=sep+1;
}
}
[[nodiscard]]bool has_connection_token(
HttpFieldsView const&headers,
SV wanted){
for(auto const header_value:headers.values("connection")){
SZ pos=0;
while(pos<=header_value.size()){
auto const comma=header_value.find(',',pos);
auto token=trim(comma==SV::npos?header_value.substr(pos):header_value.substr(pos,comma-pos));
if(!token.empty()&&conflux::http::ascii_iequals(token,wanted))
return true;
if(comma==SV::npos)
break;
pos=comma+1;
}
}
return false;
}
enum class ExpectState:u8{
none,
continue_100,
unsupported

};
[[nodiscard]]ExpectState parse_expect_header(
HttpFieldsView const&headers){
bool saw_continue=false;
for(auto const header_value:headers.values("expect")){
SZ pos=0;
while(pos<=header_value.size()){
auto const comma=header_value.find(',',pos);
auto token=trim(comma==SV::npos?header_value.substr(pos):header_value.substr(pos,comma-pos));
if(!token.empty()){
if(!conflux::http::ascii_iequals(token,"100-continue"))
return ExpectState::unsupported;
saw_continue=true;
}
if(comma==SV::npos)
break;
pos=comma+1;
}
}
return saw_continue?ExpectState::continue_100:ExpectState::none;
}
[[nodiscard]]bool has_valid_chunked_transfer_encoding(
HttpFieldsView const&headers){
SZ token_count=0;
for(auto const header_value:headers.values("transfer-encoding")){
SZ pos=0;
while(pos<=header_value.size()){
auto const comma=header_value.find(',',pos);
auto token=trim(comma==SV::npos?header_value.substr(pos):header_value.substr(pos,comma-pos));
if(token.empty())
return false;
++token_count;
if(!conflux::http::ascii_iequals(token,"chunked"))
return false;
if(comma==SV::npos)
break;
pos=comma+1;
}
}
return token_count==1;
}
struct MultipartBoundaryMatch{
SZ delim_pos{};
SZ content_end{};
};
[[nodiscard]]Opt<MultipartBoundaryMatch>find_multipart_boundary_line(
SV body,
SV delim,
SZ from)noexcept{
SZ search=from;
while(search<body.size()){
auto const pos=body.find(delim,search);
if(pos==SV::npos)
return nullopt;
bool const at_start=pos==0;
bool const after_crlf=pos>=2&&body.substr(pos-2,2)=="\r\n";
bool const after_lf=!after_crlf&&pos>=1&&body[pos-1]=='\n';
if(at_start||after_crlf||after_lf){
auto const after=pos+delim.size();
bool const valid_end=after==body.size()||body.substr(after,2)=="--"||body.substr(after,2)=="\r\n"||body[after]=='\n';
if(valid_end)
return MultipartBoundaryMatch{
.delim_pos=pos,
.content_end=after_crlf?pos-2:(after_lf?pos-1:pos)};
}
search=pos+1;
}
return nullopt;
}
// Parse multipart/form-data body.
// Text fields (no filename) go into form; file parts go into files.
void parse_multipart(
SV body,
SV boundary,
HttpFieldsView&form,
V<UploadedFile>&files){
S const delim=format("--{}",boundary);
auto first=find_multipart_boundary_line(body,delim,0);
if(!first)
return;
SZ pos=first->delim_pos;

static constexpr SZ kMaxMultipartParts=1000;
static constexpr SZ kMaxPartHeaderBytes=16U*1024U;
SZ part_count=0;
while(true){
if(++part_count>kMaxMultipartParts)
break;
pos+=delim.size();
if(pos+1>=body.size())
break;
if(body.substr(pos,2)=="--"){
break;
}// final delimiter

// Skip \r\n after delimiter
if(body.substr(pos,2)=="\r\n")
pos+=2;
else if(body[pos]=='\n')
++pos;
else
break;

auto headers_end=body.find("\r\n\r\n",pos);
if(headers_end==SV::npos)
break;
if(headers_end<pos||headers_end-pos>kMaxPartHeaderBytes)
break;

auto part_headers_sv=body.substr(pos,headers_end-pos);
auto content_start=headers_end+4;

auto next_boundary=find_multipart_boundary_line(body,delim,content_start);
if(!next_boundary)
break;

auto content_end=next_boundary->content_end;
if(content_end<content_start)
break;
auto content=body.substr(content_start,content_end-content_start);

// Parse part headers
SV disposition;
SV part_ct="text/plain";
SZ h=0;
while(h<part_headers_sv.size()){
auto le=part_headers_sv.find("\r\n",h);
auto line=le==SV::npos?part_headers_sv.substr(h):part_headers_sv.substr(h,le-h);
if(auto colon=line.find(':');colon!=SV::npos){
S const key=ascii_lower(line.substr(0,colon));
auto val=trim(line.substr(colon+1));
if(key=="content-disposition")
disposition=val;
else if(key=="content-type")
part_ct=val;
}
if(le==SV::npos)
break;
h=le+2;
}

auto name=extract_param(disposition,"name");
auto filename=extract_param(disposition,"filename");
if(!filename.empty())
files.push_back(UploadedFile::borrowed(name,filename,part_ct,content));
else if(!name.empty())
form.emplace_back(name,content);
pos=next_boundary->delim_pos;
}
}
// Parse application/x-www-form-urlencoded pairs into out.
[[gnu::pure]][[nodiscard]]bool needs_url_decode(
SV s)noexcept{
return s.find('%')!=SV::npos||s.find('+')!=SV::npos;
}
CONFLUX_FUZZ_EXPORT void parse_urlencoded(
SV data,
HttpFieldsView&out){
SZ pos=0;
while(pos<=data.size()){
auto amp=data.find('&',pos);
auto P=(amp==SV::npos)?data.substr(pos):data.substr(pos,amp-pos);
if(auto eq=P.find('=');eq!=SV::npos){
auto key=P.substr(0,eq);
auto field_value=P.substr(eq+1);
if(!needs_url_decode(key)&&!needs_url_decode(field_value))
out.emplace_back(key,field_value);
else
out.emplace_back_owned(url_decode(key),url_decode(field_value));
}else if(!P.empty()){
if(!needs_url_decode(P))
out.emplace_back(P,{});
else
out.emplace_back_owned(url_decode(P),S{});
}
if(amp==SV::npos)
break;
pos=amp+1;
}
}
// Decode chunked transfer encoding from raw chunk stream.
// Returns bytes consumed from `data` (including terminal chunk's trailing CRLF),
//         0  → incomplete (need more data),
//        -1  → malformed (send 400),
//        -2  → body exceeds max_body_size (send 413).
// On return > 0, `body` holds the decoded content. On rc ≤ 0 the contents of
// `body` are indeterminate (may hold a partial prefix) — callers must ignore
// it until a positive rc is seen.
CONFLUX_FUZZ_EXPORT i64 decode_chunked(
SV data,
SZ max_body_size,
SZ max_chunks,
S&body){
body.clear();
SZ pos=0;
SZ chunks_seen=0;
while(true){
if(++chunks_seen>max_chunks)
return-1;
auto crlf=data.find("\r\n",pos);
if(crlf==SV::npos)
return 0;

auto size_line_raw=data.substr(pos,crlf-pos);
if(size_line_raw.size()>kMaxChunkSizeLineBytes)
return-1;
auto size_digits=size_line_raw;
if(auto semi=size_digits.find(';');semi!=SV::npos)
size_digits=size_digits.substr(0,semi);// strip chunk extensions
if(size_digits.empty())
return-1;

if(size_digits.size()>kMaxChunkHexDigits)
return-1;
SZ chunk_size=0;
for(char const c:size_digits){
int const d=hex_char_to_int(c);
if(d<0)
return-1;
auto const digit=static_cast<SZ>(d);
SZ shifted=0;
if(__builtin_mul_overflow(chunk_size,SZ{16},&shifted))
return-1;
if(__builtin_add_overflow(shifted,digit,&chunk_size))
return-1;
}
pos=crlf+2;

if(chunk_size==0){
// Terminal chunk: skip Opt trailers until empty CRLF line.
SZ trailer_lines=0;
SZ trailer_bytes=0;
while(true){
auto next=data.find("\r\n",pos);
if(next==SV::npos)
return 0;
if(next==pos){
return static_cast<i64>(pos+2);
}// end
if(++trailer_lines>kMaxChunkTrailerLines)
return-1;
auto const line_bytes=next-pos+2;
if(line_bytes>kMaxChunkTrailerBytes||trailer_bytes>kMaxChunkTrailerBytes-line_bytes)
return-1;
trailer_bytes+=line_bytes;
pos=next+2;
}
}

if(body.size()>max_body_size||chunk_size>max_body_size-body.size())
return-2;
auto const remaining_wire=data.size()-pos;
if(chunk_size>remaining_wire||remaining_wire-chunk_size<2)
return 0;
body.append(data.substr(pos,chunk_size));
if(data[pos+chunk_size]!='\r'||data[pos+chunk_size+1]!='\n')
return-1;
pos+=chunk_size+2;
}
}
// Incremental variant for live HTTP/1 uploads. `pos` is absolute within
// `raw`, so callers can keep appending to the connection buffer without
// rescanning already-decoded chunks.
[[nodiscard]]i64 decode_chunked_incremental(
SV raw,
SZ body_start,
SZ max_body_size,
SZ max_chunks,
ChunkedDecodeState&st){
if(!st.active||st.body_start!=body_start){
st.reset();
st.active=true;
st.body_start=body_start;
st.pos=body_start;
}

while(true){
switch(st.phase){
case ChunkedDecodePhase::SizeLine:{
auto const crlf=raw.find("\r\n",st.pos);
if(crlf==SV::npos)
return 0;
if(++st.chunks_seen>max_chunks)
return-1;

auto size_line_raw=raw.substr(st.pos,crlf-st.pos);
if(size_line_raw.size()>kMaxChunkSizeLineBytes)
return-1;
auto size_digits=size_line_raw;
if(auto semi=size_digits.find(';');semi!=SV::npos)
size_digits=size_digits.substr(0,semi);
if(size_digits.empty())
return-1;

if(size_digits.size()>kMaxChunkHexDigits)
return-1;
SZ chunk_size=0;
for(char const c:size_digits){
int const d=hex_char_to_int(c);
if(d<0)
return-1;
auto const digit=static_cast<SZ>(d);
SZ shifted=0;
if(__builtin_mul_overflow(chunk_size,SZ{16},&shifted))
return-1;
if(__builtin_add_overflow(shifted,digit,&chunk_size))
return-1;
}

st.pos=crlf+2;
st.current_chunk_size=chunk_size;
if(chunk_size==0){
st.phase=ChunkedDecodePhase::Trailers;
break;
}
if(st.body.size()>max_body_size||chunk_size>max_body_size-st.body.size())
return-2;
st.remaining=chunk_size;
st.phase=ChunkedDecodePhase::Data;
break;
}
case ChunkedDecodePhase::Data:{
if(st.pos>=raw.size())
return 0;
auto const available=min(st.remaining,raw.size()-st.pos);
if(available>0){
st.body.append(raw.substr(st.pos,available));
st.pos+=available;
st.remaining-=available;
}
if(st.remaining>0)
return 0;
st.phase=ChunkedDecodePhase::DataCrlf;
break;
}
case ChunkedDecodePhase::DataCrlf:
if(raw.size()-st.pos<2)
return 0;
if(raw[st.pos]!='\r'||raw[st.pos+1]!='\n')
return-1;
st.pos+=2;
st.phase=ChunkedDecodePhase::SizeLine;
break;
case ChunkedDecodePhase::Trailers:{
auto const next=raw.find("\r\n",st.pos);
if(next==SV::npos)
return 0;
if(next==st.pos){
st.pos+=2;
st.phase=ChunkedDecodePhase::Complete;
return static_cast<i64>(st.pos-body_start);
}
if(++st.trailer_lines>kMaxChunkTrailerLines)
return-1;
auto const line_bytes=next-st.pos+2;
if(line_bytes>kMaxChunkTrailerBytes||st.trailer_bytes>kMaxChunkTrailerBytes-line_bytes)
return-1;
st.trailer_bytes+=line_bytes;
st.pos=next+2;
break;
}
case ChunkedDecodePhase::Complete:return static_cast<i64>(st.pos-body_start);
}
}
}
struct RecvComp{
int fd;
int res;
u32 gen;
u32 flags;
};
constexpr SZ FD_TABLE_RESERVE=4096;
constexpr unsigned DEFAULT_RING_ENTRIES=1024U;

struct Ring;

static conflux::work::root::Task<void>
do_streamed_splice(Ring*ring,int fd,u32 conn_gen,conflux::work::root::Task<SZ>splice_task);

static conflux::work::root::Task<void>do_streamed_tls_chunk(
Ring*ring,
int fd,
u32 conn_gen,
SZ want,
conflux::work::root::Task<FileReader::ReadFixedResult>read_task);
struct Ring{
struct DeferredWait{
int conn_fd{-1};
i32 stream_id{-1};
SP<DeferredResponse>response{};
};
struct WsHandoffState{
SP<WsUpgrade>upgrade{};
SP<WorkPool>pool{};
HttpRequest request{};
};
struct WsInstallEntry{
WsHandoffState state{};
S initial_buf{};
#if CONFLUX_HAS_TLS
UniqueSsl ssl{};
#endif
};
struct RetiredIncrementalBuf{
u16 id{};
bool present{};
};
static u64 pack_fd_gen(int fd,u32 gen)noexcept{
return(static_cast<u64>(static_cast<u32>(fd))<<32)|static_cast<u64>(gen);
}
static constexpr SZ BUF_SIZE=8192;
static constexpr u32 MAX_FILES=65536;

io_uring ring{};
SocketRawRing raw_{conflux::uring::RingRef{ring}};
// Backing memory for the ring when no_mmap = true. Freed on destroy.
UPD<byte[],void(*)(void*)>ring_mem{nullptr,::free};
int listen_fd=-1;
sockaddr_in6 client_addr{};
socklen_t client_addr_len=sizeof(client_addr);

V<Conn>fd_table{};
V<RecvComp>recvs{};
UM<int,DeferredWait>deferred_waits{};
UM<u64,UP<u64>>in_flight_read_bufs{};
UM<int,WsInstallEntry>ws_cancel_handoffs{};
UM<int,WsInstallEntry>ws_installs{};
UM<u64,RetiredIncrementalBuf>retired_incremental_recv{};
V<int>ws_cancel_ready{};

UP<BufferRing>buf_ring_;
UP<DirectFdTable>direct_fds_;
UP<DirectSlotPool>direct_slots_;
int tcp_opt_one_=1;// stable optval for cmd_sock SETSOCKOPT

conflux::uring::IoUringCaps caps{};
bool listen_fixed=false;
bool accepted_sockets_direct=false;
bool shutting_down=false;
bool ring_fatal_{false};
ServerFatalReason fatal_reason_{ServerFatalReason::none};
u32 fatal_cq_overflow_count_{0};
bool overflow_flush_limit_hit_{false};
bool use_recv_bundle=false;// IORING_RECVSEND_BUNDLE on multishot recv
bool use_recv_incremental_buf=false;// IOU_PBUF_RING_INC on buffer ring
bool auto_recv_arm_policy=false;// adaptive poll_first via IORING_CQE_F_SOCK_NONEMPTY
SZ max_body_size=SZ{1024}*1024;// set from Config before run_loop()
u32 request_timeout_ms=30000;// set from Config before run_loop(); 0 = disabled
u32 tls_sniff_timeout_ms=10000;// set from Config before run_loop(); 0 = disabled
bool slow_handler_diagnostics=false;// set from Config before run_loop()
u32 slow_handler_warn_ms=25;// set from Config before run_loop()
bool http_redirect_to_https=false;// set from Config before run_loop()
V<S>https_redirect_hosts{};// set from Config before run_loop()
ParserLimits parser_limits{};// set from Config before run_loop()
S alt_svc_header{};// "h3=\":443\"; ma=86400" when h3 enabled; empty otherwise

int shutdown_efd=-1;// set by HttpServer before run_loop(); not owned
u64 shutdown_buf=0;// read target for the shutdown eventfd SQE

// file_io pools — constructed after io_uring_queue_init. Shared by
// static-file serving and any other caller that grabs files.get().
UP<CompletionTable>file_completions{};
UP<FixedBufferPool>fixed_buffers{};
UP<PipePool>splice_pipes{};
UP<FileReader>files{};

// pool sizing — set from Config before run_loop()
SZ file_io_slabs=64;
SZ file_io_slab_bytes=SZ{64}*1024;
SZ file_io_pipe_pairs=16;

__kernel_timespec timer_ts{};// reused for the periodic timeout SQE

// Software fallback queue for ops that could not obtain an SQE even after
// a flush. Drained at the top of each run_loop iteration (after CQE reap
// frees SQ slots). Each thunk re-invokes the original queue_* path so
// conn state (gen, buffers) is re-read at replay time.
deque<conflux::work::root::detail::small_move_only_function<void()>>pending_ops{};

Router const*router=nullptr;// set before init(); not owned
VHostRouter const*vhost_router=nullptr;// set before init(); not owned

u16 bound_port{};// actual port after bind (handles port=0)
Atom<u16>*port_signal=nullptr;// set for ring 0; signalled after listen()
#if CONFLUX_HAS_TLS
SSL_CTX*ssl_ctx=nullptr;// non-owning; set by HttpServer before run(); null → plaintext
// vhost_ctxs: hostname → SSL_CTX* for SNI switching (non-owning, owned by HttpServer::Impl)
V<P<S,SSL_CTX*>>vhost_ctxs;
#endif

Ring()=default;
~Ring(){
buf_ring_.reset();
direct_fds_.reset();
if(listen_fd>=0)
::close(listen_fd);
io_uring_queue_exit(&ring);
}
Ring(Ring const&)=delete;
Ring&operator=(Ring const&)=delete;
Ring(Ring&&)=delete;
Ring&operator=(Ring&&)=delete;
[[nodiscard]]HttpResponse dispatch(
HttpRequestView const&req)const{
if(vhost_router!=nullptr)
return vhost_router->dispatch(req);
return router->dispatch(req);
}
[[nodiscard]]SP<WorkPool>resolve_ws_work_pool(
HttpRequestView const&req)const{
if(vhost_router!=nullptr)
return vhost_router->resolved_work_pool(req.headers["host"]);
return router->work_pool();
}
void clear_deferred_wait(
int deferred_efd){
if(deferred_efd>=0)
deferred_waits.erase(deferred_efd);
}
void queue_deferred_wait(
int conn_fd,
int deferred_efd,
SP<DeferredResponse>response,
i32 stream_id=-1){
if(deferred_efd<0||!response)
return;
auto*sqe=get_sqe();
if(sqe==nullptr){
defer_op([this,conn_fd,deferred_efd,response,stream_id]()mutable{
queue_deferred_wait(conn_fd,deferred_efd,move(response),stream_id);
});
return;
}

auto&wait=deferred_waits[deferred_efd];
wait.conn_fd=conn_fd;
wait.stream_id=stream_id;
wait.response=move(response);

auto const conn_gen=conn_for(conn_fd).gen;
auto const ud=pack(Op::DeferredPoll,conn_gen,deferred_efd);
auto buf=make_unique<u64>(0);
io_uring_prep_read(sqe,deferred_efd,buf.get(),sizeof(u64),0);
io_uring_sqe_set_data64(sqe,ud);
in_flight_read_bufs[ud]=move(buf);
}
#if CONFLUX_HAS_TLS
// Drain all encrypted bytes from wbio into conn.tls_send_buf.
static void tls_flush_wbio(
Conn&conn){
A<char,4096>buf{};
int n{};
while((n=BIO_read(SSL_get_wbio(conn.ssl.get()),buf.data(),static_cast<int>(buf.size())))>0)
conn.tls_send_buf.append(buf.data(),static_cast<SZ>(n));
}
// Submit an io_uring send for the next chunk of conn.tls_send_buf.
// Caller must set conn.send_queued = true before calling.
void tls_queue_send(
Conn&conn){
auto const off=conn.tls_send_off;
auto const tls_view=span{conn.tls_send_buf}.subspan(off);
auto handle=accepted_sockets_direct?SocketHandle::from_direct(static_cast<u32>(conn.fd)):
SocketHandle::from_os(conn.fd);
if(!submit_send_borrowed(raw_,handle,tls_view.data(),tls_view.size(),pack(Op::Send,conn.gen,conn.fd))){
int const fd=conn.fd;
defer_op([this,fd]{tls_queue_send(conn_for(fd));});
}
}
#endif// CONFLUX_HAS_TLS

#if CONFLUX_HAS_HTTP2
// ---------------------------------------------------------------------------
// nghttp2 static callbacks (passed as C function pointers; no capture)
// ---------------------------------------------------------------------------
static void h2_reject_stream(
nghttp2_session*session,
H2Stream&stream,
i32 stream_id,
u32 error_code){
stream.rejected=true;
nghttp2_submit_rst_stream(session,NGHTTP2_FLAG_NONE,stream_id,error_code);
}
[[nodiscard]]static bool h2_valid_regular_header_name(
SV name)noexcept{
if(!is_valid_header_name(name))
return false;
for(char const c:name)
if(c>='A'&&c<='Z')
return false;
return true;
}
[[nodiscard]]static bool h2_forbidden_connection_header(
SV name)noexcept{
return name=="connection"||name=="keep-alive"||name=="proxy-connection"||name=="transfer-encoding"||name=="upgrade";
}
// nghttp2 wants to write bytes to the wire; accumulate into h2_pending_send.
static ssize_t h2_send_cb(
nghttp2_session*/*unused*/,
u8 const*data,
SZ length,
int /*unused*/,
void*user_data){
auto*ctx=static_cast<H2ConnCtx*>(user_data);
auto&conn=ctx->ring->conn_for(ctx->fd);
if(conn.h2_pending_send.size()>=kH2PendingSendCap)
return NGHTTP2_ERR_WOULDBLOCK;
auto const available=kH2PendingSendCap-conn.h2_pending_send.size();
auto const to_copy=min(length,available);
conn.h2_pending_send.append(reinterpret_cast<char const*>(data),to_copy);
return static_cast<ssize_t>(to_copy);
}
// A new request stream is beginning; allocate its H2Stream entry.
static int h2_on_begin_headers_cb(
nghttp2_session*/*unused*/,
nghttp2_frame const*frame,
void*user_data){
if(frame->hd.type!=NGHTTP2_HEADERS||frame->headers.cat!=NGHTTP2_HCAT_REQUEST)
return 0;
auto*ctx=static_cast<H2ConnCtx*>(user_data);
auto&conn=ctx->ring->conn_for(ctx->fd);
conn.h2_streams.emplace(frame->hd.stream_id,H2Stream{});
return 0;
}
// Populate H2Stream fields from pseudo-headers and regular headers.
static int h2_on_header_cb(
nghttp2_session*session,
nghttp2_frame const*frame,
u8 const*name,
SZ namelen,
u8 const*header_value,
SZ valuelen,
u8 /*unused*/,
void*user_data){
auto*ctx=static_cast<H2ConnCtx*>(user_data);
auto&conn=ctx->ring->conn_for(ctx->fd);
auto it=conn.h2_streams.find(frame->hd.stream_id);
if(it==conn.h2_streams.end())
return 0;
auto&stream=it->second;
if(stream.rejected)
return 0;
SV const n{reinterpret_cast<char const*>(name),namelen};
SV const v{reinterpret_cast<char const*>(header_value),valuelen};
if(stream.header_count==NL<SZ>::max()||namelen>NL<SZ>::max()-valuelen||stream.header_list_size>NL<SZ>::max()-namelen-valuelen){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_ENHANCE_YOUR_CALM);
return 0;
}
++stream.header_count;
stream.header_list_size+=namelen+valuelen;
if(stream.header_count>ctx->ring->parser_limits.max_headers||stream.header_list_size>ctx->ring->parser_limits.max_header_block_size){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_ENHANCE_YOUR_CALM);
return 0;
}
if(n.starts_with(":")){
if(stream.regular_header_seen){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
if(n==":method"){
if(stream.seen_method){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
stream.seen_method=true;
stream.method=S{v};
}else if(n==":path"){
if(stream.seen_path){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
stream.seen_path=true;
stream.path=S{v};
}else if(n==":scheme"){
if(stream.seen_scheme){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
stream.seen_scheme=true;
stream.scheme=S{v};
}else if(n==":authority"){
if(stream.seen_authority){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
stream.seen_authority=true;
stream.authority=S{v};
}else{
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
}else{
stream.regular_header_seen=true;
if(!h2_valid_regular_header_name(n)||h2_forbidden_connection_header(n)){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
if(n=="te"&&v!="trailers"){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
if(n=="content-length"){
if(stream.seen_content_length){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
SZ content_length{};
auto const*cl_end=ranges::next(v.data(),ssize(v));
auto[ptr,ec]=from_chars(v.data(),cl_end,content_length);
if(ec==errc{}&&ptr==cl_end&&content_length<=ctx->ring->max_body_size){
stream.expected_body_size=content_length;
}else{
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_CANCEL);
return 0;
}
stream.seen_content_length=true;
}
stream.headers.emplace_back(S{n},S{v});
}
return 0;
}
// Accumulate DATA frame body bytes into stream.body.
static int h2_on_data_chunk_cb(
nghttp2_session*session,
u8 /*unused*/,
i32 stream_id,
u8 const*data,
SZ len,
void*user_data){
auto*ctx=static_cast<H2ConnCtx*>(user_data);
auto&conn=ctx->ring->conn_for(ctx->fd);
auto it=conn.h2_streams.find(stream_id);
if(it!=conn.h2_streams.end()){
auto&stream=it->second;
if(stream.rejected)
return 0;
if(len>ctx->ring->max_body_size||stream.body.size()>ctx->ring->max_body_size-len){
h2_reject_stream(session,stream,stream_id,NGHTTP2_CANCEL);
return 0;
}
if(stream.seen_content_length&&len>stream.expected_body_size-stream.body.size()){
h2_reject_stream(session,stream,stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
if(!stream.body_reserved&&stream.expected_body_size>0){
stream.body.reserve(stream.expected_body_size);
stream.body_reserved=true;
}
stream.body.append(reinterpret_cast<char const*>(data),len);
}
return 0;
}
// Data provider: feed response body bytes to nghttp2's framing layer.
// Handles both static responses and SSE streaming.
static ssize_t h2_read_cb(
nghttp2_session*session,
i32 stream_id,
u8*buf,
SZ length,
u32*data_flags,
nghttp2_data_source*source,
void*/*user_data*/){
auto&stream=*static_cast<H2Stream*>(source->ptr);

// SSE streaming path: drain from the channel, defer when empty.
if(stream.sse_channel){
if(stream.h2_sse_buf.empty())
stream.h2_sse_buf=stream.sse_channel->drain();
if(stream.h2_sse_buf.empty()){
if(stream.sse_channel->is_closed()){
*data_flags|=NGHTTP2_DATA_FLAG_EOF;
return 0;
}
return NGHTTP2_ERR_DEFERRED;
}
auto to_copy=min(stream.h2_sse_buf.size(),length);
// NOLINTNEXTLINE(bugprone-not-null-terminated-result): raw byte copy, not C-S
memcpy(buf,stream.h2_sse_buf.data(),to_copy);
stream.h2_sse_buf.erase(0,to_copy);
// Don't set EOF — channel may produce more events.
return static_cast<ssize_t>(to_copy);
}

// Static response body path.
auto remaining=stream.response_body.size()-stream.response_off;
auto to_copy=min(remaining,length);
memcpy(buf,span{stream.response_body}.subspan(stream.response_off).data(),to_copy);
stream.response_off+=to_copy;
if(stream.response_off>=stream.response_body.size()){
// If the response carries trailers, suppress the END_STREAM flag on the
// DATA frame so we can send a HEADERS frame with the trailers after.
if(!stream.response_trailers.empty()){
*data_flags|=NGHTTP2_DATA_FLAG_NO_END_STREAM;
*data_flags|=NGHTTP2_DATA_FLAG_EOF;
V<nghttp2_nv>nva;
nva.reserve(stream.response_trailers.size());
for(auto const&[n,v]:stream.response_trailers)
nva.push_back(
{reinterpret_cast<u8*>(const_cast<char*>(n.data())),
reinterpret_cast<u8*>(const_cast<char*>(v.data())),
n.size(),
v.size(),
NGHTTP2_NV_FLAG_NONE});
nghttp2_submit_trailer(session,stream_id,nva.data(),nva.size());
stream.response_trailers.clear();
}else{
*data_flags|=NGHTTP2_DATA_FLAG_EOF;
}
}
return static_cast<ssize_t>(to_copy);
}
static void h2_submit_response(
Conn&conn,
i32 stream_id,
HttpResponse resp){
auto it=conn.h2_streams.find(stream_id);
if(it==conn.h2_streams.end())
return;
auto&stream=it->second;

if(resp.is_deferred())
resp=HttpResponse::internal_error("nested deferred responses unsupported over HTTP/2");
if(resp.is_ws_upgrade())
resp=HttpResponse::internal_error("websocket upgrades unsupported over HTTP/2");
if(resp.is_mapped_file())
resp=HttpResponse::internal_error("mapped files unsupported over HTTP/2");

bool const is_sse_resp=resp.is_sse();
S const status_str=to_string(resp.status);
S const clen_str=to_string(resp.content_length());
V<P<S,S>>nv_storage;
nv_storage.reserve(3+resp.headers.size()+resp.set_cookies.size());
nv_storage.emplace_back(":status",status_str);
nv_storage.emplace_back("content-type",resp.content_type);
if(!is_sse_resp)
nv_storage.emplace_back("content-length",clen_str);
for(auto const&[k,v]:resp.headers)
nv_storage.emplace_back(k,v);
for(auto const&sc:resp.set_cookies)
nv_storage.emplace_back("set-cookie",sc);
if(conn.h2_ctx!=nullptr&&conn.h2_ctx->ring!=nullptr&&!conn.h2_ctx->ring->alt_svc_header.empty())
nv_storage.emplace_back("alt-svc",conn.h2_ctx->ring->alt_svc_header);

V<nghttp2_nv>nva;
nva.reserve(nv_storage.size());
for(auto&[n,v]:nv_storage)
nva.push_back(
{reinterpret_cast<u8*>(n.data()),
reinterpret_cast<u8*>(v.data()),
n.size(),
v.size(),
NGHTTP2_NV_FLAG_NONE});

stream.response_body=resp.take_text_body();
stream.response_off=0;
stream.response_trailers=move(resp.trailers);

nghttp2_data_provider prd{};
prd.read_callback=h2_read_cb;
prd.source.ptr=&stream;

if(is_sse_resp){
stream.sse_channel=resp.take_sse_channel();
conn.sse_efd=stream.sse_channel->eventfd_fd();
conn.sse_channel=stream.sse_channel;
conn.h2_sse_stream_id=stream_id;
conn.h2_sse_pending_wait=true;
}

nghttp2_submit_response(
conn.h2_session,
stream_id,
nva.data(),
nva.size(),
(stream.response_body.empty()&&!is_sse_resp)?nullptr:&prd);
}
// A frame is fully received.  On END_STREAM, dispatch to the router and
// submit the HTTP/2 response via nghttp2_submit_response.
static int h2_on_frame_recv_cb(
nghttp2_session*session,
nghttp2_frame const*frame,
void*user_data){
// Only act on request streams that are now complete.
if((frame->hd.flags&NGHTTP2_FLAG_END_STREAM)==0)
return 0;
if(frame->hd.type!=NGHTTP2_HEADERS&&frame->hd.type!=NGHTTP2_DATA)
return 0;

auto*ctx=static_cast<H2ConnCtx*>(user_data);
auto&conn=ctx->ring->conn_for(ctx->fd);
auto it=conn.h2_streams.find(frame->hd.stream_id);
if(it==conn.h2_streams.end())
return 0;
auto&stream=it->second;
if(stream.rejected)
return 0;
if(stream.end_stream_seen)
return 0;
stream.end_stream_seen=true;
if(!stream.seen_method||!stream.seen_path||!stream.seen_scheme||stream.method.empty()||stream.path.empty()||stream.scheme.empty()){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}
if(stream.seen_content_length&&stream.body.size()!=stream.expected_body_size){
h2_reject_stream(session,stream,frame->hd.stream_id,NGHTTP2_PROTOCOL_ERROR);
return 0;
}

SV const method=stream.method;
SV path=stream.path;
SV const version="HTTP/2";
SV const body=stream.body;
HttpFieldsView const params;
HttpFieldsView query;
HttpFieldsView form;
HttpFieldsView cookies;
V<UploadedFile>files;
if(auto q=path.find('?');q!=SV::npos){
parse_urlencoded(path.substr(q+1),query);
path=path.substr(0,q);
}
if(stream.headers["content-type"].starts_with("application/x-www-form-urlencoded"))
parse_urlencoded(body,form);
auto ct_header=stream.headers["content-type"];
if(ct_header.starts_with("multipart/form-data")){
auto boundary=extract_param(ct_header,"boundary");
if(!boundary.empty())
parse_multipart(body,boundary,form,files);
}
if(auto cookie=stream.headers["cookie"];!cookie.empty())
parse_cookies(cookie,cookies);

HttpRequestView const req{
method,
path,
version,
conn.remote_addr,
true,
params,
stream.headers,
query,
form,
cookies,
files,
body};

HttpResponse resp;
try{
resp=ctx->ring->dispatch(req);
}catch(exception const&e){resp=HttpResponse::internal_error(e.what());}catch(...){
resp=HttpResponse::internal_error();
}

if(resp.is_deferred()){
stream.deferred_efd=resp.deferred_response_ptr()->eventfd_fd();
ctx->ring
->queue_deferred_wait(ctx->fd,stream.deferred_efd,resp.take_deferred_response(),frame->hd.stream_id);
return 0;
}
h2_submit_response(conn,frame->hd.stream_id,move(resp));
return 0;
}
// Stream fully closed — release its state.
static int h2_on_stream_close_cb(
nghttp2_session*/*unused*/,
i32 stream_id,
u32 /*EC*/,
void*user_data){
auto*ctx=static_cast<H2ConnCtx*>(user_data);
auto&conn=ctx->ring->conn_for(ctx->fd);
if(auto it=conn.h2_streams.find(stream_id);it!=conn.h2_streams.end())
ctx->ring->clear_deferred_wait(it->second.deferred_efd);
// If this was the active H2 SSE stream, clear conn-level SSE state.
if(conn.h2_sse_stream_id==stream_id){
conn.h2_sse_stream_id=-1;
conn.h2_sse_pending_wait=false;
conn.sse_efd=-1;
conn.sse_channel.reset();
}
conn.h2_streams.erase(stream_id);
return 0;
}
// ---------------------------------------------------------------------------
// H2 Ring methods
// ---------------------------------------------------------------------------

// SSL_write h2_pending_send into tls_send_buf, then queue a TLS send.
// No-op if nothing pending or a send is already in flight (data accumulates
// and will be flushed when handle_send_tls_complete's H2 branch runs next).
void h2_flush_pending(
Conn&conn){
if(conn.h2_pending_send.empty()||conn.send_queued)
return;
char const*h2_data=conn.h2_pending_send.data();
int h2_remaining=static_cast<int>(conn.h2_pending_send.size());
while(h2_remaining>0){
auto const w=SSL_write(conn.ssl.get(),h2_data,h2_remaining);
if(w<=0){
queue_close(conn.fd);
return;
}
h2_data+=w;
h2_remaining-=w;
}
conn.h2_pending_send.clear();
tls_flush_wbio(conn);
if(!conn.tls_send_buf.empty()){
conn.send_queued=true;
tls_queue_send(conn);
}
}
// Drive nghttp2 output (all queued frames) and flush to io_uring.
void h2_do_send(
Conn&conn){
if(conn.h2_session==nullptr)
return;
if(!conn.h2_pending_send.empty()){
h2_flush_pending(conn);
if(conn.send_queued||conn.closing||!conn.h2_pending_send.empty())
return;
}
if(nghttp2_session_want_write(conn.h2_session)==0){
h2_flush_pending(conn);
return;
}
if(nghttp2_session_send(conn.h2_session)!=0){
queue_close(conn.fd);
return;
}
h2_flush_pending(conn);
}
// Create nghttp2 server session and submit the server connection preface
// (SETTINGS frame).  Does NOT flush — caller must call h2_do_send() after
// running nghttp2_session_mem_recv() so that the SETTINGS and SETTINGS_ACK
// are coalesced into a single TLS record.
void h2_setup_conn(
Conn&conn){
nghttp2_session_callbacks*cbs=nullptr;
nghttp2_session_callbacks_new(&cbs);
nghttp2_session_callbacks_set_send_callback(cbs,h2_send_cb);
nghttp2_session_callbacks_set_on_begin_headers_callback(cbs,h2_on_begin_headers_cb);
nghttp2_session_callbacks_set_on_header_callback(cbs,h2_on_header_cb);
nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs,h2_on_data_chunk_cb);
nghttp2_session_callbacks_set_on_frame_recv_callback(cbs,h2_on_frame_recv_cb);
nghttp2_session_callbacks_set_on_stream_close_callback(cbs,h2_on_stream_close_cb);

conn.h2_ctx=make_unique<H2ConnCtx>(H2ConnCtx{.ring=this,.fd=conn.fd});
if(nghttp2_session_server_new(&conn.h2_session,cbs,conn.h2_ctx.get())!=0)
conn.h2_session=nullptr;
nghttp2_session_callbacks_del(cbs);
if(conn.h2_session==nullptr){
queue_close(conn.fd);
return;
}

constexpr u32 kH2MaxConcurrentStreams=100;
constexpr u32 kH2InitialWindowSize=1U<<24;
constexpr u32 kH2MaxFrameSize=1U<<17;
u32 const h2_max_header_list_size=
static_cast<u32>(min<SZ>(parser_limits.max_header_block_size,static_cast<SZ>(NL<u32>::max())));
A<nghttp2_settings_entry,4>const iv{
{
{.settings_id=NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,.value=kH2MaxConcurrentStreams},
{.settings_id=NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,.value=kH2InitialWindowSize},
{.settings_id=NGHTTP2_SETTINGS_MAX_FRAME_SIZE,.value=kH2MaxFrameSize},
{.settings_id=NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,.value=h2_max_header_list_size},
}};
nghttp2_submit_settings(conn.h2_session,NGHTTP2_FLAG_NONE,iv.data(),iv.size());
// Flush deferred to caller's h2_do_send().
}
#endif// CONFLUX_HAS_HTTP2
// Must be called from the thread that will run run_loop() (SINGLE_ISSUER).
// `wq_fd`: when non-zero, sets IORING_SETUP_ATTACH_WQ so this ring shares
// the parent ring's kernel io-wq. Pass ring[0].ring.ring_fd for rings 1..N.
void init(
u16 port,
unsigned entries,
u32 uring_flags,
u32 wq_fd=0,
bool no_mmap=false){
io_uring_params params{};
params.flags=uring_flags;
if(wq_fd!=0){
params.flags|=IORING_SETUP_ATTACH_WQ;
params.wq_fd=wq_fd;
}
if(no_mmap){
ssize_t const sz=io_uring_mlock_size(entries,params.flags);
if(sz<=0)
throw RE{"io_uring_mlock_size failed"};
auto*raw=
static_cast<byte*>(::aligned_alloc(static_cast<SZ>(sysconf(_SC_PAGESIZE)),static_cast<SZ>(sz)));
if(raw==nullptr)
throw std::bad_alloc{};
ring_mem={raw,::free};
if(io_uring_queue_init_mem(entries,&ring,&params,raw,static_cast<SZ>(sz))<0)
throw RE{"io_uring_queue_init_mem failed"};
}else if(io_uring_queue_init_params(entries,&ring,&params)<0){
throw RE{"io_uring_queue_init_params failed"};
}
caps=detect_caps(conflux::uring::RingRef{ring});

listen_fd=::socket(AF_INET6,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
if(listen_fd<0)
throw SE{errno,system_category(),"socket"};

int opt=1;
int v6only=0;
::setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
::setsockopt(listen_fd,SOL_SOCKET,SO_REUSEPORT,&opt,sizeof(opt));
::setsockopt(listen_fd,IPPROTO_TCP,TCP_NODELAY,&opt,sizeof(opt));
::setsockopt(listen_fd,IPPROTO_IPV6,IPV6_V6ONLY,&v6only,sizeof(v6only));

sockaddr_in6 addr{};
addr.sin6_family=AF_INET6;
addr.sin6_addr=in6addr_any;
addr.sin6_port=htons(port);

if(::bind(listen_fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0)
throw SE{errno,system_category(),"bind"};
if(::listen(listen_fd,SOMAXCONN)<0)
throw SE{errno,system_category(),"listen"};

// Read back actual port (port=0 → OS-assigned). Signal before io_uring
// setup so callers can discover the port while rings initialise.
{
sockaddr_in6 local{};
socklen_t llen=sizeof(local);
if(getsockname(listen_fd,reinterpret_cast<sockaddr*>(&local),&llen)==0)
bound_port=ntohs(local.sin6_port);
if(port_signal!=nullptr){
port_signal->store(bound_port,memory_order_release);
port_signal->notify_all();
}
}

direct_fds_=make_unique<DirectFdTable>(conflux::uring::RingRef{ring},MAX_FILES);
if(direct_fds_->registered()&&direct_fds_->install(static_cast<u32>(listen_fd),listen_fd)){
listen_fixed=true;
caps.socket_direct_alloc=caps.op_socket&&direct_fds_->registered();
direct_slots_=make_unique<DirectSlotPool>(direct_fds_->capacity());
if(!direct_slots_->install_os_fd(static_cast<u32>(listen_fd),listen_fd)){
direct_slots_.reset();
listen_fixed=false;
caps.socket_direct_alloc=false;
}
}
accepted_sockets_direct=listen_fixed&&caps.accept_direct_supported;

// file_io pools: constructed here so register_buffers_sparse runs before
// buf_ring setup (both touch io_uring internal state; ordering is
// defensive — buf_ring uses a separate bgid). Install FileReader only
// when both streaming paths have usable resources; otherwise serve_static
// falls back to the mmap path instead of selecting an async response that
// cannot deliver its body.
if(file_io_slabs>0&&file_io_pipe_pairs>0){
auto buffers=make_unique<FixedBufferPool>(&ring,file_io_slabs,file_io_slab_bytes);
auto pipes=make_unique<PipePool>(file_io_pipe_pairs);
if(buffers->ok()&&buffers->capacity()>0&&pipes->capacity()>0){
file_completions=make_unique<CompletionTable>();
fixed_buffers=move(buffers);
splice_pipes=move(pipes);
files=make_unique<FileReader>(&ring,file_completions.get(),[](u32 slot,u32 gen)noexcept{
return pack(Op::FileIo,gen,static_cast<int>(slot));
});
}
}

buf_ring_=make_unique<BufferRing>(
conflux::uring::RingRef{ring},
BufferRingOptions{
.count=entries*4,
.buf_size=BUF_SIZE,
.group_id=0,
.huge_pages=true,
.mode=use_recv_incremental_buf?BufferRingMode::incremental:BufferRingMode::classic_one_cqe_per_buffer,
},
caps);

fd_table.reserve(FD_TABLE_RESERVE);
recvs.reserve(entries);
}
Conn&conn_for(
int fd){
auto const ufd=static_cast<SZ>(fd);
if(ufd>=fd_table.size()){
if(ufd>=1000000U)[[unlikely]]{
::close(fd);
thread_local Conn dead{};
dead=Conn{};
return dead;
}
fd_table.resize(ufd+1);
}
return fd_table[ufd];
}
void conn_erase(
int fd,
u32 gen){
auto const ufd=static_cast<SZ>(fd);
if(ufd>=fd_table.size())
return;
auto&conn=fd_table[ufd];
if(conn.gen!=gen)
return;
if(conn.is_sse&&conn.sse_channel)
conn.sse_channel->close();// notify handler thread
retire_incremental_partial(fd,gen,conn);
++conn.gen;// prevent a second Close CQE from erasing the next tenant
conn.fd=-1;
conn.recv_armed=false;
conn.last_recv_cqe_flags=0;
conn.have_last_recv_cqe_flags=false;
conn.closing=false;
conn.is_sse=false;
conn.sse_headers_sent=false;
conn.is_ws=false;
conn.is_deferred=false;
conn.sse_efd=-1;
conn.sse_channel.reset();
clear_deferred_wait(conn.deferred_efd);
conn.deferred_efd=-1;
conn.deferred_response.reset();
conn.ws_upgrade.reset();
conn.partial.clear();
conn.chunked_decode.reset();
conn.mapped_file.reset();
conn.mapped_total=0;
conn.mapped_delivered=0;
conn.is_tls=false;
#if CONFLUX_HAS_TLS
if(conn.ssl!=nullptr)
conn.ssl.reset();
conn.tls_send_buf.clear();
conn.tls_send_off=0;
conn.tls_recv_buf.clear();
conn.tls_hs_done=false;
conn.tls_sending_response=false;
#endif
#if CONFLUX_HAS_HTTP2
if(conn.h2_session!=nullptr){
nghttp2_session_del(conn.h2_session);
conn.h2_session=nullptr;
}
conn.h2_ctx.reset();
for(auto const&[_,stream]:conn.h2_streams)
clear_deferred_wait(stream.deferred_efd);
conn.h2_streams.clear();
conn.h2_pending_send.clear();
conn.is_h2=false;
conn.h2_sse_stream_id=-1;
conn.h2_sse_pending_wait=false;
#endif
}
// Acquire a raw SQE without implicit submission. Returns null when the ring
// is exhausted or fatal; callers handle that via defer_op() to avoid stalls.
io_uring_sqe*get_sqe(){
if(ring_fatal_)
return nullptr;
auto sqe=raw_.try_get_sqe();
return sqe?sqe.raw():nullptr;
}
// Defer an op whose SQE allocation failed. Replayed from run_loop once
// the CQE reap frees ring capacity.
void defer_op(
conflux::work::root::detail::small_move_only_function<void()>op){
if(ring_fatal_)
return;
pending_ops.push_back(move(op));
}
void drain_pending_ops(){
SZ remaining=pending_ops.size();
while(remaining>0&&!pending_ops.empty()){
auto op=move(pending_ops.front());
pending_ops.pop_front();
--remaining;
op();
}
}
void queue_multishot_accept(){
auto listen_handle=
listen_fixed?SocketHandle::from_direct(static_cast<u32>(listen_fd)):SocketHandle::from_os(listen_fd);
if(!submit_accept_multishot_borrowed(
raw_,
listen_handle,
reinterpret_cast<sockaddr*>(&client_addr),
&client_addr_len,
pack(Op::Accept,0,listen_fd),
caps,
accepted_sockets_direct))
defer_op([this]{queue_multishot_accept();});
}
[[nodiscard]]RecvArmPolicy resolve_recv_arm_policy(Conn const&conn)const noexcept{
return::resolve_recv_arm_policy(
auto_recv_arm_policy,
caps.recv_poll_first,
conn.have_last_recv_cqe_flags,
conn.last_recv_cqe_flags);
}
void queue_multishot_recv(
int fd){
auto&conn=conn_for(fd);
auto handle=
accepted_sockets_direct?SocketHandle::from_direct(static_cast<u32>(fd)):SocketHandle::from_os(fd);
auto const arm=resolve_recv_arm_policy(conn);
if(!submit_recv_multishot(raw_,handle,*buf_ring_,pack(Op::Recv,conn.gen,fd),use_recv_bundle,arm)){
defer_op([this,fd]{queue_multishot_recv(fd);});
return;
}
conn.recv_armed=true;
}
void queue_direct_accept_setup(
int fd){
auto&conn=conn_for(fd);
auto handle=SocketHandle::from_direct(static_cast<u32>(fd));
DirectTcpAcceptSetup setup{};
setup.tcp_quickack_once=caps.cmd_sock_setsockopt;
setup.skip_sockopt_success_cqes=true;
if(!submit_direct_tcp_accept_setup_recv(
raw_,
handle,
*buf_ring_,
pack(Op::Nop,0,0),
pack(Op::Recv,conn.gen,fd),
setup)){
defer_op([this,fd]{queue_direct_accept_setup(fd);});
return;
}
conn.recv_armed=true;
}
// Submit WRITEV for a mapped-file response.
// Adjusts iovecs to skip bytes already sent (conn.written).
void queue_send_mapped(
int fd){
auto&conn=conn_for(fd);
SZ skip=conn.written;
SZ ni{};

// iov[0]: remaining header bytes
if(skip<conn.own_response.size()){
span<char>const hdr_span{conn.own_response};
// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
conn.writev_iov[ni++]={
.iov_base=static_cast<void*>(hdr_span.subspan(skip).data()),
.iov_len=hdr_span.subspan(skip).size()};
skip=0;
}else{
skip-=conn.own_response.size();
}
// iov[1]: remaining file bytes (honouring send_offset for range requests)
if(conn.mapped_file&&skip<conn.mapped_file->send_size){
span<char>const file_span{
static_cast<char*>(conn.mapped_file->ptr),
conn.mapped_file->send_offset+conn.mapped_file->send_size};
// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
conn.writev_iov[ni++]={
.iov_base=file_span.subspan(conn.mapped_file->send_offset+skip).data(),
.iov_len=conn.mapped_file->send_size-skip};
}

if(ni==0)
return;
auto handle=
accepted_sockets_direct?SocketHandle::from_direct(static_cast<u32>(fd)):SocketHandle::from_os(fd);
if(!submit_writev_borrowed(
raw_,
handle,
conn.writev_iov.data(),
static_cast<unsigned>(ni),
pack(Op::Send,conn.gen,fd)))
defer_op([this,fd]{queue_send_mapped(fd);});
}
// file_io streaming path. Phase 1: send headers via prep_send. Phase 2:
// once headers are fully delivered, kick off splice (plain) or read_fixed+
// SSL_write (TLS). queue_send_streamed only handles phase 1; phase 2 is
// triggered from handle_send once the header bytes are acked.
void queue_send_streamed(
int fd){
auto&conn=conn_for(fd);
if(conn.streamed_headers_sent){
// Phase 2: start splice (or continue by re-arming if already flying).
if(!conn.streamed_splice_in_flight)
start_streamed_body(fd);
return;
}
auto const hdr_view=span{conn.own_response}.subspan(conn.written);
auto handle=
accepted_sockets_direct?SocketHandle::from_direct(static_cast<u32>(fd)):SocketHandle::from_os(fd);
if(!submit_send_borrowed(raw_,handle,hdr_view.data(),hdr_view.size(),pack(Op::Send,conn.gen,fd)))
defer_op([this,fd]{queue_send_streamed(fd);});
}
// Acquire a pipe P and submit the splice chain via FileReader. Completion
// calls back into handle_streamed_splice_done on the ring thread.
void start_streamed_body(
int fd){
auto&conn=conn_for(fd);
if(!conn.streamed_file||!files||!splice_pipes){
queue_close(fd);
return;
}
auto pipe=splice_pipes->try_acquire();
if(!pipe){
// All pipe pairs in-flight; retry from next CQE drain once one is returned.
defer_op([this,fd]{start_streamed_body(fd);});
return;
}
auto const remaining=conn.streamed_file->send_size-conn.streamed_delivered;
auto const off=conn.streamed_file->send_offset+conn.streamed_delivered;
conn.streamed_splice_in_flight=true;
auto const conn_gen=conn.gen;
do_streamed_splice(
this,
fd,
conn_gen,
files->splice_to_fd(
*conn.streamed_file->handle,
off,
static_cast<SZ>(remaining),
fd,
move(*pipe),
accepted_sockets_direct))
.detach();
}
#if CONFLUX_HAS_TLS
// TLS streamed body: acquire a FixedBuffer, read_fixed a chunk of the file,
// SSL_write it into wbio, flush and re-queue the TLS send. Pipelining depth
// is effectively 1 per connection — suitable for unbuffered streaming.
void start_streamed_tls_chunk(
int fd){
auto&conn=conn_for(fd);
if(!conn.streamed_file||!files||!fixed_buffers){
queue_close(fd);
return;
}
auto buf=fixed_buffers->try_acquire();
if(!buf){
// All slabs in-flight; retry from the next CQE drain once one is returned.
defer_op([this,fd]{start_streamed_tls_chunk(fd);});
return;
}
auto const remaining=conn.streamed_file->send_size-conn.streamed_delivered;
auto const off=conn.streamed_file->send_offset+conn.streamed_delivered;
auto const want=static_cast<SZ>(min<u64>(remaining,buf->size()));
FixedBuffer b=move(*buf);
auto const conn_gen=conn.gen;
conn.streamed_splice_in_flight=true;
auto&fh=*conn.streamed_file->handle;
do_streamed_tls_chunk(this,fd,conn_gen,want,files->read_fixed(fh,off,move(b),want)).detach();
}
void on_streamed_tls_chunk_done(
int fd,
u32 conn_gen,
FixedBuffer buf,
SZ bytes,
EP const&err){
auto const ufd=static_cast<SZ>(fd);
if(ufd>=fd_table.size()||fd_table[ufd].gen!=conn_gen)
return;
auto&conn=fd_table[ufd];
conn.streamed_splice_in_flight=false;
if(err||conn.ssl==nullptr||!conn.streamed_file){
conn.streamed_file.reset();
queue_close(fd);
return;
}
if(bytes==0){
// EOF earlier than expected — treat as done; trailing bytes won't
// be invented.
conn.streamed_file.reset();
queue_close(fd);
return;
}
auto view=buf.view().subspan(0,bytes);
auto const w=SSL_write(conn.ssl.get(),view.data(),static_cast<int>(view.size()));
if(w<=0){
conn.streamed_file.reset();
queue_close(fd);
return;
}
conn.streamed_delivered+=static_cast<u64>(w);
tls_flush_wbio(conn);
conn.tls_sending_response=true;
conn.send_queued=true;
tls_queue_send(conn);
// `buf` drops here → slab returned to pool.
}
void write_mapped_tls_chunk(
int fd,
Conn&conn){
if(!conn.mapped_file||conn.ssl==nullptr){
queue_close(fd);
return;
}
auto const remaining=conn.mapped_file->send_size-conn.mapped_delivered;
if(remaining==0)
return;
static constexpr u64 kMappedTlsChunk=64U*1024U;
auto const want=static_cast<SZ>(min<u64>(remaining,kMappedTlsChunk));
auto const*data=
static_cast<char const*>(conn.mapped_file->ptr)+conn.mapped_file->send_offset+conn.mapped_delivered;
auto const w=SSL_write(conn.ssl.get(),data,static_cast<int>(want));
if(w<=0){
conn.mapped_file.reset();
queue_close(fd);
return;
}
conn.mapped_delivered+=static_cast<u64>(w);
tls_flush_wbio(conn);
conn.tls_sending_response=true;
conn.send_queued=true;
tls_queue_send(conn);
}
#endif
void on_streamed_splice_done(
int fd,
u32 conn_gen,
SZ delivered,
EP const&err){
auto const ufd=static_cast<SZ>(fd);
if(ufd>=fd_table.size()||fd_table[ufd].gen!=conn_gen)
return;
auto&conn=fd_table[ufd];
conn.streamed_splice_in_flight=false;
if(err||!conn.streamed_file){
try{
if(err)
rethrow_exception(err);
}catch(...){}
conn.streamed_file.reset();
queue_close(fd);
return;
}
conn.streamed_delivered+=delivered;
if(conn.streamed_delivered<conn.streamed_file->send_size){
// Short splice — kick another leg.
start_streamed_body(fd);
return;
}
// Fully streamed — release handle (ring will close via close_async
// on drop; FileHandle from_fd path closes synchronously, acceptable
// since data is already delivered).
conn.streamed_file.reset();
conn.written=0;
conn.send_queued=false;
conn.has_response=false;
conn.own_response.clear();
#if CONFLUX_HAS_TLS
// kTLS file body went through splice, not tls_queue_send — clear manually.
if(conn.ktls_send)
conn.tls_sending_response=false;
#endif
handle_send_complete(fd,conn);
}
void queue_send(
int fd){
auto&conn=conn_for(fd);
#if CONFLUX_HAS_TLS
if(conn.ssl!=nullptr){
// TLS path: encrypt the response into tls_send_buf, then send.
if(conn.mapped_file&&!conn.has_response){
if(conn.written<conn.own_response.size()){
auto const hdr=span{conn.own_response}.subspan(conn.written);
char const*data=hdr.data();
int remaining=static_cast<int>(hdr.size());
while(remaining>0){
auto const w=SSL_write(conn.ssl.get(),data,remaining);
if(w<=0){
queue_close(fd);
return;
}
data+=w;
remaining-=w;
conn.written+=static_cast<SZ>(w);
}
tls_flush_wbio(conn);
conn.tls_sending_response=true;
tls_queue_send(conn);
return;
}
write_mapped_tls_chunk(fd,conn);
return;
}
if(!conn.has_response)
return;
auto const&resp=conn.own_response;
char const*data=resp.data();
int remaining=static_cast<int>(resp.size());
while(remaining>0){
auto const w=SSL_write(conn.ssl.get(),data,remaining);
if(w<=0){
queue_close(fd);
return;
}
data+=w;
remaining-=w;
}
tls_flush_wbio(conn);
conn.tls_sending_response=true;
tls_queue_send(conn);
return;
}
#endif
if(conn.mapped_file){
queue_send_mapped(fd);
return;
}
if(conn.streamed_file){
queue_send_streamed(fd);
return;
}
if(!conn.has_response)
return;
auto const&resp=conn.own_response;
auto const resp_view=span{resp}.subspan(conn.written);
auto handle=
accepted_sockets_direct?SocketHandle::from_direct(static_cast<u32>(fd)):SocketHandle::from_os(fd);
if(!submit_send_borrowed(raw_,handle,resp_view.data(),resp_view.size(),pack(Op::Send,conn.gen,fd)))
defer_op([this,fd]{queue_send(fd);});
}
enum class RecvCancelState:u8{
not_armed,
submitted,
deferred
};
RecvCancelState cancel_recv_if_armed(int fd,bool retry_queue_close){
auto const ufd=static_cast<SZ>(fd);
if(ufd>=fd_table.size())return RecvCancelState::not_armed;
auto&conn=fd_table[ufd];
if(conn.fd<0||!conn.recv_armed)return RecvCancelState::not_armed;
auto handle=accepted_sockets_direct?SocketHandle::from_direct(static_cast<u32>(fd)):SocketHandle::from_os(conn.fd);
if(!submit_cancel_multishot_recv(raw_,handle,pack(Op::Nop,0,0))){
u32 const gen=conn.gen;
if(retry_queue_close)
defer_op([this,fd,ufd,gen]{
if(ufd<fd_table.size()&&fd_table[ufd].gen==gen)
queue_close(fd);
});
else
defer_op([this,fd,ufd,gen]{
if(ufd<fd_table.size()&&fd_table[ufd].gen==gen&&!fd_table[ufd].closing)
cancel_recv_if_armed(fd,false);
});
return RecvCancelState::deferred;
}
conn.recv_armed=false;
return RecvCancelState::submitted;
}
void queue_close(
int fd){
u32 gen=0;
auto const ufd=static_cast<SZ>(fd);
if(ufd<fd_table.size()){
if(fd_table[ufd].closing)
return;
gen=fd_table[ufd].gen;
}
if(cancel_recv_if_armed(fd,true)==RecvCancelState::deferred)
return;

if(accepted_sockets_direct){
if(ufd<fd_table.size()){
if(fd_table[ufd].gen!=gen||fd_table[ufd].closing)
return;
}
auto handle=SocketHandle::from_direct(static_cast<u32>(fd));
if(!submit_shutdown_close(raw_,handle,pack(Op::Nop,0,0),pack(Op::Close,gen,fd))){
defer_op([this,fd,ufd,gen]{
if(ufd>=fd_table.size()||fd_table[ufd].gen!=gen)
return;
queue_close(fd);
});
return;
}
if(ufd<fd_table.size())
fd_table[ufd].closing=true;
if(direct_slots_){
if(!direct_slots_->mark_closing(static_cast<u32>(fd)))
eprintln(format("queue_close: mark_closing failed slot={}",fd));
}
return;
}

// Send FIN immediately so the peer sees EOF regardless of any
// in-flight io_uring operations (e.g. multishot recv) that hold
// a reference to the file description and would otherwise delay
// the actual socket close until those operations complete.
::shutdown(fd,SHUT_WR);

if(ufd<fd_table.size()){
if(fd_table[ufd].gen!=gen||fd_table[ufd].closing)
return;
}
auto handle=SocketHandle::from_os(fd);
if(!submit_close(raw_,handle,pack(Op::Close,gen,fd))){
defer_op([this,fd,ufd,gen]{
if(ufd>=fd_table.size()||fd_table[ufd].gen!=gen)
return;
queue_close(fd);
});
return;
}
if(ufd<fd_table.size())
fd_table[ufd].closing=true;
}
void queue_sse_wait(
int fd){
auto&conn=conn_for(fd);
auto*sqe=get_sqe();
if(sqe==nullptr){
defer_op([this,fd]{queue_sse_wait(fd);});
return;
}
auto const ud=pack(Op::SsePoll,conn.gen,fd);
auto buf=make_unique<u64>(0);
// Blocking read on the eventfd — io_uring uses io-wq when the fd
// is not immediately readable.  Offset 0 is ignored for eventfd.
io_uring_prep_read(sqe,conn.sse_efd,buf.get(),sizeof(u64),0);
io_uring_sqe_set_data64(sqe,ud);
in_flight_read_bufs[ud]=move(buf);
}
void queue_deferred_wait(
int fd){
auto&conn=conn_for(fd);
queue_deferred_wait(fd,conn.deferred_efd,conn.deferred_response);
}
void arm_shutdown_read(){
auto*sqe=get_sqe();
if(sqe==nullptr){
defer_op([this]{arm_shutdown_read();});
return;
}
io_uring_prep_read(sqe,shutdown_efd,&shutdown_buf,sizeof(shutdown_buf),0);
io_uring_sqe_set_data64(sqe,pack(Op::Shutdown,0,0));
}
// Arm a one-shot periodic timer that fires every ~1 second for connection reaping.
void arm_timer(){
if(shutting_down)
return;
if(request_timeout_ms==0&&tls_sniff_timeout_ms==0)
return;
timer_ts.tv_sec=1;
timer_ts.tv_nsec=0;
if(!submit_timeout_borrowed(raw_,&timer_ts,pack(Op::Timer,0,0)))
defer_op([this]{arm_timer();});
}
void handle_timer(){
if(request_timeout_ms==0&&tls_sniff_timeout_ms==0)
return;
auto now=chrono::steady_clock::now();
auto req_limit=chrono::milliseconds{request_timeout_ms};
auto sniff_limit=chrono::milliseconds{tls_sniff_timeout_ms};
for(auto&conn:fd_table){
if(conn.fd<0)
continue;
if(conn.is_sse){
continue;
}// SSE streams are exempt
if(conn.is_deferred){
// Deferred responses self-expire: expire_if_past_deadline forces a 504 and
// wakes the eventfd that the deferred-poll SQE is watching.
if(conn.deferred_response)
conn.deferred_response->expire_if_past_deadline(now);
continue;
}
if(conn.send_queued){
continue;
}// mid-send: handler already responded
// TLS sniff-undecided sentinel: ssl==nullptr && tls_hs_done==true && partial empty.
// Use the (usually shorter) sniff timeout to reap silent connections that opened
// the TCP socket but never sent a byte.
bool const sniff_undecided=conn.ssl==nullptr&&conn.tls_hs_done&&conn.partial.empty();
if(sniff_undecided&&tls_sniff_timeout_ms!=0){
if(now-conn.last_activity>sniff_limit)
queue_close(conn.fd);
continue;
}
if(request_timeout_ms!=0){
auto const ref=conn.request_in_progress?conn.request_started:conn.last_activity;
if(now-ref>req_limit)
queue_close(conn.fd);
}
}
arm_timer();// re-arm for next tick
}
void handle_shutdown(){
shutting_down=true;
// Cancel the multishot accept — no new connections.
submit_cancel_by_ud(raw_,pack(Op::Accept,0,listen_fd),0);
// Close idle connections immediately; mark active ones to close after send.
for(SZ i=0;i<fd_table.size();++i){
auto&conn=fd_table[i];
if(conn.fd<0)
continue;
if(conn.sse_channel)
conn.sse_channel->close();
if(conn.send_queued){
cancel_recv_if_armed(static_cast<int>(i),false);
conn.close_after_send=true;
}else{
queue_close(static_cast<int>(i));
}
}
}
void handle_accept(
int res,
u32 flg){
if(res>=0){
if(accepted_sockets_direct&&direct_slots_){
if(!direct_slots_->adopt_kernel_allocated(static_cast<u32>(res))){
eprintln(
format("handle_accept: adopt_kernel_allocated failed slot={} — stopping direct accept",res));
accepted_sockets_direct=false;
submit_cancel_by_ud(raw_,pack(Op::Accept,0,listen_fd),0);
auto const ud=pack(Op::DirectSlotClose,0,res);
if(!submit_close(raw_,SocketHandle::from_direct(static_cast<u32>(res)),ud))
defer_op([this,res,ud]{
submit_close(raw_,SocketHandle::from_direct(static_cast<u32>(res)),ud);
});
return;
}
}
auto&conn=conn_for(res);
++conn.gen;
conn.fd=res;
conn.recv_armed=false;
conn.last_recv_cqe_flags=0;
conn.have_last_recv_cqe_flags=false;
conn.have_incremental_buf_id=false;
conn.send_queued=false;
conn.closing=false;
conn.has_response=false;
conn.written=0;
conn.is_sse=false;
conn.sse_headers_sent=false;
conn.is_deferred=false;
conn.sse_efd=-1;
conn.sse_channel.reset();
conn.deferred_efd=-1;
conn.deferred_response.reset();
conn.ws_upgrade.reset();
conn.partial.clear();
conn.chunked_decode.reset();
conn.mapped_file.reset();
conn.mapped_total=0;
conn.mapped_delivered=0;
conn.last_activity=chrono::steady_clock::now();
if(!accepted_sockets_direct){
sockaddr_in6 peer_addr{};
socklen_t peer_len=sizeof(peer_addr);
if(::getpeername(res,reinterpret_cast<sockaddr*>(&peer_addr),&peer_len)==0)
conn.remote_addr=ip_to_string(peer_addr.sin6_addr);
else
conn.remote_addr.clear();
}else{
conn.remote_addr=ip_to_string(client_addr.sin6_addr);
}
conn.is_tls=false;
#if CONFLUX_HAS_TLS
// Free any SSL left by a prior tenant on this fd slot.
if(conn.ssl!=nullptr)
conn.ssl.reset();
conn.tls_send_buf.clear();
conn.tls_send_off=0;
conn.tls_recv_buf.clear();
// Sentinel: ssl==nullptr && tls_hs_done==true means "waiting for first byte".
// SSL_new() is deferred to phase1_copy_recv_bufs after the first-byte sniff.
// ssl_ctx==nullptr (plain-only server): tls_hs_done stays false — no sniff needed.
conn.tls_hs_done=(ssl_ctx!=nullptr);
conn.tls_sending_response=false;
#endif
#if CONFLUX_HAS_HTTP2
if(conn.h2_session!=nullptr){
nghttp2_session_del(conn.h2_session);
conn.h2_session=nullptr;
}
conn.h2_ctx.reset();
conn.h2_streams.clear();
conn.h2_pending_send.clear();
conn.is_h2=false;
conn.h2_sse_stream_id=-1;
conn.h2_sse_pending_wait=false;
#endif
if(!accepted_sockets_direct){
::setsockopt(res,IPPROTO_TCP,TCP_NODELAY,&tcp_opt_one_,sizeof tcp_opt_one_);
::setsockopt(res,IPPROTO_TCP,TCP_QUICKACK,&tcp_opt_one_,sizeof tcp_opt_one_);
queue_multishot_recv(res);
}else{
queue_direct_accept_setup(res);
}
if(!cqe_has_more(flg))
queue_multishot_accept();
}
}
void discard_recv_bufs(
int res,
u32 flags)noexcept{
if(!cqe_has_buffer(flags))
return;
if(buf_ring_->mode()==BufferRingMode::incremental){
if(res<=0)
return;
// slice dtor calls recycle_if_final()
auto _=try_buffer_slice_from_incremental_cqe(*buf_ring_,res,flags);
return;
}
auto slices=buffer_slices_from_cqe(*buf_ring_,res,flags,use_recv_bundle);
slices.recycle_all();
}
void discard_recv_bufs(
RecvComp&rc)noexcept{
discard_recv_bufs(rc.res,rc.flags);
rc.flags=0;
}
void retire_incremental_partial(
int fd,
u32 gen,
Conn&conn)noexcept{
if(!conn.have_incremental_buf_id)
return;
retired_incremental_recv.insert_or_assign(
pack_fd_gen(fd,gen),
RetiredIncrementalBuf{conn.incremental_buf_id,true});
conn.have_incremental_buf_id=false;
}
void reclaim_retired_incremental_recv(
int fd,
u32 gen)noexcept{
if(buf_ring_->mode()!=BufferRingMode::incremental)
return;
auto it=retired_incremental_recv.find(pack_fd_gen(fd,gen));
if(it==retired_incremental_recv.end()||!it->second.present)
return;
buf_ring_->reclaim_incremental_partial(it->second.id);
retired_incremental_recv.erase(it);
}
void clear_retired_incremental_if_final(
int fd,
u32 gen,
u32 flags)noexcept{
if(buf_ring_->mode()!=BufferRingMode::incremental)
return;
if(!cqe_has_buffer(flags)||cqe_has_buf_more(flags))
return;
auto const key=pack_fd_gen(fd,gen);
auto it=retired_incremental_recv.find(key);
if(it==retired_incremental_recv.end())
return;
u16 const id=cqe_buffer_id(flags);
if(it->second.id!=id)[[unlikely]]
return;
retired_incremental_recv.erase(it);
}
void handle_recv_cqe(
int fd,
int res,
u32 flg,
u32 gen){
auto const ufd=static_cast<SZ>(fd);
if(ufd>=fd_table.size()){
discard_recv_bufs(res,flg);
return;
}
bool const gen_match=fd_table[ufd].gen==gen;
bool const ws_pending=ws_cancel_handoffs.find(fd)!=ws_cancel_handoffs.end();
if(!gen_match&&!ws_pending){
if(res<=0&&!cqe_has_buffer(flg)){
reclaim_retired_incremental_recv(fd,gen);
}else if(res>0&&cqe_has_buffer(flg)){
discard_recv_bufs(res,flg);
clear_retired_incremental_if_final(fd,gen,flg);
return;
}
discard_recv_bufs(res,flg);
return;
}
if(!cqe_has_more(flg)&&gen_match)
fd_table[ufd].recv_armed=false;
if(res>0&&gen_match&&fd_table[ufd].fd>=0){
auto&conn=fd_table[ufd];
conn.last_recv_cqe_flags=flg;
conn.have_last_recv_cqe_flags=true;
}
recvs.push_back({fd,res,gen,flg});
}
bool handle_sse_send_complete(
int fd,
Conn&conn){
if(!conn.is_sse)
return false;
if(!conn.sse_headers_sent){
conn.sse_headers_sent=true;
queue_sse_wait(fd);
return true;
}
auto remaining=conn.sse_channel->drain();
if(!remaining.empty()){
conn.own_response=move(remaining);
conn.has_response=true;
conn.written=0;
conn.send_queued=true;
queue_send(fd);
}else if(conn.sse_channel->is_closed()){
queue_close(fd);
}else{
queue_sse_wait(fd);
}
return true;
}
void handle_http_response_send_complete(
int fd,
Conn&conn){
if(conn.close_after_send){
conn.close_after_send=false;
queue_close(fd);
return;
}
conn.partial.consume(conn.request_bytes);
if(conn.request_bytes>0){
conn.expect_continue_sent=false;
conn.chunked_decode.reset();
}
conn.request_bytes=0;
if(conn.partial.empty())
conn.request_in_progress=false;
else
conn.request_started=chrono::steady_clock::now();
if(!conn.partial.empty()){
dispatch_request(
conn,
conn.partial.view(),
*this,
max_body_size,
http_redirect_to_https,
https_redirect_hosts,
parser_limits);
if(conn.has_response||conn.mapped_file){
conn.send_queued=true;
queue_send(fd);
return;
}
if(conn.is_deferred){
queue_deferred_wait(fd);
return;
}
}
if(!conn.recv_armed)
queue_multishot_recv(fd);
}
[[nodiscard]]static bool make_blocking_fd(
int fd){
// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
int const flags=fcntl(fd,F_GETFL,0);
if(flags<0)
return false;
return fcntl(fd,F_SETFL,static_cast<int>(static_cast<unsigned>(flags)&~static_cast<unsigned>(O_NONBLOCK)))==0;
// NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
}
[[nodiscard]]WsHandoffState begin_ws_handoff(
Conn&conn){
auto state=WsHandoffState{move(conn.ws_upgrade),move(conn.ws_work_pool),move(conn.saved_req)};
++conn.gen;
conn.fd=-1;
conn.is_ws=false;
conn.recv_armed=false;
conn.send_queued=false;
return state;
}
void launch_plain_ws_handler(
WorkPool&pool,
WsHandoffState state,
int fd,
S initial_buf){
if(!pool.enqueue([state=move(state),fd,ibuf=move(initial_buf)]()mutable{
WsConn ws{fd,move(ibuf)};
state.upgrade->handler(state.request,ws);
::close(fd);
}))
::close(fd);
}
void finish_plain_ws_handoff(
int fd,
WsInstallEntry entry){
if(accepted_sockets_direct){
queue_ws_fixed_install(fd,move(entry.state),move(entry.initial_buf));
return;
}
if(!make_blocking_fd(fd)){
::close(fd);
return;
}
auto&pool=*entry.state.pool;
launch_plain_ws_handler(pool,move(entry.state),fd,move(entry.initial_buf));
}
void handoff_plain_ws(
Conn&conn,
int fd){
conn.partial.consume(conn.request_bytes);
conn.request_bytes=0;
S initial_buf=conn.partial.take();
bool const cancel_recv=conn.recv_armed;
retire_incremental_partial(fd,conn.gen,conn);
auto state=begin_ws_handoff(conn);
if(!state.pool){
if(accepted_sockets_direct){
if(direct_slots_&&!direct_slots_->mark_closing(static_cast<u32>(fd)))
eprintln(format("handoff_plain_ws: mark_closing failed slot={}",fd));
auto const ud=pack(Op::DirectSlotClose,0,fd);
if(!submit_close(raw_,SocketHandle::from_direct(static_cast<u32>(fd)),ud))
defer_op(
[this,fd,ud]{submit_close(raw_,SocketHandle::from_direct(static_cast<u32>(fd)),ud);});
}else{
::close(fd);
}
return;
}
auto entry=WsInstallEntry{
move(state),
move(initial_buf)
#if CONFLUX_HAS_TLS
,
nullptr
#endif
};
if(cancel_recv){
queue_ws_cancel(fd,move(entry));
return;
}
finish_plain_ws_handoff(fd,move(entry));
}
#if CONFLUX_HAS_TLS
void launch_tls_ws_handler(
WorkPool&pool,
WsHandoffState state,
int fd,
SSL*ssl,
S initial_buf){
UniqueSsl owned{ssl};
if(!pool.enqueue([state=move(state),fd,ssl_owned=move(owned),ibuf=move(initial_buf)]()mutable{
WsConn ws{fd,ssl_owned.release(),move(ibuf)};
state.upgrade->handler(state.request,ws);
::close(fd);
}))
::close(fd);
}
void handoff_tls_ws(
Conn&conn,
int fd){
// Strip the HTTP request bytes — only post-header data belongs in the
// WS initial buffer (pipelined WS data, if any).
conn.partial.consume(conn.request_bytes);
conn.request_bytes=0;

S initial_buf=conn.partial.take();
auto orig_ssl=move(conn.ssl);// transfer ownership to the thread
bool const cancel_recv=conn.recv_armed;
retire_incremental_partial(fd,conn.gen,conn);
auto state=begin_ws_handoff(conn);
if(!state.pool){
orig_ssl.reset();
if(accepted_sockets_direct){
if(direct_slots_&&!direct_slots_->mark_closing(static_cast<u32>(fd)))
eprintln(format("handoff_tls_ws: mark_closing failed slot={}",fd));
auto const ud=pack(Op::DirectSlotClose,0,fd);
if(!submit_close(raw_,SocketHandle::from_direct(static_cast<u32>(fd)),ud))
defer_op(
[this,fd,ud]{submit_close(raw_,SocketHandle::from_direct(static_cast<u32>(fd)),ud);});
}else{
::close(fd);
}
return;
}
auto entry=WsInstallEntry{move(state),move(initial_buf),move(orig_ssl)};
if(cancel_recv){
queue_ws_cancel(fd,move(entry));
return;
}
finish_tls_ws_handoff(fd,move(entry));
}
#endif
void queue_ws_cancel(
int fd,
WsInstallEntry entry){
auto*sqe=get_sqe();
if(sqe==nullptr){
defer_op([this,fd,e=move(entry)]()mutable{queue_ws_cancel(fd,move(e));});
return;
}
ws_cancel_handoffs.emplace(fd,move(entry));
auto handle=
accepted_sockets_direct?SocketHandle::from_direct(static_cast<u32>(fd)):SocketHandle::from_os(fd);
io_uring_prep_cancel_fd(sqe,handle.as_fd(),handle.fixed?IORING_ASYNC_CANCEL_FD_FIXED:0);
io_uring_sqe_set_data64(sqe,pack(Op::WsCancel,0,fd));
}
#if CONFLUX_HAS_TLS
void finish_tls_ws_handoff(
int fd,
WsInstallEntry entry){
if(accepted_sockets_direct){
queue_ws_fixed_install(fd,move(entry.state),move(entry.initial_buf),entry.ssl.release());
return;
}
// Replace memory BIOs with a socket BIO and make fd blocking.
// TRICKS.md #2 says "DO NOT call SSL_set_fd" for the io_uring path.
// Here we're exiting that path — blocking I/O is correct for the WS thread.
SSL_set_fd(entry.ssl.get(),fd);// replaces memory BIOs with socket BIOs
if(!make_blocking_fd(fd)){
entry.ssl.reset();
::close(fd);
return;
}
auto&pool=*entry.state.pool;
launch_tls_ws_handler(pool,move(entry.state),fd,entry.ssl.release(),move(entry.initial_buf));
}
#endif
void handle_ws_cancel(
int fd){
auto it=ws_cancel_handoffs.find(fd);
if(it==ws_cancel_handoffs.end())
return;
ws_cancel_ready.push_back(fd);
}
void queue_ws_fixed_install(
int slot_fd,
WsHandoffState state,
S initial_buf
#if CONFLUX_HAS_TLS
,
SSL*ssl=nullptr
#endif
){
ws_installs.emplace(
slot_fd,
WsInstallEntry{
move(state),
move(initial_buf)
#if CONFLUX_HAS_TLS
,
UniqueSsl{ssl}
#endif
});
if(!submit_fixed_fd_install(raw_,static_cast<u32>(slot_fd),pack(Op::FixedFdInstall,0,slot_fd))){
auto entry=move(ws_installs.at(slot_fd));
ws_installs.erase(slot_fd);
defer_op([this,
slot_fd,
s=move(entry.state),
ib=move(entry.initial_buf)
#if CONFLUX_HAS_TLS
,
ssl_raw=entry.ssl.release()
#endif
]()mutable{
queue_ws_fixed_install(
slot_fd,
move(s),
move(ib)
#if CONFLUX_HAS_TLS
,
ssl_raw
#endif
);
});
}
}
void handle_fixed_fd_install(
int slot_fd,
int real_fd){
auto it=ws_installs.find(slot_fd);
if(it==ws_installs.end()){
if(real_fd>=0)
::close(real_fd);
return;
}
auto entry=move(it->second);
ws_installs.erase(it);

auto free_slot=[this,slot_fd]{
if(direct_slots_&&!direct_slots_->mark_closing(static_cast<u32>(slot_fd)))
eprintln(format("free_slot: mark_closing failed slot={}",slot_fd));
if(!submit_close(
raw_,
SocketHandle::from_direct(static_cast<u32>(slot_fd)),
pack(Op::DirectSlotClose,0,slot_fd)))
defer_op([this,slot_fd]{
submit_close(
raw_,
SocketHandle::from_direct(static_cast<u32>(slot_fd)),
pack(Op::DirectSlotClose,0,slot_fd));
});
};
free_slot();

if(real_fd<0||!entry.state.pool){
if(real_fd>=0)
::close(real_fd);
return;
}

#if CONFLUX_HAS_TLS
if(entry.ssl){
SSL_set_fd(entry.ssl.get(),real_fd);
if(!make_blocking_fd(real_fd)){
entry.ssl.reset();
::close(real_fd);
return;
}
auto&pool=*entry.state.pool;
launch_tls_ws_handler(pool,move(entry.state),real_fd,entry.ssl.release(),move(entry.initial_buf));
return;
}
#endif

if(!make_blocking_fd(real_fd)){
::close(real_fd);
return;
}
auto&pool=*entry.state.pool;
launch_plain_ws_handler(pool,move(entry.state),real_fd,move(entry.initial_buf));
}
#if CONFLUX_HAS_TLS
// Called when all bytes in tls_send_buf have been sent.  Drives the
// post-send state machine for TLS connections.
void handle_send_tls_complete(
int fd,
Conn&conn){
conn.tls_send_buf.clear();
conn.tls_send_off=0;
conn.send_queued=false;

#if CONFLUX_HAS_HTTP2
if(conn.is_h2){
if(conn.close_after_send){
queue_close(fd);
return;
}
// Drive any nghttp2 output queued while the previous send was in flight,
// then re-arm recv if nothing new was sent.
h2_do_send(conn);
if(!conn.send_queued&&!conn.recv_armed)
queue_multishot_recv(fd);
return;
}
#endif

if(conn.tls_sending_response){
if(conn.mapped_file){
if(conn.mapped_delivered<conn.mapped_file->send_size){
write_mapped_tls_chunk(fd,conn);
return;
}
conn.mapped_file.reset();
conn.mapped_total=0;
conn.mapped_delivered=0;
}

// file_io TLS streaming: after the header batch is acked, pull
// plaintext via read_fixed and SSL_write one chunk; repeat until the
// whole file is delivered. handle_send_tls_complete fires once per
// tls_send_buf drain, so this naturally interleaves with TLS sends.
if(conn.streamed_file){
if(conn.streamed_delivered<conn.streamed_file->send_size){
if(!conn.streamed_splice_in_flight){
if(conn.ktls_send&&splice_pipes)
start_streamed_body(fd);
else
start_streamed_tls_chunk(fd);
}
return;
}
conn.streamed_file.reset();
conn.streamed_headers_sent=false;
conn.streamed_delivered=0;
}

// An HTTP response (or SSE/WS payload) was fully delivered.
conn.tls_sending_response=false;
conn.has_response=false;
conn.own_response.clear();
conn.written=0;

if(handle_sse_send_complete(fd,conn))
return;
if(conn.is_ws){
handoff_tls_ws(conn,fd);
return;
}
handle_http_response_send_complete(fd,conn);
}else{
// TLS handshake data was sent (or a post-handshake alert).
// If an HTTP response accumulated while we were busy, send it now.
if(conn.has_response){
conn.send_queued=true;
queue_send(fd);
}else if(!conn.recv_armed){
queue_multishot_recv(fd);
}
}
}
#endif// CONFLUX_HAS_TLS
// Called once a response (or chunk) has been fully delivered.
// Drives SSE/WS/normal post-send state machine.
void handle_send_complete(
int fd,
Conn&conn){
if(handle_sse_send_complete(fd,conn))
return;
if(conn.is_ws){
handoff_plain_ws(conn,fd);
return;
}
handle_http_response_send_complete(fd,conn);
}
void handle_send(
int fd,
int res,
u32 gen){
auto const ufd=static_cast<SZ>(fd);
if(ufd>=fd_table.size()||fd_table[ufd].gen!=gen)
return;
auto&conn=fd_table[ufd];

#if CONFLUX_HAS_TLS
// TLS path: track progress through tls_send_buf.
if(conn.ssl!=nullptr){
if(res<=0){
queue_close(fd);
return;
}
conn.tls_send_off+=static_cast<SZ>(res);
if(conn.tls_send_off<conn.tls_send_buf.size()){
tls_queue_send(conn);// continue sending
return;
}
handle_send_tls_complete(fd,conn);
return;
}
#endif
if(conn.mapped_file){
// mmap/WRITEV path.
if(res<=0){
conn.mapped_file.reset();
queue_close(fd);
return;
}
conn.written+=static_cast<SZ>(res);
if(conn.written<conn.mapped_total){
queue_send_mapped(fd);// partial — resubmit with adjusted iovecs
return;
}
// Fully sent: release mmap region, clean up send state.
conn.mapped_file.reset();
conn.mapped_total=0;
conn.mapped_delivered=0;
conn.written=0;
conn.send_queued=false;
conn.own_response.clear();
handle_send_complete(fd,conn);
return;
}

if(conn.streamed_file){
if(res<=0){
conn.streamed_file.reset();
queue_close(fd);
return;
}
conn.written+=static_cast<SZ>(res);
if(conn.written<conn.own_response.size()){
queue_send_streamed(fd);// headers: resubmit remainder
return;
}
// Headers fully sent; kick off body streaming.
conn.streamed_headers_sent=true;
start_streamed_body(fd);
return;
}

if(res>0){
if(!conn.has_response){
conn.send_queued=false;
if(!conn.recv_armed&&!conn.is_sse&&!conn.is_ws&&!conn.is_deferred)
queue_multishot_recv(fd);
return;
}
conn.written+=static_cast<SZ>(res);
if(conn.written<conn.own_response.size()){
queue_send(fd);
return;
}
// Response (or chunk) fully sent.
conn.written=0;
conn.send_queued=false;
conn.has_response=false;
conn.own_response.clear();

handle_send_complete(fd,conn);
}else{
queue_close(fd);
}
}
void handle_sse_poll(
int fd,
int res,
u32 gen){
in_flight_read_bufs.erase(pack(Op::SsePoll,gen,fd));
auto const ufd=static_cast<SZ>(fd);
if(ufd>=fd_table.size()||fd_table[ufd].gen!=gen)
return;
auto&conn=fd_table[ufd];

// res == sizeof(u64) == 8 → io_uring read the eventfd counter;
// res < 0 → error (fd closed, cancelled, etc.) → tear down.
if(res<=0){
queue_close(fd);
return;
}

// io_uring already read (and reset) the eventfd counter into sse_read_buf.

#if CONFLUX_HAS_HTTP2
// H2 SSE: resume the deferred data stream and drive the nghttp2 send loop.
if(conn.is_h2&&conn.h2_sse_stream_id>=0){
if(conn.h2_session!=nullptr){
int const r=nghttp2_session_resume_data(conn.h2_session,conn.h2_sse_stream_id);
if(r==0)
h2_do_send(conn);
}
// Re-arm if the channel is still open.
if(conn.sse_channel&&!conn.sse_channel->is_closed()&&conn.h2_sse_stream_id>=0)
queue_sse_wait(fd);
return;
}
#endif

auto data=conn.sse_channel->drain();
if(!data.empty()){
conn.own_response=move(data);
conn.has_response=true;
conn.written=0;
conn.send_queued=true;
queue_send(fd);
// handle_send will re-arm wait or close after chunk is delivered.
}else if(conn.sse_channel->is_closed()){
queue_close(fd);
}else{
queue_sse_wait(fd);// spurious wakeup, re-arm
}
}
void handle_deferred_poll(
int deferred_efd,
int res,
u32 gen){
in_flight_read_bufs.erase(pack(Op::DeferredPoll,gen,deferred_efd));
auto it=deferred_waits.find(deferred_efd);
if(it==deferred_waits.end())
return;

auto const fd=it->second.conn_fd;
auto const stream_id=it->second.stream_id;
auto const ufd=static_cast<SZ>(fd);
if(ufd>=fd_table.size()||fd_table[ufd].gen!=gen){
deferred_waits.erase(it);
return;
}
auto&conn=fd_table[ufd];
if(res<=0||!it->second.response){
deferred_waits.erase(it);
queue_close(fd);
return;
}

auto ready=it->second.response->take_ready();
if(!ready){
queue_deferred_wait(fd,deferred_efd,it->second.response,stream_id);
return;
}

deferred_waits.erase(it);
conn.last_activity=chrono::steady_clock::now();
if(stream_id>=0){
#if CONFLUX_HAS_HTTP2
auto stream_it=conn.h2_streams.find(stream_id);
if(!conn.is_h2||conn.h2_session==nullptr||stream_it==conn.h2_streams.end())
return;
stream_it->second.deferred_efd=-1;
h2_submit_response(conn,stream_id,move(*ready));
h2_do_send(conn);
if(conn.h2_sse_pending_wait){
conn.h2_sse_pending_wait=false;
queue_sse_wait(fd);
}
#endif
return;
}

conn.is_deferred=false;
conn.deferred_efd=-1;
conn.deferred_response.reset();
if(conn.deferred_head_only)
ready->head_only=true;
conn.deferred_head_only=false;
if(ready->is_mapped_file()){
conn.own_response=format_response(*ready,alt_svc_header,conn.close_after_send);
if(ready->head_only){
conn.has_response=true;
}else{
conn.mapped_file=ready->take_mapped_file();
conn.mapped_total=conn.own_response.size()+conn.mapped_file->send_size;
conn.mapped_delivered=0;
conn.has_response=false;
}
}else if(ready->is_streamed_file()){
conn.own_response=format_response(*ready,alt_svc_header,conn.close_after_send);
if(ready->head_only){
conn.has_response=true;
}else{
conn.streamed_file=ready->take_streamed_file();
conn.streamed_headers_sent=false;
conn.streamed_delivered=0;
conn.streamed_splice_in_flight=false;
conn.has_response=true;
}
}else{
conn.own_response=format_response(*ready,alt_svc_header,conn.close_after_send);
conn.has_response=true;
}
conn.written=0;
conn.send_queued=true;
queue_send(fd);
}
void handle_conn_close(
int fd,
int res,
u32 gen){
if(direct_slots_&&accepted_sockets_direct){
auto const slot=static_cast<u32>(fd);
if(res>=0){
if(!direct_slots_->release_closed(slot))
eprintln(format("handle_conn_close: release_closed failed slot={}",slot));
}else{
direct_slots_->poison(slot,res);
}
}
conn_erase(fd,gen);
}
void handle_direct_slot_close(
int fd,
int res){
if(!direct_slots_)
return;
auto const slot=static_cast<u32>(fd);
if(res>=0){
if(!direct_slots_->release_closed(slot))
eprintln(format("handle_direct_slot_close: release_closed failed slot={}",slot));
}else{
direct_slots_->poison(slot,res);
}
}
void dispatch_cqe(
Op op,
int fd,
int res,
u32 flg,
u32 gen){
switch(op){
case Op::Accept:handle_accept(res,flg);break;
case Op::Recv:handle_recv_cqe(fd,res,flg,gen);break;
case Op::Send:handle_send(fd,res,gen);break;
case Op::Close:handle_conn_close(fd,res,gen);break;
case Op::SsePoll:handle_sse_poll(fd,res,gen);break;
case Op::DeferredPoll:handle_deferred_poll(fd,res,gen);break;
case Op::Shutdown:handle_shutdown();break;
case Op::Timer:handle_timer();break;
case Op::FileIo:
if(file_completions)
file_completions->dispatch(static_cast<u32>(fd),gen,res,flg);
break;
case Op::WsCancel:handle_ws_cancel(fd);break;
case Op::FixedFdInstall:handle_fixed_fd_install(fd,res);break;
case Op::DirectSlotClose:handle_direct_slot_close(fd,res);break;
case Op::Nop:break;
}
}
// Phase 1: copy recv data out of provided buffers, return immediately.
template<typename F>
struct ScopeExit{
F fn;
~ScopeExit()noexcept{fn();}
};
template<typename F>
ScopeExit(F)->ScopeExit<F>;
template<typename Buf>
bool append_recv_buf_to(
Buf&dst,
RecvComp&rc){
if(buf_ring_->mode()==BufferRingMode::incremental){
// slice dtor calls recycle_if_final()
auto result=try_buffer_slice_from_incremental_cqe(*buf_ring_,rc.res,rc.flags);
rc.flags=0;
if(!result)[[unlikely]]
return false;
dst.append(reinterpret_cast<char const*>(result->bytes().data()),result->bytes().size());
return true;
}
auto slices=buffer_slices_from_cqe(*buf_ring_,rc.res,rc.flags,use_recv_bundle);
ScopeExit const recycle{[&]()noexcept{
slices.recycle_all();
rc.flags=0;
}};
for(auto const&s:slices)
dst.append(reinterpret_cast<char const*>(s.bytes.data()),s.bytes.size());
return true;
}
void phase1_copy_recv_bufs(){
for(auto&rc:recvs){
if(!cqe_has_buffer(rc.flags))
continue;
auto const ufd=static_cast<SZ>(rc.fd);
auto ws_it=ws_cancel_handoffs.find(rc.fd);
bool const ws_pending=ws_it!=ws_cancel_handoffs.end()
#if CONFLUX_HAS_TLS
&&ws_it->second.ssl==nullptr
#endif
;
if(rc.res<=0||ufd>=fd_table.size()||(!ws_pending&&(fd_table[ufd].gen!=rc.gen||fd_table[ufd].fd<0))){
u32 const orig_flags=rc.flags;
discard_recv_bufs(rc);
if(rc.res<=0&&!cqe_has_buffer(orig_flags))
reclaim_retired_incremental_recv(rc.fd,rc.gen);
else if(rc.res>0&&cqe_has_buffer(orig_flags))
clear_retired_incremental_if_final(rc.fd,rc.gen,orig_flags);
continue;
}
if(ws_pending&&(fd_table[ufd].gen!=rc.gen||fd_table[ufd].fd<0)){
u32 const orig_flags=rc.flags;
if(!append_recv_buf_to(ws_it->second.initial_buf,rc))
continue;
clear_retired_incremental_if_final(rc.fd,rc.gen,orig_flags);
continue;
}
auto&conn=fd_table[ufd];
u32 const orig_flags=rc.flags;
if(!append_recv_buf_to(conn.partial,rc))[[unlikely]]{
queue_close(static_cast<int>(ufd));
continue;
}
if(buf_ring_->mode()==BufferRingMode::incremental&&cqe_has_buffer(orig_flags)){
if(cqe_has_buf_more(orig_flags)){
conn.incremental_buf_id=cqe_buffer_id(orig_flags);
conn.have_incremental_buf_id=true;
if(!cqe_has_more(orig_flags))[[unlikely]]{
eprintln(format("incremental ring fault: fd={} !MORE+BUF_MORE; closing",static_cast<int>(ufd)));
queue_close(static_cast<int>(ufd));
continue;
}
}else{
conn.have_incremental_buf_id=false;
}
}
conn.last_activity=chrono::steady_clock::now();
if(!conn.is_tls&&!conn.partial.empty()&&!conn.request_in_progress){
conn.request_started=conn.last_activity;
conn.request_in_progress=true;
}
if(conn.send_queued)
continue;
auto bounded_add=[](SZ a,SZ b)noexcept{
if(a>NL<SZ>::max()-b)
return NL<SZ>::max();
return a+b;
};
auto bounded_mul=[](SZ a,SZ b)noexcept{
if(a!=0&&b>NL<SZ>::max()/a)
return NL<SZ>::max();
return a*b;
};
SZ raw_receive_cap=max_body_size;
raw_receive_cap=bounded_add(raw_receive_cap,parser_limits.max_header_block_size);
raw_receive_cap=bounded_add(raw_receive_cap,parser_limits.max_request_line_size);
raw_receive_cap=
bounded_add(raw_receive_cap,bounded_mul(parser_limits.max_chunks,kMaxChunkSizeLineBytes+4));
raw_receive_cap=bounded_add(raw_receive_cap,kMaxChunkTrailerBytes);
raw_receive_cap=bounded_add(raw_receive_cap,6);
if(conn.partial.size()>raw_receive_cap){
conn.own_response.clear();
conn.own_response.append(
"HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
conn.has_response=true;
conn.close_after_send=true;
conn.send_queued=true;
queue_send(static_cast<int>(ufd));
continue;
}
#if CONFLUX_HAS_TLS
// Protocol sniff: ssl==nullptr && tls_hs_done==true is the "undecided" sentinel
// (set in handle_accept when ssl_ctx!=nullptr). Decide on the very first byte.
if(!conn.ssl&&conn.tls_hs_done&&!conn.partial.empty()){
if(static_cast<unsigned char>(conn.partial.front())==0x16U){
// TLS ClientHello record type — create SSL and start handshake.
conn.ssl.reset(SSL_new(ssl_ctx));
if(conn.ssl){
BIO*rbio=BIO_new_mem_buf("",0);
if(rbio!=nullptr)
BIO_set_mem_eof_return(rbio,-1);
BIO*wbio=BIO_new(BIO_s_mem());
if(rbio==nullptr||wbio==nullptr){
if(rbio!=nullptr)
BIO_free(rbio);
if(wbio!=nullptr)
BIO_free(wbio);
conn.ssl.reset();
queue_close(conn.fd);
continue;
}
SSL_set_bio(conn.ssl.get(),rbio,wbio);
SSL_set_accept_state(conn.ssl.get());
}else{
queue_close(conn.fd);
continue;
}
conn.is_tls=true;
conn.tls_hs_done=false;// handshake not yet complete
}else{
// Plain HTTP on TLS-capable port — disable handshake sentinel.
conn.tls_hs_done=false;
}
}
#endif// CONFLUX_HAS_TLS
}
}
void finish_ready_ws_handoffs(){
for(int const fd:ws_cancel_ready){
auto it=ws_cancel_handoffs.find(fd);
if(it==ws_cancel_handoffs.end())
continue;
auto entry=move(it->second);
ws_cancel_handoffs.erase(it);
#if CONFLUX_HAS_TLS
if(entry.ssl!=nullptr){
finish_tls_ws_handoff(fd,move(entry));
continue;
}
#endif
finish_plain_ws_handoff(fd,move(entry));
}
ws_cancel_ready.clear();
}
#if CONFLUX_HAS_TLS
// Per-connection TLS recv handler: feeds ciphertext into OpenSSL, drives the
// handshake, and decrypts application data back into conn.partial.
void phase1b_tls_one(
Conn&conn,
RecvComp&rc){
// Move fresh ciphertext from partial into tls_recv_buf (zero-copy via move
// when tls_recv_buf is empty; append otherwise). rbio is a BIO_new_mem_buf
// pointing at tls_recv_buf.data() — avoids BIO_s_mem internal buffer and
// the BIO_write memcpy.
if(!conn.partial.empty()){
if(conn.tls_recv_buf.empty()){
conn.tls_recv_buf=conn.partial.take();
}else{
conn.tls_recv_buf.append(conn.partial.data(),conn.partial.size());
conn.partial.clear();
}
}
auto const recv_len_before=conn.tls_recv_buf.size();
if(recv_len_before>0){
BIO*const fresh=BIO_new_mem_buf(conn.tls_recv_buf.data(),static_cast<int>(recv_len_before));
if(fresh!=nullptr){
BIO_set_mem_eof_return(fresh,-1);
SSL_set0_rbio(conn.ssl.get(),fresh);
}
}

auto const compact_and_refresh_rbio=[&]{
if(recv_len_before==0)
return;
BIO*const rbio=SSL_get_rbio(conn.ssl.get());
long const remaining=rbio!=nullptr?BIO_pending(rbio):0;
if(remaining>=0&&static_cast<SZ>(remaining)<=recv_len_before){
SZ const consumed=recv_len_before-static_cast<SZ>(remaining);
if(consumed==conn.tls_recv_buf.size())
conn.tls_recv_buf.clear();
else if(consumed>0)
conn.tls_recv_buf.erase(0,consumed);
}
BIO*const refreshed=
conn.tls_recv_buf.empty()?
BIO_new_mem_buf("",0):
BIO_new_mem_buf(conn.tls_recv_buf.data(),static_cast<int>(conn.tls_recv_buf.size()));
if(refreshed!=nullptr){
BIO_set_mem_eof_return(refreshed,-1);
SSL_set0_rbio(conn.ssl.get(),refreshed);
}
};

// Drive the handshake until it completes or needs more data.
if(!conn.tls_hs_done){
int const r=SSL_do_handshake(conn.ssl.get());
tls_flush_wbio(conn);
if(r==1){
conn.tls_hs_done=true;
conn.ktls_send=(BIO_get_ktls_send(SSL_get_wbio(conn.ssl.get()))!=0);
#if CONFLUX_HAS_HTTP2
conn.is_h2=http2_negotiated(conn.ssl.get());
#endif
}else{
int const err=SSL_get_error(conn.ssl.get(),r);
if(err==SSL_ERROR_WANT_READ||err==SSL_ERROR_WANT_WRITE){
if(!conn.tls_send_buf.empty()&&!conn.send_queued){
conn.send_queued=true;
tls_queue_send(conn);
}
compact_and_refresh_rbio();
return;// wait for more handshake data from client
}
if(!conn.send_queued)
queue_close(conn.fd);
rc.res=-1;
compact_and_refresh_rbio();
return;
}
}

// Handshake done — decrypt application data into partial.
A<char,BUF_SIZE>plain{};
int n{};
while((n=SSL_read(conn.ssl.get(),plain.data(),static_cast<int>(plain.size())))>0)
conn.partial.append(plain.data(),static_cast<SZ>(n));
int const ssl_err=SSL_get_error(conn.ssl.get(),n);
if(ssl_err==SSL_ERROR_ZERO_RETURN||(ssl_err!=SSL_ERROR_WANT_READ&&ssl_err!=SSL_ERROR_NONE)){
if(!conn.send_queued)
queue_close(conn.fd);
rc.res=-1;
}

compact_and_refresh_rbio();

#if CONFLUX_HAS_HTTP2
if(!conn.is_h2&&!conn.partial.empty()&&!conn.request_in_progress){
#else
if(!conn.partial.empty()&&!conn.request_in_progress){
#endif
conn.request_started=chrono::steady_clock::now();
conn.request_in_progress=true;
}

tls_flush_wbio(conn);
if(!conn.tls_send_buf.empty()&&!conn.send_queued){
conn.send_queued=true;
tls_queue_send(conn);
}
}
#endif// CONFLUX_HAS_TLS (phase1b_tls_one)
// Phase 1b: run TLS recv processing.
// Plain connections: no-op when TLS not compiled in.
void phase1b_process(){
#if CONFLUX_HAS_TLS
for(auto&rc:recvs){
if(rc.res<=0)
continue;
auto const ufd=static_cast<SZ>(rc.fd);
if(ufd>=fd_table.size()||fd_table[ufd].gen!=rc.gen)
continue;
auto&conn=fd_table[ufd];
if(conn.ssl!=nullptr)
phase1b_tls_one(conn,rc);
}
#endif
}
// Phase 2: build responses (serial — parallelism comes from multiple rings).
void phase2_build_responses(){
for(auto&rc:recvs){
if(rc.res<=0)
continue;
auto const ufd=static_cast<SZ>(rc.fd);
if(ufd>=fd_table.size()||fd_table[ufd].gen!=rc.gen||fd_table[ufd].fd<0)
continue;
auto&conn=fd_table[ufd];
#if CONFLUX_HAS_HTTP2
if(conn.is_h2){
if(conn.h2_session==nullptr)
h2_setup_conn(conn);
if(!conn.partial.empty()){
auto n=nghttp2_session_mem_recv(
conn.h2_session,
reinterpret_cast<u8 const*>(conn.partial.data()),
conn.partial.size());
conn.partial.clear();
if(n<0){
queue_close(conn.fd);
continue;
}
}
h2_do_send(conn);
// Arm SSE eventfd poll for new H2 SSE streams.
if(conn.h2_sse_pending_wait){
conn.h2_sse_pending_wait=false;
queue_sse_wait(conn.fd);
}
continue;
}
#endif
// Skip SSE/WS connections — their I/O is driven by separate loops.
if(!conn.has_response&&!conn.is_sse&&!conn.is_ws)
dispatch_request(
conn,
conn.partial.view(),
*this,
max_body_size,
http_redirect_to_https,
https_redirect_hosts,
parser_limits);
}
}
// Phase 3: return unconsumed buffers + dispatch send/close.
void phase3_dispatch(){
for(auto&rc:recvs){
if(cqe_has_buffer(rc.flags))
discard_recv_bufs(rc);

auto const ufd=static_cast<SZ>(rc.fd);
if(ufd>=fd_table.size()||fd_table[ufd].gen!=rc.gen)
continue;
auto&conn=fd_table[ufd];
if(rc.res<=0){
if(!conn.send_queued)
queue_close(rc.fd);
continue;
}
if(conn.fd<0){
queue_close(rc.fd);
}else if((conn.has_response||conn.mapped_file)&&!conn.send_queued){
conn.send_queued=true;
queue_send(rc.fd);
}else if(conn.is_deferred&&!conn.send_queued){
queue_deferred_wait(rc.fd);
}else if(
!conn.is_sse&&!conn.is_ws&&!conn.is_deferred&&!conn.has_response&&!conn.mapped_file&&!conn.recv_armed){
// Normal connection: re-arm recv.
// SSE/WS connections: their own I/O loops drive further work.
queue_multishot_recv(rc.fd);
}
}
}
[[nodiscard]]bool ring_integrity_suspect()const noexcept{return raw_.ring().cq_has_overflow();}
void enter_ring_fatal(
ServerFatalReason reason)noexcept{
ring_fatal_=true;
shutting_down=true;
pending_ops.clear();
fatal_reason_=reason;
fatal_cq_overflow_count_=raw_.ring().cq_overflow_count();
}
void close_tracked_fds_sync()noexcept{
for(auto&conn:fd_table){
if(conn.fd>=0){
::close(conn.fd);
conn.fd=-1;
}
}
}
void recycle_recv_buffer_direct(
io_uring_cqe const*cqe)noexcept{
if((cqe->flags&IORING_CQE_F_BUFFER)==0u)
return;
if(cqe->res<=0)
return;
auto const buf_id=static_cast<u16>(cqe->flags>>IORING_CQE_BUFFER_SHIFT);
if(buf_ring_->mode()==BufferRingMode::incremental){
// slice dtor recycles if final; silent drop on malformed CQE during shutdown
auto _=try_buffer_slice_from_incremental_cqe(*buf_ring_,cqe->res,cqe->flags);
}else if(use_recv_bundle){
SZ remaining=static_cast<SZ>(cqe->res);
u16 cur=buf_id;
u32 const bcount=buf_ring_->count();
while(remaining>0){
SZ const chunk=min(remaining,buf_ring_->buf_size());
buf_ring_->recycle(cur);
remaining-=chunk;
cur=static_cast<u16>((cur+1U)%bcount);
}
}else{
buf_ring_->recycle(buf_id);
}
}
void dispatch_cqe_fatal(
io_uring_cqe const*cqe)noexcept{
auto const[op,_,_]=unpack(cqe->user_data);
switch(op){
case Op::Recv:recycle_recv_buffer_direct(cqe);break;
case Op::Accept:
if(!accepted_sockets_direct&&cqe->res>=0)
::close(cqe->res);
break;
case Op::Close:
case Op::Send:
case Op::Timer:
case Op::FileIo:
case Op::SsePoll:
case Op::DeferredPoll:
case Op::Shutdown:
case Op::WsCancel:
case Op::Nop:break;
case Op::FixedFdInstall:
if(cqe->res>=0)
::close(cqe->res);
break;
default:
// unknown op — future Op additions must be handled here
eprintln(format("dispatch_cqe_fatal: unknown op={} ud={:#x}",static_cast<u8>(op),cqe->user_data));
break;
}
}
void emit_ring_diagnostics()noexcept{
auto const features_str=caps_to_log_string(caps);
eprintln(format("ring_features={}",features_str.empty()?"none":features_str));
u32 const overflow_now=raw_.ring().cq_overflow_count();
eprintln(format("ring_cq_overflow={}",overflow_now));
if(fatal_cq_overflow_count_>0)
eprintln(format(
"ring_cq_overflow_delta={}",
overflow_now>fatal_cq_overflow_count_?overflow_now-fatal_cq_overflow_count_:0u));
eprintln(format("ring_sq_busy={}",io_uring_sq_ready(&ring)));
{
u32 const v=ring.sq.kdropped!=nullptr?*ring.sq.kdropped:0u;
eprintln(format("ring_sq_dropped={}",v));
}
// Parse fdinfo for CqOverflowList (overflow list depth, Linux 6.x+)
int const rfd=ring.ring_fd;
if(rfd>=0){
auto const path=format("/proc/self/fdinfo/{}",rfd);
if(auto f=std::ifstream{path};f){
S line;
while(getline(f,line)){
if(line.starts_with("CqOverflowList:")){
auto pos=line.find(':');
if(pos!=S::npos)
eprintln(format("ring_cq_overflow_list={}",line.substr(pos+1)));
}
}
}
}
if(fatal_reason_!=ServerFatalReason::none){
SV reason_str;
switch(fatal_reason_){
case ServerFatalReason::cq_overflow:reason_str="cq_overflow";break;
case ServerFatalReason::cq_overflow_no_nodrop:reason_str="cq_overflow_no_nodrop";break;
case ServerFatalReason::submit_wait_ebadr:reason_str="submit_wait_ebadr";break;
case ServerFatalReason::internal_exception:reason_str="internal_exception";break;
default:reason_str="unknown";break;
}
eprintln(format("ring_fatal_reason={}",reason_str));
}
if(overflow_flush_limit_hit_)
eprintln("ring_overflow_flush_limit_hit=1");
}
void flush_overflow_cqes_until_clear_or_limit()noexcept{
static constexpr unsigned max_iters=16;
static constexpr unsigned BATCH=256;
for(unsigned i=0;i<max_iters&&ring_integrity_suspect();++i){
io_uring_get_events(&ring);
A<io_uring_cqe*,BATCH>cqes{};
unsigned const n=io_uring_peek_batch_cqe(&ring,cqes.data(),BATCH);
if(n==0)
break;
for(unsigned j=0;j<n;++j)
dispatch_cqe_fatal(cqes[j]);// NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
io_uring_cq_advance(&ring,n);
}
if(ring_integrity_suspect())
overflow_flush_limit_hit_=true;
emit_ring_diagnostics();
close_tracked_fds_sync();
}
RunStatus run_loop(){
static constexpr unsigned BATCH=256;

CurrentFileReaderScope const file_reader_scope{files.get()};

queue_multishot_accept();
arm_shutdown_read();
arm_timer();
auto _=raw_.submit();

for(;;){
if(ring_integrity_suspect()){
if(!caps.feat_nodrop){
enter_ring_fatal(ServerFatalReason::cq_overflow_no_nodrop);
emit_ring_diagnostics();
close_tracked_fds_sync();
return RunStatus::fatal_cq_overflow_no_nodrop;
}
// NODROP: overflow list non-empty but CQEs are not lost.
// io_uring_submit_and_wait drains overflow into the ring; continue.
}

drain_pending_ops();

int const rc=io_uring_submit_and_wait(&ring,1);
if(rc<0){
if(rc==-EINTR)
continue;
if(rc==-EBADR){
enter_ring_fatal(ServerFatalReason::submit_wait_ebadr);
flush_overflow_cqes_until_clear_or_limit();
return RunStatus::fatal_submit_wait_ebadr;
}
if(ring_integrity_suspect()&&!caps.feat_nodrop){
enter_ring_fatal(ServerFatalReason::cq_overflow_no_nodrop);
emit_ring_diagnostics();
close_tracked_fds_sync();
return RunStatus::fatal_cq_overflow_no_nodrop;
}
continue;
}

A<io_uring_cqe*,BATCH>cqes{};
unsigned const count=io_uring_peek_batch_cqe(&ring,cqes.data(),BATCH);

bool const overflowed=ring_integrity_suspect();

if(count==0){
if(overflowed&&!caps.feat_nodrop){
enter_ring_fatal(ServerFatalReason::cq_overflow_no_nodrop);
emit_ring_diagnostics();
close_tracked_fds_sync();
return RunStatus::fatal_cq_overflow_no_nodrop;
}
continue;
}

recvs.clear();

for(unsigned i=0;i<count;++i){
// NOLINT(cppcoreguidelines-pro-bounds-constant-A-index): runtime batch index
auto[op,cqe_gen,fd]=
unpack(io_uring_cqe_get_data64(cqes[i]));// NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
auto*cqe=cqes[i];// NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
dispatch_cqe(op,fd,cqe->res,cqe->flags,cqe_gen);
}
io_uring_cq_advance(&ring,count);

phase1_copy_recv_bufs();
finish_ready_ws_handoffs();
phase1b_process();
phase2_build_responses();
phase3_dispatch();

if(shutting_down){
bool const any_open=ranges::any_of(fd_table,[](Conn const&c){return c.fd>=0;});
if(!any_open){
if(file_completions&&!file_completions->cancel_all())
continue;
return RunStatus::stopped_normally;
}
}
}
}
};
static conflux::work::root::Task<void>do_streamed_splice(
Ring*ring,
int fd,
u32 conn_gen,
conflux::work::root::Task<SZ>splice_task){
try{
auto const delivered=co_await move(splice_task);
ring->on_streamed_splice_done(fd,conn_gen,delivered,{});
}catch(...){ring->on_streamed_splice_done(fd,conn_gen,SZ{0},current_exception());}
}
static conflux::work::root::Task<void>do_streamed_tls_chunk(
Ring*ring,
int fd,
u32 conn_gen,
SZ want,
conflux::work::root::Task<FileReader::ReadFixedResult>read_task){
try{
auto result=co_await move(read_task);
ring->on_streamed_tls_chunk_done(fd,conn_gen,move(result.buffer),min(result.bytes,want),{});
}catch(...){ring->on_streamed_tls_chunk_done(fd,conn_gen,FixedBuffer{},0,current_exception());}
}
[[gnu::pure]]u32 build_uring_flags(
Config const&c){
u32 f=0;
if(c.single_issuer)
f|=IORING_SETUP_SINGLE_ISSUER;
if(c.defer_taskrun)
f|=IORING_SETUP_DEFER_TASKRUN;
if(c.sqpoll)
f|=IORING_SETUP_SQPOLL;
if(c.coop_taskrun)
f|=IORING_SETUP_COOP_TASKRUN;
if(c.taskrun_flag)
f|=IORING_SETUP_TASKRUN_FLAG;
if(c.submit_all)
f|=IORING_SETUP_SUBMIT_ALL;
if(c.no_sqarray)
f|=IORING_SETUP_NO_SQARRAY;
if(c.cqe_mixed)
f|=IORING_SETUP_CQE_MIXED;
return f;
}
// Returns the wq_fd for ring[i] given the parent ring fd. Zero = no attachment.
u32 wq_fd_for_ring(
Config const&c,
unsigned i,
int parent_ring_fd){
if(!c.attach_wq||i==0||parent_ring_fd<0)
return 0;
return static_cast<u32>(parent_ring_fd);
}
S flags_str(
Config const&c){
S s;
auto app=[&](char const*name){
if(!s.empty())
s+='|';
s+=name;
};
if(c.single_issuer)
app("SINGLE_ISSUER");
if(c.defer_taskrun)
app("DEFER_TASKRUN");
if(c.sqpoll)
app("SQPOLL");
if(c.coop_taskrun)
app("COOP_TASKRUN");
if(c.taskrun_flag)
app("TASKRUN_FLAG");
if(c.submit_all)
app("SUBMIT_ALL");
if(c.attach_wq)
app("ATTACH_WQ");
if(c.no_sqarray)
app("NO_SQARRAY");
if(c.cqe_mixed)
app("CQE_MIXED");
if(c.no_mmap)
app("NO_MMAP");
if(c.recv_bundle)
app("RECV_BUNDLE");
if(c.auto_recv_arm_policy)
app("AUTO_RECV_ARM");
return s.empty()?"none":s;
}
namespace{
enum class ParseError:u8{
None,
BadRequest,
UriTooLong,
HeaderFieldsTooLarge

};
void emit_parse_error(
Conn&conn,
SV raw,
ParseError err,
SV alt_svc){
HttpResponse r;
switch(err){
case ParseError::UriTooLong:r=HttpResponse::uri_too_long();break;
case ParseError::HeaderFieldsTooLarge:r=HttpResponse::header_fields_too_large();break;
case ParseError::BadRequest:
default:r=HttpResponse::bad_request();break;
}
conn.own_response=format_response(r,alt_svc,true);
conn.has_response=true;
conn.close_after_send=true;
conn.request_bytes=raw.size();
}
}// namespace
void dispatch_request(
Conn&conn,
SV raw,
Ring const&ring,
SZ max_body_size,
bool http_redirect_to_https,
V<S>const&https_redirect_hosts,
ParserLimits const&limits){
conn.has_response=false;
conn.written=0;
conn.mapped_file.reset();
conn.mapped_total=0;
conn.mapped_delivered=0;
conn.is_sse=false;
conn.sse_headers_sent=false;

conflux::http1::ParsedRequest parsed;
switch(conflux::http1::parse_request(raw,limits,parsed)){
case conflux::http1::ParseStatus::Incomplete:return;
case conflux::http1::ParseStatus::UriTooLong:
emit_parse_error(conn,raw,ParseError::UriTooLong,ring.alt_svc_header);
return;
case conflux::http1::ParseStatus::HeaderFieldsTooLarge:
emit_parse_error(conn,raw,ParseError::HeaderFieldsTooLarge,ring.alt_svc_header);
return;
case conflux::http1::ParseStatus::BadRequest:
emit_parse_error(conn,raw,ParseError::BadRequest,ring.alt_svc_header);
return;
case conflux::http1::ParseStatus::Ok:break;
}
auto const header_end=parsed.header_end_offset;

SV const method=parsed.method;
SV path=parsed.target;
SV redirect_query;
SV const version=parsed.version;
HttpFieldsView const params;
HttpFieldsView headers{true};
HttpFieldsView query;
HttpFieldsView form;
HttpFieldsView cookies;
V<UploadedFile>files;
SV body;

if(auto q=path.find('?');q!=SV::npos){
redirect_query=path.substr(q);
parse_urlencoded(path.substr(q+1),query);
path=path.substr(0,q);
}

for(auto const&[name,field_value]:parsed.headers)
headers.emplace_back(name,field_value);

if(path.starts_with("https://")){
auto slash=path.find('/',8);
path=(slash!=SV::npos)?path.substr(slash):SV{"/"};
}else if(path.starts_with("http://")){
auto slash=path.find('/',7);
path=(slash!=SV::npos)?path.substr(slash):SV{"/"};
}
S redirect_target{path.empty()?SV{"/"}:path};
redirect_target+=redirect_query;

if(version=="HTTP/1.1"){
auto const hosts=headers.values("host");
if(hosts.empty()||hosts.size()>1){
emit_parse_error(conn,raw,ParseError::BadRequest,ring.alt_svc_header);
return;
}
}

if(http_redirect_to_https&&!conn.is_tls){
auto host=headers["host"];
auto strip_host_port=[](SV h)->SV{
if(h.starts_with('[')){
auto b=h.find(']');
if(b!=SV::npos){
auto c=h.find(':',b+1);
// strip port if present, then strip surrounding brackets
SV inner=c!=SV::npos?h.substr(0,c):h;
if(inner.starts_with('[')&&inner.ends_with(']')){
inner.remove_prefix(1);
inner.remove_suffix(1);
}
return inner;
}
return h;
}
auto c=h.rfind(':');
return c!=SV::npos?h.substr(0,c):h;
};
auto const host_bare=ascii_lower(S{strip_host_port(host)});
SV canonical_host;
for(auto const&h:https_redirect_hosts){
if(ascii_lower(h)==host_bare){
canonical_host=h;
break;
}
}
if(host.empty()||canonical_host.empty()){
auto r=HttpResponse{};
r.status=kHttpBadRequest;
r.status_text="Bad Request";
r.content_type="text/plain; charset=utf-8";
r.set_text_body("Bad Request");
conn.own_response=format_response(r,ring.alt_svc_header,true);
conn.has_response=true;
conn.close_after_send=true;
conn.request_bytes=raw.size();
return;
}
conn.own_response=format_response(
HttpResponse::redirect(format("https://{}{}",canonical_host,redirect_target),308),
ring.alt_svc_header,
true);
conn.has_response=true;
conn.close_after_send=true;
conn.request_bytes=raw.size();
return;
}

auto body_start=header_end+4;
SZ body_stream_bytes=0;

auto const content_lengths=headers.values("content-length");
auto const transfer_encodings=headers.values("transfer-encoding");
if(!content_lengths.empty()&&!transfer_encodings.empty()){
emit_parse_error(conn,raw,ParseError::BadRequest,ring.alt_svc_header);
return;
}
if(content_lengths.size()>1){
emit_parse_error(conn,raw,ParseError::BadRequest,ring.alt_svc_header);
return;
}
if(!transfer_encodings.empty()&&!has_valid_chunked_transfer_encoding(headers)){
emit_parse_error(conn,raw,ParseError::BadRequest,ring.alt_svc_header);
return;
}

auto const expect_state=parse_expect_header(headers);
if(expect_state==ExpectState::unsupported){
HttpResponse r;
r.status=417;
r.status_text="Expectation Failed";
r.content_type="text/html; charset=utf-8";
r.set_text_body("<html><body><h1>417 Expectation Failed</h1></body></html>");
conn.own_response=format_response(r,ring.alt_svc_header,true);
conn.has_response=true;
conn.close_after_send=true;
conn.request_bytes=raw.size();
return;
}
auto const queue_continue=[&]{
conn.own_response="HTTP/1.1 100 Continue\r\n\r\n";
conn.has_response=true;
conn.written=0;
conn.request_bytes=0;
conn.expect_continue_sent=true;
};

if(!content_lengths.empty()){
auto cl=content_lengths.front();
SZ content_length{};
auto const*cl_end=ranges::next(cl.data(),ssize(cl));
auto[ptr,ec]=from_chars(cl.data(),cl_end,content_length);
if(ec!=errc{}||ptr!=cl_end){
conn.own_response=format_response(HttpResponse::bad_request(),ring.alt_svc_header,true);
conn.has_response=true;
conn.close_after_send=true;
conn.request_bytes=raw.size();
return;
}
if(content_length>max_body_size){
conn.own_response=format_response(HttpResponse::content_too_large(),ring.alt_svc_header,true);
conn.has_response=true;
conn.close_after_send=true;
conn.request_bytes=raw.size();
return;
}
if(raw.size()-body_start<content_length){
if(expect_state==ExpectState::continue_100&&!conn.expect_continue_sent)
queue_continue();
return;
}
body=raw.substr(body_start,content_length);
body_stream_bytes=content_length;
}else if(!transfer_encodings.empty()){
auto rc=decode_chunked_incremental(raw,body_start,max_body_size,limits.max_chunks,conn.chunked_decode);
if(rc==0){
if(expect_state==ExpectState::continue_100&&!conn.expect_continue_sent)
queue_continue();
return;
}
if(rc==-1){
conn.own_response=format_response(HttpResponse::bad_request(),ring.alt_svc_header,true);
conn.has_response=true;
conn.close_after_send=true;
conn.request_bytes=raw.size();
return;
}
if(rc==-2){
conn.own_response=format_response(HttpResponse::content_too_large(),ring.alt_svc_header,true);
conn.has_response=true;
conn.close_after_send=true;
conn.request_bytes=raw.size();
return;
}
body=conn.chunked_decode.body;
body_stream_bytes=static_cast<SZ>(rc);
}

conn.expect_continue_sent=false;

if(headers["content-type"].starts_with("application/x-www-form-urlencoded"))
parse_urlencoded(body,form);

auto ct_header=headers["content-type"];
if(ct_header.starts_with("multipart/form-data")){
auto boundary=extract_param(ct_header,"boundary");
if(!boundary.empty())
parse_multipart(body,boundary,form,files);
}

if(auto cookie=headers["cookie"];!cookie.empty())
parse_cookies(cookie,cookies);

{
bool keep_alive=(version=="HTTP/1.1");
if(has_connection_token(headers,"close"))
keep_alive=false;
else if(has_connection_token(headers,"keep-alive"))
keep_alive=true;
conn.close_after_send=!keep_alive;
}

conn.request_bytes=header_end+4+body_stream_bytes;

HttpRequestView const
req{method,path,version,conn.remote_addr,conn.is_tls,params,headers,query,form,cookies,files,body};
auto const handler_started=chrono::steady_clock::now();
HttpResponse resp;
try{
resp=ring.dispatch(req);
}catch(exception const&e){resp=HttpResponse::internal_error(e.what());}catch(...){
resp=HttpResponse::internal_error();
}
if(ring.slow_handler_diagnostics){
auto const elapsed_ms=
chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now()-handler_started).count();
if(elapsed_ms>=static_cast<i64>(ring.slow_handler_warn_ms))
eprintln(format(
"warning: slow handler on ring thread (method={}, path={}, elapsed_ms={})",
method,
path,
elapsed_ms));
}
if(resp.is_deferred()){
#if CONFLUX_HAS_HTTP2
if(conn.is_h2){
conn.own_response=format_response(
HttpResponse::internal_error("deferred responses unsupported over HTTP/2"),
ring.alt_svc_header,
true);
conn.has_response=true;
return;
}
#endif
conn.is_deferred=true;
conn.deferred_head_only=resp.head_only;
conn.deferred_efd=resp.deferred_response_ptr()->eventfd_fd();
conn.deferred_response=resp.take_deferred_response();
conn.has_response=false;
}else if(resp.is_ws_upgrade()){
conn.is_ws=true;
conn.ws_upgrade=resp.ws_upgrade_ptr();
conn.ws_work_pool=ring.resolve_ws_work_pool(req);
conn.saved_req=req.to_owned();
conn.close_after_send=false;
conn.own_response=format_response(resp);
conn.has_response=true;
}else if(resp.is_sse()){
conn.is_sse=true;
conn.sse_efd=resp.sse_channel_ptr()->eventfd_fd();
conn.sse_channel=resp.take_sse_channel();
conn.own_response=S{format_sse_headers(conn.close_after_send)};
conn.has_response=true;
}else if(resp.is_mapped_file()){
conn.own_response=format_response(resp,ring.alt_svc_header,conn.close_after_send);
if(resp.head_only){
conn.has_response=true;
}else{
conn.mapped_file=resp.take_mapped_file();
conn.mapped_total=conn.own_response.size()+conn.mapped_file->send_size;
conn.mapped_delivered=0;
conn.has_response=false;
}
}else if(resp.is_streamed_file()){
conn.own_response=format_response(resp,ring.alt_svc_header,conn.close_after_send);
if(resp.head_only){
conn.has_response=true;
}else{
conn.streamed_file=resp.take_streamed_file();
conn.streamed_headers_sent=false;
conn.streamed_delivered=0;
conn.streamed_splice_in_flight=false;
conn.has_response=true;
}
}else{
conn.own_response=format_response(resp,ring.alt_svc_header,conn.close_after_send);
conn.has_response=true;
}
}
export class HttpServer{
struct Impl{
Config cfg{};
unsigned rings{};
u32 uring_flags{};
Router router;
VHostRouter vhost_router;
bool use_vhost=false;
V<UP<Ring>>ring_vec;
V<int>shutdown_efds;
Atom<u16>bound_port_;
mutex startup_error_mu;
EP startup_error{};
std::atomic_bool startup_failed{false};
Atom<u8>run_status_{static_cast<u8>(RunStatus::stopped_normally)};
// Signalled by ring[0] after init when attach_wq=true. Ring[1..N] wait
// here for the wq_fd before calling io_uring_queue_init_params. -2 = unset.
Atom<int>wq_ring_fd_{-2};
#if CONFLUX_HAS_TLS
Opt<TlsServerContext>tls_ctx;// owned; shared (read-only) across rings
#endif
#if CONFLUX_HAS_HTTP3
mutex http3_mu;
UP<Http3Listener>http3_listener;
#endif
};
UP<Impl>impl_;
void initialize(
Config const&cfg){
impl_->cfg=cfg;
impl_->rings=cfg.rings==0?thread::hardware_concurrency():cfg.rings;
impl_->uring_flags=build_uring_flags(cfg);

#if CONFLUX_HAS_TLS
// TLS setup: create SSL_CTX if cert and key are provided.
if(!cfg.cert_file.empty()&&!cfg.key_file.empty()){
init_openssl_once();
TlsServerOptions const primary_opts{
.cert_file=cfg.cert_file,
.key_file=cfg.key_file,
.cipher_list=cfg.tls_cipher_list,
.ciphersuites=cfg.tls_ciphersuites,
.ktls=cfg.ktls,
};
impl_->tls_ctx.emplace(primary_opts);
SSL_CTX*const ctx=impl_->tls_ctx->native_handle();
#if CONFLUX_HAS_HTTP3
if(cfg.http3.enabled){
http3_configure_alpn(ctx);// prefer h3, then h2, then http/1.1
}
#if CONFLUX_HAS_HTTP2
else{
http2_configure_alpn(ctx);// prefer h2, fall back to http/1.1
}
#endif
#elif CONFLUX_HAS_HTTP2
http2_configure_alpn(ctx);// prefer h2, fall back to http/1.1
#endif

// Load per-hostname SSL_CTX for SNI virtual hosts.
for(auto const&vh:cfg.virtual_hosts)
impl_->tls_ctx->add_vhost(
vh.hostname,
TlsServerOptions{
.cert_file=vh.cert_file,
.key_file=vh.key_file,
.cipher_list=cfg.tls_cipher_list,
.ciphersuites=cfg.tls_ciphersuites,
.ktls=cfg.ktls,
});
impl_->tls_ctx->install_sni();
}
#endif// CONFLUX_HAS_TLS

impl_->ring_vec.reserve(impl_->rings);
impl_->shutdown_efds.reserve(impl_->rings);
for(unsigned i=0;i<impl_->rings;++i){
impl_->ring_vec.emplace_back(make_unique<Ring>());
int const efd=::eventfd(0,EFD_CLOEXEC);
if(efd<0)
throw SE{errno,system_category(),"eventfd (shutdown)"};
impl_->shutdown_efds.push_back(efd);
}
}
public:
explicit HttpServer(
Config const&cfg,
Router&&router)
:impl_(make_unique<Impl>()){
impl_->router=move(router);
initialize(cfg);
}
explicit HttpServer(
Config const&cfg,
VHostRouter&&vhost_router)
:impl_(make_unique<Impl>()){
impl_->use_vhost=true;
impl_->vhost_router=move(vhost_router);
initialize(cfg);
}
~HttpServer(){
if(impl_)
for(int const efd:impl_->shutdown_efds)
::close(efd);
}
// Thread-safe and async-signal-safe. Signals all rings to stop accepting,
// drain in-flight responses, and exit. run() returns once all rings stop.
void shutdown(){
u64 const v=1;
for(int const efd:impl_->shutdown_efds)
if(::write(efd,&v,sizeof(v))<0&&errno!=EAGAIN)
eprintln(format("HttpServer::shutdown: eventfd write: {}",strerror(errno)));
#if CONFLUX_HAS_HTTP3
UP<Http3Listener>to_stop;
{
SL const lk{impl_->http3_mu};
to_stop=move(impl_->http3_listener);
}
if(to_stop)
to_stop->stop();
#endif
}
[[nodiscard]]RunStatus run()noexcept{
try{
unsigned const entries=impl_->cfg.ring_entries==0?DEFAULT_RING_ENTRIES:impl_->cfg.ring_entries;

V<thread>threads;
threads.reserve(impl_->rings);

for(unsigned i=0;i<impl_->rings;++i){
threads.emplace_back([this,i,entries]{
try{
auto&r=*impl_->ring_vec[i];
r.router=impl_->use_vhost?nullptr:&impl_->router;
r.vhost_router=impl_->use_vhost?&impl_->vhost_router:nullptr;
r.shutdown_efd=impl_->shutdown_efds[i];
r.max_body_size=impl_->cfg.max_body_size;
r.request_timeout_ms=impl_->cfg.request_timeout_ms;
r.tls_sniff_timeout_ms=impl_->cfg.tls_sniff_timeout_ms;
r.slow_handler_diagnostics=impl_->cfg.slow_handler_diagnostics;
r.slow_handler_warn_ms=impl_->cfg.slow_handler_warn_ms;
r.http_redirect_to_https=impl_->cfg.http_redirect_to_https;
r.https_redirect_hosts=impl_->cfg.https_redirect_hosts;
r.parser_limits=impl_->cfg.parser_limits;
r.file_io_slabs=impl_->cfg.fixed_buffer_slabs;
r.file_io_slab_bytes=impl_->cfg.fixed_buffer_bytes;
r.file_io_pipe_pairs=impl_->cfg.splice_pipe_pairs;
#if CONFLUX_HAS_TLS
r.ssl_ctx=impl_->tls_ctx?impl_->tls_ctx->native_handle():nullptr;
// vhost_ctxs on Ring is informational only; SNI callback is already
// registered on the primary SSL_CTX in the constructor.
#endif
if(i==0)
r.port_signal=&impl_->bound_port_;
u32 wq_fd=0;
if(impl_->cfg.attach_wq&&i>0){
impl_->wq_ring_fd_.wait(-2,memory_order_acquire);
int const parent=impl_->wq_ring_fd_.load(memory_order_acquire);
if(parent>=0)
wq_fd=static_cast<u32>(parent);
}
r.use_recv_incremental_buf=impl_->cfg.recv_incremental_buf;
r.init(impl_->cfg.port,entries,impl_->uring_flags,wq_fd,impl_->cfg.no_mmap);
if(!impl_->cfg.recv_incremental_buf&&impl_->cfg.recv_bundle&&r.caps.recvsend_bundle)
r.use_recv_bundle=true;
r.auto_recv_arm_policy=impl_->cfg.auto_recv_arm_policy;
if(impl_->cfg.attach_wq&&i==0){
impl_->wq_ring_fd_.store(r.ring.ring_fd,memory_order_release);
impl_->wq_ring_fd_.notify_all();
}
#if CONFLUX_HAS_HTTP3
if(impl_->cfg.http3.enabled&&!impl_->use_vhost&&impl_->tls_ctx)
r.alt_svc_header=http3_alt_svc_value(r.bound_port,impl_->cfg.http3.alt_svc_max_age_sec);
#endif

if(i==0){
auto const feat_s=caps_to_log_string(r.caps);
S sflg_s;
auto as=[&](char const*n,u32 b){
if((r.ring.flags&b)==0u)
return;
if(!sflg_s.empty())
sflg_s+=',';
sflg_s+=n;
};
as("SINGLE_ISSUER",IORING_SETUP_SINGLE_ISSUER);
as("DEFER_TASKRUN",IORING_SETUP_DEFER_TASKRUN);
as("COOP_TASKRUN",IORING_SETUP_COOP_TASKRUN);
as("TASKRUN_FLAG",IORING_SETUP_TASKRUN_FLAG);
as("SQPOLL",IORING_SETUP_SQPOLL);
as("IOPOLL",IORING_SETUP_IOPOLL);
eprintln(format("uring_features={}",feat_s.empty()?"none":feat_s));
eprintln(format("uring_setup_flags={}",sflg_s.empty()?"none":sflg_s));
}
if(i==0&&impl_->cfg.startup_banner)
eprintln(format(
"listening on {}://0.0.0.0:{}  " "(rings={}, entries={}, flags={}, listen_fixed={}, accepted_sockets_direct={}, " "buf_ring=true)",
#if CONFLUX_HAS_TLS
impl_->tls_ctx?"http/https":"http",
#else
"http",
#endif
r.bound_port,
impl_->rings,
entries,
flags_str(impl_->cfg),
r.listen_fixed,
r.accepted_sockets_direct));

auto const status=r.run_loop();
if(status!=RunStatus::stopped_normally){
u8 expected=static_cast<u8>(RunStatus::stopped_normally);
impl_->run_status_.compare_exchange_strong(
expected,
static_cast<u8>(status),
memory_order_release,
memory_order_relaxed);
shutdown();
}
}catch(...){
{
SL const lk{impl_->startup_error_mu};
if(!impl_->startup_error)
impl_->startup_error=current_exception();
}
impl_->startup_failed.store(true,memory_order_release);
{
u8 expected=static_cast<u8>(RunStatus::stopped_normally);
impl_->run_status_.compare_exchange_strong(
expected,
static_cast<u8>(RunStatus::fatal_internal_exception),
memory_order_release,
memory_order_relaxed);
}
impl_->bound_port_.store(NL<u16>::max(),memory_order_release);
impl_->bound_port_.notify_all();
if(impl_->cfg.attach_wq&&i==0){
impl_->wq_ring_fd_.store(-1,memory_order_release);
impl_->wq_ring_fd_.notify_all();
}
shutdown();
}
});
}

#if CONFLUX_HAS_HTTP3
if(impl_->cfg.http3.enabled&&impl_->tls_ctx&&!impl_->use_vhost){
u16 const h3_port=port();
auto listener=make_unique<Http3Listener>(
impl_->use_vhost?nullptr:&impl_->router,
impl_->cfg.http3,
h3_port,
impl_->tls_ctx->native_handle());
listener->start();
{
SL const lk{impl_->http3_mu};
impl_->http3_listener=move(listener);
}
}
#endif

for(auto&t:threads)
t.join();
#if CONFLUX_HAS_HTTP3
UP<Http3Listener>to_reset;
{
SL const lk{impl_->http3_mu};
to_reset=move(impl_->http3_listener);
}
if(to_reset)
to_reset->stop();
#endif
return static_cast<RunStatus>(impl_->run_status_.load(memory_order_acquire));
}catch(...){return RunStatus::fatal_internal_exception;}
}
// Blocks until ring 0 has bound and called listen(); returns the actual port.
// Safe to call from any thread after run() has been dispatched.
[[nodiscard]]u16 port()const{
u16 p=0;
while((p=impl_->bound_port_.load(memory_order_acquire))==0){
if(impl_->startup_failed.load(memory_order_acquire)){
SL const lk{impl_->startup_error_mu};
if(impl_->startup_error)
rethrow_exception(impl_->startup_error);
throw RE{"HttpServer startup failed"};
}
impl_->bound_port_.wait(0,memory_order_acquire);
}
if(impl_->startup_failed.load(memory_order_acquire)){
SL const lk{impl_->startup_error_mu};
if(impl_->startup_error)
rethrow_exception(impl_->startup_error);
throw RE{"HttpServer startup failed"};
}
return p;
}
HttpServer(HttpServer const&)=delete;
HttpServer&operator=(HttpServer const&)=delete;
HttpServer(HttpServer&&)=delete;
HttpServer&operator=(HttpServer&&)=delete;
};
