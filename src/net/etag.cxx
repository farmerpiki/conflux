// ETag middleware: computes a strong ETag for dynamic responses and
// handles conditional GET (If-None-Match → 304 Not Modified).
// Uses FNV-1a 64-bit std::hash — fast, no dependencies.
// Only applied to responses that do not already carry an ETag header and that
// have a non-empty body.  Responses with mapped_file (static files) are skipped
// because serve_static already sets ETags based on size+mtime.
module;
#include <cstdint>

export module conflux.net.etag;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;
export struct ETagOptions {
	// Use weak ETags (W/"std::hash"). Weak ETags are semantically equivalent
	// but tolerate minor std::byte-level differences (e.g. gzip vary).
	bool weak{false};
};
namespace etag_detail {

std::string_view weak_value(
	std::string_view tag) noexcept {
	if (tag.starts_with("W/")) {
		tag.remove_prefix(2);
	}
	return tag;
}
bool weak_match(
	std::string_view lhs,
	std::string_view rhs) noexcept {
	return weak_value(lhs) == weak_value(rhs);
}
conflux::http::Response not_modified(
	std::string_view etag) {
	return conflux::http::Response::not_modified(etag);
}

} // namespace etag_detail
export conflux::http::Router::Middleware etag_middleware(
	ETagOptions opts = {}) {
	return [opts](conflux::http::RequestView const &req, conflux::http::Router::Handler const &next) -> conflux::http::Response {
		auto resp = next(req);

		// Skip: already has ETag, empty body, SSE/WS, or mmap response.
		if (!std::as_const(resp.headers)["ETag"].empty()) {
			return resp;
		}
		if (!resp.is_text()) {
			return resp;
		}
		if (resp.text_body().empty()) {
			return resp;
		}

		// FNV-1a 64-bit.
		std::uint64_t hash_value = 14695981039346656037ULL;
		for (char const ch: resp.text_body()) {
			hash_value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
			hash_value *= 1099511628211ULL;
		}

		auto etag = opts.weak ? std::format("W/\"{:x}\"", hash_value) : std::format("\"{:x}\"", hash_value);
		resp.headers["ETag"] = etag;

		// Check If-None-Match (comma-separated list of ETags).
		auto inm = req.headers["if-none-match"];
		if (!inm.empty()) {
			bool const matched = std::ranges::any_of(conflux::http::header_tokens(inm), [&](std::string_view token) {
				return token == "*" || etag_detail::weak_match(token, etag);
			});
			if (matched) {
				return etag_detail::not_modified(etag);
			}
		}
		return resp;
	};
}
