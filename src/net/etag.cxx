// ETag middleware: computes a strong ETag for dynamic responses and
// handles conditional GET (If-None-Match → 304 Not Modified).
// Uses FNV-1a 64-bit hash — fast, no dependencies.
// Only applied to responses that do not already carry an ETag header and that
// have a non-empty body.  Responses with mapped_file (static files) are skipped
// because serve_static already sets ETags based on size+mtime.
module;
#include<cstdint>

export module conflux.net.etag;
import std;
import conflux.types;
import conflux.net.router;
export struct ETagOptions{
// Use weak ETags (W/"hash"). Weak ETags are semantically equivalent
// but tolerate minor byte-level differences (e.g. gzip vary).
bool weak{false};
};
namespace etag_detail{
SV weak_value(
SV tag)noexcept{
if(tag.starts_with("W/"))
tag.remove_prefix(2);
return tag;
}
bool weak_match(
SV lhs,
SV rhs)noexcept{
return weak_value(lhs)==weak_value(rhs);
}
HttpResponse not_modified(
SV etag){
HttpResponse r{.status=304,.status_text="Not Modified"};
r.headers["ETag"]=S{etag};
return r;
}
}// namespace etag_detail
export Router::Middleware etag_middleware(
ETagOptions opts={}){
return[opts](HttpRequestView const&req,Router::Handler const&next)->HttpResponse{
auto resp=next(req);

// Skip: already has ETag, empty body, SSE/WS, or mmap response.
if(!std::as_const(resp.headers)["ETag"].empty())
return resp;
if(!resp.is_text())
return resp;
if(resp.text_body().empty())
return resp;

// FNV-1a 64-bit.
u64 hash=14695981039346656037ULL;
for(char const ch:resp.text_body()){
hash^=static_cast<u64>(static_cast<unsigned char>(ch));
hash*=1099511628211ULL;
}

auto etag=opts.weak?format("W/\"{:x}\"",hash):format("\"{:x}\"",hash);
resp.headers["ETag"]=etag;

// Check If-None-Match (comma-separated list of ETags).
auto inm=req.headers["if-none-match"];
if(!inm.empty()){
// Handle "*" wildcard.
if(inm=="*")
return etag_detail::not_modified(etag);
// Scan comma-separated values.
SZ pos=0;
while(pos<inm.size()){
// skip whitespace
while(pos<inm.size()&&(inm[pos]==' '||inm[pos]==','))
++pos;
auto end=inm.find(',',pos);
auto token=(end==SV::npos)?inm.substr(pos):inm.substr(pos,end-pos);
// trim trailing whitespace
while(!token.empty()&&token.back()==' ')
token.remove_suffix(1);
if(etag_detail::weak_match(token,etag))
return etag_detail::not_modified(etag);
pos=(end==SV::npos)?inm.size():end+1;
}
}
return resp;
};
}
