export module conflux.net.redirect;
import std;
import conflux.net.router;
using namespace std;

export struct RedirectRule {
	// Path to match. When prefix_match is false, exact match only.
	string from;
	// Target URL or path. For prefix matches the unmatched suffix is appended.
	string to;
	// HTTP redirect status code (301, 302, 307, 308).
	int status{302};
	// When true, match any request path starting with `from`.
	bool prefix_match{false};
};

export struct RedirectOptions {
	vector<RedirectRule> rules;
};

// Middleware factory: redirect requests matching configured rules.
// Rules are evaluated in order; first match wins.
export Router::Middleware redirect_middleware(
	RedirectOptions opts = {}) {
	return [opts = move(opts)](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		for (auto const &rule: opts.rules) {
			bool matched = false;
			string target = rule.to;
			if (rule.prefix_match) {
				if (req.path.starts_with(rule.from)) {
					matched = true;
					// Append the unmatched suffix to the target.
					target += req.path.substr(rule.from.size());
				}
			} else {
				matched = (req.path == rule.from);
			}
			if (matched) {
				return HttpResponse::redirect(move(target), rule.status);
			}
		}
		return next(req);
	};
}
