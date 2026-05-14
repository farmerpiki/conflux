module;
#include <ctime>
#include <sys/stat.h>
export module conflux.net.http.static_core;

import std;
import conflux.types;

export struct StaticRequest {
	S file_param;
	S method;
	S accept_encoding;
	S if_none_match;
	S if_modified_since;
	S range;
	bool tls{};
};

export struct StaticCacheEntry {
	S body;
	S mime;
	S etag;
	S last_modified;
	S content_encoding;
	off_t size{};
	time_t mtime{};
	dev_t dev{};
	ino_t ino{};
	u64 tick{};
};
export struct StaticCacheStore {
	mutex mtx;
	UM<S, StaticCacheEntry> entries;
	SZ total_bytes{};
	u64 tick{};
	[[nodiscard]] Opt<StaticCacheEntry> get(
		S const &key,
		struct ::stat const &st) {
		SL const lk{mtx};
		auto it = entries.find(key);
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
		S key,
		StaticCacheEntry entry,
		SZ max_total_bytes) {
		SL const lk{mtx};
		if (entry.body.size() > max_total_bytes) {
			return;
		}
		if (auto it = entries.find(key); it != entries.end()) {
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
		entries.emplace(move(key), move(entry));
	}
	void evict(
		S const &key) {
		SL const lk{mtx};
		if (auto it = entries.find(key); it != entries.end()) {
			total_bytes -= it->second.body.size();
			entries.erase(it);
		}
	}
	void evict_all_encodings(
		S const &path) {
		evict(path + "|");
		evict(path + "|br");
		evict(path + "|gzip");
	}
};

export [[nodiscard]] Opt<std::string> normalize_static_path(
	SV raw) {
	S const fp{raw};
	V<S> parts;
	SZ pos = 0;
	bool bad = false;
	while (pos < fp.size()) {
		auto next = fp.find('/', pos);
		auto seg = (next == S::npos) ? SV{fp}.substr(pos) : SV{fp}.substr(pos, next - pos);
		if (seg.find('\0') != SV::npos) {
			bad = true;
			break;
		}
		if (seg == "..") {
			if (parts.empty()) {
				bad = true;
				break;
			}
			parts.pop_back();
		} else if (!seg.empty() && seg != ".") {
			parts.emplace_back(seg);
		}
		if (next == S::npos) {
			break;
		}
		pos = next + 1;
	}
	if (bad) {
		return nullopt;
	}
	S result;
	for (auto const &part: parts) {
		result += '/';
		result += part;
	}
	return result;
}
