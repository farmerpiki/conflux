// Response caching middleware: in-memory LRU cache with TTL.
// Only caches GET requests with 200 responses and no Set-Cookie headers.
// Cache key is the full request path (including query S).
// TTL is taken from the response Cache-Control max-age if present; otherwise
// falls back to ResponseCacheOptions::default_ttl.
export module conflux.net.response_cache;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;
export struct ResponseCacheOptions{
// Maximum number of entries in the LRU cache.
SZ max_entries{256};
// Maximum total bytes of cached response bodies (0 = unlimited).
SZ max_bytes{64ULL*1024*1024};
// Default TTL when the response has no Cache-Control max-age.
chrono::seconds default_ttl{60};
// When true, Vary: * responses are not cached.
bool respect_vary{true};
};
// Internal cache entry type (not exported; module-scope to avoid GCC TU-local error).
struct RespCacheEntry{
HttpResponse resp;
chrono::steady_clock::time_point expires;
};
// LRU cache: module-scope (not exported, not anonymous-namespace).
class RespLruCache{
public:
explicit RespLruCache(
SZ max,
SZ max_bytes)
:max_(max),max_bytes_(max_bytes){}
// Returns pointer to cached entry (nullptr if absent or expired).
RespCacheEntry const*get(
S const&key){
auto it=map_.find(key);
if(it==map_.end())
return nullptr;
if(chrono::steady_clock::now()>=it->second.expires){
total_bytes_-=it->second.resp.text_body().size();
order_.erase(iters_.at(key));
iters_.erase(key);
map_.erase(it);
return nullptr;
}
// Move to front (MRU) in O(1).
order_.erase(iters_.at(key));
order_.push_front(key);
iters_[key]=order_.begin();
return&it->second;
}
void put(
S const&key,
RespCacheEntry entry){
SZ const entry_bytes=entry.resp.text_body().size();
if(max_bytes_>0&&entry_bytes>max_bytes_)
return;
auto it=map_.find(key);
if(it!=map_.end()){
total_bytes_-=it->second.resp.text_body().size();
map_.erase(it);
order_.erase(iters_.at(key));
iters_.erase(key);
// Fall through to eviction+insertion path below.
}
while((max_bytes_>0&&total_bytes_+entry_bytes>max_bytes_)||map_.size()>=max_){
if(order_.empty())
return;
S const&lru=order_.back();
total_bytes_-=map_.at(lru).resp.text_body().size();
map_.erase(lru);
iters_.erase(lru);
order_.pop_back();
}
total_bytes_+=entry_bytes;
order_.push_front(key);
iters_.emplace(key,order_.begin());
map_.emplace(key,move(entry));
}
[[nodiscard]]V<S>const*vary_for(
S const&path)const{
auto it=path_vary_.find(path);
return it==path_vary_.end()?nullptr:&it->second;
}
void set_vary(
S const&path,
V<S>headers){
path_vary_[path]=move(headers);
}
private:
SZ max_;
SZ max_bytes_;
SZ total_bytes_{0};
std::list<S>order_;
UM<S,std::list<S>::iterator>iters_;
UM<S,RespCacheEntry>map_;
UM<S,V<S>>path_vary_;
};
namespace response_cache_detail{
bool ascii_iequals(
SV lhs,
SV rhs)noexcept{
if(lhs.size()!=rhs.size())
return false;
for(SZ i=0;i<lhs.size();++i){
auto const l=static_cast<unsigned char>(lhs[i]);
auto const r=static_cast<unsigned char>(rhs[i]);
if((l|0x20U)!=(r|0x20U))
return false;
}
return true;
}
bool cache_control_directive_contains(
SV cc,
SV directive){
while(!cc.empty()){
auto comma=cc.find(',');
auto part=trim((comma==SV::npos)?cc:cc.substr(0,comma));
auto eq=part.find('=');
auto name=trim((eq==SV::npos)?part:part.substr(0,eq));
if(ascii_iequals(name,directive))
return true;
if(comma==SV::npos)
return false;
cc.remove_prefix(comma+1);
}
return false;
}
// Parse max-age from a Cache-Control header value. Returns 0 if not found.
chrono::seconds parse_max_age(
SV cc){
while(!cc.empty()){
auto comma=cc.find(',');
auto part=trim((comma==SV::npos)?cc:cc.substr(0,comma));
auto eq=part.find('=');
if(eq!=SV::npos){
auto name=trim(part.substr(0,eq));
if(ascii_iequals(name,"max-age")){
auto val=trim(part.substr(eq+1));
long v=0;
auto[ptr,ec]=from_chars(val.data(),val.data()+val.size(),v);
if(ec!=errc{}||ptr!=val.data()+val.size())
return chrono::seconds{0};
return chrono::seconds{v};
}
}
if(comma==SV::npos)
return chrono::seconds{0};
cc.remove_prefix(comma+1);
}
return chrono::seconds{0};
}
// Parse a Vary header value into a sorted, lowercased, deduped list of header names.
// Returns empty V for empty input or "*".
V<S>parse_vary(
SV vary){
V<S>out;
SZ i=0;
while(i<vary.size()){
while(i<vary.size()&&(vary[i]==' '||vary[i]=='\t'||vary[i]==','))
++i;
SZ const start=i;
while(i<vary.size()&&vary[i]!=',')
++i;
auto token=vary.substr(start,i-start);
while(!token.empty()&&(token.back()==' '||token.back()=='\t'))
token.remove_suffix(1);
if(token.empty()||token=="*")
continue;
out.push_back(ascii_lower(token));
}
ranges::sort(out);
auto dup=ranges::unique(out);
out.erase(dup.begin(),dup.end());
return out;
}
S build_cache_key(
SV path,
HttpFieldsView const&query,
V<S>const&vary,
HttpFieldsView const&req_headers){
S key{path};
if(!query.empty()){
key+="|q:";
for(auto const&[name,value]:query){
key+=format("{}:",name.size());
key+=name;
key+=format("{}:",value.size());
key+=value;
}
}
if(vary.empty())
return key;
for(auto const&h:vary){
key+='|';
key+=h;
key+='=';
key+=req_headers[h];
}
return key;
}
}// namespace response_cache_detail
export Router::Middleware response_cache_middleware(
ResponseCacheOptions opts={}){
auto cache=make_shared<RespLruCache>(opts.max_entries,opts.max_bytes);
auto mtx=make_shared<mutex>();

return[opts,cache,mtx](HttpRequestView const&req,Router::Handler const&next)->HttpResponse{
bool const is_head=req.method=="HEAD";
if(req.method!="GET"&&!is_head)
return next(req);

S const path{req.path};

{
SL const lk{*mtx};
auto const*vary=cache->vary_for(path);
auto lookup_key=
response_cache_detail::build_cache_key(path,req.query,vary?*vary:V<S>{},req.headers);
auto const*entry=cache->get(lookup_key);
if(entry!=nullptr){
auto resp=entry->resp;
if(is_head)
resp.head_only=true;
return resp;
}
}

auto resp=next(req);

if(is_head)
return resp;
if(resp.status!=200)
return resp;
if(!resp.set_cookies.empty())
return resp;
if(!resp.is_text())
return resp;
auto cc=std::as_const(resp.headers)["Cache-Control"];
if(response_cache_detail::cache_control_directive_contains(cc,"no-store"))
return resp;
if(response_cache_detail::cache_control_directive_contains(cc,"no-cache"))
return resp;// no-cache requires revalidation which this cache cannot perform
if(response_cache_detail::cache_control_directive_contains(cc,"private"))
return resp;
auto vary_hdr=std::as_const(resp.headers)["Vary"];
if(opts.respect_vary&&vary_hdr.find('*')!=SV::npos)
return resp;
auto vary_list=response_cache_detail::parse_vary(vary_hdr);

auto ttl=response_cache_detail::parse_max_age(cc);
if(ttl.count()==0&&response_cache_detail::cache_control_directive_contains(cc,"max-age"))
return resp;// max-age=0: do not cache
if(ttl.count()==0)
ttl=opts.default_ttl;

{
SL const lk{*mtx};
if(!vary_list.empty())
cache->set_vary(path,vary_list);
auto store_key=response_cache_detail::build_cache_key(path,req.query,vary_list,req.headers);
cache->put(store_key,{.resp=resp,.expires=chrono::steady_clock::now()+ttl});
}
return resp;
};
}
