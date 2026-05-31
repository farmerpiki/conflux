export module conflux.net.trailing_slash;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;

export enum class TrailingSlashMode {
	remove, // /foo/  → /foo   (default; canonical form for most APIs)
	add, // /foo   → /foo/  (useful for directory-style sites)
};
export struct TrailingSlashOptions {
	TrailingSlashMode mode{TrailingSlashMode::remove};
	int redirect_status{301}; // 301 Moved Permanently or 308 Permanent Redirect
};
namespace trailing_slash_detail {

inline std::string build_query(
	HttpFieldsView const &query) {
	std::size_t size = query.empty() ? 0 : query.size() - 1;
	for (auto const &[k, v]: query) {
		size += url_percent_encoded_size(k) + 1 + url_percent_encoded_size(v);
	}

	std::string out;
	out.reserve(size);
	bool first = true;
	for (auto const &[k, v]: query) {
		if (!first) {
			out.push_back('&');
		}
		first = false;
		append_url_percent_encoded(out, k);
		out.push_back('=');
		append_url_percent_encoded(out, v);
	}
	return out;
}

} // namespace trailing_slash_detail
// Middleware factory: redirect requests with a trailing slash mismatch.
// The root path "/" is never redirected regardless of mode.
// Query std::string is re-serialized from parsed fields (percent-encoded per RFC 3986
// unreserved set) and appended to the Location header.
export conflux::http::Router::Middleware trailing_slash_middleware(
	TrailingSlashOptions opts = {}) {
	return [opts](conflux::http::RequestView const &req, conflux::http::Router::Handler const &next) -> conflux::http::Response {
		auto const &path = req.path;

		// Never touch the root.
		if (path == "/" || path.empty()) {
			return next(req);
		}

		bool const has_slash = path.back() == '/';

		auto make_redirect = [&](std::string new_path) {
			if (!req.query.empty()) {
				new_path.push_back('?');
				new_path += trailing_slash_detail::build_query(req.query);
			}
			auto redirect = conflux::http::Response::redirect(new_path, opts.redirect_status);
			redirect.content_type = "text/plain; charset=utf-8";
			return redirect;
		};

		if (opts.mode == TrailingSlashMode::remove && has_slash) {
			return make_redirect(std::string{path.substr(0, path.size() - 1)});
		}
		if (opts.mode == TrailingSlashMode::add && !has_slash) {
			std::string new_path{path};
			new_path.push_back('/');
			return make_redirect(std::move(new_path));
		}

		return next(req);
	};
}
