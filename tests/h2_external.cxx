// HTTP/2 end-to-end tests.
// Starts a real HTTPS/H2 server (TLS + ALPN "h2"), then exercises it with a
// minimal synchronous nghttp2 client over a blocking TLS socket.
// No external tools required — nghttp2 lib is used directly.
//
// Plain-TU intentionally (not a module unit).  See TRICKS.md #4.
#include<arpa/inet.h>
#include<catch2/catch_test_macros.hpp>
#include<netinet/in.h>
#include<nghttp2/nghttp2.h>
#include<openssl/ssl.h>
#include<sys/socket.h>
#include<unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import conflux.net.tls;
import conflux.work;
import conflux.tests.external_support;
namespace{
// ---------------------------------------------------------------------------
// H2Response + H2Client
// ---------------------------------------------------------------------------

struct H2Response{
int status=0;
S body;
bool closed=false;
u32 error_code=0;
V<P<S,S>>trailers;
};
// Minimal synchronous nghttp2 client over a blocking TLS socket.
// Call get()/post() for serial requests; for concurrent streams use
// submit_get()/pump_all().
struct H2Client{
// Internal body state for nghttp2 data provider.
struct ReqBody{
S data;
SZ off{0};
};
// --- public API ---

H2Client(H2Client const&)=delete;
H2Client&operator=(H2Client const&)=delete;
explicit H2Client(
u16 port)
:ctx_(SSL_CTX_new(TLS_client_method())),fd_(::socket(AF_INET,SOCK_STREAM,0)){
SSL_CTX_set_verify(ctx_.get(),SSL_VERIFY_NONE,nullptr);
// Advertise "h2" in ALPN.
static constexpr unsigned char kAlpn[]="\x02h2";
SSL_CTX_set_alpn_protos(ctx_.get(),kAlpn,sizeof(kAlpn)-1);

timeval tv{.tv_sec=5,.tv_usec=0};
::setsockopt(fd_,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));

sockaddr_in addr{};
addr.sin_family=AF_INET;
addr.sin_port=htons(port);
::inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
if(::connect(fd_,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0)
throw RE{"H2Client: connect failed"};

ssl_.reset(SSL_new(ctx_.get()));
SSL_set_fd(ssl_.get(),fd_);
SSL_ctrl(
ssl_.get(),
SSL_CTRL_SET_TLSEXT_HOSTNAME,
TLSEXT_NAMETYPE_host_name,
const_cast<void*>(static_cast<void const*>("localhost")));
if(SSL_connect(ssl_.get())!=1)
throw RE{"H2Client: TLS handshake failed"};

// Verify ALPN negotiated "h2".
unsigned char const*proto=nullptr;
unsigned int proto_len=0;
SSL_get0_alpn_selected(ssl_.get(),&proto,&proto_len);
if(proto_len!=2||proto[0]!='h'||proto[1]!='2')
throw RE{"H2Client: server did not negotiate h2"};

nghttp2_session_callbacks*cbs=nullptr;
nghttp2_session_callbacks_new(&cbs);
nghttp2_session_callbacks_set_send_callback(cbs,send_cb);
nghttp2_session_callbacks_set_on_header_callback(cbs,on_header_cb);
nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs,on_data_chunk_cb);
nghttp2_session_callbacks_set_on_stream_close_callback(cbs,on_stream_close_cb);
nghttp2_session_client_new(&session_,cbs,this);
nghttp2_session_callbacks_del(cbs);

// Send client connection preface (magic bytes + empty SETTINGS).
nghttp2_submit_settings(session_,NGHTTP2_FLAG_NONE,nullptr,0);
nghttp2_session_send(session_);
}
~H2Client(){
if(session_!=nullptr)
nghttp2_session_del(session_);
if(ssl_){
SSL_shutdown(ssl_.get());
ssl_.reset();
}
ctx_.reset();
if(fd_>=0)
::close(fd_);
}
// Submit a GET and block until response received.
H2Response get(
SV path){
i32 const sid=submit_request("GET",path,nullptr);
pump_until_closed(sid);
return responses_[sid];
}
H2Response get_with_headers(
SV path,
V<P<S,S>>extra_headers){
i32 const sid=submit_request("GET",path,nullptr,move(extra_headers));
pump_until_closed(sid);
return responses_[sid];
}
// Submit a POST with body and block until response received.
// ReqBody must outlive the pump — kept in req_bodies_ for stability.
H2Response post(
SV path,
SV body_data){
return post_with_headers(path,body_data,{});
}
H2Response post_with_content_length(
SV path,
SV body_data,
SZ content_length){
return post_with_headers(path,body_data,{{"content-length",to_string(content_length)}});
}
H2Response post_with_headers(
SV path,
SV body_data,
V<P<S,S>>extra_headers){
auto rb=make_unique<ReqBody>(ReqBody{.data=S{body_data},.off=0});
ReqBody*rb_ptr=rb.get();
nghttp2_data_provider prd{};
prd.read_callback=read_cb;
prd.source.ptr=rb_ptr;

i32 const sid=submit_request("POST",path,&prd,move(extra_headers));
req_bodies_.emplace(sid,move(rb));// pointer still valid after move
pump_until_closed(sid);
return responses_[sid];
}
// Submit a GET without pumping (for concurrent-stream tests).
i32 submit_get(
SV path){
return submit_request("GET",path,nullptr);
}
// Pump until all listed streams are closed (or timeout).
void pump_all(
span<i32 const>sids){
auto deadline=chrono::steady_clock::now()+chrono::seconds{5};
auto all_done=[&]{return ranges::all_of(sids,[&](i32 s){return responses_[s].closed;});};
while(!all_done()&&chrono::steady_clock::now()<deadline)
pump_once();
}
M<i32,H2Response>responses_;
private:
UniqueSslCtx ctx_;
UniqueSsl ssl_;
int fd_=-1;
nghttp2_session*session_=nullptr;
M<i32,UP<ReqBody>>req_bodies_;
i32 submit_request(
SV method,
SV path,
nghttp2_data_provider const*prd,
V<P<S,S>>extra_headers={}){
S ms{method};
S ps{path};
V<P<S,S>>nv_store;
nv_store.emplace_back(":method",ms);
nv_store.emplace_back(":path",ps);
nv_store.emplace_back(":scheme","https");
nv_store.emplace_back(":authority","localhost");
for(auto&h:extra_headers)
nv_store.push_back(move(h));

V<nghttp2_nv>nva;
nva.reserve(nv_store.size());
for(auto&[n,v]:nv_store)
nva.push_back(
{reinterpret_cast<u8*>(n.data()),
reinterpret_cast<u8*>(v.data()),
n.size(),
v.size(),
NGHTTP2_NV_FLAG_NONE});

i32 const sid=nghttp2_submit_request(session_,nullptr,nva.data(),nva.size(),prd,nullptr);
if(sid<0)
throw RE{"nghttp2_submit_request failed"};
return sid;
}
void pump_once(){
nghttp2_session_send(session_);

A<char,16384>buf{};
int const n=SSL_read(ssl_.get(),buf.data(),static_cast<int>(buf.size()));
if(n>0)
nghttp2_session_mem_recv(session_,reinterpret_cast<u8 const*>(buf.data()),static_cast<SZ>(n));
// n <= 0: timeout or close — caller checks stream state
}
void pump_until_closed(
i32 sid){
auto deadline=chrono::steady_clock::now()+chrono::seconds{5};
while(!responses_[sid].closed&&chrono::steady_clock::now()<deadline)
pump_once();
}
// --- nghttp2 static callbacks ---

static ssize_t send_cb(
nghttp2_session*/*unused*/,
u8 const*data,
SZ length,
int /*unused*/,
void*ud){
auto*c=static_cast<H2Client*>(ud);
int const n=SSL_write(c->ssl_.get(),data,static_cast<int>(length));
return n>0?static_cast<ssize_t>(n):static_cast<ssize_t>(NGHTTP2_ERR_CALLBACK_FAILURE);
}
static int on_header_cb(
nghttp2_session*/*unused*/,
nghttp2_frame const*frame,
u8 const*name,
SZ namelen,
u8 const*value,
SZ valuelen,
u8 /*unused*/,
void*ud){
auto*c=static_cast<H2Client*>(ud);
SV const n{reinterpret_cast<char const*>(name),namelen};
SV const v{reinterpret_cast<char const*>(value),valuelen};
if(frame->headers.cat==NGHTTP2_HCAT_HEADERS){
// Trailer HEADERS frame (follows DATA frames) — capture all fields.
c->responses_[frame->hd.stream_id].trailers.emplace_back(S{n},S{v});
}else if(n==":status"){
int st=0;
from_chars(v.data(),v.data()+v.size(),st);
c->responses_[frame->hd.stream_id].status=st;
}
return 0;
}
static int on_data_chunk_cb(
nghttp2_session*/*unused*/,
u8 /*unused*/,
i32 stream_id,
u8 const*data,
SZ len,
void*ud){
auto*c=static_cast<H2Client*>(ud);
c->responses_[stream_id].body.append(reinterpret_cast<char const*>(data),len);
return 0;
}
static int on_stream_close_cb(
nghttp2_session*/*unused*/,
i32 stream_id,
u32 error_code,
void*ud){
auto*c=static_cast<H2Client*>(ud);
c->responses_[stream_id].closed=true;
c->responses_[stream_id].error_code=error_code;
return 0;
}
static ssize_t read_cb(
nghttp2_session*/*unused*/,
i32 /*unused*/,
u8*buf,
SZ length,
u32*data_flags,
nghttp2_data_source*source,
void*/*unused*/){
auto&rb=*static_cast<ReqBody*>(source->ptr);
auto remaining=rb.data.size()-rb.off;
auto to_copy=min(remaining,length);
std::memcpy(
buf,
rb.data.data()+rb.off,// NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
to_copy);
rb.off+=to_copy;
if(rb.off>=rb.data.size())
*data_flags|=NGHTTP2_DATA_FLAG_EOF;
return static_cast<ssize_t>(to_copy);
}
};
Router make_router(){
Router r=conflux::tests::make_external_test_router();
return r;
}
}// namespace
// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE(
"h2: ALPN negotiates h2 (connection setup succeeds)"){
conflux::tests::HttpsServerFixture const fx{make_router()};
// H2Client constructor throws if ALPN does not yield "h2".
REQUIRE_NOTHROW(H2Client{fx.port()});
}
TEST_CASE(
"h2: GET /ping returns 200 with JSON body"){
conflux::tests::HttpsServerFixture const fx{make_router()};
H2Client client{fx.port()};
auto resp=client.get("/ping");
REQUIRE(resp.status==200);
REQUIRE(resp.body==R"({"ok":true})");
}
TEST_CASE(
"h2: GET with path param echoes name"){
conflux::tests::HttpsServerFixture const fx{make_router()};
H2Client client{fx.port()};
auto resp=client.get("/hello/conflux");
REQUIRE(resp.status==200);
REQUIRE(resp.body=="hello conflux");
}
TEST_CASE(
"h2: POST body is echoed"){
conflux::tests::HttpsServerFixture const fx{make_router()};
H2Client client{fx.port()};
auto resp=client.post("/echo","hello h2 world");
REQUIRE(resp.status==200);
REQUIRE(resp.body=="hello h2 world");
}
TEST_CASE(
"h2: content-length over max body resets stream"){
Config cfg=Config::test();
cfg.max_body_size=8;
conflux::tests::HttpsServerFixture const fx{cfg,make_router()};
H2Client client{fx.port()};
auto resp=client.post_with_content_length("/echo","",9);
REQUIRE(resp.closed);
CHECK(resp.status==0);
CHECK(resp.error_code==NGHTTP2_CANCEL);
}
TEST_CASE(
"h2: DATA over max body resets stream"){
Config cfg=Config::test();
cfg.max_body_size=8;
conflux::tests::HttpsServerFixture const fx{cfg,make_router()};
H2Client client{fx.port()};
auto resp=client.post("/echo","012345678");
REQUIRE(resp.closed);
CHECK(resp.status==0);
CHECK(resp.error_code==NGHTTP2_CANCEL);
}
TEST_CASE(
"h2: header count over parser limit resets stream"){
Config cfg=Config::test();
cfg.parser_limits.max_headers=16;
conflux::tests::HttpsServerFixture const fx{cfg,make_router()};
H2Client client{fx.port()};
auto ok=client.get("/ping");
REQUIRE(ok.status==200);
V<P<S,S>>headers;
for(int i=0;i<20;++i)
headers.emplace_back(format("x-extra-{}",i),"1");
auto resp=client.get_with_headers("/ping",move(headers));
REQUIRE(resp.closed);
CHECK(resp.status==0);
CHECK(resp.error_code==NGHTTP2_ENHANCE_YOUR_CALM);
}
TEST_CASE(
"h2: header list bytes over parser limit resets stream"){
Config cfg=Config::test();
cfg.parser_limits.max_header_block_size=256;
conflux::tests::HttpsServerFixture const fx{cfg,make_router()};
H2Client client{fx.port()};
auto ok=client.get("/ping");
REQUIRE(ok.status==200);
auto resp=client.get_with_headers("/ping",{{"x-large",S(256,'x')}});
REQUIRE(resp.closed);
CHECK(resp.status==0);
CHECK(resp.error_code==NGHTTP2_ENHANCE_YOUR_CALM);
}
TEST_CASE(
"h2: unknown route returns 404"){
conflux::tests::HttpsServerFixture const fx{make_router()};
H2Client client{fx.port()};
auto resp=client.get("/does-not-exist");
REQUIRE(resp.status==404);
}
TEST_CASE(
"h2: multiple sequential requests on same connection"){
conflux::tests::HttpsServerFixture const fx{make_router()};
H2Client client{fx.port()};

for(int i=0;i<5;++i){
auto resp=client.get("/ping");
REQUIRE(resp.status==200);
REQUIRE(resp.body==R"({"ok":true})");
}
}
TEST_CASE(
"h2: multiple concurrent streams"){
conflux::tests::HttpsServerFixture const fx{make_router()};
H2Client client{fx.port()};

// Submit three requests without pumping between them.
A<i32,3>sids{
client.submit_get("/ping"),
client.submit_get("/hello/world"),
client.submit_get("/ping"),
};

client.pump_all(sids);

REQUIRE(client.responses_[sids[0]].status==200);
REQUIRE(client.responses_[sids[0]].body==R"({"ok":true})");
REQUIRE(client.responses_[sids[1]].status==200);
REQUIRE(client.responses_[sids[1]].body=="hello world");
REQUIRE(client.responses_[sids[2]].status==200);
REQUIRE(client.responses_[sids[2]].body==R"({"ok":true})");
}
TEST_CASE(
"h2: deferred response completes over HTTP/2"){
auto pool=make_shared<WorkPool>();
Router router;
router.get("/deferred",[pool](HttpRequest const&){
auto deferred=make_shared<DeferredResponse>();
auto queued=pool->enqueue([deferred]{
auto resp=HttpResponse::text("deferred h2 ok");
resp.headers["x-deferred"]="yes";
deferred->complete(move(resp));
});
if(!queued)
return HttpResponse::internal_error("pool enqueue failed");
return HttpResponse::deferred(move(deferred));
});
conflux::tests::HttpsServerFixture const fx{move(router)};
H2Client client{fx.port()};
auto resp=client.get("/deferred");
REQUIRE(resp.status==200);
REQUIRE(resp.body=="deferred h2 ok");
}
TEST_CASE(
"h2: SSE delivers all events over HTTP/2 before channel close"){
Router r;
r.get("/ping",[](HttpRequest const&){return HttpResponse::json(R"({"ok":true})");});
r.sse("/events",[](HttpRequest const&,SP<SseChannel>const&ch){
ch->send("data: alpha\n\n");
ch->send("data: beta\n\n");
ch->send("data: gamma\n\n");
ch->close();
});
conflux::tests::HttpsServerFixture const fx{move(r)};
H2Client client{fx.port()};
auto resp=client.get("/events");
REQUIRE(resp.status==200);
REQUIRE(resp.body.find("data: alpha\n\n")!=S::npos);
REQUIRE(resp.body.find("data: beta\n\n")!=S::npos);
REQUIRE(resp.body.find("data: gamma\n\n")!=S::npos);
REQUIRE(resp.closed);
}
TEST_CASE(
"h2: SSE send_event delivers typed event"){
Router r;
r.get("/ping",[](HttpRequest const&){return HttpResponse::json(R"({"ok":true})");});
r.sse("/typed",[](HttpRequest const&,SP<SseChannel>const&ch){
ch->send_event("update","payload42");
ch->close();
});
conflux::tests::HttpsServerFixture const fx{move(r)};
H2Client client{fx.port()};
auto resp=client.get("/typed");
REQUIRE(resp.status==200);
REQUIRE(resp.body.find("event: update\n")!=S::npos);
REQUIRE(resp.body.find("data: payload42\n")!=S::npos);
REQUIRE(resp.closed);
}
TEST_CASE(
"h2: response trailers arrive after body"){
Router router;
router.get("/ping",[](HttpRequest const&){return HttpResponse::json(R"({"ok":true})");});
router.get("/with-trailers",[](HttpRequest const&){
HttpResponse resp;
resp.status=200;
resp.status_text="OK";
resp.content_type="text/plain; charset=utf-8";
resp.set_text_body("hello trailers");
resp.trailers={
{"x-checksum","crc32:deadbeef"},
{"x-server","conflux"}};
return resp;
});
conflux::tests::HttpsServerFixture const fx{move(router)};
H2Client client{fx.port()};
auto resp=client.get("/with-trailers");
REQUIRE(resp.status==200);
REQUIRE(resp.body=="hello trailers");
auto has_trailer=[&](SV key,SV val){
return ranges::any_of(resp.trailers,[&](auto const&kv){return kv.first==key&&kv.second==val;});
};
REQUIRE(has_trailer("x-checksum","crc32:deadbeef"));
REQUIRE(has_trailer("x-server","conflux"));
}
TEST_CASE(
"h2: large body (>65535 bytes) is fully received via flow control"){
// Default H2 initial window is 65535 bytes.  A 128 KiB response forces the
// server to pause and the client to send WINDOW_UPDATE before delivery completes.
static constexpr SZ kBodySize=128*1024;
S large_body(kBodySize,'X');
Router r;
r.get("/ping",[](HttpRequest const&){return HttpResponse::json(R"({"ok":true})");});
r.get("/big",[&large_body](HttpRequest const&){return HttpResponse::text(large_body);});
conflux::tests::HttpsServerFixture const fx{move(r)};
H2Client client{fx.port()};
auto resp=client.get("/big");
REQUIRE(resp.status==200);
REQUIRE(resp.body.size()==kBodySize);
REQUIRE(resp.body==large_body);
}
TEST_CASE(
"h2: HTTP/1.1 client can still connect to h2-capable server"){
conflux::tests::HttpsServerFixture fx{make_router()};
// curl without --http2 negotiates http/1.1 via ALPN; must still get a 200.
auto[status,body]=fx.curl_https("/ping");
REQUIRE(status==0);
REQUIRE(body==R"({"ok":true})");
}
