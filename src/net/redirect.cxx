export module conflux.net.redirect;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;
[[nodiscard]] inline bool is_safe_redirect_suffix(
	std::string_view s) noexcept {
	if (s.starts_with("//") || s.starts_with("/\\")) {
		return false;
	}
	return std::ranges::none_of(s, [](char c) { return c == '@' || c == '\\' || c == '\r' || c == '\n'; });
}
export struct RedirectRule {
	// Path to match. When prefix_match is false, exact match only.
	std::string from;
	// Target URL or path. For prefix matches the unmatched suffix is appended.
	std::string to;
	// HTTP redirect status code (301, 302, 307, 308).
	int status{302};
	// When true, match any request path starting with `from`.
	bool prefix_match{false};
};
export struct RedirectOptions {
	std::vector<RedirectRule> rules;
};
// Middleware factory: redirect requests matching configured rules.
// Rules are evaluated in order; first match wins.
export conflux::http::Router::Middleware redirect_middleware(
	RedirectOptions opts = {}) {
	return [opts = std::move(
				opts)](RequestView const &req, conflux::http::Router::Handler const &next) -> conflux::http::Response {
		for (auto const &rule: opts.rules) {
			bool matched = false;
			std::string target = rule.to;
			if (rule.prefix_match) {
				if (req.path.starts_with(rule.from)) {
					auto const suffix = req.path.substr(rule.from.size());
					if (!is_safe_redirect_suffix(suffix)) {
						return conflux::http::Response::not_found(req.path);
					}
					matched = true;
					target += suffix;
				}
			} else {
				matched = (req.path == rule.from);
			}
			if (matched) {
				return conflux::http::Response::redirect(std::move(target), rule.status);
			}
		}
		return next(req);
	};
}
