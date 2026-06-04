export module conflux.net.forwarded;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;
export namespace conflux::http {

struct ForwardedOptions {
	// CIDRs trusted to set forwarding headers.
	// If empty, trust nobody. Forwarding headers are stripped for every request.
	std::vector<std::string> trusted_proxies;

	// Header to read the real client IP from (checked in order).
	// X-Forwarded-For may contain a comma-separated chain; first entry is used.
	bool use_x_forwarded_for{true};
	bool use_x_real_ip{true};
};
namespace forwarded_detail {

// Extract the first (leftmost) IP from a comma-separated X-Forwarded-For value.
std::string_view xff_first(
	std::string_view value) noexcept {
	auto comma = value.find(',');
	return conflux::utils::trim((comma == std::string_view::npos) ? value : std::string_view{value.data(), comma});
}

} // namespace forwarded_detail
// Middleware factory: rewrite req.remote_addr from trusted proxy headers.
// If the direct peer is in trusted_proxies (or trusted_proxies is empty),
// the real client IP is extracted from X-Forwarded-For / X-Real-IP.
// Headers from untrusted peers are stripped before passing downstream.
Router::Middleware forwarded_middleware(
	ForwardedOptions opts = {}) {
	auto cidrs = conflux::utils::parse_cidr_list(opts.trusted_proxies);

	return [opts = std::move(opts), cidrs = std::move(cidrs)](
			   conflux::http::RequestView const &req,
			   conflux::http::Router::Handler const &next) -> conflux::http::Response {
		bool const trusted = [&] {
			auto const peer_ip = conflux::utils::parse_ip(req.remote_addr).value_or(conflux::utils::IpAddr{});
			return std::ranges::any_of(cidrs, [&](conflux::utils::IpCidr const &c) {
				return conflux::utils::cidr_match(c, peer_ip);
			});
		}();

		if (!trusted) {
			auto sanitized = req.to_owned();
			sanitized.headers["x-forwarded-for"] = "";
			sanitized.headers["x-real-ip"] = "";
			return next(sanitized);
		}

		std::string real_ip;
		if (opts.use_x_forwarded_for) {
			auto xff = req.headers["x-forwarded-for"];
			if (!xff.empty()) {
				real_ip = std::string{forwarded_detail::xff_first(xff)};
			}
		}
		if (real_ip.empty() && opts.use_x_real_ip) {
			auto xri = req.headers["x-real-ip"];
			if (!xri.empty()) {
				real_ip = std::string{conflux::utils::trim(xri)};
			}
		}

		if (real_ip.empty()) {
			return next(req);
		}

		// Normalize to canonical form so downstream modules (rate limiter,
		// ip_filter) key on the same std::string regardless of proxy notation.
		if (auto parsed = conflux::utils::parse_ip(real_ip)) {
			real_ip = conflux::utils::ip_to_string(*parsed);
		}

		auto enriched = req.to_owned();
		enriched.remote_addr = std::move(real_ip);
		return next(enriched);
	};
}

} // namespace conflux::http
