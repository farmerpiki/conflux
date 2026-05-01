module;
#include <cerrno>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#if CONFLUX_HAS_TLS
	#include <openssl/ssl.h>
#endif

export module conflux.net.router;

#ifdef CONFLUX_BUILD_FUZZ
	#define CONFLUX_FUZZ_EXPORT export
#else
	#define CONFLUX_FUZZ_EXPORT
#endif

import std;
import conflux.types;
import conflux.crypto;
import conflux.work;
import conflux.file_io;
import conflux.utils;
import conflux.net.config;

[[nodiscard]] S html_escape(
	SV s) {
	S out;
	out.reserve(s.size());
	for (char const c: s) {
		switch (c) {
		case '&' : out += "&amp;"; break;
		case '<' : out += "&lt;"; break;
		case '>' : out += "&gt;"; break;
		case '"' : out += "&quot;"; break;
		case '\'': out += "&#39;"; break;
		default  : out += c; break;
		}
	}
	return out;
}

struct StaticCacheEntry {
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

struct StaticCacheStore {
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
			return std::nullopt;
		}
		auto &e = it->second;
		if (e.size != st.st_size || e.mtime != st.st_mtime || e.dev != st.st_dev || e.ino != st.st_ino) {
			total_bytes -= e.body.size();
			entries.erase(it);
			return std::nullopt;
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

export struct FieldHash {
	using is_transparent = void;
	bool ci{false};

	[[nodiscard]] SZ operator ()(
		SV s) const noexcept {
		SZ h = 14695981039346656037ULL;
		for (char const ch: s) {
			auto const c = static_cast<unsigned char>(ch);
			unsigned char const k = ci ? static_cast<unsigned char>(c | 0x20) : c;
			h ^= k;
			h *= 1099511628211ULL;
		}
		return h;
	}
	[[nodiscard]] SZ operator ()(
		S const &s) const noexcept {
		return operator ()(SV{s});
	}
};

export struct FieldEq {
	using is_transparent = void;
	bool ci{false};

	[[nodiscard]] bool operator ()(
		SV a,
		SV b) const noexcept {
		if (a.size() != b.size()) {
			return false;
		}
		if (!ci) {
			return a == b;
		}
		return ranges::equal(a, b, [](unsigned char x, unsigned char y) { return (x | 0x20) == (y | 0x20); });
	}
	[[nodiscard]] bool operator ()(
		S const &a,
		SV b) const noexcept {
		return operator ()(SV{a}, b);
	}
	[[nodiscard]] bool operator ()(
		SV a,
		S const &b) const noexcept {
		return operator ()(a, SV{b});
	}
	[[nodiscard]] bool operator ()(
		S const &a,
		S const &b) const noexcept {
		return operator ()(SV{a}, SV{b});
	}
};

// Vector-backed S map: operator[] returns SV (empty if absent).
// Preferred over UM for small/bounded collections — contiguous storage,
// no hashing, linear scan faster for N < ~100.
export class HttpFields {
	V<P<S, S>> data_;
	std::unordered_multimap<S, SZ, FieldHash, FieldEq> index_;
	bool case_insensitive_{false};

	void index_entry(
		SZ i) {
		index_.emplace(data_[i].first, i);
	}

public:
	HttpFields(
		bool case_insensitive = false)
		: index_(0, FieldHash{case_insensitive}, FieldEq{case_insensitive})
		, case_insensitive_(case_insensitive) {}
	HttpFields(
		std::initializer_list<P<S, S>> init)
		: data_(init) {
		for (SZ i = 0; i < data_.size(); ++i) {
			index_entry(i);
		}
	}

	// Read: returns empty view when key absent.
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	SV operator [](
		SV key) const noexcept {
		return get(key).value_or(SV{});
	}

	// Write: inserts key with empty value when absent, then returns ref.
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	S &operator [](
		SV key) {
		auto range = index_.equal_range(key);
		if (range.first != range.second) {
			return data_[range.first->second].second;
		}
		data_.emplace_back(S{key}, S{});
		index_entry(data_.size() - 1);
		return data_.back().second;
	}

	[[nodiscard]] Opt<SV> get(
		SV key) const noexcept {
		auto range = index_.equal_range(key);
		if (range.first == range.second) {
			return std::nullopt;
		}
		return SV{data_[range.first->second].second};
	}

	[[nodiscard]] V<SV> values(
		SV key) const {
		V<SV> out;
		auto range = index_.equal_range(key);
		for (auto it = range.first; it != range.second; ++it) {
			out.push_back(data_[it->second].second);
		}
		return out;
	}

	[[nodiscard]] bool contains(
		SV key) const noexcept {
		auto range = index_.equal_range(key);
		return range.first != range.second;
	}

	[[nodiscard]] SV value_or(
		SV key,
		SV def = {}) const noexcept {
		return get(key).value_or(def);
	}

	void emplace_back(
		S k,
		S v) {
		data_.emplace_back(move(k), move(v));
		index_entry(data_.size() - 1);
	}

	void append(
		S k,
		S v) {
		emplace_back(move(k), move(v));
	}

	void set(
		S key,
		S field_value) {
		auto range = index_.equal_range(SV{key});
		if (range.first == range.second) {
			emplace_back(move(key), move(field_value));
			return;
		}
		auto idx_view =
			ranges::subrange(range.first, range.second) | std::views::transform([](auto const &e) { return e.second; });
		auto const keep_idx = ranges::min(idx_view);
		data_[keep_idx].second = move(field_value);
		US<SZ> to_remove;
		ranges::copy_if(idx_view, inserter(to_remove, to_remove.end()), [&](SZ i) { return i != keep_idx; });
		if (to_remove.empty()) {
			return;
		}
		SZ cursor = 0;
		erase_if(data_, [&](auto const &) { return to_remove.contains(cursor++); });
		index_.clear();
		for (SZ i = 0; i < data_.size(); ++i) {
			index_entry(i);
		}
	}

	SZ erase(
		SV key) {
		auto range = index_.equal_range(key);
		if (range.first == range.second) {
			return 0;
		}
		US<SZ> to_remove;
		ranges::copy(
			ranges::subrange(range.first, range.second) | std::views::transform([](auto const &e) { return e.second; }),
			inserter(to_remove, to_remove.end()));
		SZ cursor = 0;
		SZ const removed = erase_if(data_, [&](auto const &) { return to_remove.contains(cursor++); });
		index_.clear();
		for (SZ i = 0; i < data_.size(); ++i) {
			index_entry(i);
		}
		return removed;
	}
	void clear() noexcept {
		data_.clear();
		index_.clear();
	}
	[[nodiscard]] bool empty() const noexcept { return data_.empty(); }
	[[nodiscard]] SZ size() const noexcept { return data_.size(); }
	[[nodiscard]] bool case_insensitive() const noexcept { return case_insensitive_; }

	auto begin() { return data_.begin(); }
	auto end() { return data_.end(); }
	[[nodiscard]] auto begin() const { return data_.begin(); }
	[[nodiscard]] auto end() const { return data_.end(); }
};

export class HttpFieldsView {
	V<P<SV, SV>> data_;
	std::unordered_multimap<SV, SZ, FieldHash, FieldEq> index_;
	SP<deque<S>> owned_storage_{};
	bool case_insensitive_{false};

	void index_entry(
		SZ i) {
		index_.emplace(data_[i].first, i);
	}

	[[nodiscard]] SV store_owned(
		S owned_value) {
		if (!owned_storage_) {
			owned_storage_ = make_shared<deque<S>>();
		}
		owned_storage_->push_back(move(owned_value));
		return owned_storage_->back();
	}

public:
	HttpFieldsView(
		bool case_insensitive = false)
		: index_(0, FieldHash{case_insensitive}, FieldEq{case_insensitive})
		, case_insensitive_(case_insensitive) {}

	HttpFieldsView(
		HttpFields const &fields)
		: index_(0, FieldHash{fields.case_insensitive()}, FieldEq{fields.case_insensitive()})
		, case_insensitive_(fields.case_insensitive()) {
		for (auto const &[k, v]: fields) {
			emplace_back(k, v);
		}
	}

	[[nodiscard]] bool case_insensitive() const noexcept { return case_insensitive_; }

	SV operator [](
		SV key) const noexcept {
		return get(key).value_or(SV{});
	}

	[[nodiscard]] Opt<SV> get(
		SV key) const noexcept {
		auto range = index_.equal_range(key);
		if (range.first == range.second) {
			return std::nullopt;
		}
		return data_[range.first->second].second;
	}

	[[nodiscard]] V<SV> values(
		SV key) const {
		V<SV> out;
		auto range = index_.equal_range(key);
		for (auto it = range.first; it != range.second; ++it) {
			out.push_back(data_[it->second].second);
		}
		return out;
	}

	[[nodiscard]] bool contains(
		SV key) const noexcept {
		auto range = index_.equal_range(key);
		return range.first != range.second;
	}

	[[nodiscard]] SV value_or(
		SV key,
		SV def = {}) const noexcept {
		return get(key).value_or(def);
	}

	void emplace_back(
		SV k,
		SV v) {
		data_.emplace_back(k, v);
		index_entry(data_.size() - 1);
	}

	void emplace_back_owned(
		S k,
		S v) {
		data_.emplace_back(store_owned(move(k)), store_owned(move(v)));
		index_entry(data_.size() - 1);
	}

	void clear() noexcept {
		data_.clear();
		index_.clear();
		owned_storage_.reset();
	}

	[[nodiscard]] bool empty() const noexcept { return data_.empty(); }
	[[nodiscard]] SZ size() const noexcept { return data_.size(); }

	[[nodiscard]] HttpFields to_owned() const {
		HttpFields out{case_insensitive_};
		for (auto const &[k, v]: data_) {
			out.emplace_back(S{k}, S{v});
		}
		return out;
	}

	auto begin() { return data_.begin(); }
	auto end() { return data_.end(); }
	[[nodiscard]] auto begin() const { return data_.begin(); }
	[[nodiscard]] auto end() const { return data_.end(); }
};

export struct UploadedFile {
	SV name;
	SV filename;
	SV content_type;
	SV data;
	S owned_name;
	S owned_filename;
	S owned_content_type;
	S owned_data;
	bool owns_metadata = false;
	bool owns_data = false;

	UploadedFile() = default;

	UploadedFile(
		SV name_,
		SV filename_,
		SV content_type_,
		SV data_)
		: name(name_)
		, filename(filename_)
		, content_type(content_type_)
		, data(data_) {}

	UploadedFile(
		UploadedFile const &other)
		: owned_name(other.owns_metadata ? other.owned_name : S{})
		, owned_filename(other.owns_metadata ? other.owned_filename : S{})
		, owned_content_type(other.owns_metadata ? other.owned_content_type : S{})
		, owned_data(other.owns_data ? other.owned_data : S{})
		, owns_metadata(other.owns_metadata)
		, owns_data(other.owns_data) {
		name = owns_metadata ? SV{owned_name} : other.name;
		filename = owns_metadata ? SV{owned_filename} : other.filename;
		content_type = owns_metadata ? SV{owned_content_type} : other.content_type;
		data = owns_data ? SV{owned_data} : other.data;
	}

	UploadedFile(
		UploadedFile &&other) noexcept
		: name(other.name)
		, filename(other.filename)
		, content_type(other.content_type)
		, data(other.data)
		, owned_name(move(other.owned_name))
		, owned_filename(move(other.owned_filename))
		, owned_content_type(move(other.owned_content_type))
		, owned_data(move(other.owned_data))
		, owns_metadata(other.owns_metadata)
		, owns_data(other.owns_data) {
		if (owns_metadata) {
			name = owned_name;
			filename = owned_filename;
			content_type = owned_content_type;
		}
		if (owns_data) {
			data = owned_data;
		}
	}

	UploadedFile &operator =(
		UploadedFile const &other) {
		if (this == &other) {
			return *this;
		}
		owned_name = other.owns_metadata ? other.owned_name : S{};
		owned_filename = other.owns_metadata ? other.owned_filename : S{};
		owned_content_type = other.owns_metadata ? other.owned_content_type : S{};
		owned_data = other.owns_data ? other.owned_data : S{};
		owns_metadata = other.owns_metadata;
		owns_data = other.owns_data;
		name = owns_metadata ? SV{owned_name} : other.name;
		filename = owns_metadata ? SV{owned_filename} : other.filename;
		content_type = owns_metadata ? SV{owned_content_type} : other.content_type;
		data = owns_data ? SV{owned_data} : other.data;
		return *this;
	}

	UploadedFile &operator =(
		UploadedFile &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		name = other.name;
		filename = other.filename;
		content_type = other.content_type;
		data = other.data;
		owned_name = move(other.owned_name);
		owned_filename = move(other.owned_filename);
		owned_content_type = move(other.owned_content_type);
		owned_data = move(other.owned_data);
		owns_metadata = other.owns_metadata;
		owns_data = other.owns_data;
		if (owns_metadata) {
			name = owned_name;
			filename = owned_filename;
			content_type = owned_content_type;
		}
		if (owns_data) {
			data = owned_data;
		}
		return *this;
	}

	[[nodiscard]] static UploadedFile borrowed(
		SV name_,
		SV filename_,
		SV content_type_,
		SV data_) {
		return UploadedFile{name_, filename_, content_type_, data_};
	}

	[[nodiscard]] static UploadedFile owned(
		S name_,
		S filename_,
		S content_type_,
		S data_) {
		UploadedFile file{SV{}, SV{}, SV{}, SV{}};
		file.owned_name = move(name_);
		file.owned_filename = move(filename_);
		file.owned_content_type = move(content_type_);
		file.owned_data = move(data_);
		file.name = file.owned_name;
		file.filename = file.owned_filename;
		file.content_type = file.owned_content_type;
		file.data = file.owned_data;
		file.owns_metadata = true;
		file.owns_data = true;
		return file;
	}

	[[nodiscard]] UploadedFile to_owned() const {
		if (owns_metadata && owns_data) {
			return *this;
		}
		return UploadedFile::owned(S{name}, S{filename}, S{content_type}, S{data});
	}
};

export struct HttpRequest {
	S method;
	S path; // path only, no query S
	S version;
	S remote_addr; // peer IP address (best-effort with multishot accept)
	bool is_tls = false; // true when request arrived over a TLS connection
	HttpFields params; // {name} captures
	HttpFields headers{true}; // case-insensitive lookup
	HttpFields query; // parsed from URL ?k=v&...
	HttpFields form; // parsed from application/x-www-form-urlencoded body or multipart text fields
	HttpFields cookies; // parsed from Cookie: header
	V<UploadedFile> files; // parsed from multipart/form-data body
	S body;

	[[nodiscard]] HttpRequest to_owned() const { return *this; }
};

export struct HttpRequestView {
	SV method;
	SV path;
	SV version;
	SV remote_addr;
	bool is_tls = false;
	HttpFieldsView params;
	HttpFieldsView headers;
	HttpFieldsView query;
	HttpFieldsView form;
	HttpFieldsView cookies;
	span<UploadedFile const> files;
	SV body;

	HttpRequestView(
		SV method_,
		SV path_,
		SV version_,
		SV remote_addr_,
		bool is_tls_,
		HttpFieldsView params_,
		HttpFieldsView headers_,
		HttpFieldsView query_,
		HttpFieldsView form_,
		HttpFieldsView cookies_,
		span<UploadedFile const> files_,
		SV body_)
		: method(method_)
		, path(path_)
		, version(version_)
		, remote_addr(remote_addr_)
		, is_tls(is_tls_)
		, params(move(params_))
		, headers(move(headers_))
		, query(move(query_))
		, form(move(form_))
		, cookies(move(cookies_))
		, files(files_)
		, body(body_) {}

	HttpRequestView(
		HttpRequest const &req)
		: method(req.method)
		, path(req.path)
		, version(req.version)
		, remote_addr(req.remote_addr)
		, is_tls(req.is_tls)
		, params(req.params)
		, headers(req.headers)
		, query(req.query)
		, form(req.form)
		, cookies(req.cookies)
		, files(req.files)
		, body(req.body) {}

	[[nodiscard]] HttpRequest to_owned() const {
		HttpRequest owned;
		owned.method = S{method};
		owned.path = S{path};
		owned.version = S{version};
		owned.remote_addr = S{remote_addr};
		owned.is_tls = is_tls;
		owned.params = params.to_owned();
		owned.headers = headers.to_owned();
		owned.query = query.to_owned();
		owned.form = form.to_owned();
		owned.cookies = cookies.to_owned();
		owned.files.reserve(files.size());
		for (auto const &file: files) {
			owned.files.push_back(file.to_owned());
		}
		owned.body = S{body};
		return owned;
	}
};

template<typename>
class CloneableFunction;

template<typename R, typename... Args>
class CloneableFunction<R(Args...)> {
	struct Concept {
		virtual ~Concept() = default;
		virtual R invoke(Args... args) = 0;
		[[nodiscard]] virtual UP<Concept> clone() const = 0;
	};

	template<typename F>
	struct Model final : Concept {
		F fn;

		explicit Model(
			F f)
			: fn(move(f)) {}

		R invoke(
			Args... args) override {
			return std::invoke(fn, forward<Args>(args)...);
		}

		[[nodiscard]] UP<Concept> clone() const override { return make_unique<Model>(fn); }
	};

	UP<Concept> fn_{};

public:
	CloneableFunction() = default;
	CloneableFunction(
		std::nullptr_t) {}

	template<typename F>
		requires(!std::same_as<std::remove_cvref_t<F>, CloneableFunction>)
	CloneableFunction(
		F &&f)
		: fn_(make_unique<Model<std::remove_cvref_t<F>>>(forward<F>(f))) {}

	CloneableFunction(
		CloneableFunction const &o)
		: fn_(o.fn_ ? o.fn_->clone() : nullptr) {}
	CloneableFunction &operator =(
		CloneableFunction const &o) {
		if (this != &o) {
			fn_ = o.fn_ ? o.fn_->clone() : nullptr;
		}
		return *this;
	}
	CloneableFunction(CloneableFunction &&) noexcept = default;
	CloneableFunction &operator =(CloneableFunction &&) noexcept = default;

	[[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(fn_); }

	R operator ()(
		Args... args) const {
		return fn_->invoke(forward<Args>(args)...);
	}
};

export struct WsUpgrade; // defined after HttpResponse, before Router
export class DeferredResponse; // defined after HttpResponse

// RAII owner of an mmap'd file region.  munmap() called on destruction.
export struct MappedFile {
	void *ptr{};
	SZ size{}; // mmap extent for munmap
	SZ send_offset{}; // byte offset within ptr for send start
	SZ send_size{}; // bytes to send
	MappedFile(
		void *p,
		SZ s)
		: ptr{p}
		, size{s}
		, send_size{s} {}
	MappedFile(
		void *p,
		SZ mmap_sz,
		SZ offset,
		SZ len)
		: ptr{p}
		, size{mmap_sz}
		, send_offset{offset}
		, send_size{len} {}
	~MappedFile() {
		if (ptr != nullptr) {
			::munmap(ptr, size);
		}
	}
	MappedFile(MappedFile const &) = delete;
	MappedFile &operator =(MappedFile const &) = delete;
	MappedFile(
		MappedFile &&o) noexcept
		: ptr{exchange(o.ptr, nullptr)}
		, size{o.size}
		, send_offset{o.send_offset}
		, send_size{o.send_size} {}
	MappedFile &operator =(
		MappedFile &&o) noexcept {
		if (this != &o) {
			if (ptr != nullptr) {
				::munmap(ptr, size);
			}
			ptr = exchange(o.ptr, nullptr);
			size = o.size;
			send_offset = o.send_offset;
			send_size = o.send_size;
		}
		return *this;
	}
};

// Carrier for a file about to be streamed through io_uring (splice on plain,
// fixed-buffer read on TLS). Owns a FileHandle — send dispatch consumes it and
// issues a close_async on the owning ring when the stream finishes.
export struct StreamedFile {
	SP<FileHandle> handle;
	u64 send_offset{};
	u64 send_size{};
	// total file size — needed for Content-Range and range-validation paths.
	u64 total_size{};
};

export enum class SseOverflowPolicy : u8 {
	DropNewest,
	DropOldest,
	Disconnect,
};

export class SseChannel {
private:
	int efd_{};
	mutex mtx_{};
	std::queue<S> pending_{};
	atomic_flag closed_{};
	SZ queued_bytes_{0};
	SZ max_queue_bytes_{};
	SseOverflowPolicy overflow_{SseOverflowPolicy::DropNewest};
	Atom<SZ> dropped_{0};

public:
	static constexpr SZ kDefaultMaxQueueBytes = SZ{4} * 1024 * 1024;

	SseChannel(SseChannel const &) = delete;
	SseChannel &operator =(SseChannel const &) = delete;
	SseChannel(SseChannel &&) = delete;
	SseChannel &operator =(SseChannel &&) = delete;
	explicit SseChannel(
		SZ max_queue_bytes = kDefaultMaxQueueBytes,
		SseOverflowPolicy overflow = SseOverflowPolicy::DropNewest)
		: efd_(::eventfd(0, EFD_CLOEXEC))
		, max_queue_bytes_(max_queue_bytes)
		, overflow_(overflow) {
		if (efd_ < 0) {
			throw SE{errno, system_category(), "eventfd"};
		}
	}

	~SseChannel() noexcept {
		try {
			close();
		} catch (...) {} // NOLINT(bugprone-empty-catch): dtor must not propagate
		::close(efd_);
	}

	// Returns true if the frame was enqueued, false if dropped (overflow).
	// Also returns false if the channel is closed, regardless of policy.
	// Channel takes ownership of frame.
	[[nodiscard]] bool send(
		S frame) {
		bool enqueued = false;
		bool wake = false;
		{
			SL const lk{mtx_};
			if (closed_.test()) {
				return false;
			}
			SZ const frame_bytes = frame.size();
			SZ const would_be = queued_bytes_ + frame_bytes;
			if (would_be > max_queue_bytes_ && max_queue_bytes_ != 0) {
				switch (overflow_) {
				case SseOverflowPolicy::DropNewest: dropped_.fetch_add(1, memory_order_relaxed); return false;
				case SseOverflowPolicy::DropOldest:
					while (!pending_.empty() && queued_bytes_ + frame_bytes > max_queue_bytes_) {
						queued_bytes_ -= pending_.front().size();
						pending_.pop();
						dropped_.fetch_add(1, memory_order_relaxed);
					}
					break;
				case SseOverflowPolicy::Disconnect:
					closed_.test_and_set();
					dropped_.fetch_add(1, memory_order_relaxed);
					wake = true;
					break;
				}
			}
			if (!closed_.test()) {
				queued_bytes_ += frame_bytes;
				pending_.push(move(frame));
				enqueued = true;
				wake = true;
			}
		}
		if (wake) {
			u64 v = 1;
			if (::write(efd_, &v, sizeof(v)) < 0 && errno != EAGAIN) {
				println(cerr, "SseChannel::send: eventfd write: {}", strerror(errno));
			}
		}
		return enqueued;
	}

	// Zero-copy intent: caller owns the backing buffer and is responsible for
	// keeping it alive until the frame is flushed to the socket. Currently
	// copies into the queue; when the queue migrates to SV storage this
	// contract becomes a hard lifetime requirement.
	[[nodiscard]] bool send_view(
		SV frame) {
		return send(S{frame});
	}

	[[nodiscard]] bool send_event(
		SV type,
		SV data) {
		// Reject newlines in type and data: they would break SSE framing and
		// allow injection of arbitrary events.
		auto has_nl = [](SV s) { return s.find('\n') != SV::npos || s.find('\r') != SV::npos; };
		if (has_nl(type) || has_nl(data)) {
			throw std::invalid_argument{"SseChannel::send_event: type and data must not contain newlines"};
		}
		return send(format("event: {}\ndata: {}\n\n", type, data));
	}

	void close() {
		if (closed_.test_and_set()) {
			return;
		} // already closed
		u64 v = 1;
		if (::write(efd_, &v, sizeof(v)) < 0 && errno != EAGAIN) {
			println(cerr, "SseChannel::close: eventfd write: {}", strerror(errno));
		} // wake the io_uring poll
	}

	[[nodiscard]] S drain() {
		SL const lk{mtx_};
		S result;
		while (!pending_.empty()) {
			result += pending_.front();
			pending_.pop();
		}
		queued_bytes_ = 0;
		return result;
	}

	[[nodiscard]] bool is_closed() const noexcept { return closed_.test(); }
	[[nodiscard]] int eventfd_fd() const noexcept { return efd_; }
	[[nodiscard]] SZ dropped_count() const noexcept { return dropped_.load(memory_order_relaxed); }
	[[nodiscard]] SZ max_queue_bytes() const noexcept { return max_queue_bytes_; }
};

export struct HttpResponse {
	enum class BodyKind : u8 {
		text,
		sse,
		ws_upgrade,
		mapped_file,
		streamed_file,
		deferred,
	};

	using BodyPayload =
		variant<S, SP<SseChannel>, SP<WsUpgrade>, SP<MappedFile>, SP<StreamedFile>, SP<DeferredResponse>>;

	int status = kHttpOk;
	S status_text = "OK";
	S content_type = "text/html; charset=utf-8";
	HttpFields headers{true}; // extra response headers (added after Content-Type/Content-Length)
	V<S> set_cookies{}; // Set-Cookie headers (one per entry)
	HttpFields trailers{true}; // HTTP/2 trailer headers sent after the DATA frames
	bool head_only = false; // true → send headers only, suppress body (HEAD requests)
	SZ content_length_hint{0}; // non-zero overrides content_length() (HEAD static file responses)
	BodyKind body_kind = BodyKind::text;
	BodyPayload body_payload{S{}};

	[[nodiscard]] bool is_text() const noexcept { return body_kind == BodyKind::text; }
	[[nodiscard]] bool is_sse() const noexcept { return body_kind == BodyKind::sse; }
	[[nodiscard]] bool is_ws_upgrade() const noexcept { return body_kind == BodyKind::ws_upgrade; }
	[[nodiscard]] bool is_mapped_file() const noexcept { return body_kind == BodyKind::mapped_file; }
	[[nodiscard]] bool is_streamed_file() const noexcept { return body_kind == BodyKind::streamed_file; }
	[[nodiscard]] bool is_deferred() const noexcept { return body_kind == BodyKind::deferred; }

	[[nodiscard]] SV text_body() const noexcept {
		if (auto const *text = get_if<S>(&body_payload)) {
			return *text;
		}
		return {};
	}

	[[nodiscard]] S &text_body_mut() {
		if (!is_text() || !holds_alternative<S>(body_payload)) {
			body_kind = BodyKind::text;
			body_payload = S{};
		}
		return get<S>(body_payload);
	}

	[[nodiscard]] S take_text_body() {
		if (!holds_alternative<S>(body_payload)) {
			return {};
		}
		return move(get<S>(body_payload));
	}

	[[nodiscard]] SP<SseChannel> const &sse_channel_ptr() const {
		static SP<SseChannel> const empty{};
		if (auto const *ch = get_if<SP<SseChannel>>(&body_payload)) {
			return *ch;
		}
		return empty;
	}

	[[nodiscard]] SP<WsUpgrade> const &ws_upgrade_ptr() const {
		static SP<WsUpgrade> const empty{};
		if (auto const *up = get_if<SP<WsUpgrade>>(&body_payload)) {
			return *up;
		}
		return empty;
	}

	[[nodiscard]] SP<MappedFile> const &mapped_file_ptr() const {
		static SP<MappedFile> const empty{};
		if (auto const *file = get_if<SP<MappedFile>>(&body_payload)) {
			return *file;
		}
		return empty;
	}

	[[nodiscard]] SP<StreamedFile> const &streamed_file_ptr() const {
		static SP<StreamedFile> const empty{};
		if (auto const *file = get_if<SP<StreamedFile>>(&body_payload)) {
			return *file;
		}
		return empty;
	}

	[[nodiscard]] SP<DeferredResponse> const &deferred_response_ptr() const {
		static SP<DeferredResponse> const empty{};
		if (auto const *deferred = get_if<SP<DeferredResponse>>(&body_payload)) {
			return *deferred;
		}
		return empty;
	}

	[[nodiscard]] SP<SseChannel> take_sse_channel() {
		if (!holds_alternative<SP<SseChannel>>(body_payload)) {
			return {};
		}
		return move(get<SP<SseChannel>>(body_payload));
	}

	[[nodiscard]] SP<WsUpgrade> take_ws_upgrade() {
		if (!holds_alternative<SP<WsUpgrade>>(body_payload)) {
			return {};
		}
		return move(get<SP<WsUpgrade>>(body_payload));
	}

	[[nodiscard]] SP<MappedFile> take_mapped_file() {
		if (!holds_alternative<SP<MappedFile>>(body_payload)) {
			return {};
		}
		return move(get<SP<MappedFile>>(body_payload));
	}

	[[nodiscard]] SP<StreamedFile> take_streamed_file() {
		if (!holds_alternative<SP<StreamedFile>>(body_payload)) {
			return {};
		}
		return move(get<SP<StreamedFile>>(body_payload));
	}

	[[nodiscard]] SP<DeferredResponse> take_deferred_response() {
		if (!holds_alternative<SP<DeferredResponse>>(body_payload)) {
			return {};
		}
		return move(get<SP<DeferredResponse>>(body_payload));
	}

	void set_text_body(
		S text) {
		body_kind = BodyKind::text;
		body_payload = move(text);
	}

	void set_sse_channel(
		SP<SseChannel> ch) {
		body_kind = BodyKind::sse;
		body_payload = move(ch);
	}

	void set_ws_upgrade(
		SP<WsUpgrade> up) {
		body_kind = BodyKind::ws_upgrade;
		body_payload = move(up);
	}

	void set_mapped_file(
		SP<MappedFile> file) {
		body_kind = BodyKind::mapped_file;
		body_payload = move(file);
	}

	void set_streamed_file(
		SP<StreamedFile> file) {
		body_kind = BodyKind::streamed_file;
		body_payload = move(file);
	}

	void set_deferred_response(
		SP<DeferredResponse> deferred) {
		body_kind = BodyKind::deferred;
		body_payload = move(deferred);
	}

	[[nodiscard]] SZ content_length() const noexcept {
		if (content_length_hint != 0) {
			return content_length_hint;
		}
		if (is_mapped_file() && mapped_file_ptr()) {
			return mapped_file_ptr()->send_size;
		}
		if (is_streamed_file() && streamed_file_ptr()) {
			return static_cast<SZ>(streamed_file_ptr()->send_size);
		}
		return text_body().size();
	}

	[[nodiscard]] static HttpResponse html(
		S body) {
		HttpResponse r;
		r.status = kHttpOk;
		r.status_text = "OK";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	[[nodiscard]] static HttpResponse html(
		S body,
		int status,
		S status_text) {
		HttpResponse r;
		r.status = status;
		r.status_text = move(status_text);
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	[[nodiscard]] static HttpResponse json(
		S body) {
		HttpResponse r;
		r.status = kHttpOk;
		r.status_text = "OK";
		r.content_type = "application/json";
		r.set_text_body(move(body));
		return r;
	}

	[[nodiscard]] static HttpResponse json(
		S body,
		int status,
		S status_text) {
		HttpResponse r;
		r.status = status;
		r.status_text = move(status_text);
		r.content_type = "application/json";
		r.set_text_body(move(body));
		return r;
	}

	[[nodiscard]] static HttpResponse text(
		S body) {
		HttpResponse r;
		r.status = kHttpOk;
		r.status_text = "OK";
		r.content_type = "text/plain; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	[[nodiscard]] static HttpResponse text(
		S body,
		int status,
		S status_text) {
		HttpResponse r;
		r.status = status;
		r.status_text = move(status_text);
		r.content_type = "text/plain; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	[[nodiscard]] static HttpResponse redirect(
		SV location,
		int code = kHttpFound) {
		char const *status_text = "Found";
		switch (code) {
		case kHttpMovedPermanently : status_text = "Moved Permanently"; break;
		case kHttpTemporaryRedirect: status_text = "Temporary Redirect"; break;
		case kHttpPermanentRedirect: status_text = "Permanent Redirect"; break;
		default                    : break;
		}
		HttpResponse r{.status = code, .status_text = status_text, .content_type = "text/html; charset=utf-8"};
		r.headers["Location"] = S{location};
		return r;
	}

	[[nodiscard]] static HttpResponse not_found(
		SV path) {
		HttpResponse r;
		r.status = kHttpNotFound;
		r.status_text = "Not Found";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(format("<html><body><h1>404 Not Found</h1><p>{}</p></body></html>", html_escape(path)));
		return r;
	}

	[[nodiscard]] static HttpResponse bad_request(
		SV detail = {}) {
		auto body =
			detail.empty() ?
				S{"<html><body><h1>400 Bad Request</h1></body></html>"} :
				format("<html><body><h1>400 Bad Request</h1><p>{}</p></body></html>", html_escape(detail));
		HttpResponse r;
		r.status = kHttpBadRequest;
		r.status_text = "Bad Request";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	[[nodiscard]] static HttpResponse unauthorized(
		SV www_authenticate = {}) {
		HttpResponse r;
		r.status = kHttpUnauthorized;
		r.status_text = "Unauthorized";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>401 Unauthorized</h1></body></html>");
		if (!www_authenticate.empty()) {
			r.headers["WWW-Authenticate"] = S{www_authenticate};
		}
		return r;
	}

	[[nodiscard]] static HttpResponse forbidden(
		SV detail = {}) {
		auto body =
			detail.empty() ?
				S{"<html><body><h1>403 Forbidden</h1></body></html>"} :
				format("<html><body><h1>403 Forbidden</h1><p>{}</p></body></html>", html_escape(detail));
		HttpResponse r;
		r.status = kHttpForbidden;
		r.status_text = "Forbidden";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	[[nodiscard]] static HttpResponse method_not_allowed(
		std::initializer_list<SV> allowed = {}) {
		HttpResponse r;
		r.status = kHttpMethodNotAllowed;
		r.status_text = "Method Not Allowed";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>405 Method Not Allowed</h1></body></html>");
		if (allowed.size() > 0) {
			S allow;
			for (auto it = allowed.begin(); it != allowed.end(); ++it) {
				if (it != allowed.begin()) {
					allow += ", ";
				}
				allow += *it;
			}
			r.headers["Allow"] = move(allow);
		}
		return r;
	}

	[[nodiscard]] static HttpResponse unprocessable_entity(
		SV detail = {}) {
		auto body =
			detail.empty() ?
				S{"<html><body><h1>422 Unprocessable Entity</h1></body></html>"} :
				format(
					"<html><body><h1>422 Unprocessable Entity</h1><p>{}</p></body></html>", html_escape(detail));
		HttpResponse r;
		r.status = kHttpUnprocessableEntity;
		r.status_text = "Unprocessable Entity";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	[[nodiscard]] static HttpResponse uri_too_long() {
		HttpResponse r;
		r.status = kHttpUriTooLong;
		r.status_text = "URI Too Long";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>414 URI Too Long</h1></body></html>");
		return r;
	}

	[[nodiscard]] static HttpResponse header_fields_too_large() {
		HttpResponse r;
		r.status = kHttpRequestHeaderFieldsTooLarge;
		r.status_text = "Request Header Fields Too Large";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>431 Request Header Fields Too Large</h1></body></html>");
		return r;
	}

	[[nodiscard]] static HttpResponse gateway_timeout() {
		HttpResponse r;
		r.status = kHttpGatewayTimeout;
		r.status_text = "Gateway Timeout";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>504 Gateway Timeout</h1></body></html>");
		return r;
	}

	[[nodiscard]] static HttpResponse sse(
		SP<SseChannel> ch) {
		HttpResponse r{.status = kHttpOk, .status_text = "OK", .content_type = "text/event-stream"};
		r.set_sse_channel(move(ch));
		return r;
	}

	[[nodiscard]] static HttpResponse deferred(
		SP<DeferredResponse> response) {
		HttpResponse r;
		r.set_deferred_response(move(response));
		return r;
	}

	[[nodiscard]] static HttpResponse internal_error(
		SV detail = {}) {
		auto body =
			detail.empty() ?
				S{"<html><body><h1>500 Internal Server Error</h1></body></html>"} :
				format("<html><body><h1>500 Internal Server Error</h1><p>{}</p></body></html>", html_escape(detail));
		HttpResponse r;
		r.status = kHttpInternalServerError;
		r.status_text = "Internal Server Error";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	[[nodiscard]] static HttpResponse no_content() {
		return {.status = kHttpNoContent, .status_text = "No Content"};
	}

	[[nodiscard]] static HttpResponse content_too_large() {
		HttpResponse r;
		r.status = kHttpRequestEntityTooLarge;
		r.status_text = "Content Too Large";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>413 Content Too Large</h1></body></html>");
		return r;
	}

	// Append a Set-Cookie header. Attributes are Opt; pass empty strings to omit.
	// Example: resp.set_cookie("session", "abc123", "Path=/; HttpOnly; SameSite=Lax")
	HttpResponse &set_cookie(
		SV name,
		SV cookie_value,
		SV attributes = {}) {
		if (attributes.empty()) {
			set_cookies.push_back(format("{}={}", name, cookie_value));
		} else {
			set_cookies.push_back(format("{}={}; {}", name, cookie_value, attributes));
		}
		return *this;
	}
};

export class DeferredResponse {
	int efd_{-1};
	mutable mutex mtx_{};
	UP<HttpResponse> ready_{};
	chrono::steady_clock::time_point deadline_{};

public:
	static constexpr chrono::milliseconds kDefaultTimeout{30'000};

	explicit DeferredResponse(chrono::milliseconds timeout = kDefaultTimeout);
	~DeferredResponse() noexcept;
	DeferredResponse(DeferredResponse const &) = delete;
	DeferredResponse &operator =(DeferredResponse const &) = delete;
	DeferredResponse(DeferredResponse &&) = delete;
	DeferredResponse &operator =(DeferredResponse &&) = delete;

	[[nodiscard]] int eventfd_fd() const noexcept;
	void complete(HttpResponse response);
	[[nodiscard]] bool is_ready() const;
	[[nodiscard]] Opt<HttpResponse> take_ready();
	[[nodiscard]] chrono::steady_clock::time_point deadline() const;
	void set_deadline(chrono::steady_clock::time_point deadline);
	// Force-complete with 504 if the deadline has passed and no response is ready.
	// Returns true if this call expired the response (i.e. the caller should expect
	// the ready signal to fire).
	bool expire_if_past_deadline(chrono::steady_clock::time_point now);
};

DeferredResponse::DeferredResponse(
	chrono::milliseconds timeout)
	: efd_{::eventfd(0, EFD_CLOEXEC)}
	, deadline_{chrono::steady_clock::now() + timeout} {
	if (efd_ < 0) {
		throw SE{errno, system_category(), "eventfd"};
	}
}

DeferredResponse::~DeferredResponse() noexcept {
	if (efd_ >= 0) {
		::close(efd_);
	}
}

int DeferredResponse::eventfd_fd() const noexcept {
	return efd_;
}

void DeferredResponse::complete(
	HttpResponse response) {
	{
		SL const lk{mtx_};
		if (ready_) {
			return;
		}
		ready_ = make_unique<HttpResponse>(move(response));
	}
	u64 wake = 1;
	if (::write(efd_, &wake, sizeof(wake)) < 0 && errno != EAGAIN) {
		println(cerr, "DeferredResponse::complete: eventfd write: {}", strerror(errno));
	}
}

bool DeferredResponse::is_ready() const {
	SL const lk{mtx_};
	return ready_ != nullptr;
}

Opt<HttpResponse> DeferredResponse::take_ready() {
	SL const lk{mtx_};
	if (!ready_) {
		return std::nullopt;
	}
	auto response = move(*ready_);
	ready_.reset();
	return response;
}

chrono::steady_clock::time_point DeferredResponse::deadline() const {
	SL const lk{mtx_};
	return deadline_;
}

void DeferredResponse::set_deadline(
	chrono::steady_clock::time_point deadline) {
	SL const lk{mtx_};
	deadline_ = deadline;
}

bool DeferredResponse::expire_if_past_deadline(
	chrono::steady_clock::time_point now) {
	{
		SL const lk{mtx_};
		if (ready_) {
			return false;
		}
		if (now < deadline_) {
			return false;
		}
		ready_ = make_unique<HttpResponse>(HttpResponse::gateway_timeout());
	}
	u64 wake = 1;
	if (::write(efd_, &wake, sizeof(wake)) < 0 && errno != EAGAIN) {
		println(cerr, "DeferredResponse::expire_if_past_deadline: eventfd write: {}", strerror(errno));
	}
	return true;
}

// ---------------------------------------------------------------------------
// WebSocket support (placed before Router so Router::ws() can reference these)
// ---------------------------------------------------------------------------

namespace ws_detail {

// Compute Sec-WebSocket-Accept from Sec-WebSocket-Key.
CONFLUX_FUZZ_EXPORT S ws_accept_key(
	SV client_key) {
	static constexpr SV kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	S input{client_key};
	input += kMagic;
	auto digest = sha1(span{reinterpret_cast<unsigned char const *>(input.data()), input.size()});
	return base64_encode(span{digest.data(), digest.size()});
}

bool ascii_iequals(
	SV lhs,
	SV rhs) noexcept {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (SZ i = 0; i < lhs.size(); ++i) {
		auto const l = static_cast<unsigned char>(lhs[i]);
		auto const r = static_cast<unsigned char>(rhs[i]);
		if ((l | 0x20U) != (r | 0x20U)) {
			return false;
		}
	}
	return true;
}

bool header_token_contains(
	SV header,
	SV token) noexcept {
	while (!header.empty()) {
		auto comma = header.find(',');
		auto part = trim((comma == SV::npos) ? header : header.substr(0, comma));
		if (ascii_iequals(part, token)) {
			return true;
		}
		if (comma == SV::npos) {
			return false;
		}
		header.remove_prefix(comma + 1);
	}
	return false;
}

bool is_valid_client_key(
	SV key) {
	if (key.size() != 24) {
		return false;
	}
	auto decoded = base64_decode(key);
	return decoded.size() == 16
		&& base64_encode(span{reinterpret_cast<unsigned char const *>(decoded.data()), decoded.size()}) == key;
}

bool is_valid_handshake(
	HttpRequestView const &req) {
	return header_token_contains(req.headers["upgrade"], "websocket")
		&& header_token_contains(req.headers["connection"], "upgrade")
		&& trim(req.headers["sec-websocket-version"]) == "13"
		&& is_valid_client_key(trim(req.headers["sec-websocket-key"]));
}

// Build a complete WebSocket frame (server→client, unmasked) in one buffer so
// the transport call below emits header+payload as a single TCP segment / TLS record.
S ws_build_frame(
	u8 opcode,
	span<byte const> payload) {
	A<u8, 10> hdr{};
	SZ hdr_len = 0;
	hdr[hdr_len++] = 0x80U | opcode; // FIN + opcode
	SZ const len = payload.size();
	if (len < 126) {
		hdr[hdr_len++] = static_cast<u8>(len);
	} else if (len <= 0xFFFF) {
		hdr[hdr_len++] = 126;
		hdr[hdr_len++] = static_cast<u8>(len >> 8);
		hdr[hdr_len++] = static_cast<u8>(len & 0xFF);
	} else {
		hdr[hdr_len++] = 127;
		for (int s = 56; s >= 0; s -= 8) {
			hdr[hdr_len++] = static_cast<u8>((len >> s) & 0xFF);
		}
	}
	S frame;
	frame.reserve(hdr_len + len);
	frame.append(reinterpret_cast<char const *>(hdr.data()), hdr_len);
	frame.append(reinterpret_cast<char const *>(payload.data()), len);
	return frame;
}

bool ws_send_frame(
	int fd,
	u8 opcode,
	span<byte const> payload) {
	auto frame = ws_detail::ws_build_frame(opcode, payload);
	SZ sent = 0;
	while (sent < frame.size()) {
		auto n = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		sent += static_cast<SZ>(n);
	}
	return true;
}

#if CONFLUX_HAS_TLS
bool ws_tls_send_frame(
	SSL *ssl,
	u8 opcode,
	span<byte const> payload) {
	auto frame = ws_detail::ws_build_frame(opcode, payload);
	SZ sent = 0;
	while (sent < frame.size()) {
		auto const chunk = min<SZ>(frame.size() - sent, static_cast<SZ>(NL<int>::max()));
		int const n = SSL_write(ssl, frame.data() + sent, static_cast<int>(chunk));
		if (n > 0) {
			sent += static_cast<SZ>(n);
			continue;
		}
		int const err = SSL_get_error(ssl, n);
		if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
			continue;
		}
		return false;
	}
	return true;
}
#endif // CONFLUX_HAS_TLS

[[nodiscard]] bool is_valid_close_code(
	u16 code) {
	if (code < 1000U) {
		return false;
	}
	if (code <= 1003U) {
		return true;
	}
	if (code <= 1006U) {
		return false;
	}
	if (code <= 1014U) {
		return true;
	}
	if (code <= 4999U) {
		return code >= 3000U;
	}
	return false;
}

CONFLUX_FUZZ_EXPORT bool utf8_is_valid(
	SV s) {
	SZ i = 0;
	while (i < s.size()) {
		auto const b = static_cast<u8>(s[i]);
		SZ extra{};
		u32 min_cp{};
		u32 cp{};
		if (b < 0x80U) {
			++i;
			continue;
		}
		if ((b & 0xE0U) == 0xC0U) {
			extra = 1;
			min_cp = 0x80U;
			cp = b & 0x1FU;
		} else if ((b & 0xF0U) == 0xE0U) {
			extra = 2;
			min_cp = 0x800U;
			cp = b & 0x0FU;
		} else if ((b & 0xF8U) == 0xF0U) {
			extra = 3;
			min_cp = 0x10000U;
			cp = b & 0x07U;
		} else {
			return false;
		}
		if (i + extra >= s.size()) {
			return false;
		}
		for (SZ k = 1; k <= extra; ++k) {
			auto const c = static_cast<u8>(s[i + k]);
			if ((c & 0xC0U) != 0x80U) {
				return false;
			}
			cp = (cp << 6U) | (c & 0x3FU);
		}
		if (cp < min_cp) {
			return false;
		}
		if (cp >= 0xD800U && cp <= 0xDFFFU) {
			return false;
		}
		if (cp > 0x10FFFFU) {
			return false;
		}
		i += extra + 1;
	}
	return true;
}

CONFLUX_FUZZ_EXPORT struct FrameHeader {
	u8 opcode{};
	bool fin{};
	bool masked{};
	u64 payload_len{};
	A<u8, 4> mask{};
	SZ header_size{};
};

CONFLUX_FUZZ_EXPORT enum class FrameParseStatus : u8 {
	Ok,
	Incomplete,
	ProtocolError,
	ControlTooLarge,
};

CONFLUX_FUZZ_EXPORT FrameParseStatus parse_frame_header(
	span<byte const> buf,
	FrameHeader &out) {
	if (buf.size() < 2) {
		return FrameParseStatus::Incomplete;
	}
	auto const b0 = to_integer<u8>(buf[0]);
	auto const b1 = to_integer<u8>(buf[1]);
	out.fin = (b0 & 0x80U) != 0;
	out.opcode = b0 & 0x0FU;
	out.masked = (b1 & 0x80U) != 0;
	u64 plen = b1 & 0x7FU;
	bool const is_control = (out.opcode & 0x08U) != 0;

	if ((b0 & 0x70U) != 0) {
		return FrameParseStatus::ProtocolError;
	}
	if ((out.opcode >= 0x3U && out.opcode <= 0x7U) || out.opcode >= 0xBU) {
		return FrameParseStatus::ProtocolError;
	}
	if (is_control && (!out.fin || plen > 125)) {
		return FrameParseStatus::ControlTooLarge;
	}
	if (!out.masked) {
		return FrameParseStatus::ProtocolError;
	}

	SZ off = 2;
	if (plen == 126) {
		if (buf.size() < off + 2) {
			return FrameParseStatus::Incomplete;
		}
		plen = (static_cast<u64>(to_integer<u8>(buf[off])) << 8U) | static_cast<u64>(to_integer<u8>(buf[off + 1]));
		if (plen < 126) {
			return FrameParseStatus::ProtocolError;
		}
		off += 2;
	} else if (plen == 127) {
		if (buf.size() < off + 8) {
			return FrameParseStatus::Incomplete;
		}
		plen = 0;
		for (SZ i = 0; i < 8; ++i) {
			plen = (plen << 8U) | to_integer<u8>(buf[off + i]);
		}
		if (plen <= 0xFFFF) {
			return FrameParseStatus::ProtocolError;
		}
		off += 8;
	}
	if (buf.size() < off + 4) {
		return FrameParseStatus::Incomplete;
	}
	out.mask = {
		to_integer<u8>(buf[off]),
		to_integer<u8>(buf[off + 1]),
		to_integer<u8>(buf[off + 2]),
		to_integer<u8>(buf[off + 3])};
	off += 4;
	out.payload_len = plen;
	out.header_size = off;
	return FrameParseStatus::Ok;
}

} // namespace ws_detail

// WebSocket connection object passed to the WsHandler callback.
// Thread-safe for concurrent send; recv is single-consumer.
export class WsConn {
public:
	enum class Opcode : u8 {
		Text = 1,
		Binary = 2,
		Close = 8,
		Ping = 9,
		Pong = 10,
	};

	struct Frame {
		Opcode opcode{};
		S payload;
	};

	WsConn(WsConn const &) = delete;
	WsConn &operator =(WsConn const &) = delete;
	WsConn(WsConn &&) = delete;
	WsConn &operator =(WsConn &&) = delete;

	explicit WsConn(
		int fd,
		S initial_buf = {})
		: fd_(fd)
		, buf_(move(initial_buf)) {}
#if CONFLUX_HAS_TLS
	// TLS variant: ssl must already have the handshake complete and be wired to
	// a socket BIO (SSL_set_fd called by the server before handing off).
	// initial_buf carries any plaintext bytes already decrypted before handoff.
	explicit WsConn(
		int fd,
		SSL *ssl,
		S initial_buf)
		: fd_(fd)
		, ssl_(ssl)
		, buf_(move(initial_buf)) {}
#endif
	~WsConn() noexcept {
		stop_keepalive();
		if (!closed_.test_and_set()) {
			::shutdown(fd_, SHUT_WR);
		}
	}

	Opt<Frame> recv() {
		while (true) {
			if (!fill(2)) {
				return std::nullopt;
			}
			ws_detail::FrameHeader hdr{};
			// First parse pass on 2 bytes surfaces protocol errors (rsv, opcode,
			// unmasked, control-size) without waiting for mask bytes — the wire
			// may legitimately have no mask for a rejected frame.
			auto const pre = ws_detail::parse_frame_header(as_bytes(span{buf_.data(), 2}), hdr);
			auto emit_protocol_close = [&]() {
				auto const b0 = static_cast<u8>(buf_[0]);
				if ((b0 & 0x70U) != 0) {
					close(1002, "rsv bits set");
				} else if (u8 const op = b0 & 0x0FU; (op >= 0x3U && op <= 0x7U) || op >= 0xBU) {
					close(1002, "reserved opcode");
				} else {
					close(1002, "unmasked frame");
				}
			};
			if (pre == ws_detail::FrameParseStatus::ProtocolError) {
				emit_protocol_close();
				return std::nullopt;
			}
			if (pre == ws_detail::FrameParseStatus::ControlTooLarge) {
				close(1002, "invalid control frame");
				return std::nullopt;
			}
			// pre is Ok (no extended length) or Incomplete (need extended length + mask).
			auto const b1 = static_cast<u8>(buf_[1]);
			u64 const len7 = b1 & 0x7FU;
			SZ const header_needed = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0) + 4;
			if (!fill(header_needed)) {
				return std::nullopt;
			}
			auto const status = ws_detail::parse_frame_header(as_bytes(span{buf_.data(), header_needed}), hdr);
			if (status != ws_detail::FrameParseStatus::Ok) {
				if (status == ws_detail::FrameParseStatus::ProtocolError) {
					close(1002, "invalid frame header");
				} else if (status == ws_detail::FrameParseStatus::ControlTooLarge) {
					close(1002, "invalid control frame");
				}
				return std::nullopt;
			}
			consume(hdr.header_size);

			bool const fin = hdr.fin;
			u8 const opcode_raw = hdr.opcode;
			u64 const plen = hdr.payload_len;
			bool const is_control = (opcode_raw & 0x08U) != 0;
			A<u8, 4> const mask_key = hdr.mask;

			if (plen > kMaxMessageSize) {
				close(1009, "message too big");
				return std::nullopt;
			}
			if (!is_control && (frag_payload_.size() + plen) > kMaxMessageSize) {
				close(1009, "message too big");
				return std::nullopt;
			}
			if (!fill(static_cast<SZ>(plen))) {
				return std::nullopt;
			}
			S payload(buf_.data(), static_cast<SZ>(plen));
			consume(static_cast<SZ>(plen));
			for (SZ i = 0; i < payload.size(); ++i) {
				payload[i] = static_cast<char>(
					static_cast<unsigned char>(payload[i])
					^ mask_key[i & 3]); // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			}

			if (opcode_raw == 0x9U) {
				do_send_frame(10, as_bytes(span{payload}));
				continue;
			}
			if (opcode_raw == 0xAU) {
				continue;
			}
			if (opcode_raw == 0x8U) {
				if (plen == 1) {
					close(1002, "invalid close payload");
					return std::nullopt;
				}
				u16 echo_code = 1000;
				if (plen >= 2) {
					echo_code = static_cast<u16>(
						(static_cast<unsigned>(static_cast<u8>(payload[0])) << 8U)
						| static_cast<unsigned>(static_cast<u8>(payload[1])));
					if (!ws_detail::is_valid_close_code(echo_code)) {
						close(1002, "invalid close code");
						return std::nullopt;
					}
					if (payload.size() > 2 && !ws_detail::utf8_is_valid(SV{payload}.substr(2))) {
						close(1007, "invalid utf-8");
						return std::nullopt;
					}
				}
				close(echo_code, {});
				return std::nullopt;
			}

			if (opcode_raw == 0x0U) {
				if (!frag_opcode_) {
					close(1002, "unexpected continuation");
					return std::nullopt;
				}
				frag_payload_.append(payload);
				if (!fin) {
					continue;
				}
				auto const final_op = *frag_opcode_;
				S final_payload = move(frag_payload_);
				frag_opcode_.reset();
				frag_payload_.clear();
				if (final_op == Opcode::Text && !ws_detail::utf8_is_valid(final_payload)) {
					close(1007, "invalid utf-8");
					return std::nullopt;
				}
				return Frame{.opcode = final_op, .payload = move(final_payload)};
			}

			if (opcode_raw != 0x1U && opcode_raw != 0x2U) {
				close(1002, "reserved opcode");
				return std::nullopt;
			}
			if (frag_opcode_) {
				close(1002, "nested data frame");
				return std::nullopt;
			}
			auto const opcode = static_cast<Opcode>(opcode_raw);
			if (!fin) {
				frag_opcode_ = opcode;
				frag_payload_ = move(payload);
				continue;
			}
			if (opcode == Opcode::Text && !ws_detail::utf8_is_valid(payload)) {
				close(1007, "invalid utf-8");
				return std::nullopt;
			}
			return Frame{.opcode = opcode, .payload = move(payload)};
		}
	}

	[[nodiscard]] bool send_text(
		SV data) {
		SL const lk{send_mtx_};
		return do_send_frame(1, as_bytes(span{data}));
	}
	[[nodiscard]] bool send_binary(
		span<byte const> data) {
		SL const lk{send_mtx_};
		return do_send_frame(2, data);
	}
	[[nodiscard]] bool send_ping(
		SV data = {}) {
		if (data.size() > 125) {
			throw std::invalid_argument{"WsConn::send_ping: payload exceeds 125-byte control frame limit"};
		}
		SL const lk{send_mtx_};
		return do_send_frame(9, as_bytes(span{data}));
	}
	void close(
		u16 code = 1000,
		SV reason = {}) {
		if (!ws_detail::is_valid_close_code(code)) {
			throw std::invalid_argument{"WsConn::close: invalid close code"};
		}
		if (reason.size() > 123) {
			throw std::invalid_argument{"WsConn::close: reason exceeds 123-byte limit (control frame payload max 125)"};
		}
		if (!ws_detail::utf8_is_valid(reason)) {
			throw std::invalid_argument{"WsConn::close: reason must be valid UTF-8"};
		}
		if (closed_.test_and_set()) {
			return;
		}
		stop_keepalive();
		A<char, 2> code_bytes{static_cast<char>(code >> 8), static_cast<char>(code & 0xFF)};
		S payload{code_bytes.data(), 2};
		payload += reason;
		{
			SL const lk{send_mtx_};
			do_send_frame(8, as_bytes(span{payload}));
		}
#if CONFLUX_HAS_TLS
		if (ssl_ != nullptr) {
			// Do NOT call SSL_shutdown(): in blocking mode it waits for the peer's
			// close_notify, deadlocking against a client that sent a WS close frame
			// but hasn't yet issued a TLS close_notify.  WS close frames are
			// application-level; just free the SSL object and shut the socket.
			SSL_free(ssl_);
			ssl_ = nullptr;
		}
#endif
		::shutdown(fd_, SHUT_WR);
	}

	[[nodiscard]] bool is_open() const noexcept { return !closed_.test(); }
	[[nodiscard]] int fd() const noexcept { return fd_; }

	// Start a background keepalive thread that sends a Ping frame every
	// interval_ms milliseconds.  The thread exits when the connection closes.
	// Multiple calls are ignored after the first.
	void start_keepalive(
		unsigned interval_ms) {
		if (keepalive_thread_.joinable()) {
			return; // already started
		}
		keepalive_thread_ = jthread([this, interval_ms](stop_token const &st) {
			std::unique_lock lk{keepalive_mtx_};
			while (is_open()) {
				if (keepalive_cv_.wait_for(lk, st, chrono::milliseconds{interval_ms}, [this] { return !is_open(); })) {
					break;
				}
				lk.unlock();
				(void)send_ping();
				lk.lock();
			}
		});
	}

private:
	void stop_keepalive() noexcept {
		if (!keepalive_thread_.joinable()) {
			return;
		}
		keepalive_thread_.request_stop();
		keepalive_cv_.notify_all();
	}

	static constexpr u64 kMaxMessageSize = 16ULL * 1024 * 1024;

	int fd_;
#if CONFLUX_HAS_TLS
	SSL *ssl_ = nullptr;
#endif
	atomic_flag closed_{};
	mutex send_mtx_;
	mutex keepalive_mtx_;
	std::condition_variable_any keepalive_cv_;
	jthread keepalive_thread_{};
	S buf_;
	Opt<Opcode> frag_opcode_{};
	S frag_payload_{};

	bool fill(
		SZ n) {
		while (buf_.size() < n) {
			A<char, 4096> tmp{};
#if CONFLUX_HAS_TLS
			if (ssl_ != nullptr) {
				auto rc = SSL_read(ssl_, tmp.data(), static_cast<int>(tmp.size()));
				if (rc <= 0) {
					return false;
				}
				buf_.append(tmp.data(), static_cast<SZ>(rc));
				continue;
			}
#endif
			auto rc = ::recv(fd_, tmp.data(), tmp.size(), 0);
			if (rc <= 0) {
				return false;
			}
			buf_.append(tmp.data(), static_cast<SZ>(rc));
		}
		return true;
	}
	void consume(
		SZ n) {
		buf_.erase(0, n);
	}

	// Send a WebSocket frame over either TLS or plain socket.
	bool do_send_frame(
		u8 opcode,
		span<byte const> payload) {
#if CONFLUX_HAS_TLS
		if (ssl_ != nullptr) {
			return ws_detail::ws_tls_send_frame(ssl_, opcode, payload);
		}
#endif
		return ws_detail::ws_send_frame(fd_, opcode, payload);
	}
};

// Token carried in HttpResponse.ws_upgrade to signal a 101 WebSocket upgrade.
export struct WsUpgrade {
	S accept_key;
	CloneableFunction<void(HttpRequestView const &, WsConn &)> handler;
};

// ---------------------------------------------------------------------------

struct Segment {
	S value;
	bool is_param; // true → {name} single-segment capture
	bool is_wildcard; // true → {*name} greedy tail capture (must be last segment)
};

V<Segment> parse_pattern(
	SV pattern) {
	V<Segment> segs;
	SZ pos = 0;
	while (true) {
		auto next = pattern.find('/', pos);
		auto part = (next == SV::npos) ? pattern.substr(pos) : pattern.substr(pos, next - pos);

		if (part.size() >= 3 && part.front() == '{' && part.back() == '}' && part[1] == '*') {
			segs.push_back({S{part.substr(2, part.size() - 3)}, false, true});
		} else if (part.size() >= 2 && part.front() == '{' && part.back() == '}') {
			segs.push_back({S{part.substr(1, part.size() - 2)}, true, false});
		} else {
			segs.push_back({S{part}, false, false});
		}

		if (next == SV::npos) {
			break;
		}
		pos = next + 1;
	}
	return segs;
}

bool match_segments(
	V<Segment> const &pattern,
	SV path,
	HttpFieldsView &out_params) {
	// Wildcard tail: last segment {*name} matches everything remaining.
	if (!pattern.empty() && pattern.back().is_wildcard) {
		// Match all non-wildcard leading segments first.
		auto prefix_count = pattern.size() - 1;
		SZ pos = 0;
		HttpFieldsView tmp;
		for (SZ i = 0; i < prefix_count; ++i) {
			if (pos >= path.size()) {
				return false;
			}
			auto next = path.find('/', pos);
			auto part = (next == SV::npos) ? path.substr(pos) : path.substr(pos, next - pos);
			if (next == SV::npos && i + 1 < prefix_count) {
				return false;
			}
			if (pattern[i].is_param) {
				tmp.emplace_back_owned(S{pattern[i].value}, url_decode_path(part));
			} else if (pattern[i].value != part) {
				return false;
			}
			pos = (next == SV::npos) ? path.size() : next + 1;
		}
		// Capture the remainder (may be empty for trailing slash).
		tmp.emplace_back_owned(S{pattern.back().value}, url_decode_path(path.substr(pos)));
		out_params = move(tmp);
		return true;
	}

	V<SV> parts;
	SZ pos = 0;
	while (true) {
		auto next = path.find('/', pos);
		parts.push_back((next == SV::npos) ? path.substr(pos) : path.substr(pos, next - pos));
		if (next == SV::npos) {
			break;
		}
		pos = next + 1;
	}

	if (parts.size() != pattern.size()) {
		return false;
	}

	HttpFieldsView tmp;
	for (SZ i = 0; i < pattern.size(); ++i) {
		if (pattern[i].is_param) {
			tmp.emplace_back_owned(S{pattern[i].value}, url_decode_path(parts[i]));
		} else if (pattern[i].value != parts[i]) {
			return false;
		}
	}
	out_params = move(tmp);
	return true;
}

// Metadata for a single registered route, exposed by Router::route_infos().
export struct RouteInfo {
	S method;
	S path_pattern; // OpenAPI-style path e.g. /users/{id}
	V<S> path_params; // captured parameter names in order
};

// Reconstruct an OpenAPI path S from a parsed Segment V.
// The first segment is always an empty literal (artifact of the leading '/');
// skip it so the result starts with a single '/'.
inline S segments_to_pattern(
	V<Segment> const &segs) {
	S out;
	bool first = true;
	for (auto const &seg: segs) {
		if (first && seg.value.empty() && !seg.is_param && !seg.is_wildcard) {
			first = false;
			continue; // skip leading empty segment
		}
		first = false;
		out += '/';
		if (seg.is_wildcard || seg.is_param) {
			out += '{';
			out += seg.value;
			out += '}';
		} else {
			out += seg.value;
		}
	}
	if (out.empty()) {
		out = "/";
	}
	return out;
}

export struct StaticOptions {
	// Cache-Control header value. Empty = no Cache-Control header set.
	S cache_control{"max-age=3600, public"};
	// Serve pre-compressed .gz or .br sidecars when the client accepts them.
	bool precompressed{true};
	// Generate an HTML directory listing when no index.html is found.
	bool directory_listing{false};
	// When set, stat/open/mmap happen on this pool's threads via DeferredResponse,
	// keeping the io_uring thread free while slow disks resolve.
	SP<WorkPool> offload_pool{};
	// Small static file cache. Disabled by default to preserve existing memory
	// behavior unless callers opt in via Config/Router defaults or per route.
	StaticFileCacheConfig file_cache{};
	bool allow_put{false};
	bool allow_delete{false};
};

Task<void> do_serve_file(
	SP<DeferredResponse> dr,
	HttpResponse base,
	SZ send_off,
	SZ send_sz,
	SZ total_size,
	conflux::work::root::Task<FileHandle> open_task) {
	try {
		auto fh = co_await std::move(open_task);
		base.set_streamed_file(
			make_shared<StreamedFile>(StreamedFile{
				.handle = make_shared<FileHandle>(move(fh)),
				.send_offset = send_off,
				.send_size = send_sz,
				.total_size = total_size}));
		dr->complete(move(base));
	} catch (...) { dr->complete(HttpResponse::not_found("async open failed")); }
}

Task<void> do_save_file(
	FileReader *fr,
	SP<S> body_owned,
	SP<S> fp,
	bool existed,
	SP<StaticCacheStore> static_cache,
	SP<DeferredResponse> dr,
	conflux::work::root::Task<FileHandle> open_task) {
	try {
		auto fh = co_await std::move(open_task);
		auto fh_ptr = make_shared<FileHandle>(move(fh));
		co_await fr->write_into(*fh_ptr, 0, as_bytes(span{*body_owned}));
		static_cache->evict_all_encodings(*fp);
		HttpResponse resp;
		resp.status = existed ? kHttpNoContent : kHttpCreated;
		resp.status_text = existed ? "No Content" : "Created";
		dr->complete(move(resp));
	} catch (...) { dr->complete(HttpResponse::internal_error()); }
}

Task<void> do_delete_file(
	SP<DeferredResponse> dr,
	SP<S> fp,
	SP<StaticCacheStore> static_cache,
	conflux::work::root::Task<void> unlink_task) {
	try {
		co_await std::move(unlink_task);
		static_cache->evict_all_encodings(*fp);
		dr->complete(HttpResponse::no_content());
	} catch (FileIoError const &e) {
		dr->complete(e.code().value() == ENOENT ? HttpResponse::not_found(*fp) : HttpResponse::internal_error());
	} catch (...) { dr->complete(HttpResponse::internal_error()); }
}

export class Router {
public:
	using Handler = CloneableFunction<HttpResponse(HttpRequestView const &)>;
	using SseHandler = CloneableFunction<void(HttpRequestView const &, SP<SseChannel>)>;
	// next is the downstream handler (or next middleware); call it to continue the chain.
	using Middleware = CloneableFunction<HttpResponse(HttpRequestView const &, Handler const &)>;
	using WsHandler = CloneableFunction<void(HttpRequestView const &, WsConn &)>;
	using ErrorHandler = CloneableFunction<HttpResponse(HttpRequestView const &, exception const &)>;

	Router()
		: impl_(make_unique<Impl>()) {}
	explicit Router(
		Config const &cfg)
		: impl_(make_unique<Impl>()) {
		impl_->static_file_cache = cfg.static_file_cache;
	}
	~Router() {} // NOLINT(modernize-use-equals-default) — GCC module bug

	Router(Router const &) = delete;
	Router &operator =(Router const &) = delete;
	Router(
		Router &&o) noexcept
		: impl_(move(o.impl_)) {}
	Router &operator =(
		Router &&o) noexcept {
		impl_ = move(o.impl_);
		return *this;
	}

	// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks): false positive on CloneableFunction ownership.
	template<typename F>
	Router &add(
		SV method,
		SV path,
		F &&handler) {
		impl_->routes.push_back({S{method}, parse_pattern(path), make_handler(impl_->work_pool, forward<F>(handler))});
		return *this;
	}
	// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

	template<typename F>
	Router &get(
		SV path,
		F &&handler) {
		return add("GET", path, forward<F>(handler));
	}

	template<typename F>
	Router &post(
		SV path,
		F &&handler) {
		return add("POST", path, forward<F>(handler));
	}

	template<typename F>
	Router &put(
		SV path,
		F &&handler) {
		return add("PUT", path, forward<F>(handler));
	}

	template<typename F>
	Router &patch(
		SV path,
		F &&handler) {
		return add("PATCH", path, forward<F>(handler));
	}

	template<typename F>
	Router &del(
		SV path,
		F &&handler) {
		return add("DELETE", path, forward<F>(handler));
	}

	template<typename F>
	Router &options(
		SV path,
		F &&handler) {
		return add("OPTIONS", path, forward<F>(handler));
	}

	template<typename F>
	Router &use(
		F &&mw) {
		impl_->middlewares.push_back(make_middleware(forward<F>(mw)));
		return *this;
	}

	template<typename F>
	Router &on_not_found(
		F &&handler) {
		impl_->not_found_handler = make_handler(impl_->work_pool, forward<F>(handler));
		return *this;
	}

	template<typename F>
	Router &on_error(
		F &&handler) {
		impl_->error_handler = make_error_handler(forward<F>(handler));
		return *this;
	}

	// Return metadata for all registered routes (regular routes only).
	[[nodiscard]] V<RouteInfo> route_infos() const {
		V<RouteInfo> result;
		result.reserve(impl_->routes.size());
		for (auto const &route: impl_->routes) {
			RouteInfo info;
			info.method = route.method;
			info.path_pattern = segments_to_pattern(route.pattern);
			for (auto const &seg: route.pattern) {
				if (seg.is_param || seg.is_wildcard) {
					info.path_params.push_back(seg.value);
				}
			}
			result.push_back(move(info));
		}
		return result;
	}

	template<typename F>
	Router &sse(
		SV path,
		F &&handler) {
		impl_->sse_routes.push_back({parse_pattern(path), make_sse_handler(forward<F>(handler))});
		return *this;
	}

	Router &set_work_pool(
		SP<WorkPool> pool) {
		impl_->work_pool = move(pool);
		return *this;
	}

	[[nodiscard]] SP<WorkPool> work_pool() const { return impl_->work_pool; }

	Router &set_static_file_cache(
		StaticFileCacheConfig cfg) {
		impl_->static_file_cache = cfg;
		return *this;
	}

	// Register a WebSocket upgrade handler. GET requests with a valid Upgrade: websocket
	// handshake are upgraded to WebSocket; the handler runs on the router's work pool.
	template<typename F>
	Router &ws(
		SV path,
		F &&handler) {
		auto ws_handler = make_ws_handler(forward<F>(handler));
		// Implement as a regular GET route that returns a WsUpgrade response.
		add("GET", path, [h = move(ws_handler)](HttpRequestView const &req) mutable -> HttpResponse {
			if (!ws_detail::is_valid_handshake(req)) {
				return HttpResponse::bad_request();
			}
			auto key = trim(req.headers["sec-websocket-key"]);
			auto up = make_shared<WsUpgrade>();
			up->accept_key = ws_detail::ws_accept_key(key);
			up->handler = move(h);
			HttpResponse r{.status = 101, .status_text = "Switching Protocols"};
			r.set_ws_upgrade(move(up));
			return r;
		});
		return *this;
	}

	// Route group: scopes a set of routes under a path prefix with Opt group-local middleware.
	// Group middleware wraps only the routes registered inside the group callback;
	// it does NOT affect routes registered outside. The group callback receives a Group&.
	class Group {
	public:
		template<typename F>
		Group &use(
			F &&mw) {
			middlewares_.push_back(Router::make_middleware(forward<F>(mw)));
			return *this;
		}
		template<typename F>
		Group &add(
			SV method,
			SV path,
			F &&handler) {
			router_.add(
				method,
				prefix_ + S{path},
				wrap(Router::make_handler(router_.impl_->work_pool, forward<F>(handler))));
			return *this;
		}
		template<typename F>
		Group &get(
			SV path,
			F &&handler) {
			return add("GET", path, forward<F>(handler));
		}
		template<typename F>
		Group &post(
			SV path,
			F &&handler) {
			return add("POST", path, forward<F>(handler));
		}
		template<typename F>
		Group &put(
			SV path,
			F &&handler) {
			return add("PUT", path, forward<F>(handler));
		}
		template<typename F>
		Group &patch(
			SV path,
			F &&handler) {
			return add("PATCH", path, forward<F>(handler));
		}
		template<typename F>
		Group &del(
			SV path,
			F &&handler) {
			return add("DELETE", path, forward<F>(handler));
		}
		template<typename F>
		Group &options(
			SV path,
			F &&handler) {
			return add("OPTIONS", path, forward<F>(handler));
		}

	private:
		friend class Router;
		Group(
			Router &router,
			S prefix)
			: router_(router)
			, prefix_(move(prefix))
			, middlewares_{} {}

		// Apply group middlewares around h (innermost first, so first-registered is outermost).
		// Capture mw by value: the Group object is destroyed after router.group() returns,
		// so capturing by reference would dangle.
		[[nodiscard]] Handler wrap(
			Handler h) const {
			for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i) {
				auto mw = middlewares_[static_cast<SZ>(i)]; // copy: Group is destroyed after group() returns
				h = [mw = move(mw), n = move(h)](HttpRequestView const &r) { return mw(r, n); };
			}
			return h;
		}

		Router &router_;
		S prefix_;
		V<Middleware> middlewares_;
	};

	template<typename F>
	Router &group(
		SV prefix,
		F &&fn) {
		Group g{*this, S{prefix}};
		forward<F>(fn)(g);
		return *this;
	}

	// Serve static files from root_dir for GET/HEAD requests under url_prefix.
	// url_prefix must not end with '/'. Files are served at url_prefix/{*file}.
	// Path traversal ("..") is rejected with 403.
	// ETag based on size+mtime; Range requests (206 Partial Content) supported.
	// Pre-compressed sidecar files (.gz, .br) served when client accepts them.
	Router &serve_static(
		SV url_prefix,
		S root_dir,
		StaticOptions const &sopts = {}) {
		// Strip trailing slash from root_dir.
		while (!root_dir.empty() && root_dir.back() == '/') {
			root_dir.pop_back();
		}

		auto pattern = S{url_prefix} + "/{*file}";
		auto effective_sopts = sopts;
		if (!effective_sopts.file_cache.enabled) {
			effective_sopts.file_cache = impl_->static_file_cache;
		}

		struct StaticReq {
			S file_param;
			S method;
			S accept_encoding;
			S if_none_match;
			S if_modified_since;
			S range;
		};

		auto static_cache = make_shared<StaticCacheStore>();

		auto do_work =
			[static_cache](S const &rd, StaticOptions const &static_options, StaticReq const &r) -> HttpResponse {
			try {
				S file_param = r.file_param;
				auto full_path = rd + file_param;

				struct ::stat st{};
				if (::stat(full_path.c_str(), &st) != 0) {
					return HttpResponse::not_found(file_param);
				}
				if (S_ISDIR(st.st_mode)) {
					// Try index.html first.
					auto index = full_path + "/index.html";
					if (::stat(index.c_str(), &st) == 0) {
						full_path = move(index);
						file_param += "/index.html";
					} else if (static_options.directory_listing) {
						// Generate HTML directory listing.
						int const dfd = ::open(full_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
						if (dfd < 0) {
							return HttpResponse::html(
								"<html><body><h1>403 Forbidden</h1></body></html>",
								kHttpForbidden,
								"Forbidden");
						}
						auto *dir = ::fdopendir(dfd);
						if (dir == nullptr) {
							::close(dfd);
							return HttpResponse::html(
								"<html><body><h1>403 Forbidden</h1></body></html>",
								kHttpForbidden,
								"Forbidden");
						}
						S html = format(
							"<html><head><title>Index of {}</title></head>"
							"<body><h1>Index of {}</h1><ul>",
							html_escape(file_param),
							html_escape(file_param));
						if (!file_param.empty() && file_param != "/") {
							html += "<li><a href=\"../\">..</a></li>";
						}
						struct ::dirent *ent{};
						V<S> names;
						while ((ent = ::readdir(dir)) != nullptr) {
							// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-A-to-pointer-decay,hicpp-no-A-decay)
							SV const n{ent->d_name};
							if (n == "." || n == "..") {
								continue;
							}
							names.emplace_back(n);
						}
						::closedir(dir);
						ranges::sort(names);
						for (auto const &name: names) {
							html += format("<li><a href=\"{}\">{}</a></li>", html_escape(name), html_escape(name));
						}
						html += "</ul></body></html>";
						return HttpResponse::html(move(html));
					} else {
						return HttpResponse::html(
							"<html><body><h1>403 Forbidden</h1></body></html>",
							kHttpForbidden,
							"Forbidden");
					}
				}

				// Pre-compressed sidecar: try .br then .gz.
				S content_encoding;
				if (static_options.precompressed) {
					auto const &accept_enc = r.accept_encoding;
					if (accept_enc.find("br") != S::npos) {
						auto br_path = full_path + ".br";
						struct ::stat br_st{};
						if (::stat(br_path.c_str(), &br_st) == 0) {
							full_path = move(br_path);
							st = br_st;
							content_encoding = "br";
						}
					}
					if (content_encoding.empty() && accept_enc.find("gzip") != S::npos) {
						auto gz_path = full_path + ".gz";
						struct ::stat gz_st{};
						if (::stat(gz_path.c_str(), &gz_st) == 0) {
							full_path = move(gz_path);
							st = gz_st;
							content_encoding = "gzip";
						}
					}
				}

				// Build ETag from size + mtime.
				auto etag = format("\"{:x}-{:x}\"", st.st_size, st.st_mtime);

				// Format Last-Modified. Thread-local cache keyed on mtime — a hot
				// directory typically serves many files sharing a handful of mtimes,
				// so strftime runs once per mtime value per thread.
				thread_local time_t last_mtime_cached = 0;
				thread_local S last_modified_cached;
				S last_modified;
				if (st.st_mtime == last_mtime_cached && !last_modified_cached.empty()) {
					last_modified = last_modified_cached;
				} else {
					tm tm_val{};
					::gmtime_r(&st.st_mtime, &tm_val);
					A<char, 64> buf{};
					if (strftime(buf.data(), buf.size(), "%a, %d %b %Y %H:%M:%S GMT", &tm_val) > 0) {
						last_modified = buf.data();
						last_modified_cached = last_modified;
						last_mtime_cached = st.st_mtime;
					}
				}

				// 304 Not Modified checks.
				if (auto const &inm = r.if_none_match; !inm.empty() && inm == etag) {
					HttpResponse resp;
					resp.status = kHttpNotModified;
					resp.status_text = "Not Modified";
					resp.content_type.clear();
					resp.set_text_body({});
					return resp;
				}
				if (auto const &ims = r.if_modified_since; !ims.empty()) {
					tm req_tm{};
					if (::strptime(ims.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &req_tm)) {
						req_tm.tm_isdst = 0;
						if (st.st_mtime <= ::timegm(&req_tm)) {
							HttpResponse resp;
							resp.status = kHttpNotModified;
							resp.status_text = "Not Modified";
							resp.content_type.clear();
							resp.set_text_body({});
							return resp;
						}
					}
				}

				// MIME type from extension (use original file_param, not .gz/.br path).
				auto ext_pos = file_param.rfind('.');
				SV mime = "application/octet-stream";
				if (ext_pos != S::npos) {
					auto ext = SV{file_param}.substr(ext_pos);
					if (ext == ".html" || ext == ".htm") {
						mime = "text/html; charset=utf-8";
					} else if (ext == ".css") {
						mime = "text/css; charset=utf-8";
					} else if (ext == ".js" || ext == ".mjs") {
						mime = "application/javascript; charset=utf-8";
					} else if (ext == ".json") {
						mime = "application/json";
					} else if (ext == ".xml") {
						mime = "application/xml";
					} else if (ext == ".txt") {
						mime = "text/plain; charset=utf-8";
					} else if (ext == ".svg") {
						mime = "image/svg+xml";
					} else if (ext == ".png") {
						mime = "image/png";
					} else if (ext == ".jpg" || ext == ".jpeg") {
						mime = "image/jpeg";
					} else if (ext == ".gif") {
						mime = "image/gif";
					} else if (ext == ".webp") {
						mime = "image/webp";
					} else if (ext == ".ico") {
						mime = "image/x-icon";
					} else if (ext == ".woff") {
						mime = "font/woff";
					} else if (ext == ".woff2") {
						mime = "font/woff2";
					} else if (ext == ".ttf") {
						mime = "font/ttf";
					} else if (ext == ".otf") {
						mime = "font/otf";
					} else if (ext == ".pdf") {
						mime = "application/pdf";
					} else if (ext == ".gz") {
						mime = "application/gzip";
					} else if (ext == ".zip") {
						mime = "application/zip";
					} else if (ext == ".wasm") {
						mime = "application/wasm";
					}
				}

				auto file_size = static_cast<SZ>(st.st_size);

				auto base_response = [&](int status, SV status_text) {
					HttpResponse resp{.status = status, .status_text = S{status_text}, .content_type = S{mime}};
					resp.headers["ETag"] = etag;
					resp.headers["Last-Modified"] = last_modified;
					resp.headers["Accept-Ranges"] = "bytes";
					if (!content_encoding.empty()) {
						resp.headers["Content-Encoding"] = content_encoding;
					}
					if (!static_options.cache_control.empty()) {
						resp.headers["Cache-Control"] = static_options.cache_control;
					}
					return resp;
				};

				// HEAD: return headers only (no body, but correct Content-Length).
				if (r.method == "HEAD") {
					auto resp = base_response(kHttpOk, "OK");
					resp.head_only = true;
					resp.content_length_hint = file_size;
					return resp;
				}

				// Zero-size file: skip mmap, return empty body directly.
				if (file_size == 0) {
					return base_response(kHttpOk, "OK");
				}

				// Parse Range header for partial content (only supported when no precompression applied).
				SZ range_start = 0;
				SZ range_end = file_size - 1;
				bool is_range_request = false;
				if (content_encoding.empty()) {
					auto const &range_hdr = r.range;
					if (!range_hdr.empty() && range_hdr.starts_with("bytes=")) {
						auto spec = SV{range_hdr}.substr(6);
						auto dash = spec.find('-');
						if (dash != SV::npos) {
							auto start_sv = spec.substr(0, dash);
							auto end_sv = spec.substr(dash + 1);
							SZ rs = 0;
							SZ re = file_size - 1;
							bool ok = true;
							if (!start_sv.empty()) {
								auto [p, ec] =
									from_chars(start_sv.data(), ranges::next(start_sv.data(), ssize(start_sv)), rs);
								if (ec != errc{}) {
									ok = false;
								}
							}
							if (!end_sv.empty()) {
								if (start_sv.empty()) {
									// suffix range: "-N" = last N bytes; not implemented
									ok = false;
								} else {
									auto [p, ec] =
										from_chars(end_sv.data(), ranges::next(end_sv.data(), ssize(end_sv)), re);
									if (ec != errc{}) {
										ok = false;
									}
								}
							} else if (start_sv.empty()) {
								ok = false; // both empty: malformed
							}
							if (ok && rs <= re && re < file_size) {
								range_start = rs;
								range_end = re;
								is_range_request = true;
							} else if (ok) {
								// Range not satisfiable
								auto resp = HttpResponse{};
								resp.status = kHttpRangeNotSatisfiable;
								resp.status_text = "Range Not Satisfiable";
								resp.content_type = "text/plain; charset=utf-8";
								resp.set_text_body(format("bytes */{}", file_size));
								return resp;
							}
						}
					}
				}

				if (static_options.file_cache.enabled && file_size <= static_options.file_cache.small_file_max_bytes) {
					auto const cache_key = full_path + "|" + content_encoding;
					auto make_cached_response = [&](StaticCacheEntry const &entry) {
						if (is_range_request) {
							auto send_sz = range_end - range_start + 1;
							auto resp = HttpResponse{
								.status = kHttpPartialContent,
								.status_text = "Partial Content",
								.content_type = entry.mime};
							resp.headers["ETag"] = entry.etag;
							resp.headers["Last-Modified"] = entry.last_modified;
							resp.headers["Accept-Ranges"] = "bytes";
							resp.headers["Content-Range"] = format("bytes {}-{}/{}", range_start, range_end, file_size);
							if (!entry.content_encoding.empty()) {
								resp.headers["Content-Encoding"] = entry.content_encoding;
							}
							if (!static_options.cache_control.empty()) {
								resp.headers["Cache-Control"] = static_options.cache_control;
							}
							resp.set_text_body(entry.body.substr(range_start, send_sz));
							return resp;
						}
						auto resp = HttpResponse{.status = kHttpOk, .status_text = "OK", .content_type = entry.mime};
						resp.headers["ETag"] = entry.etag;
						resp.headers["Last-Modified"] = entry.last_modified;
						resp.headers["Accept-Ranges"] = "bytes";
						if (!entry.content_encoding.empty()) {
							resp.headers["Content-Encoding"] = entry.content_encoding;
						}
						if (!static_options.cache_control.empty()) {
							resp.headers["Cache-Control"] = static_options.cache_control;
						}
						resp.set_text_body(entry.body);
						return resp;
					};
					if (auto cached = static_cache->get(cache_key, st)) {
						return make_cached_response(*cached);
					}
					int const fd = ::open(full_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
					if (fd < 0) {
						return HttpResponse::not_found(file_param);
					}
					S body(file_size, '\0');
					SZ off = 0;
					while (off < body.size()) {
						ssize_t const n = ::read(fd, body.data() + off, body.size() - off);
						if (n < 0) {
							if (errno == EINTR) {
								continue;
							}
							::close(fd);
							return HttpResponse::internal_error();
						}
						if (n == 0) {
							break;
						}
						off += static_cast<SZ>(n);
					}
					::close(fd);
					if (off != body.size()) {
						body.resize(off);
					}
					StaticCacheEntry entry{
						.body = move(body),
						.mime = S{mime},
						.etag = etag,
						.last_modified = last_modified,
						.content_encoding = content_encoding,
						.size = st.st_size,
						.mtime = st.st_mtime,
						.dev = st.st_dev,
						.ino = st.st_ino};
					auto resp = make_cached_response(entry);
					static_cache->put(cache_key, move(entry), static_options.file_cache.max_total_bytes);
					return resp;
				}

				// Async uring path: when a FileReader is installed for this
				// thread (i.e. we are running on a ring thread, not offloaded to
				// a WorkPool) and no compressed variant was picked, open the
				// file via IORING_OP_OPENAT and return a deferred response that
				// carries a StreamedFile once the open CQE fires. Otherwise
				// fall back to the synchronous mmap path below.
				if (auto *fr = current_file_reader(); fr != nullptr && content_encoding.empty()) {
					auto dr = make_shared<DeferredResponse>();
					auto base = is_range_request ? base_response(kHttpPartialContent, "Partial Content") :
												   base_response(kHttpOk, "OK");
					if (is_range_request) {
						base.headers["Content-Range"] = format("bytes {}-{}/{}", range_start, range_end, file_size);
					}
					auto const send_off = is_range_request ? range_start : SZ{0};
					auto const send_sz = is_range_request ? (range_end - range_start + 1) : file_size;
					co_spawn(do_serve_file(
						dr,
						move(base),
						send_off,
						send_sz,
						file_size,
						fr->open_async(AT_FDCWD, full_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)));
					return HttpResponse::deferred(move(dr));
				}

				// mmap the file.
				// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
				int const fd = ::open(full_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
				if (fd < 0) {
					return HttpResponse::not_found(file_param);
				}
				// NOLINTNEXTLINE(misc-const-correctness): pointee non-const required by munmap/MappedFile C API
				void *const ptr = ::mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
				::close(fd);
				if (ptr == MAP_FAILED) {
					return HttpResponse::internal_error();
				}

				if (is_range_request) {
					auto send_sz = range_end - range_start + 1;
					auto resp = base_response(kHttpPartialContent, "Partial Content");
					resp.headers["Content-Range"] = format("bytes {}-{}/{}", range_start, range_end, file_size);
					resp.set_mapped_file(make_shared<MappedFile>(ptr, file_size, range_start, send_sz));
					return resp;
				}

				auto resp = base_response(kHttpOk, "OK");
				resp.set_mapped_file(make_shared<MappedFile>(ptr, file_size));
				return resp;
			} catch (...) { return HttpResponse::internal_error(); }
		};

		auto rd = move(root_dir);
		auto normalize_path = [](SV raw) -> Opt<S> {
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
				return std::nullopt;
			}
			S result;
			for (auto const &p: parts) {
				result += '/';
				result += p;
			}
			return result;
		};

		// NOLINTNEXTLINE(bugprone-exception-escape): lambda already handles failures via top-level try/catch.
		get(pattern,
			[rd, sopts = effective_sopts, do_work, normalize_path](HttpRequestView const &req) -> HttpResponse {
				try {
					auto norm = normalize_path(req.params["file"]);
					if (!norm) {
						return HttpResponse::html(
							"<html><body><h1>403 Forbidden</h1></body></html>",
							kHttpForbidden,
							"Forbidden");
					}

					StaticReq sreq{
						.file_param = move(*norm),
						.method = S{req.method},
						.accept_encoding = S{req.headers["accept-encoding"]},
						.if_none_match = S{std::as_const(req.headers)["if-none-match"]},
						.if_modified_since = S{std::as_const(req.headers)["if-modified-since"]},
						.range = S{req.headers["range"]},
					};

					if (sopts.offload_pool) {
						auto dr = make_shared<DeferredResponse>();
						auto ok = sopts.offload_pool->enqueue([rd, sopts, sreq = move(sreq), do_work, dr]() mutable {
							try {
								dr->complete(do_work(rd, sopts, sreq));
							} catch (...) { dr->complete(HttpResponse::internal_error()); }
						});
						if (!ok) {
							return HttpResponse::internal_error("offload queue full");
						}
						return HttpResponse::deferred(move(dr));
					}

					return do_work(rd, sopts, sreq);
				} catch (...) { return HttpResponse::internal_error(); }
			});

		if (effective_sopts.allow_put) {
			// NOLINTNEXTLINE(bugprone-exception-escape)
			put(pattern,
				[rd, sopts = effective_sopts, static_cache, normalize_path](
					HttpRequestView const &req) -> HttpResponse {
					try {
						auto norm = normalize_path(req.params["file"]);
						if (!norm) {
							return HttpResponse::html(
								"<html><body><h1>403 Forbidden</h1></body></html>",
								kHttpForbidden,
								"Forbidden");
						}
						auto full_path = rd + *norm;

						struct ::stat st{};
						bool const existed = ::stat(full_path.c_str(), &st) == 0;

						if (auto *fr = current_file_reader(); fr != nullptr) {
							auto body_owned = make_shared<S>(req.body);
							auto dr = make_shared<DeferredResponse>();
							auto fp = make_shared<S>(full_path);
							co_spawn(do_save_file(
								fr,
								body_owned,
								fp,
								existed,
								static_cache,
								dr,
								fr->open_async(
									AT_FDCWD,
									*fp,
									O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
									0644)));
							return HttpResponse::deferred(move(dr));
						}

						if (sopts.offload_pool) {
							auto dr = make_shared<DeferredResponse>();
							auto body_owned = make_shared<S>(req.body);
							auto ok = sopts.offload_pool->enqueue(
								[full_path = move(full_path), body_owned, existed, static_cache, dr]() mutable {
									try {
										int const wfd = ::open(
											full_path.c_str(),
											O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
											0644);
										if (wfd < 0) {
											dr->complete(HttpResponse::internal_error());
											return;
										}
										auto const body = span<char const>{body_owned->data(), body_owned->size()};
										SZ off = 0;
										while (off < body.size()) {
											ssize_t const n = ::write(wfd, body.data() + off, body.size() - off);
											if (n < 0) {
												if (errno == EINTR) {
													continue;
												}
												::close(wfd);
												dr->complete(HttpResponse::internal_error());
												return;
											}
											off += static_cast<SZ>(n);
										}
										::close(wfd);
										static_cache->evict_all_encodings(full_path);
										HttpResponse resp;
										resp.status = existed ? kHttpNoContent : kHttpCreated;
										resp.status_text = existed ? "No Content" : "Created";
										dr->complete(move(resp));
									} catch (...) { dr->complete(HttpResponse::internal_error()); }
								});
							if (!ok) {
								return HttpResponse::internal_error("offload queue full");
							}
							return HttpResponse::deferred(move(dr));
						}

						int const wfd =
							::open(full_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
						if (wfd < 0) {
							return HttpResponse::internal_error();
						}
						auto const body = span<char const>{req.body.data(), req.body.size()};
						SZ off = 0;
						while (off < body.size()) {
							ssize_t const n = ::write(wfd, body.data() + off, body.size() - off);
							if (n < 0) {
								if (errno == EINTR) {
									continue;
								}
								::close(wfd);
								return HttpResponse::internal_error();
							}
							off += static_cast<SZ>(n);
						}
						::close(wfd);
						static_cache->evict_all_encodings(full_path);
						HttpResponse resp;
						resp.status = existed ? kHttpNoContent : kHttpCreated;
						resp.status_text = existed ? "No Content" : "Created";
						return resp;
					} catch (...) { return HttpResponse::internal_error(); }
				});
		}

		if (effective_sopts.allow_delete) {
			// NOLINTNEXTLINE(bugprone-exception-escape)
			del(pattern,
				[rd, sopts = effective_sopts, static_cache, normalize_path](
					HttpRequestView const &req) -> HttpResponse {
					try {
						auto norm = normalize_path(req.params["file"]);
						if (!norm) {
							return HttpResponse::html(
								"<html><body><h1>403 Forbidden</h1></body></html>",
								kHttpForbidden,
								"Forbidden");
						}
						auto full_path = rd + *norm;

						if (auto *fr = current_file_reader(); fr != nullptr) {
							auto dr = make_shared<DeferredResponse>();
							auto fp = make_shared<S>(full_path);
							co_spawn(do_delete_file(dr, fp, static_cache, fr->unlink_async(AT_FDCWD, full_path)));
							return HttpResponse::deferred(move(dr));
						}

						if (sopts.offload_pool) {
							auto dr = make_shared<DeferredResponse>();
							auto ok =
								sopts.offload_pool->enqueue([full_path = move(full_path), static_cache, dr]() mutable {
									try {
										if (::unlink(full_path.c_str()) != 0) {
											dr->complete(
												errno == ENOENT ? HttpResponse::not_found(full_path) :
																  HttpResponse::internal_error());
											return;
										}
										static_cache->evict_all_encodings(full_path);
										dr->complete(
											HttpResponse::no_content());
									} catch (...) { dr->complete(HttpResponse::internal_error()); }
								});
							if (!ok) {
								return HttpResponse::internal_error("offload queue full");
							}
							return HttpResponse::deferred(move(dr));
						}

						if (::unlink(full_path.c_str()) != 0) {
							return errno == ENOENT ? HttpResponse::not_found(*norm) : HttpResponse::internal_error();
						}
						static_cache->evict_all_encodings(full_path);
						return HttpResponse::no_content();
					} catch (...) { return HttpResponse::internal_error(); }
				});
		}

		return *this;
	}

	[[nodiscard]] HttpResponse dispatch(
		HttpRequest const &req) const {
		HttpRequestView const req_view{req};
		return dispatch(req_view);
	}

	[[nodiscard]] HttpResponse dispatch(
		HttpRequestView const &req) const {
		// HEAD is dispatched as GET; response body is suppressed before sending.
		bool const is_head = (req.method == "HEAD");

		// Strip query S before matching.
		auto path_sv = SV{req.path};
		if (auto q = path_sv.find('?'); q != SV::npos) {
			path_sv = path_sv.substr(0, q);
		}

		// Inner handler: performs route matching + 404. Middleware wraps this whole thing.
		Handler inner = [this, path_sv, is_head](HttpRequestView const &r) -> HttpResponse {
			try {
				HttpFieldsView matched_params;

				// Regular routes first.
				for (auto const &route: impl_->routes) {
					if (route.method != r.method && !(is_head && route.method == "GET")) {
						continue;
					}
					matched_params.clear();
					if (match_segments(route.pattern, path_sv, matched_params)) {
						auto all_params = r.params;
						for (auto const &[k, v]: matched_params) {
							if (!all_params.get(k)) {
								all_params.emplace_back(k, v);
							}
						}
						// HEAD matched to a GET route: present as GET so handlers are HEAD-transparent.
						SV const effective_method = (is_head && route.method == "GET") ? SV{"GET"} : r.method;
						HttpRequestView const matched_view{
							effective_method,
							r.path,
							r.version,
							r.remote_addr,
							r.is_tls,
							move(all_params),
							r.headers,
							r.query,
							r.form,
							r.cookies,
							r.files,
							r.body};
						try {
							auto resp = route.handler(matched_view);
							if (is_head) {
								resp.head_only = true;
							}
							return resp;
						} catch (exception const &ex) {
							return impl_->error_handler ? impl_->error_handler(matched_view, ex) :
														  HttpResponse::internal_error(ex.what());
						} catch (...) {
							return impl_->error_handler ? impl_->error_handler(matched_view, RE{"unknown exception"}) :
														  HttpResponse::internal_error();
						}
					}
				}

				// SSE routes (GET only).
				if (r.method == "GET") {
					for (auto const &route: impl_->sse_routes) {
						matched_params.clear();
						if (match_segments(route.pattern, path_sv, matched_params)) {
							auto channel = make_shared<SseChannel>();
							HttpRequest matched = r.to_owned();
							for (auto &[k, v]: matched_params) {
								matched.params.emplace_back(S{k}, S{v});
							}
							launch_sse_handler(impl_->work_pool, route.handler, move(matched), channel);
							return HttpResponse::sse(move(channel));
						}
					}
				}

				if (impl_->not_found_handler) {
					return impl_->not_found_handler(r);
				}
				return HttpResponse::not_found(path_sv);
			} catch (...) { return HttpResponse::internal_error(); }
		};

		return wrap_middlewares(move(inner))(req);
	}

private:
	template<class>
	static constexpr bool kDependentFalse = false;

	[[nodiscard]] static HttpResponse run_async_http_task(
		SP<WorkPool> const &pool,
		conflux::work::root::Task<HttpResponse> task) {
		if (!pool) {
			return HttpResponse::internal_error("async handler requires a work pool");
		}
		auto deferred = make_shared<DeferredResponse>();
		bool const enqueued = pool->enqueue([deferred, task = move(task)]() mutable {
			try {
				deferred->complete(conflux::work::root::value(move(task)));
			} catch (exception const &ex) { deferred->complete(HttpResponse::internal_error(ex.what())); } catch (...) {
				deferred->complete(HttpResponse::internal_error());
			}
		});
		if (!enqueued) {
			return HttpResponse::internal_error("offload queue full");
		}
		return HttpResponse::deferred(move(deferred));
	}

	template<typename F>
	static Handler make_handler(
		SP<WorkPool> const &pool,
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &>) {
			using Ret = std::invoke_result_t<Fn &, HttpRequestView const &>;
			if constexpr (std::same_as<Ret, HttpResponse>) {
				return Handler{forward<F>(fn)};
			} else if constexpr (std::same_as<Ret, conflux::work::root::Task<HttpResponse>>) {
				return Handler{
					[wrapped = Fn(forward<F>(fn)), pool](HttpRequestView const &req) mutable -> HttpResponse {
						return run_async_http_task(pool, invoke(wrapped, req));
					}};
			} else {
				static_assert(
					kDependentFalse<Fn>,
					"Handler returning HttpRequestView const& must return HttpResponse or root::Task<HttpResponse>");
			}
		} else if constexpr (std::invocable<Fn &, HttpRequest const &>) {
			using Ret = std::invoke_result_t<Fn &, HttpRequest const &>;
			if constexpr (std::same_as<Ret, HttpResponse>) {
				return Handler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return invoke(wrapped, owned);
				}};
			} else if constexpr (std::same_as<Ret, conflux::work::root::Task<HttpResponse>>) {
				return Handler{
					[wrapped = Fn(forward<F>(fn)), pool](HttpRequestView const &req) mutable -> HttpResponse {
						auto owned = req.to_owned();
						return run_async_http_task(pool, invoke(wrapped, owned));
					}};
			} else {
				static_assert(
					kDependentFalse<Fn>,
					"Handler returning HttpRequest const& must return HttpResponse or root::Task<HttpResponse>");
			}
		} else {
			static_assert(kDependentFalse<Fn>, "Handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}

	template<typename F>
	static Middleware make_middleware(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, Handler const &>) {
			return Middleware{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, Handler const &>) {
			return Middleware{
				[wrapped =
					 Fn(forward<F>(fn))](HttpRequestView const &req, Handler const &next) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return invoke(wrapped, owned, next);
				}};
		} else {
			static_assert(kDependentFalse<Fn>, "Middleware must accept HttpRequestView const& or HttpRequest const&");
		}
	}

	template<typename F>
	static SseHandler make_sse_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, SP<SseChannel>>) {
			return SseHandler{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, SP<SseChannel>>) {
			return SseHandler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req, SP<SseChannel> ch) mutable {
				auto owned = req.to_owned();
				invoke(wrapped, owned, move(ch));
			}};
		} else {
			static_assert(kDependentFalse<Fn>, "SSE handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}

	template<typename F>
	static WsHandler make_ws_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, WsConn &>) {
			return WsHandler{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, WsConn &>) {
			return WsHandler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req, WsConn &ws) mutable {
				auto owned = req.to_owned();
				invoke(wrapped, owned, ws);
			}};
		} else {
			static_assert(
				kDependentFalse<Fn>,
				"WebSocket handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}

	template<typename F>
	static ErrorHandler make_error_handler(
		F &&fn) {
		using Fn = std::decay_t<F>;
		if constexpr (std::invocable<Fn &, HttpRequestView const &, exception const &>) {
			return ErrorHandler{forward<F>(fn)};
		} else if constexpr (std::invocable<Fn &, HttpRequest const &, exception const &>) {
			return ErrorHandler{
				[wrapped =
					 Fn(forward<F>(fn))](HttpRequestView const &req, exception const &ex) mutable -> HttpResponse {
					auto owned = req.to_owned();
					return invoke(wrapped, owned, ex);
				}};
		} else {
			static_assert(
				kDependentFalse<Fn>,
				"Error handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}

	struct Impl {
		struct Route {
			S method{};
			V<Segment> pattern{};
			Handler handler{};
		};
		struct SseRoute {
			V<Segment> pattern{};
			SseHandler handler{};
		};
		V<Route> routes{};
		V<SseRoute> sse_routes{};
		V<Middleware> middlewares{};
		Handler not_found_handler{};
		ErrorHandler error_handler{};
		SP<WorkPool> work_pool{make_shared<WorkPool>()};
		StaticFileCacheConfig static_file_cache{};
	};
	UP<Impl> impl_;

	static void launch_sse_handler(
		SP<WorkPool> const &pool,
		SseHandler handler,
		HttpRequest matched,
		SP<SseChannel> const &channel) {
		if (!pool->enqueue([h = move(handler), matched = move(matched), channel]() mutable {
				HttpRequestView const matched_view{matched};
				h(matched_view, channel);
				channel->close();
			})) {
			channel->close();
		}
	}

	// Wrap h in the registered middleware chain. First-registered runs outermost.
	[[nodiscard]] Handler wrap_middlewares(
		Handler h) const {
		return [this, h = move(h)](HttpRequestView const &req) -> HttpResponse {
			struct Step {
				Router::Impl const *impl_;
				Handler const *h_;
				size_t idx_{0};
				HttpResponse call(
					HttpRequestView const &r) {
					if (idx_ == impl_->middlewares.size()) {
						return (*h_)(r);
					}
					auto const &mw = impl_->middlewares[idx_++];
					return mw(r, [this](HttpRequestView const &rr) -> HttpResponse { return call(rr); });
				}
			};
			Step s{impl_.get(), &h};
			return s.call(req);
		};
	}
};

// ---------------------------------------------------------------------------
// SseBroadcaster: fan-out pub/sub for SSE streams.
// ---------------------------------------------------------------------------
// Maintains a set of active SseChannel weak_ptrs.  broadcast() delivers an
// SSE event to every currently-connected subscriber.  Stale weak_ptrs are
// reaped on each broadcast call.
export class SseBroadcaster {
public:
	SseBroadcaster() = default;
	~SseBroadcaster() = default;
	SseBroadcaster(SseBroadcaster const &) = delete;
	SseBroadcaster &operator =(SseBroadcaster const &) = delete;
	SseBroadcaster(SseBroadcaster &&) = delete;
	SseBroadcaster &operator =(SseBroadcaster &&) = delete;

	// Register a new subscriber.  Returns the SP to pass to HttpResponse::sse().
	SP<SseChannel> subscribe() {
		auto ch = make_shared<SseChannel>();
		SL const lk{mtx_};
		channels_.emplace_back(ch);
		return ch;
	}

	// Broadcast an SSE event to all active subscribers.
	void broadcast(
		SV event,
		SV data) {
		auto frame = format("event: {}\ndata: {}\n\n", event, data);
		broadcast_raw(frame);
	}

	// Broadcast a data-only SSE message to all active subscribers.
	void broadcast_data(
		SV data) {
		auto frame = format("data: {}\n\n", data);
		broadcast_raw(frame);
	}

	// Number of currently-active subscribers (approximate; may include ones
	// that have just disconnected).
	[[nodiscard]] SZ subscriber_count() const {
		SL const lk{mtx_};
		return channels_.size();
	}

private:
	void broadcast_raw(
		S const &frame) {
		SL const lk{mtx_};
		// Erase stale weak_ptrs while delivering to live ones.
		erase_if(channels_, [&](weak_ptr<SseChannel> const &wch) {
			auto ch = wch.lock();
			if (!ch || ch->is_closed()) {
				return true;
			}
			ch->send(frame);
			return false;
		});
	}

	mutable mutex mtx_;
	V<weak_ptr<SseChannel>> channels_;
};

// Returns a middleware that formats each request as:
//   [ISO8601] METHOD path status bytes elapsed_ms
// and passes the formatted line to `sink`. Thread-safety of `sink` is
// the caller's responsibility.
export Router::Middleware make_access_log_middleware(
	Fn<void(S const &)> sink) {
	return [sink = move(sink)](HttpRequestView const &req, Router::Handler const &next) {
		auto const t0 = chrono::steady_clock::now();
		auto resp = next(req);
		auto const elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();

		auto const now = chrono::system_clock::now();
		auto const tt = chrono::system_clock::to_time_t(now);
		A<char, 32> ts_buf{};
		SV ts{};
		if (strftime(ts_buf.data(), ts_buf.size(), "%Y-%m-%dT%H:%M:%SZ", gmtime(&tt)) > 0) {
			ts = ts_buf.data();
		}

		sink(format("[{}] {} {} {} {} {}ms", ts, req.method, req.path, resp.status, resp.text_body().size(), elapsed));
		return resp;
	};
}
