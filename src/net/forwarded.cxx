export module conflux.net.forwarded;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;
export struct ForwardedOptions {
	// CIDRs trusted to set forwarding headers.
	// If empty and strict_mode is true (the default): trust nobody. Forwarding
	// headers are stripped for every request. If empty and strict_mode is false:
	// legacy behaviour — all peers are trusted.
	std::vector<std::string> trusted_proxies;

	// Header to read the real client IP from (checked in order).
	// X-Forwarded-For may contain a comma-separated chain; first entry is used.
	bool use_x_forwarded_for{true};
	bool use_x_real_ip{true};
	// When true (default) an empty trusted_proxies list means no header is trusted.
	// Flip to false only for deployments that consciously rely on the legacy
	// trust-all-on-empty semantics.
	bool strict_mode{true};
};
namespace forwarded_detail {

// Extract the first (leftmost) IP from a comma-separated X-Forwarded-For value.
std::string_view xff_first(
	std::string_view value) noexcept {
	auto comma = value.find(',');
	return trim((comma == std::string_view::npos) ? value : std::string_view{value.data(), comma});
}

} // namespace forwarded_detail
// Middleware factory: rewrite req.remote_addr from trusted proxy headers.
// If the direct peer is in trusted_proxies (or trusted_proxies is empty),
// the real client IP is extracted from X-Forwarded-For / X-Real-IP.
// Headers from untrusted peers are stripped before passing downstream.
export Router::Middleware forwarded_middleware(
	ForwardedOptions opts = {}) {
	auto cidrs = parse_cidr_list(opts.trusted_proxies);
	if (opts.trusted_proxies.empty() && !opts.strict_mode) {
		eprintln("forwarded_middleware: empty trusted_proxies with strict_mode=false trusts every peer");
	}

	return [opts = move(opts),
			cidrs = move(cidrs)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		bool const trust_empty = opts.trusted_proxies.empty() && !opts.strict_mode;
		bool const trusted = trust_empty || [&] {
			auto const peer_ip = parse_ip(req.remote_addr).value_or(IpAddr{});
			return ranges::any_of(cidrs, [&](IpCidr const &c) { return cidr_match(c, peer_ip); });
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
				real_ip = std::string{trim(xri)};
			}
		}

		if (real_ip.empty()) {
			return next(req);
		}

		// Normalize to canonical form so downstream modules (rate limiter,
		// ip_filter) key on the same std::string regardless of proxy notation.
		if (auto parsed = parse_ip(real_ip)) {
			real_ip = ip_to_string(*parsed);
		}

		auto enriched = req.to_owned();
		enriched.remote_addr = move(real_ip);
		return next(enriched);
	};
}
