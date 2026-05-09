export module conflux.net.ip_filter;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;

export enum class IpFilterMode{
allowlist,// only listed CIDRs pass; all others → 403
blocklist// listed CIDRs are blocked → 403; all others pass
};
export struct IpFilterOptions{
IpFilterMode mode{IpFilterMode::allowlist};
V<S>cidrs;
};
namespace ip_filter_detail{
HttpResponse forbidden(){
HttpResponse r;
r.status=403;
r.status_text="Forbidden";
r.content_type="text/plain; charset=utf-8";
r.set_text_body("Forbidden");
return r;
}
}// namespace ip_filter_detail
// Middleware factory: allow or block requests by IP address/CIDR.
// Operates on req.remote_addr — compose after forwarded_middleware when
// running behind a reverse proxy.
export Router::Middleware ip_filter_middleware(
IpFilterOptions opts={}){
auto parsed=parse_cidr_list(opts.cidrs);

return[opts=move(opts),
parsed=move(parsed)](HttpRequestView const&req,Router::Handler const&next)->HttpResponse{
auto const ip=parse_ip(req.remote_addr).value_or(IpAddr{});
bool const matched=ranges::any_of(parsed,[&ip](IpCidr const&c){return cidr_match(c,ip);});

if(opts.mode==IpFilterMode::allowlist)
return matched?next(req):ip_filter_detail::forbidden();
return matched?ip_filter_detail::forbidden():next(req);
};
}
