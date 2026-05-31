module;
#include <ctime>
#include <sys/stat.h>
export module conflux.net.http.static_core;

import std;
import conflux.types;

export namespace conflux::http::detail {

struct StaticRequest {
	std::string_view file_param;
	std::string_view method;
	std::string_view accept_encoding;
	std::string_view if_none_match;
	std::string_view if_modified_since;
	std::string_view range;
	bool tls{};
};

struct StaticRequestStorage {
	std::string file_param;
	std::string method;
	std::string accept_encoding;
	std::string if_none_match;
	std::string if_modified_since;
	std::string range;
	bool tls{};

	[[nodiscard]] static StaticRequestStorage from(
		StaticRequest const &request) {
		return StaticRequestStorage{
			.file_param = std::string{request.file_param},
			.method = std::string{request.method},
			.accept_encoding = std::string{request.accept_encoding},
			.if_none_match = std::string{request.if_none_match},
			.if_modified_since = std::string{request.if_modified_since},
			.range = std::string{request.range},
			.tls = request.tls,
		};
	}

	[[nodiscard]] StaticRequest view() const noexcept {
		return StaticRequest{
			.file_param = file_param,
			.method = method,
			.accept_encoding = accept_encoding,
			.if_none_match = if_none_match,
			.if_modified_since = if_modified_since,
			.range = range,
			.tls = tls,
		};
	}
};

struct StaticCacheEntry {
	std::string body;
	std::string mime;
	std::string etag;
	std::string last_modified;
	std::string content_encoding;
	off_t size{};
	time_t mtime{};
	dev_t dev{};
	ino_t ino{};
	std::uint64_t tick{};
};

struct StaticCacheKey {
	std::string path;
	std::string content_encoding;
};

struct StaticCacheKeyView {
	std::string_view path;
	std::string_view content_encoding;
};

struct StaticCacheKeyHash {
	using is_transparent = void;
	[[nodiscard]] std::size_t operator ()(
		StaticCacheKey const &key) const noexcept {
		return (*this)(StaticCacheKeyView{key.path, key.content_encoding});
	}
	[[nodiscard]] std::size_t operator ()(
		StaticCacheKeyView key) const noexcept {
		auto const h1 = std::hash<std::string_view>{}(key.path);
		auto const h2 = std::hash<std::string_view>{}(key.content_encoding);
		return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6U) + (h1 >> 2U));
	}
};

struct StaticCacheKeyEqual {
	using is_transparent = void;
	[[nodiscard]] bool operator ()(
		StaticCacheKey const &a,
		StaticCacheKey const &b) const noexcept {
		return a.path == b.path && a.content_encoding == b.content_encoding;
	}
	[[nodiscard]] bool operator ()(
		StaticCacheKey const &a,
		StaticCacheKeyView b) const noexcept {
		return a.path == b.path && a.content_encoding == b.content_encoding;
	}
	[[nodiscard]] bool operator ()(
		StaticCacheKeyView a,
		StaticCacheKey const &b) const noexcept {
		return a.path == b.path && a.content_encoding == b.content_encoding;
	}
};

struct StaticCacheStore {
	std::mutex mtx;
	std::unordered_map<StaticCacheKey, StaticCacheEntry, StaticCacheKeyHash, StaticCacheKeyEqual> entries;
	std::size_t total_bytes{};
	std::uint64_t tick{};
	template<class F>
	[[nodiscard]] auto with_cached(
		std::string_view path,
		std::string_view content_encoding,
		struct ::stat const &st,
		F &&fn) {
		using Result = std::remove_cvref_t<std::invoke_result_t<F, StaticCacheEntry const &>>;
		std::scoped_lock const lk{mtx};
		auto it = entries.find(StaticCacheKeyView{path, content_encoding});
		if (it == entries.end()) {
			return std::optional<Result>{};
		}
		auto &e = it->second;
		if (e.size != st.st_size || e.mtime != st.st_mtime || e.dev != st.st_dev || e.ino != st.st_ino) {
			total_bytes -= e.body.size();
			entries.erase(it);
			return std::optional<Result>{};
		}
		e.tick = ++tick;
		return std::optional<Result>{std::in_place, std::forward<F>(fn)(e)};
	}
	[[nodiscard]] std::optional<StaticCacheEntry> get(
		std::string_view path,
		std::string_view content_encoding,
		struct ::stat const &st) {
		return with_cached(path, content_encoding, st, [](StaticCacheEntry const &entry) { return entry; });
	}
	void put(
		std::string path,
		std::string content_encoding,
		StaticCacheEntry entry,
		std::size_t max_total_bytes) {
		std::scoped_lock const lk{mtx};
		if (entry.body.size() > max_total_bytes) {
			return;
		}
		if (auto it = entries.find(StaticCacheKeyView{path, content_encoding}); it != entries.end()) {
			total_bytes -= it->second.body.size();
			entries.erase(it);
		}
		while (total_bytes + entry.body.size() > max_total_bytes && !entries.empty()) {
			auto victim = std::ranges::min_element(entries, {}, [](auto const &kv) { return kv.second.tick; });
			total_bytes -= victim->second.body.size();
			entries.erase(victim);
		}
		entry.tick = ++tick;
		total_bytes += entry.body.size();
		entries.emplace(StaticCacheKey{std::move(path), std::move(content_encoding)}, std::move(entry));
	}
	void evict(
		std::string_view path,
		std::string_view content_encoding) {
		std::scoped_lock const lk{mtx};
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
	void evict_path_and_sidecars(
		std::string const &path) {
		evict_all_encodings(path);
		evict_all_encodings(path + ".br");
		evict_all_encodings(path + ".gz");
	}
};

[[nodiscard]] std::optional<std::string> normalize_static_path(
	std::string_view raw) {
	std::string result;
	result.reserve(raw.size() + 1);
	std::size_t pos = 0;
	while (pos < raw.size()) {
		auto const next = raw.find('/', pos);
		std::string_view const seg = next == std::string_view::npos ? raw.substr(pos) : raw.substr(pos, next - pos);
		if (seg.find('\0') != std::string_view::npos) {
			return std::nullopt;
		}
		if (seg == "..") {
			if (result.empty()) {
				return std::nullopt;
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

} // namespace conflux::http::detail
