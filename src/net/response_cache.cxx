// Response caching middleware: in-memory LRU cache with TTL.
// Only caches GET requests with 200 responses and no Set-Cookie headers.
// Cache key is the full request path (including query string).
// TTL is taken from the response Cache-Control max-age if present; otherwise
// falls back to ResponseCacheOptions::default_ttl.
export module conflux.net.response_cache;
import std;
import conflux.utils;
import conflux.net.router;
using namespace std;

export struct ResponseCacheOptions {
	// Maximum number of entries in the LRU cache.
	size_t max_entries{256};
	// Maximum total bytes of cached response bodies (0 = unlimited).
	size_t max_bytes{64ULL * 1024 * 1024};
	// Default TTL when the response has no Cache-Control max-age.
	chrono::seconds default_ttl{60};
	// When true, Vary: * responses are not cached.
	bool respect_vary{true};
};

// Internal cache entry type (not exported; module-scope to avoid GCC TU-local error).
struct RespCacheEntry {
	HttpResponse resp;
	chrono::steady_clock::time_point expires;
};

// LRU cache: module-scope (not exported, not anonymous-namespace).
class RespLruCache {
public:
	explicit RespLruCache(
		size_t max,
		size_t max_bytes)
		: max_(max)
		, max_bytes_(max_bytes) {}

	// Returns pointer to cached entry (nullptr if absent or expired).
	RespCacheEntry const *get(
		string const &key) {
		auto it = map_.find(key);
		if (it == map_.end()) {
			return nullptr;
		}
		if (chrono::steady_clock::now() >= it->second.expires) {
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
		string const &key,
		RespCacheEntry entry) {
		size_t const entry_bytes = entry.resp.text_body().size();
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
			string const &lru = order_.back();
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

	[[nodiscard]] vector<string> const *vary_for(
		string const &path) const {
		auto it = path_vary_.find(path);
		return it == path_vary_.end() ? nullptr : &it->second;
	}

	void set_vary(
		string const &path,
		vector<string> headers) {
		path_vary_[path] = move(headers);
	}

private:
	size_t max_;
	size_t max_bytes_;
	size_t total_bytes_{0};
	list<string> order_;
	unordered_map<string, list<string>::iterator> iters_;
	unordered_map<string, RespCacheEntry> map_;
	unordered_map<string, vector<string>> path_vary_;
};

namespace response_cache_detail {

// Parse max-age from a Cache-Control header value. Returns 0 if not found.
chrono::seconds parse_max_age(
	string_view cc) {
	auto pos = cc.find("max-age=");
	if (pos == string_view::npos) {
		return chrono::seconds{0};
	}
	auto val = cc.substr(pos + 8);
	auto end = val.find_first_of(", \t");
	if (end != string_view::npos) {
		val = val.substr(0, end);
	}
	long v = 0;
	auto [ptr, ec] = from_chars(val.data(), val.data() + val.size(), v);
	if (ec != errc{}) {
		return chrono::seconds{0};
	}
	return chrono::seconds{v};
}

// Parse a Vary header value into a sorted, lowercased, deduped list of header names.
// Returns empty vector for empty input or "*".
vector<string> parse_vary(
	string_view vary) {
	vector<string> out;
	size_t i = 0;
	while (i < vary.size()) {
		while (i < vary.size() && (vary[i] == ' ' || vary[i] == '\t' || vary[i] == ',')) {
			++i;
		}
		size_t const start = i;
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
	ranges::sort(out);
	auto dup = ranges::unique(out);
	out.erase(dup.begin(), dup.end());
	return out;
}

string build_cache_key(
	string_view path,
	HttpFieldsView const &query,
	vector<string> const &vary,
	HttpFieldsView const &req_headers) {
	string key{path};
	if (!query.empty()) {
		key += "|q:";
		for (auto const &[name, value]: query) {
			key += format("{}:", name.size());
			key += name;
			key += format("{}:", value.size());
			key += value;
		}
	}
	if (vary.empty()) {
		return key;
	}
	for (auto const &h: vary) {
		key += '|';
		key += h;
		key += '=';
		key += req_headers[h];
	}
	return key;
}

} // namespace response_cache_detail

export Router::Middleware response_cache_middleware(
	ResponseCacheOptions opts = {}) {
	auto cache = make_shared<RespLruCache>(opts.max_entries, opts.max_bytes);
	auto mtx = make_shared<mutex>();

	return [opts, cache, mtx](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		if (req.method != "GET") {
			return next(req);
		}

		string const path{req.path};

		{
			scoped_lock const lk{*mtx};
			auto const *vary = cache->vary_for(path);
			auto lookup_key = response_cache_detail::build_cache_key(path, req.query, vary ? *vary : vector<string>{}, req.headers);
			auto const *entry = cache->get(lookup_key);
			if (entry != nullptr) {
				return entry->resp;
			}
		}

		auto resp = next(req);

		if (resp.status != 200) {
			return resp;
		}
		if (!resp.set_cookies.empty()) {
			return resp;
		}
		if (!resp.is_text()) {
			return resp;
		}
		auto cc = as_const(resp.headers)["Cache-Control"];
		if (cc.find("no-store") != string_view::npos) {
			return resp;
		}
		if (cc.find("no-cache") != string_view::npos) {
			return resp; // no-cache requires revalidation which this cache cannot perform
		}
		if (cc.find("private") != string_view::npos) {
			return resp;
		}
		auto vary_hdr = as_const(resp.headers)["Vary"];
		if (opts.respect_vary && vary_hdr.find('*') != string_view::npos) {
			return resp;
		}
		auto vary_list = response_cache_detail::parse_vary(vary_hdr);

		auto ttl = response_cache_detail::parse_max_age(cc);
		if (ttl.count() == 0 && cc.find("max-age=") != string_view::npos) {
			return resp; // max-age=0: do not cache
		}
		if (ttl.count() == 0) {
			ttl = opts.default_ttl;
		}

		{
			scoped_lock const lk{*mtx};
			if (!vary_list.empty()) {
				cache->set_vary(path, vary_list);
			}
			auto store_key = response_cache_detail::build_cache_key(path, req.query, vary_list, req.headers);
			cache->put(store_key, {.resp = resp, .expires = chrono::steady_clock::now() + ttl});
		}
		return resp;
	};
}
