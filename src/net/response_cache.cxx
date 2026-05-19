// Response caching middleware: in-memory LRU cache with TTL.
// Only caches GET requests with 200 responses and no Set-Cookie headers.
// Cache key is the full request path (including query string).
// TTL is taken from the response Cache-Control max-age if present; otherwise
// falls back to ResponseCacheOptions::default_ttl.
module;
#include <memory>

export module conflux.net.response_cache;
import std;
import conflux.types;
import conflux.utils;
import conflux.net.http.types;
import conflux.net.router;
export struct ResponseCacheOptions {
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
	HttpResponse resp;
	std::chrono::steady_clock::time_point expires;
};

struct TransparentStringHash {
	using is_transparent = void;
	[[nodiscard]] std::size_t operator ()(
		std::string_view value) const noexcept {
		return hash<std::string_view>{}(value);
	}
	[[nodiscard]] std::size_t operator ()(
		std::string const &value) const noexcept {
		return operator ()(std::string_view{value});
	}
};

struct TransparentStringEqual {
	using is_transparent = void;
	[[nodiscard]] bool operator ()(
		std::string_view lhs,
		std::string_view rhs) const noexcept {
		return lhs == rhs;
	}
	[[nodiscard]] bool operator ()(
		std::string const &lhs,
		std::string_view rhs) const noexcept {
		return std::string_view{lhs} == rhs;
	}
	[[nodiscard]] bool operator ()(
		std::string_view lhs,
		std::string const &rhs) const noexcept {
		return lhs == std::string_view{rhs};
	}
	[[nodiscard]] bool operator ()(
		std::string const &lhs,
		std::string const &rhs) const noexcept {
		return lhs == rhs;
	}
};
// LRU cache: module-scope (not exported, not anonymous-namespace).
class RespLruCache {
public:
	explicit RespLruCache(
		std::size_t max,
		std::size_t max_bytes)
		: max_(max)
		, max_bytes_(max_bytes) {}
	// Returns pointer to cached entry (nullptr if absent or expired).
	RespCacheEntry const *get(
		std::string const &key) {
		auto it = map_.find(key);
		if (it == map_.end()) {
			return nullptr;
		}
		if (std::chrono::steady_clock::now() >= it->second.expires) {
			total_bytes_ -= it->second.resp.text_body().size();
			order_.erase(iters_.at(key));
			iters_.erase(key);
			map_.erase(it);
			return nullptr;
		}
		// Move to front (MRU) in O(1).
		order_.erase(iters_.at(key));
		order_.push_front(key);
		iters_[key] = order_.begin();
		return &it->second;
	}
	void put(
		std::string const &key,
		RespCacheEntry entry) {
		std::size_t const entry_bytes = entry.resp.text_body().size();
		if (max_bytes_ > 0 && entry_bytes > max_bytes_) {
			return;
		}
		auto it = map_.find(key);
		if (it != map_.end()) {
			total_bytes_ -= it->second.resp.text_body().size();
			map_.erase(it);
			order_.erase(iters_.at(key));
			iters_.erase(key);
			// Fall through to eviction+insertion path below.
		}
		while ((max_bytes_ > 0 && total_bytes_ + entry_bytes > max_bytes_) || map_.size() >= max_) {
			if (order_.empty()) {
				return;
			}
			auto const &lru = order_.back();
			total_bytes_ -= map_.at(lru).resp.text_body().size();
			map_.erase(lru);
			iters_.erase(lru);
			order_.pop_back();
		}
		total_bytes_ += entry_bytes;
		order_.push_front(key);
		iters_.emplace(key, order_.begin());
		map_.emplace(key, move(entry));
	}
	[[nodiscard]] std::vector<std::string> const *vary_for(
		std::string_view path) const {
		auto it = path_vary_.find(path);
		return it == path_vary_.end() ? nullptr : &it->second;
	}
	void set_vary(
		std::string_view path,
		std::vector<std::string> headers) {
		path_vary_[std::string{path}] = move(headers);
	}

private:
	std::size_t max_;
	std::size_t max_bytes_;
	std::size_t total_bytes_{0};
	std::list<std::string> order_;
	std::unordered_map<std::string, std::list<std::string>::iterator> iters_;
	std::unordered_map<std::string, RespCacheEntry> map_;
	std::unordered_map<std::string, std::vector<std::string>, TransparentStringHash, TransparentStringEqual> path_vary_;
};
namespace response_cache_detail {

bool cache_control_directive_contains(
	std::string_view cc,
	std::string_view directive) {
	while (!cc.empty()) {
		auto comma = cc.find(',');
		auto part = trim((comma == std::string_view::npos) ? cc : cc.substr(0, comma));
		auto eq = part.find('=');
		auto name = trim((eq == std::string_view::npos) ? part : part.substr(0, eq));
		if (conflux::http::ascii_iequals(name, directive)) {
			return true;
		}
		if (comma == std::string_view::npos) {
			return false;
		}
		cc.remove_prefix(comma + 1);
	}
	return false;
}
// Parse max-age from a Cache-Control header value. Returns 0 if not found.
std::chrono::seconds parse_max_age(
	std::string_view cc) {
	while (!cc.empty()) {
		auto comma = cc.find(',');
		auto part = trim((comma == std::string_view::npos) ? cc : cc.substr(0, comma));
		auto eq = part.find('=');
		if (eq != std::string_view::npos) {
			auto name = trim(part.substr(0, eq));
			if (conflux::http::ascii_iequals(name, "max-age")) {
				auto val = trim(part.substr(eq + 1));
				long v = 0;
				auto [ptr, ec] = from_chars(val.data(), val.data() + val.size(), v);
				if (ec != errc{} || ptr != val.data() + val.size()) {
					return std::chrono::seconds{0};
				}
				return std::chrono::seconds{v};
			}
		}
		if (comma == std::string_view::npos) {
			return std::chrono::seconds{0};
		}
		cc.remove_prefix(comma + 1);
	}
	return std::chrono::seconds{0};
}
// Parse a Vary header value into a sorted, lowercased, deduped list of header names.
// Returns empty vector for empty input or "*".
std::vector<std::string> parse_vary(
	std::string_view vary) {
	std::vector<std::string> out;
	std::size_t i = 0;
	while (i < vary.size()) {
		while (i < vary.size() && (vary[i] == ' ' || vary[i] == '\t' || vary[i] == ',')) {
			++i;
		}
		std::size_t const start = i;
		while (i < vary.size() && vary[i] != ',') {
			++i;
		}
		auto token = vary.substr(start, i - start);
		while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
			token.remove_suffix(1);
		}
		if (token.empty() || token == "*") {
			continue;
		}
		out.push_back(ascii_lower(token));
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
	auto const [ptr, ec] = to_chars(buf.data(), buf.data() + buf.size(), value);
	(void)ec;
	out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
	out.push_back(':');
}
std::string build_cache_key(
	std::string_view path,
	HttpFieldsView const &query,
	std::span<std::string const> vary,
	HttpFieldsView const &req_headers) {
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
export Router::Middleware response_cache_middleware(
	ResponseCacheOptions opts = {}) {
	auto cache = make_shared<RespLruCache>(opts.max_entries, opts.max_bytes);
	auto mtx = make_shared<mutex>();

	return [opts, cache, mtx](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
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
