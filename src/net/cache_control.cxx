export module conflux.net.cache_control;
import std;
import conflux.types;
import conflux.net.router;

export struct CacheRule {
	// MIME prefix to match (e.g. "image/", "text/css", "application/json").
	// Empty S matches everything — useful as a fallback rule.
	S mime_prefix;

	// Value for the Cache-Control header (e.g. "max-age=3600, public").
	// Set to "no-store" to explicitly disable caching.
	S directive;
};

export struct CacheControlOptions {
	// Rules are evaluated in order; first match wins.
	V<CacheRule> rules;

	// Default directive when no rule matches.
	// Empty S → no Cache-Control header added.
	S default_directive;
};

// Middleware factory: set Cache-Control header on responses.
// Rules are matched against the response Content-Type (MIME prefix).
// First matching rule wins; falls back to default_directive if set.
export Router::Middleware cache_control_middleware(
	CacheControlOptions opts = {}) {
	return [opts = move(opts)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto resp = next(req);

		// Don't overwrite an explicit Cache-Control already set by the handler.
		if (!resp.headers["Cache-Control"].empty()) {
			return resp;
		}

		SV const ct = resp.content_type;
		// Strip parameters (e.g. "; charset=utf-8") for matching.
		auto semi = ct.find(';');
		auto mime = (semi == SV::npos) ? ct : ct.substr(0, semi);
		// Trim trailing whitespace.
		while (!mime.empty() && mime.back() == ' ') {
			mime.remove_suffix(1);
		}

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
