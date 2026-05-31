export module conflux.net.ip_filter;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;

export enum class IpFilterMode {
	allowlist, // only listed CIDRs pass; all others → 403
	blocklist, // listed CIDRs are blocked → 403; all others pass
};
export struct IpFilterOptions {
	IpFilterMode mode{IpFilterMode::allowlist};
	std::vector<std::string> cidrs;
};
namespace ip_filter_detail {

conflux::http::Response forbidden() {
	return conflux::http::Response::text("Forbidden", kHttpForbidden);
}

} // namespace ip_filter_detail
// Middleware factory: allow or block requests by IP address/CIDR.
// Operates on req.remote_addr — compose after forwarded_middleware when
// running behind a reverse proxy.
export conflux::http::Router::Middleware ip_filter_middleware(
	IpFilterOptions opts = {}) {
	auto parsed = parse_cidr_list(opts.cidrs);

	return [opts = std::move(opts), parsed = std::move(parsed)](
			   conflux::http::RequestView const &req,
			   conflux::http::Router::Handler const &next) -> conflux::http::Response {
		auto const ip = parse_ip(req.remote_addr).value_or(IpAddr{});
		bool const matched = std::ranges::any_of(parsed, [&ip](IpCidr const &c) { return cidr_match(c, ip); });

		if (opts.mode == IpFilterMode::allowlist) {
			return matched ? next(req) : ip_filter_detail::forbidden();
		}
		return matched ? ip_filter_detail::forbidden() : next(req);
	};
}
