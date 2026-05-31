export module conflux.net.cache_control;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.parse_helpers;
import conflux.net.router;
import conflux.net.http.response;
export namespace conflux::http {
struct CacheRule {
	// MIME prefix to match (e.g. "image/", "text/css", "application/json").
	// Empty std::string matches everything — useful as a fallback rule.
	std::string mime_prefix;

	// Value for the Cache-Control header (e.g. "max-age=3600, public").
	// Set to "no-store" to explicitly disable caching.
	std::string directive;
};
struct CacheControlOptions {
	// Rules are evaluated in order; first match wins.
	std::vector<CacheRule> rules;

	// Default directive when no rule matches.
	// Empty std::string → no Cache-Control header added.
	std::string default_directive;
};
// Middleware factory: set Cache-Control header on responses.
// Rules are matched against the response Content-Type (MIME prefix).
// First matching rule wins; falls back to default_directive if set.
Router::Middleware cache_control_middleware(
	CacheControlOptions opts = {}) {
	return [opts = std::move(
				opts)](conflux::http::RequestView const &req, conflux::http::Router::Handler const &next) -> conflux::http::Response {
		auto resp = next(req);

		// Don't overwrite an explicit Cache-Control already set by the handler.
		if (!resp.headers["Cache-Control"].empty()) {
			return resp;
		}

		auto const mime = conflux::http::content_type_media_type(resp.content_type);

		for (auto const &rule: opts.rules) {
			if (rule.mime_prefix.empty() || mime.starts_with(rule.mime_prefix)) {
				if (!rule.directive.empty()) {
					resp.headers["Cache-Control"] = rule.directive;
				}
				return resp;
			}
		}

		if (!opts.default_directive.empty()) {
			resp.headers["Cache-Control"] = opts.default_directive;
		}
		return resp;
	};
}

} // namespace conflux::http
