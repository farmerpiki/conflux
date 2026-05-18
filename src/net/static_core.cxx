module;
#include <ctime>
#include <sys/stat.h>
export module conflux.net.http.static_core;

import std;
import conflux.types;

export struct StaticRequest {
	std::string file_param;
	std::string method;
	std::string accept_encoding;
	std::string if_none_match;
	std::string if_modified_since;
	std::string range;
	bool tls{};
};

export struct StaticCacheEntry {
	std::string body;
	std::string mime;
	std::string etag;
	std::string last_modified;
	std::string content_encoding;
	off_t size{};
	time_t mtime{};
	dev_t dev{};
	ino_t ino{};
	u64 tick{};
};

export struct StaticCacheKey {
	std::string path;
	std::string content_encoding;
};

export struct StaticCacheKeyView {
	std::string_view path;
	std::string_view content_encoding;
};

export struct StaticCacheKeyHash {
	using is_transparent = void;
	[[nodiscard]] std::size_t operator()(
		StaticCacheKey const &key) const noexcept {
		return (*this)(StaticCacheKeyView{key.path, key.content_encoding});
	}
	[[nodiscard]] std::size_t operator()(
		StaticCacheKeyView key) const noexcept {
		auto const h1 = hash<std::string_view>{}(key.path);
		auto const h2 = hash<std::string_view>{}(key.content_encoding);
		return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6U) + (h1 >> 2U));
	}
};

export struct StaticCacheKeyEqual {
	using is_transparent = void;
	[[nodiscard]] bool operator()(
		StaticCacheKey const &a,
		StaticCacheKey const &b) const noexcept {
		return a.path == b.path && a.content_encoding == b.content_encoding;
	}
	[[nodiscard]] bool operator()(
		StaticCacheKey const &a,
		StaticCacheKeyView b) const noexcept {
		return a.path == b.path && a.content_encoding == b.content_encoding;
	}
	[[nodiscard]] bool operator()(
		StaticCacheKeyView a,
		StaticCacheKey const &b) const noexcept {
		return a.path == b.path && a.content_encoding == b.content_encoding;
	}
};

export struct StaticCacheStore {
	mutex mtx;
	std::unordered_map<StaticCacheKey, StaticCacheEntry, StaticCacheKeyHash, StaticCacheKeyEqual> entries;
	std::size_t total_bytes{};
	u64 tick{};
	[[nodiscard]] std::optional<StaticCacheEntry> get(
		std::string_view path,
		std::string_view content_encoding,
		struct ::stat const &st) {
		SL const lk{mtx};
		auto it = entries.find(StaticCacheKeyView{path, content_encoding});
		if (it == entries.end()) {
			return nullopt;
		}
		auto &e = it->second;
		if (e.size != st.st_size || e.mtime != st.st_mtime || e.dev != st.st_dev || e.ino != st.st_ino) {
			total_bytes -= e.body.size();
			entries.erase(it);
			return nullopt;
		}
		e.tick = ++tick;
		return e;
	}
	void put(
		std::string path,
		std::string content_encoding,
		StaticCacheEntry entry,
		std::size_t max_total_bytes) {
		SL const lk{mtx};
		if (entry.body.size() > max_total_bytes) {
			return;
		}
		if (auto it = entries.find(StaticCacheKeyView{path, content_encoding}); it != entries.end()) {
			total_bytes -= it->second.body.size();
			entries.erase(it);
		}
		while (total_bytes + entry.body.size() > max_total_bytes && !entries.empty()) {
			auto victim = ranges::min_element(entries, {}, [](auto const &kv) { return kv.second.tick; });
			total_bytes -= victim->second.body.size();
			entries.erase(victim);
		}
		entry.tick = ++tick;
		total_bytes += entry.body.size();
		entries.emplace(StaticCacheKey{move(path), move(content_encoding)}, move(entry));
	}
	void evict(
		std::string_view path,
		std::string_view content_encoding) {
		SL const lk{mtx};
		if (auto it = entries.find(StaticCacheKeyView{path, content_encoding}); it != entries.end()) {
			total_bytes -= it->second.body.size();
			entries.erase(it);
		}
	}
	void evict_all_encodings(
		std::string const &path) {
		evict(path, {});
		evict(path, "br");
		evict(path, "gzip");
	}
};

export [[nodiscard]] std::optional<std::string> normalize_static_path(
	std::string_view raw) {
	std::string result;
	result.reserve(raw.size() + 1);
	std::size_t pos = 0;
	while (pos < raw.size()) {
		auto const next = raw.find('/', pos);
		std::string_view const seg = next == std::string_view::npos ? raw.substr(pos) : raw.substr(pos, next - pos);
		if (seg.find('\0') != std::string_view::npos) {
			return nullopt;
		}
		if (seg == "..") {
			if (result.empty()) {
				return nullopt;
			}
			auto const slash = result.rfind('/');
			if (slash == 0) {
				result.clear();
			} else {
				result.erase(slash);
			}
		} else if (!seg.empty() && seg != ".") {
			result.push_back('/');
			result += seg;
		}
		if (next == std::string_view::npos) {
			break;
		}
		pos = next + 1;
	}
	return result;
}
