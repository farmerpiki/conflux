export module conflux.net.trailing_slash;
import std;
import conflux.types;
import conflux.net.router;

export enum class TrailingSlashMode {
	remove, // /foo/  → /foo   (default; canonical form for most APIs)
	add, // /foo   → /foo/  (useful for directory-style sites)
};

export struct TrailingSlashOptions {
	TrailingSlashMode mode{TrailingSlashMode::remove};
	int redirect_status{301}; // 301 Moved Permanently or 308 Permanent Redirect
};

namespace trailing_slash_detail {

inline bool is_unreserved(
	unsigned char c) noexcept {
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
		return true;
	}
	return c == '-' || c == '.' || c == '_' || c == '~';
}

inline S percent_encode(
	SV in) {
	S out;
	out.reserve(in.size());
	static constexpr char kHex[] = "0123456789ABCDEF";
	for (char const ch: in) {
		auto const c = static_cast<unsigned char>(ch);
		if (is_unreserved(c)) {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(kHex[c >> 4U]);
			out.push_back(kHex[c & 0x0FU]);
		}
	}
	return out;
}

inline S build_query(
	HttpFieldsView const &query) {
	S out;
	bool first = true;
	for (auto const &[k, v]: query) {
		if (!first) {
			out.push_back('&');
		}
		first = false;
		out += percent_encode(k);
		out.push_back('=');
		out += percent_encode(v);
	}
	return out;
}

} // namespace trailing_slash_detail

// Middleware factory: redirect requests with a trailing slash mismatch.
// The root path "/" is never redirected regardless of mode.
// Query S is re-serialized from parsed fields (percent-encoded per RFC 3986
// unreserved set) and appended to the Location header.
export Router::Middleware trailing_slash_middleware(
	TrailingSlashOptions opts = {}) {
	return [opts](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto const &path = req.path;

		// Never touch the root.
		if (path == "/" || path.empty()) {
			return next(req);
		}

		bool const has_slash = path.back() == '/';

		auto make_redirect = [&](S new_path) {
			if (!req.query.empty()) {
				new_path.push_back('?');
				new_path += trailing_slash_detail::build_query(req.query);
			}
			char const *status_text = "Found";
			switch (opts.redirect_status) {
			case 301: status_text = "Moved Permanently"; break;
			case 307: status_text = "Temporary Redirect"; break;
			case 308: status_text = "Permanent Redirect"; break;
			default : break;
			}
			HttpResponse r{
				.status = opts.redirect_status,
				.status_text = status_text,
				.content_type = "text/plain; charset=utf-8"};
			r.headers["Location"] = move(new_path);
			return r;
		};

		if (opts.mode == TrailingSlashMode::remove && has_slash) {
			return make_redirect(S{path.substr(0, path.size() - 1)});
		}
		if (opts.mode == TrailingSlashMode::add && !has_slash) {
			return make_redirect(S{path} + '/');
		}

		return next(req);
	};
}
