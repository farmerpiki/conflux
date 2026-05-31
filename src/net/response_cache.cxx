// conflux::http::Response caching middleware: in-memory LRU cache with TTL.
// Only caches GET requests with 200 responses and no Set-Cookie headers.
// Cache key is the full request path (including query string).
// TTL is taken from the response Cache-Control max-age if present; otherwise
// falls back to ResponseCacheOptions::default_ttl.
module;

export module conflux.net.response_cache;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.http.response;
export namespace conflux::http {
struct ResponseCacheOptions {
	// Maximum number of entries in the LRU cache.
	std::size_t max_entries{256};
	// Maximum total bytes of cached response bodies (0 = unlimited).
	std::size_t max_bytes{64ULL * 1024 * 1024};
	// Default TTL when the response has no Cache-Control max-age.
	std::chrono::seconds default_ttl{60};
	// When true, Vary: * responses are not cached.
	bool respect_vary{true};
};
// Internal cache entry type (not exported; module-scope to avoid GCC TU-local error).
struct RespCacheEntry {
	conflux::http::Response resp;
	std::chrono::steady_clock::time_point expires;
};

// LRU cache: module-scope (not exported, not anonymous-namespace).
class RespLruCache {
public:
	explicit RespLruCache(
		std::size_t max_entries,
		std::size_t max_bytes)
		: max_(max_entries)
		, max_bytes_(max_bytes)
		, entries_(std::max<std::size_t>(max_entries, 1)) {}
	// Returns pointer to cached entry (nullptr if absent or expired).
	RespCacheEntry const *get(
		std::string_view key) {
		auto *entry = entries_.find(key);
		if (entry == nullptr) {
			return nullptr;
		}
		if (std::chrono::steady_clock::now() < entry->expires) {
			return entry;
		}
		total_bytes_ -= entry->resp.text_body().size();
		(void)entries_.erase(key);
		return nullptr;
	}
	void put(
		std::string_view key,
		RespCacheEntry entry) {
		if (max_ == 0) {
			return;
		}
		std::size_t const entry_bytes = entry.resp.text_body().size();
		if (max_bytes_ > 0 && entry_bytes > max_bytes_) {
			return;
		}
		if (auto *existing = entries_.find(key); existing != nullptr) {
			total_bytes_ -= existing->resp.text_body().size();
			(void)entries_.erase(key);
		}
		while ((max_bytes_ > 0 && total_bytes_ + entry_bytes > max_bytes_) || entries_.size() >= max_) {
			if (!entries_.evict_lru(
					[this](std::string_view, RespCacheEntry &old) { total_bytes_ -= old.resp.text_body().size(); })) {
				return;
			}
		}
		total_bytes_ += entry_bytes;
		(void)entries_.insert_or_assign(key, std::move(entry));
	}
	[[nodiscard]] std::vector<std::string> const *vary_for(
		std::string_view path) const {
		auto it = path_vary_.find(path);
		return it == path_vary_.end() ? nullptr : &it->second;
	}
	void set_vary(
		std::string_view path,
		std::vector<std::string> headers) {
		path_vary_[std::string{path}] = std::move(headers);
	}

private:
	std::size_t max_;
	std::size_t max_bytes_;
	std::size_t total_bytes_{0};
	conflux::support::StringLruMap<RespCacheEntry> entries_;
	conflux::support::TransparentStringMap<std::vector<std::string>> path_vary_;
};
namespace response_cache_detail {

bool cache_control_directive_contains(
	std::string_view cc,
	std::string_view directive) {
	return std::ranges::any_of(conflux::http::header_items(cc), [&](conflux::http::HeaderItem item) {
		return conflux::http::ascii_iequals(item.name, directive);
	});
}
// Parse max-age from a Cache-Control header value. Returns 0 if not found.
std::chrono::seconds parse_max_age(
	std::string_view cc) {
	std::chrono::seconds result{0};
	for (auto const item: conflux::http::header_items(cc)) {
		if (!item.has_value || !conflux::http::ascii_iequals(item.name, "max-age")) {
			continue;
		}
		auto const val = conflux::http::trim_http_whitespace(item.value);
		long v = 0;
		auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), v);
		if (ec == std::errc{} && ptr == val.data() + val.size()) {
			result = std::chrono::seconds{v};
		}
		break;
	}
	return result;
}
// Parse a Vary header value into a sorted, lowercased, deduped list of header names.
// Returns empty vector for empty input or "*".
std::vector<std::string> parse_vary(
	std::string_view vary) {
	std::vector<std::string> out;
	for (auto const token: conflux::http::header_tokens(vary)) {
		if (!token.empty() && token != "*") {
			out.push_back(ascii_lower(token));
		}
	}
	std::ranges::sort(out);
	auto dup = std::ranges::unique(out);
	out.erase(dup.begin(), dup.end());
	return out;
}
void append_len_field(
	std::string &out,
	std::size_t value) {
	std::array<char, 20> buf{};
	auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
	(void)ec;
	out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
	out.push_back(':');
}
std::string build_cache_key(
	std::string_view path,
	conflux::http::HttpFieldsView const &query,
	std::span<std::string const> vary,
	conflux::http::HttpFieldsView const &req_headers) {
	std::size_t capacity = path.size();
	if (!query.empty()) {
		capacity += 3;
		for (auto const &[name, value]: query) {
			capacity += name.size() + value.size() + 42;
		}
	}
	for (auto const &h: vary) {
		capacity += h.size() + req_headers[h].size() + 2;
	}

	std::string key;
	key.reserve(capacity);
	key.append(path.data(), path.size());
	if (!query.empty()) {
		key += "|q:";
		for (auto const &[name, value]: query) {
			append_len_field(key, name.size());
			key.append(name.data(), name.size());
			append_len_field(key, value.size());
			key.append(value.data(), value.size());
		}
	}
	for (auto const &h: vary) {
		key.push_back('|');
		key += h;
		key.push_back('=');
		auto const header = req_headers[h];
		key.append(header.data(), header.size());
	}
	return key;
}

} // namespace response_cache_detail
Router::Middleware response_cache_middleware(
	ResponseCacheOptions opts = {}) {
	auto cache = std::make_shared<RespLruCache>(opts.max_entries, opts.max_bytes);
	auto mtx = std::make_shared<std::mutex>();

	return [opts,
			cache,
			mtx](conflux::http::RequestView const &req, conflux::http::Router::Handler const &next) -> conflux::http::Response {
		bool const is_head = req.method == "HEAD";
		if (req.method != "GET" && !is_head) {
			return next(req);
		}

		std::string_view const path{req.path};

		{
			std::scoped_lock const lk{*mtx};
			auto const *vary = cache->vary_for(path);
			std::span<std::string const> const vary_headers =
				vary ? std::span<std::string const>{*vary} : std::span<std::string const>{};
			auto const lookup_key = response_cache_detail::build_cache_key(path, req.query, vary_headers, req.headers);
			auto const *entry = cache->get(lookup_key);
			if (entry != nullptr) {
				auto resp = entry->resp;
				if (is_head) {
					resp.head_only = true;
				}
				return resp;
			}
		}

		auto resp = next(req);

		if (is_head) {
			return resp;
		}
		if (resp.status != 200) {
			return resp;
		}
		if (!resp.set_cookies.empty()) {
			return resp;
		}
		if (!resp.is_text()) {
			return resp;
		}
		auto cc = std::as_const(resp.headers)["Cache-Control"];
		if (response_cache_detail::cache_control_directive_contains(cc, "no-store")) {
			return resp;
		}
		if (response_cache_detail::cache_control_directive_contains(cc, "no-cache")) {
			return resp; // no-cache requires revalidation which this cache cannot perform
		}
		if (response_cache_detail::cache_control_directive_contains(cc, "private")) {
			return resp;
		}
		auto vary_hdr = std::as_const(resp.headers)["Vary"];
		if (opts.respect_vary && vary_hdr.find('*') != std::string_view::npos) {
			return resp;
		}
		auto vary_list = response_cache_detail::parse_vary(vary_hdr);

		auto ttl = response_cache_detail::parse_max_age(cc);
		if (ttl.count() == 0 && response_cache_detail::cache_control_directive_contains(cc, "max-age")) {
			return resp; // max-age=0: do not cache
		}
		if (ttl.count() == 0) {
			ttl = opts.default_ttl;
		}

		{
			std::scoped_lock const lk{*mtx};
			if (!vary_list.empty()) {
				cache->set_vary(path, vary_list);
			}
			auto store_key = response_cache_detail::build_cache_key(path, req.query, vary_list, req.headers);
			cache->put(store_key, {.resp = resp, .expires = std::chrono::steady_clock::now() + ttl});
		}
		return resp;
	};
}

} // namespace conflux::http
