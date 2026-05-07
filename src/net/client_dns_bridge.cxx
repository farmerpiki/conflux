import std;
import conflux.types;
import conflux.net.dns;

#include "client_dns_bridge.hxx"

namespace dns=conflux::net::dns;
namespace{
void set_error(
char*dst,
SZ dst_size,
SV message)noexcept{
if(dst_size==0)
return;
SZ const n=min(dst_size-1,message.size());
std::memcpy(dst,message.data(),n);
dst[n]='\0';
}
}// namespace
namespace conflux::http::client_dns_bridge{
bool resolve(
void*resolver_ptr,
char const*host_data,
size_t host_size,
uint16_t port,
long long timeout_ms,
void*sink_ctx,
EndpointSink sink,
char*error_data,
size_t error_size)noexcept{
try{
auto*resolver=static_cast<dns::Resolver*>(resolver_ptr);
if(resolver==nullptr||sink==nullptr){
set_error(error_data,error_size,"missing resolver");
return false;
}
SV const host{host_data,host_size};
auto result=resolver->resolve_blocking(
host,
port,
{.total_timeout=chrono::milliseconds{timeout_ms}});
if(!result){
set_error(error_data,error_size,result.error().what());
return false;
}
for(auto const&ep:result->endpoints){
Endpoint out{};
std::memcpy(out.addr,&ep.addr,min(sizeof(out.addr),sizeof(ep.addr)));
out.addr_len=static_cast<unsigned int>(ep.addr_len);
out.family=(ep.family==dns::AddressFamily::v4)?4:6;
if(!sink(sink_ctx,out)){
set_error(error_data,error_size,"endpoint sink rejected DNS result");
return false;
}
}
return true;
}catch(exception const&e){
set_error(error_data,error_size,e.what());
return false;
}catch(...){
set_error(error_data,error_size,"unknown DNS resolver error");
return false;
}
}
}// namespace conflux::http::client_dns_bridge
