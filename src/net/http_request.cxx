module;
#include<cassert>
#include<time.h>

export module conflux.net.http.request;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.json;
export namespace conflux::http{
// ─── HttpRequest ──────────────────────────────────────────────────────────────

class HttpRequest{
public:
class Builder;

static Builder get(SV url);
static Builder post(SV url);
static Builder put(SV url);
static Builder patch(SV url);
static Builder del(SV url);
static Builder head(SV url);
static Builder method(SV m,SV url);
[[nodiscard]]SV method()const noexcept{return method_;}
[[nodiscard]]Url const&url()const noexcept{return url_;}
[[nodiscard]]HttpFields const&headers()const noexcept{return headers_;}
[[nodiscard]]S const&body()const noexcept{return body_;}
[[nodiscard]]HttpTimeouts timeouts()const noexcept{return timeouts_;}
[[nodiscard]]bool verify_peer()const noexcept{return verify_peer_;}
[[nodiscard]]SV server_name()const noexcept{return server_name_;}
[[nodiscard]]int max_redirects()const noexcept{return max_redirects_;}
private:
friend class Builder;

S method_{"GET"};
Url url_{};
HttpFields headers_{true};// case-insensitive
S body_{};
HttpTimeouts timeouts_{};
bool verify_peer_{true};
S server_name_{};
int max_redirects_{0};

explicit HttpRequest()=default;
};
// ─── HttpRequest::Builder ─────────────────────────────────────────────────────

class HttpRequest::Builder{
HttpRequest req_;
bool body_set_{false};
static Url parse_or_throw(
SV raw){
auto r=Url::parse(raw);
if(!r)
throw std::invalid_argument(format("invalid URL: {}",r.error().message));
return move(*r);
}
void assert_single_body(){
#ifndef NDEBUG
assert(!body_set_&&"body set twice without clear_body()");
#endif
body_set_=true;
}
public:
explicit Builder(
SV method_str,
SV url_raw){
req_.method_=S{method_str};
req_.url_=parse_or_throw(url_raw);
}
// Implicit conversion: Builder&& → HttpRequest (no-copy).
operator HttpRequest()&&{return move(req_);}// NOLINT(google-explicit-constructor)
[[nodiscard]]HttpRequest build()&&{return move(req_);}
// ── verbs / URL ──────────────────────────────────────────────────────────

Builder&method(
SV m)&{
req_.method_=S{m};
return*this;
}
Builder&url(
SV raw)&{
req_.url_=parse_or_throw(raw);
return*this;
}
Builder&url(
Url u)&{
req_.url_=move(u);
return*this;
}
Builder&&method(
SV m)&&{
return move(method(m));
}
Builder&&url(
SV raw)&&{
return move(url(raw));
}
Builder&&url(
Url u)&&{
return move(url(move(u)));
}
// ── query ─────────────────────────────────────────────────────────────────

Builder&query(
SV name,
SV value)&{
req_.url_.set_query_param(name,value);
return*this;
}
Builder&query_params(
HttpFields const&kv)&{
for(auto const&[k,v]:kv)
req_.url_.set_query_param(k,v);
return*this;
}
Builder&&query(
SV name,
SV value)&&{
return move(query(name,value));
}
Builder&&query_params(
HttpFields const&kv)&&{
return move(query_params(kv));
}
// ── headers ───────────────────────────────────────────────────────────────

Builder&header(
SV name,
SV value)&{
req_.headers_.set(S{name},S{value});
return*this;
}
Builder&headers(
HttpFields const&h)&{
for(auto const&[k,v]:h)
req_.headers_.set(k,v);
return*this;
}
Builder&bearer(
SV token)&{
return header("Authorization",format("Bearer {}",token));
}
Builder&basic(
SV user,
SV pass)&{
// Base64-encode user:pass.
auto const creds=format("{}:{}",user,pass);
// Simple base64 without external lib.
static constexpr SV kAlpha="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
S b64;
b64.reserve(((creds.size()+2)/3)*4);
for(SZ i=0;i<creds.size();i+=3){
auto const a=static_cast<unsigned char>(creds[i]);
auto const b=(i+1<creds.size())?static_cast<unsigned char>(creds[i+1]):0u;
auto const c=(i+2<creds.size())?static_cast<unsigned char>(creds[i+2]):0u;
b64+=kAlpha[(static_cast<unsigned>(a)>>2)&0x3Fu];
b64+=kAlpha[((static_cast<unsigned>(a)<<4)|(b>>4))&0x3Fu];
b64+=(i+1<creds.size())?kAlpha[((b<<2)|(c>>6))&0x3Fu]:'=';
b64+=(i+2<creds.size())?kAlpha[c&0x3Fu]:'=';
}
return header("Authorization",format("Basic {}",b64));
}
Builder&user_agent(
SV ua)&{
return header("User-Agent",ua);
}
Builder&accept(
SV mime)&{
return header("Accept",mime);
}
Builder&accept_json()&{return accept("application/json");}
Builder&content_type(
SV ct)&{
return header("Content-Type",ct);
}
Builder&if_match(
SV etag)&{
return header("If-Match",etag);
}
Builder&if_none_match(
SV etag)&{
return header("If-None-Match",etag);
}
Builder&if_modified_since(
chrono::system_clock::time_point tp)&{
// RFC 9110 HTTP-date format.
auto const tt=chrono::system_clock::to_time_t(tp);
tm gmt{};
gmtime_r(&tt,&gmt);
A<char,32>buf{};
strftime(buf.data(),buf.size(),"%a, %d %b %Y %H:%M:%S GMT",&gmt);
return header("If-Modified-Since",buf.data());
}
Builder&if_unmodified_since(
chrono::system_clock::time_point tp)&{
auto const tt=chrono::system_clock::to_time_t(tp);
tm gmt{};
gmtime_r(&tt,&gmt);
A<char,32>buf{};
strftime(buf.data(),buf.size(),"%a, %d %b %Y %H:%M:%S GMT",&gmt);
return header("If-Unmodified-Since",buf.data());
}
Builder&&header(
SV name,
SV value)&&{
return move(header(name,value));
}
Builder&&headers(
HttpFields h)&&{
return move(headers(move(h)));
}
Builder&&bearer(
SV token)&&{
return move(bearer(token));
}
Builder&&basic(
SV user,
SV pass)&&{
return move(basic(user,pass));
}
Builder&&user_agent(
SV ua)&&{
return move(user_agent(ua));
}
Builder&&accept(
SV mime)&&{
return move(accept(mime));
}
Builder&&accept_json()&&{return move(accept_json());}
Builder&&content_type(
SV ct)&&{
return move(content_type(ct));
}
Builder&&if_match(
SV etag)&&{
return move(if_match(etag));
}
Builder&&if_none_match(
SV etag)&&{
return move(if_none_match(etag));
}
Builder&&if_modified_since(
chrono::system_clock::time_point tp)&&{
return move(if_modified_since(tp));
}
Builder&&if_unmodified_since(
chrono::system_clock::time_point tp)&&{
return move(if_unmodified_since(tp));
}
// ── body ──────────────────────────────────────────────────────────────────
// Each body_* method asserts in debug that no prior body was set.
// Release builds: last-wins + header overwrite.

Builder&body(
S s)&{
assert_single_body();
req_.body_=move(s);
return*this;
}
Builder&body_view(
SV sv)&{
assert_single_body();
req_.body_=S{sv};
return*this;
}
Builder&body_json(
Document const&doc)&{
assert_single_body();
auto dumped=doc.dump();
if(dumped)
req_.body_=move(*dumped);
return content_type("application/json");
}
Builder&body_json_raw(
S already_serialized)&{
assert_single_body();
req_.body_=move(already_serialized);
return content_type("application/json");
}
Builder&body_form(
HttpFields const&fields)&{
assert_single_body();
S encoded;
for(auto const&[k,v]:fields){
if(!encoded.empty())
encoded+='&';
auto encode_part=[](SV s){
S out;
for(auto const raw_c:s){
unsigned char const c=static_cast<unsigned char>(raw_c);
if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')
out+=static_cast<char>(c);
else if(c==' ')
out+='+';
else
out+=format("%{:02X}",c);
}
return out;
};
encoded+=encode_part(k);
encoded+='=';
encoded+=encode_part(v);
}
req_.body_=move(encoded);
return content_type("application/x-www-form-urlencoded");
}
Builder&clear_body()&{
req_.body_.clear();
body_set_=false;
return*this;
}
Builder&&body(
S s)&&{
return move(body(move(s)));
}
Builder&&body_view(
SV sv)&&{
return move(body_view(sv));
}
Builder&&body_json(
Document const&d)&&{
return move(body_json(d));
}
Builder&&body_json_raw(
S s)&&{
return move(body_json_raw(move(s)));
}
Builder&&body_form(
HttpFields f)&&{
return move(body_form(move(f)));
}
Builder&&clear_body()&&{return move(clear_body());}
// ── execution policy ──────────────────────────────────────────────────────

Builder&timeouts(
HttpTimeouts t)&{
req_.timeouts_=t;
return*this;
}
Builder&follow_redirects(
int max=10)&{
req_.max_redirects_=max;
return*this;
}
Builder&disable_redirects()&{
req_.max_redirects_=0;
return*this;
}
Builder&verify_peer(
bool v)&{
req_.verify_peer_=v;
return*this;
}
Builder&server_name(
SV sni)&{
req_.server_name_=S{sni};
return*this;
}
Builder&&timeouts(
HttpTimeouts t)&&{
return move(timeouts(t));
}
Builder&&follow_redirects(
int max)&&{
return move(follow_redirects(max));
}
Builder&&disable_redirects()&&{return move(disable_redirects());}
Builder&&verify_peer(
bool v)&&{
return move(verify_peer(v));
}
Builder&&server_name(
SV s)&&{
return move(server_name(s));
}
};
// ─── Static factory implementations ──────────────────────────────────────────

HttpRequest::Builder HttpRequest::get(
SV url){
return Builder{"GET",url};
}
HttpRequest::Builder HttpRequest::post(
SV url){
return Builder{"POST",url};
}
HttpRequest::Builder HttpRequest::put(
SV url){
return Builder{"PUT",url};
}
HttpRequest::Builder HttpRequest::patch(
SV url){
return Builder{"PATCH",url};
}
HttpRequest::Builder HttpRequest::del(
SV url){
return Builder{"DELETE",url};
}
HttpRequest::Builder HttpRequest::head(
SV url){
return Builder{"HEAD",url};
}
HttpRequest::Builder HttpRequest::method(
SV m,
SV url){
return Builder{m,url};
}
}// namespace conflux::http
