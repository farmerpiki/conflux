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
using namespace std;

[[nodiscard]] string html_escape(
	string_view s) {
	string out;
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

export struct FieldHash {
	using is_transparent = void;
	bool ci{false};

	[[nodiscard]] size_t operator ()(
		string_view s) const noexcept {
		size_t h = 14695981039346656037ULL;
		for (char const ch: s) {
			auto const c = static_cast<unsigned char>(ch);
			unsigned char const k = ci ? static_cast<unsigned char>(c | 0x20) : c;
			h ^= k;
			h *= 1099511628211ULL;
		}
		return h;
	}
	[[nodiscard]] size_t operator ()(
		string const &s) const noexcept {
		return operator ()(string_view{s});
	}
};

export struct FieldEq {
	using is_transparent = void;
	bool ci{false};

	[[nodiscard]] bool operator ()(
		string_view a,
		string_view b) const noexcept {
		if (a.size() != b.size()) {
			return false;
		}
		if (!ci) {
			return a == b;
		}
		return ranges::equal(a, b, [](unsigned char x, unsigned char y) { return (x | 0x20) == (y | 0x20); });
	}
	[[nodiscard]] bool operator ()(
		string const &a,
		string_view b) const noexcept {
		return operator ()(string_view{a}, b);
	}
	[[nodiscard]] bool operator ()(
		string_view a,
		string const &b) const noexcept {
		return operator ()(a, string_view{b});
	}
	[[nodiscard]] bool operator ()(
		string const &a,
		string const &b) const noexcept {
		return operator ()(string_view{a}, string_view{b});
	}
};

// Vector-backed string map: operator[] returns string_view (empty if absent).
// Preferred over unordered_map for small/bounded collections — contiguous storage,
// no hashing, linear scan faster for N < ~100.
export class HttpFields {
	vector<pair<string, string>> data_;
	unordered_multimap<string, size_t, FieldHash, FieldEq> index_;
	bool case_insensitive_{false};

	void index_entry(
		size_t i) {
		index_.emplace(data_[i].first, i);
	}

public:
	HttpFields(
		bool case_insensitive = false)
		: index_(0, FieldHash{case_insensitive}, FieldEq{case_insensitive})
		, case_insensitive_(case_insensitive) {}
	HttpFields(
		initializer_list<pair<string, string>> init)
		: data_(init) {
		for (size_t i = 0; i < data_.size(); ++i) {
			index_entry(i);
		}
	}

	// Read: returns empty view when key absent.
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	string_view operator [](
		string_view key) const noexcept {
		return get(key).value_or(string_view{});
	}

	// Write: inserts key with empty value when absent, then returns ref.
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	string &operator [](
		string_view key) {
		auto range = index_.equal_range(key);
		if (range.first != range.second) {
			return data_[range.first->second].second;
		}
		data_.emplace_back(string{key}, string{});
		index_entry(data_.size() - 1);
		return data_.back().second;
	}

	[[nodiscard]] optional<string_view> get(
		string_view key) const noexcept {
		auto range = index_.equal_range(key);
		if (range.first == range.second) {
			return nullopt;
		}
		return string_view{data_[range.first->second].second};
	}

	[[nodiscard]] vector<string_view> values(
		string_view key) const {
		vector<string_view> out;
		auto range = index_.equal_range(key);
		for (auto it = range.first; it != range.second; ++it) {
			out.push_back(data_[it->second].second);
		}
		return out;
	}

	[[nodiscard]] bool contains(
		string_view key) const noexcept {
		auto range = index_.equal_range(key);
		return range.first != range.second;
	}

	[[nodiscard]] string_view value_or(
		string_view key,
		string_view def = {}) const noexcept {
		return get(key).value_or(def);
	}

	void emplace_back(
		string k,
		string v) {
		data_.emplace_back(move(k), move(v));
		index_entry(data_.size() - 1);
	}

	void append(
		string k,
		string v) {
		emplace_back(move(k), move(v));
	}

	void set(
		string key,
		string field_value) {
		auto range = index_.equal_range(string_view{key});
		if (range.first == range.second) {
			emplace_back(move(key), move(field_value));
			return;
		}
		auto idx_view =
			ranges::subrange(range.first, range.second) | views::transform([](auto const &e) { return e.second; });
		auto const keep_idx = ranges::min(idx_view);
		data_[keep_idx].second = move(field_value);
		unordered_set<size_t> to_remove;
		ranges::copy_if(idx_view, inserter(to_remove, to_remove.end()), [&](size_t i) { return i != keep_idx; });
		if (to_remove.empty()) {
			return;
		}
		size_t cursor = 0;
		erase_if(data_, [&](auto const &) { return to_remove.contains(cursor++); });
		index_.clear();
		for (size_t i = 0; i < data_.size(); ++i) {
			index_entry(i);
		}
	}

	size_t erase(
		string_view key) {
		auto range = index_.equal_range(key);
		if (range.first == range.second) {
			return 0;
		}
		unordered_set<size_t> to_remove;
		ranges::copy(
			ranges::subrange(range.first, range.second) | views::transform([](auto const &e) { return e.second; }),
			inserter(to_remove, to_remove.end()));
		size_t cursor = 0;
		size_t const removed = erase_if(data_, [&](auto const &) { return to_remove.contains(cursor++); });
		index_.clear();
		for (size_t i = 0; i < data_.size(); ++i) {
			index_entry(i);
		}
		return removed;
	}
	void clear() noexcept {
		data_.clear();
		index_.clear();
	}
	[[nodiscard]] bool empty() const noexcept { return data_.empty(); }
	[[nodiscard]] size_t size() const noexcept { return data_.size(); }
	[[nodiscard]] bool case_insensitive() const noexcept { return case_insensitive_; }

	auto begin() { return data_.begin(); }
	auto end() { return data_.end(); }
	[[nodiscard]] auto begin() const { return data_.begin(); }
	[[nodiscard]] auto end() const { return data_.end(); }
};

export class HttpFieldsView {
	vector<pair<string_view, string_view>> data_;
	unordered_multimap<string_view, size_t, FieldHash, FieldEq> index_;
	shared_ptr<deque<string>> owned_storage_{};
	bool case_insensitive_{false};

	void index_entry(
		size_t i) {
		index_.emplace(data_[i].first, i);
	}

	[[nodiscard]] string_view store_owned(
		string owned_value) {
		if (!owned_storage_) {
			owned_storage_ = make_shared<deque<string>>();
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

	string_view operator [](
		string_view key) const noexcept {
		return get(key).value_or(string_view{});
	}

	[[nodiscard]] optional<string_view> get(
		string_view key) const noexcept {
		auto range = index_.equal_range(key);
		if (range.first == range.second) {
			return nullopt;
		}
		return data_[range.first->second].second;
	}

	[[nodiscard]] vector<string_view> values(
		string_view key) const {
		vector<string_view> out;
		auto range = index_.equal_range(key);
		for (auto it = range.first; it != range.second; ++it) {
			out.push_back(data_[it->second].second);
		}
		return out;
	}

	[[nodiscard]] bool contains(
		string_view key) const noexcept {
		auto range = index_.equal_range(key);
		return range.first != range.second;
	}

	[[nodiscard]] string_view value_or(
		string_view key,
		string_view def = {}) const noexcept {
		return get(key).value_or(def);
	}

	void emplace_back(
		string_view k,
		string_view v) {
		data_.emplace_back(k, v);
		index_entry(data_.size() - 1);
	}

	void emplace_back_owned(
		string k,
		string v) {
		data_.emplace_back(store_owned(move(k)), store_owned(move(v)));
		index_entry(data_.size() - 1);
	}

	void clear() noexcept {
		data_.clear();
		index_.clear();
		owned_storage_.reset();
	}

	[[nodiscard]] bool empty() const noexcept { return data_.empty(); }
	[[nodiscard]] size_t size() const noexcept { return data_.size(); }

	[[nodiscard]] HttpFields to_owned() const {
		HttpFields out{case_insensitive_};
		for (auto const &[k, v]: data_) {
			out.emplace_back(string{k}, string{v});
		}
		return out;
	}

	auto begin() { return data_.begin(); }
	auto end() { return data_.end(); }
	[[nodiscard]] auto begin() const { return data_.begin(); }
	[[nodiscard]] auto end() const { return data_.end(); }
};

export struct UploadedFile {
	string_view name;
	string_view filename;
	string_view content_type;
	string_view data;
	string owned_name;
	string owned_filename;
	string owned_content_type;
	string owned_data;
	bool owns_metadata = false;
	bool owns_data = false;

	UploadedFile() = default;

	UploadedFile(
		string_view name_,
		string_view filename_,
		string_view content_type_,
		string_view data_)
		: name(name_)
		, filename(filename_)
		, content_type(content_type_)
		, data(data_) {}

	UploadedFile(
		UploadedFile const &other)
		: owned_name(other.owns_metadata ? other.owned_name : string{})
		, owned_filename(other.owns_metadata ? other.owned_filename : string{})
		, owned_content_type(other.owns_metadata ? other.owned_content_type : string{})
		, owned_data(other.owns_data ? other.owned_data : string{})
		, owns_metadata(other.owns_metadata)
		, owns_data(other.owns_data) {
		name = owns_metadata ? string_view{owned_name} : other.name;
		filename = owns_metadata ? string_view{owned_filename} : other.filename;
		content_type = owns_metadata ? string_view{owned_content_type} : other.content_type;
		data = owns_data ? string_view{owned_data} : other.data;
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
		owned_name = other.owns_metadata ? other.owned_name : string{};
		owned_filename = other.owns_metadata ? other.owned_filename : string{};
		owned_content_type = other.owns_metadata ? other.owned_content_type : string{};
		owned_data = other.owns_data ? other.owned_data : string{};
		owns_metadata = other.owns_metadata;
		owns_data = other.owns_data;
		name = owns_metadata ? string_view{owned_name} : other.name;
		filename = owns_metadata ? string_view{owned_filename} : other.filename;
		content_type = owns_metadata ? string_view{owned_content_type} : other.content_type;
		data = owns_data ? string_view{owned_data} : other.data;
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
		string_view name_,
		string_view filename_,
		string_view content_type_,
		string_view data_) {
		return UploadedFile{name_, filename_, content_type_, data_};
	}

	[[nodiscard]] static UploadedFile owned(
		string name_,
		string filename_,
		string content_type_,
		string data_) {
		UploadedFile file{string_view{}, string_view{}, string_view{}, string_view{}};
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
		return UploadedFile::owned(string{name}, string{filename}, string{content_type}, string{data});
	}
};

export struct HttpRequest {
	string method;
	string path; // path only, no query string
	string version;
	string remote_addr; // peer IP address (best-effort with multishot accept)
	bool is_tls = false; // true when request arrived over a TLS connection
	HttpFields params; // {name} captures
	HttpFields headers{true}; // case-insensitive lookup
	HttpFields query; // parsed from URL ?k=v&...
	HttpFields form; // parsed from application/x-www-form-urlencoded body or multipart text fields
	HttpFields cookies; // parsed from Cookie: header
	vector<UploadedFile> files; // parsed from multipart/form-data body
	string body;

	[[nodiscard]] HttpRequest to_owned() const { return *this; }
};

export struct HttpRequestView {
	string_view method;
	string_view path;
	string_view version;
	string_view remote_addr;
	bool is_tls = false;
	HttpFieldsView params;
	HttpFieldsView headers;
	HttpFieldsView query;
	HttpFieldsView form;
	HttpFieldsView cookies;
	span<UploadedFile const> files;
	string_view body;

	HttpRequestView(
		string_view method_,
		string_view path_,
		string_view version_,
		string_view remote_addr_,
		bool is_tls_,
		HttpFieldsView params_,
		HttpFieldsView headers_,
		HttpFieldsView query_,
		HttpFieldsView form_,
		HttpFieldsView cookies_,
		span<UploadedFile const> files_,
		string_view body_)
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
		owned.method = string{method};
		owned.path = string{path};
		owned.version = string{version};
		owned.remote_addr = string{remote_addr};
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
		owned.body = string{body};
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
		[[nodiscard]] virtual unique_ptr<Concept> clone() const = 0;
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

		[[nodiscard]] unique_ptr<Concept> clone() const override { return make_unique<Model>(fn); }
	};

	unique_ptr<Concept> fn_{};

public:
	CloneableFunction() = default;
	CloneableFunction(
		nullptr_t) {}

	template<typename F>
		requires(!same_as<remove_cvref_t<F>, CloneableFunction>)
	CloneableFunction(
		F &&f)
		: fn_(make_unique<Model<remove_cvref_t<F>>>(forward<F>(f))) {}

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
	size_t size{}; // mmap extent for munmap
	size_t send_offset{}; // byte offset within ptr for send start
	size_t send_size{}; // bytes to send
	MappedFile(
		void *p,
		size_t s)
		: ptr{p}
		, size{s}
		, send_size{s} {}
	MappedFile(
		void *p,
		size_t mmap_sz,
		size_t offset,
		size_t len)
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
	shared_ptr<FileHandle> handle;
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
	queue<string> pending_{};
	atomic_flag closed_{};
	size_t queued_bytes_{0};
	size_t max_queue_bytes_{};
	SseOverflowPolicy overflow_{SseOverflowPolicy::DropNewest};
	atomic<size_t> dropped_{0};

public:
	static constexpr size_t kDefaultMaxQueueBytes = size_t{4} * 1024 * 1024;

	SseChannel(SseChannel const &) = delete;
	SseChannel &operator =(SseChannel const &) = delete;
	SseChannel(SseChannel &&) = delete;
	SseChannel &operator =(SseChannel &&) = delete;
	explicit SseChannel(
		size_t max_queue_bytes = kDefaultMaxQueueBytes,
		SseOverflowPolicy overflow = SseOverflowPolicy::DropNewest)
		: efd_(::eventfd(0, EFD_CLOEXEC))
		, max_queue_bytes_(max_queue_bytes)
		, overflow_(overflow) {
		if (efd_ < 0) {
			throw system_error{errno, system_category(), "eventfd"};
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
	bool send(
		string frame) {
		bool enqueued = false;
		bool wake = false;
		{
			scoped_lock const lk{mtx_};
			if (closed_.test()) {
				return false;
			}
			size_t const frame_bytes = frame.size();
			size_t const would_be = queued_bytes_ + frame_bytes;
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

	bool send_event(
		string_view type,
		string_view data) {
		// Reject newlines in type and data: they would break SSE framing and
		// allow injection of arbitrary events.
		auto has_nl = [](string_view s) {
			return s.find('\n') != string_view::npos || s.find('\r') != string_view::npos;
		};
		if (has_nl(type) || has_nl(data)) {
			throw invalid_argument{"SseChannel::send_event: type and data must not contain newlines"};
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

	[[nodiscard]] string drain() {
		scoped_lock const lk{mtx_};
		string result;
		while (!pending_.empty()) {
			result += pending_.front();
			pending_.pop();
		}
		queued_bytes_ = 0;
		return result;
	}

	[[nodiscard]] bool is_closed() const noexcept { return closed_.test(); }
	[[nodiscard]] int eventfd_fd() const noexcept { return efd_; }
	[[nodiscard]] size_t dropped_count() const noexcept { return dropped_.load(memory_order_relaxed); }
	[[nodiscard]] size_t max_queue_bytes() const noexcept { return max_queue_bytes_; }
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

	using BodyPayload = variant<
		string,
		shared_ptr<SseChannel>,
		shared_ptr<WsUpgrade>,
		shared_ptr<MappedFile>,
		shared_ptr<StreamedFile>,
		shared_ptr<DeferredResponse>>;

	int status = kHttpOk;
	string status_text = "OK";
	string content_type = "text/html; charset=utf-8";
	HttpFields headers{true}; // extra response headers (added after Content-Type/Content-Length)
	vector<string> set_cookies{}; // Set-Cookie headers (one per entry)
	HttpFields trailers{true}; // HTTP/2 trailer headers sent after the DATA frames
	bool head_only = false; // true → send headers only, suppress body (HEAD requests)
	size_t content_length_hint{0}; // non-zero overrides content_length() (HEAD static file responses)
	BodyKind body_kind = BodyKind::text;
	BodyPayload body_payload{string{}};

	[[nodiscard]] bool is_text() const noexcept { return body_kind == BodyKind::text; }
	[[nodiscard]] bool is_sse() const noexcept { return body_kind == BodyKind::sse; }
	[[nodiscard]] bool is_ws_upgrade() const noexcept { return body_kind == BodyKind::ws_upgrade; }
	[[nodiscard]] bool is_mapped_file() const noexcept { return body_kind == BodyKind::mapped_file; }
	[[nodiscard]] bool is_streamed_file() const noexcept { return body_kind == BodyKind::streamed_file; }
	[[nodiscard]] bool is_deferred() const noexcept { return body_kind == BodyKind::deferred; }

	[[nodiscard]] string_view text_body() const noexcept {
		if (auto const *text = get_if<string>(&body_payload)) {
			return *text;
		}
		return {};
	}

	[[nodiscard]] string &text_body_mut() {
		if (!is_text() || !holds_alternative<string>(body_payload)) {
			body_kind = BodyKind::text;
			body_payload = string{};
		}
		return get<string>(body_payload);
	}

	[[nodiscard]] string take_text_body() {
		if (!holds_alternative<string>(body_payload)) {
			return {};
		}
		return move(get<string>(body_payload));
	}

	[[nodiscard]] shared_ptr<SseChannel> const &sse_channel_ptr() const {
		static shared_ptr<SseChannel> const empty{};
		if (auto const *ch = get_if<shared_ptr<SseChannel>>(&body_payload)) {
			return *ch;
		}
		return empty;
	}

	[[nodiscard]] shared_ptr<WsUpgrade> const &ws_upgrade_ptr() const {
		static shared_ptr<WsUpgrade> const empty{};
		if (auto const *up = get_if<shared_ptr<WsUpgrade>>(&body_payload)) {
			return *up;
		}
		return empty;
	}

	[[nodiscard]] shared_ptr<MappedFile> const &mapped_file_ptr() const {
		static shared_ptr<MappedFile> const empty{};
		if (auto const *file = get_if<shared_ptr<MappedFile>>(&body_payload)) {
			return *file;
		}
		return empty;
	}

	[[nodiscard]] shared_ptr<StreamedFile> const &streamed_file_ptr() const {
		static shared_ptr<StreamedFile> const empty{};
		if (auto const *file = get_if<shared_ptr<StreamedFile>>(&body_payload)) {
			return *file;
		}
		return empty;
	}

	[[nodiscard]] shared_ptr<DeferredResponse> const &deferred_response_ptr() const {
		static shared_ptr<DeferredResponse> const empty{};
		if (auto const *deferred = get_if<shared_ptr<DeferredResponse>>(&body_payload)) {
			return *deferred;
		}
		return empty;
	}

	[[nodiscard]] shared_ptr<SseChannel> take_sse_channel() {
		if (!holds_alternative<shared_ptr<SseChannel>>(body_payload)) {
			return {};
		}
		return move(get<shared_ptr<SseChannel>>(body_payload));
	}

	[[nodiscard]] shared_ptr<WsUpgrade> take_ws_upgrade() {
		if (!holds_alternative<shared_ptr<WsUpgrade>>(body_payload)) {
			return {};
		}
		return move(get<shared_ptr<WsUpgrade>>(body_payload));
	}

	[[nodiscard]] shared_ptr<MappedFile> take_mapped_file() {
		if (!holds_alternative<shared_ptr<MappedFile>>(body_payload)) {
			return {};
		}
		return move(get<shared_ptr<MappedFile>>(body_payload));
	}

	[[nodiscard]] shared_ptr<StreamedFile> take_streamed_file() {
		if (!holds_alternative<shared_ptr<StreamedFile>>(body_payload)) {
			return {};
		}
		return move(get<shared_ptr<StreamedFile>>(body_payload));
	}

	[[nodiscard]] shared_ptr<DeferredResponse> take_deferred_response() {
		if (!holds_alternative<shared_ptr<DeferredResponse>>(body_payload)) {
			return {};
		}
		return move(get<shared_ptr<DeferredResponse>>(body_payload));
	}

	void set_text_body(
		string text) {
		body_kind = BodyKind::text;
		body_payload = move(text);
	}

	void set_sse_channel(
		shared_ptr<SseChannel> ch) {
		body_kind = BodyKind::sse;
		body_payload = move(ch);
	}

	void set_ws_upgrade(
		shared_ptr<WsUpgrade> up) {
		body_kind = BodyKind::ws_upgrade;
		body_payload = move(up);
	}

	void set_mapped_file(
		shared_ptr<MappedFile> file) {
		body_kind = BodyKind::mapped_file;
		body_payload = move(file);
	}

	void set_streamed_file(
		shared_ptr<StreamedFile> file) {
		body_kind = BodyKind::streamed_file;
		body_payload = move(file);
	}

	void set_deferred_response(
		shared_ptr<DeferredResponse> deferred) {
		body_kind = BodyKind::deferred;
		body_payload = move(deferred);
	}

	[[nodiscard]] size_t content_length() const noexcept {
		if (content_length_hint != 0) {
			return content_length_hint;
		}
		if (is_mapped_file() && mapped_file_ptr()) {
			return mapped_file_ptr()->send_size;
		}
		if (is_streamed_file() && streamed_file_ptr()) {
			return static_cast<size_t>(streamed_file_ptr()->send_size);
		}
		return text_body().size();
	}

	static HttpResponse html(
		string body) {
		HttpResponse r;
		r.status = kHttpOk;
		r.status_text = "OK";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	static HttpResponse html(
		string body,
		int status,
		string status_text) {
		HttpResponse r;
		r.status = status;
		r.status_text = move(status_text);
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	static HttpResponse json(
		string body) {
		HttpResponse r;
		r.status = kHttpOk;
		r.status_text = "OK";
		r.content_type = "application/json";
		r.set_text_body(move(body));
		return r;
	}

	static HttpResponse json(
		string body,
		int status,
		string status_text) {
		HttpResponse r;
		r.status = status;
		r.status_text = move(status_text);
		r.content_type = "application/json";
		r.set_text_body(move(body));
		return r;
	}

	static HttpResponse text(
		string body) {
		HttpResponse r;
		r.status = kHttpOk;
		r.status_text = "OK";
		r.content_type = "text/plain; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	static HttpResponse redirect(
		string location,
		int code = kHttpFound) {
		char const *status_text = "Found";
		switch (code) {
		case kHttpMovedPermanently : status_text = "Moved Permanently"; break;
		case kHttpTemporaryRedirect: status_text = "Temporary Redirect"; break;
		case kHttpPermanentRedirect: status_text = "Permanent Redirect"; break;
		default                    : break;
		}
		HttpResponse r{.status = code, .status_text = status_text, .content_type = "text/html; charset=utf-8"};
		r.headers["Location"] = move(location);
		return r;
	}

	static HttpResponse not_found(
		string_view path) {
		HttpResponse r;
		r.status = kHttpNotFound;
		r.status_text = "Not Found";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(format("<html><body><h1>404 Not Found</h1><p>{}</p></body></html>", html_escape(path)));
		return r;
	}

	static HttpResponse bad_request() {
		HttpResponse r;
		r.status = kHttpBadRequest;
		r.status_text = "Bad Request";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>400 Bad Request</h1></body></html>");
		return r;
	}

	static HttpResponse uri_too_long() {
		HttpResponse r;
		r.status = kHttpUriTooLong;
		r.status_text = "URI Too Long";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>414 URI Too Long</h1></body></html>");
		return r;
	}

	static HttpResponse header_fields_too_large() {
		HttpResponse r;
		r.status = kHttpRequestHeaderFieldsTooLarge;
		r.status_text = "Request Header Fields Too Large";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>431 Request Header Fields Too Large</h1></body></html>");
		return r;
	}

	static HttpResponse gateway_timeout() {
		HttpResponse r;
		r.status = kHttpGatewayTimeout;
		r.status_text = "Gateway Timeout";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>504 Gateway Timeout</h1></body></html>");
		return r;
	}

	static HttpResponse sse(
		shared_ptr<SseChannel> ch) {
		HttpResponse r{.status = kHttpOk, .status_text = "OK", .content_type = "text/event-stream"};
		r.set_sse_channel(move(ch));
		return r;
	}

	static HttpResponse deferred(
		shared_ptr<DeferredResponse> response) {
		HttpResponse r;
		r.set_deferred_response(move(response));
		return r;
	}

	static HttpResponse internal_error(
		string_view detail = {}) {
		auto body =
			detail.empty() ?
				string{"<html><body><h1>500 Internal Server Error</h1></body></html>"} :
				format("<html><body><h1>500 Internal Server Error</h1><p>{}</p></body></html>", html_escape(detail));
		HttpResponse r;
		r.status = 500;
		r.status_text = "Internal Server Error";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body(move(body));
		return r;
	}

	// Append a Set-Cookie header. Attributes are optional; pass empty strings to omit.
	// Example: resp.set_cookie("session", "abc123", "Path=/; HttpOnly; SameSite=Lax")
	HttpResponse &set_cookie(
		string_view name,
		string_view cookie_value,
		string_view attributes = {}) {
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
	unique_ptr<HttpResponse> ready_{};
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
	[[nodiscard]] optional<HttpResponse> take_ready();
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
		throw system_error{errno, system_category(), "eventfd"};
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
		scoped_lock const lk{mtx_};
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
	scoped_lock const lk{mtx_};
	return ready_ != nullptr;
}

optional<HttpResponse> DeferredResponse::take_ready() {
	scoped_lock const lk{mtx_};
	if (!ready_) {
		return nullopt;
	}
	auto response = move(*ready_);
	ready_.reset();
	return response;
}

chrono::steady_clock::time_point DeferredResponse::deadline() const {
	scoped_lock const lk{mtx_};
	return deadline_;
}

void DeferredResponse::set_deadline(
	chrono::steady_clock::time_point deadline) {
	scoped_lock const lk{mtx_};
	deadline_ = deadline;
}

bool DeferredResponse::expire_if_past_deadline(
	chrono::steady_clock::time_point now) {
	{
		scoped_lock const lk{mtx_};
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
CONFLUX_FUZZ_EXPORT string ws_accept_key(
	string_view client_key) {
	static constexpr string_view kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	string input{client_key};
	input += kMagic;
	auto digest = sha1(span{reinterpret_cast<unsigned char const *>(input.data()), input.size()});
	return base64_encode(span{digest.data(), digest.size()});
}

bool ascii_iequals(
	string_view lhs,
	string_view rhs) noexcept {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (size_t i = 0; i < lhs.size(); ++i) {
		auto const l = static_cast<unsigned char>(lhs[i]);
		auto const r = static_cast<unsigned char>(rhs[i]);
		if ((l | 0x20U) != (r | 0x20U)) {
			return false;
		}
	}
	return true;
}

bool header_token_contains(
	string_view header,
	string_view token) noexcept {
	while (!header.empty()) {
		auto comma = header.find(',');
		auto part = trim((comma == string_view::npos) ? header : header.substr(0, comma));
		if (ascii_iequals(part, token)) {
			return true;
		}
		if (comma == string_view::npos) {
			return false;
		}
		header.remove_prefix(comma + 1);
	}
	return false;
}

bool is_valid_client_key(
	string_view key) {
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
string ws_build_frame(
	u8 opcode,
	span<byte const> payload) {
	array<u8, 10> hdr{};
	size_t hdr_len = 0;
	hdr[hdr_len++] = 0x80U | opcode; // FIN + opcode
	size_t const len = payload.size();
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
	string frame;
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
	size_t sent = 0;
	while (sent < frame.size()) {
		auto n = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		sent += static_cast<size_t>(n);
	}
	return true;
}

#if CONFLUX_HAS_TLS
bool ws_tls_send_frame(
	SSL *ssl,
	u8 opcode,
	span<byte const> payload) {
	auto frame = ws_detail::ws_build_frame(opcode, payload);
	size_t sent = 0;
	while (sent < frame.size()) {
		auto const chunk = min<size_t>(frame.size() - sent, static_cast<size_t>(numeric_limits<int>::max()));
		int const n = SSL_write(ssl, frame.data() + sent, static_cast<int>(chunk));
		if (n > 0) {
			sent += static_cast<size_t>(n);
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
	string_view s) {
	size_t i = 0;
	while (i < s.size()) {
		auto const b = static_cast<u8>(s[i]);
		size_t extra{};
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
		for (size_t k = 1; k <= extra; ++k) {
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
	array<u8, 4> mask{};
	size_t header_size{};
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

	size_t off = 2;
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
		for (size_t i = 0; i < 8; ++i) {
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
		string payload;
	};

	WsConn(WsConn const &) = delete;
	WsConn &operator =(WsConn const &) = delete;
	WsConn(WsConn &&) = delete;
	WsConn &operator =(WsConn &&) = delete;

	explicit WsConn(
		int fd,
		string initial_buf = {})
		: fd_(fd)
		, buf_(move(initial_buf)) {}
#if CONFLUX_HAS_TLS
	// TLS variant: ssl must already have the handshake complete and be wired to
	// a socket BIO (SSL_set_fd called by the server before handing off).
	// initial_buf carries any plaintext bytes already decrypted before handoff.
	explicit WsConn(
		int fd,
		SSL *ssl,
		string initial_buf)
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

	optional<Frame> recv() {
		while (true) {
			if (!fill(2)) {
				return nullopt;
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
				return nullopt;
			}
			if (pre == ws_detail::FrameParseStatus::ControlTooLarge) {
				close(1002, "invalid control frame");
				return nullopt;
			}
			// pre is Ok (no extended length) or Incomplete (need extended length + mask).
			auto const b1 = static_cast<u8>(buf_[1]);
			u64 const len7 = b1 & 0x7FU;
			size_t const header_needed = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0) + 4;
			if (!fill(header_needed)) {
				return nullopt;
			}
			auto const status = ws_detail::parse_frame_header(as_bytes(span{buf_.data(), header_needed}), hdr);
			if (status != ws_detail::FrameParseStatus::Ok) {
				if (status == ws_detail::FrameParseStatus::ProtocolError) {
					close(1002, "invalid frame header");
				} else if (status == ws_detail::FrameParseStatus::ControlTooLarge) {
					close(1002, "invalid control frame");
				}
				return nullopt;
			}
			consume(hdr.header_size);

			bool const fin = hdr.fin;
			u8 const opcode_raw = hdr.opcode;
			u64 const plen = hdr.payload_len;
			bool const is_control = (opcode_raw & 0x08U) != 0;
			array<u8, 4> const mask_key = hdr.mask;

			if (plen > kMaxMessageSize) {
				close(1009, "message too big");
				return nullopt;
			}
			if (!is_control && (frag_payload_.size() + plen) > kMaxMessageSize) {
				close(1009, "message too big");
				return nullopt;
			}
			if (!fill(static_cast<size_t>(plen))) {
				return nullopt;
			}
			string payload(buf_.data(), static_cast<size_t>(plen));
			consume(static_cast<size_t>(plen));
			for (size_t i = 0; i < payload.size(); ++i) {
				payload[i] = static_cast<char>(
					static_cast<unsigned char>(payload[i])
					^ mask_key[i & 3]); // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
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
					return nullopt;
				}
				u16 echo_code = 1000;
				if (plen >= 2) {
					echo_code = static_cast<u16>(
						(static_cast<unsigned>(static_cast<u8>(payload[0])) << 8U)
						| static_cast<unsigned>(static_cast<u8>(payload[1])));
					if (!ws_detail::is_valid_close_code(echo_code)) {
						close(1002, "invalid close code");
						return nullopt;
					}
					if (payload.size() > 2 && !ws_detail::utf8_is_valid(string_view{payload}.substr(2))) {
						close(1007, "invalid utf-8");
						return nullopt;
					}
				}
				close(echo_code, {});
				return nullopt;
			}

			if (opcode_raw == 0x0U) {
				if (!frag_opcode_) {
					close(1002, "unexpected continuation");
					return nullopt;
				}
				frag_payload_.append(payload);
				if (!fin) {
					continue;
				}
				auto const final_op = *frag_opcode_;
				string final_payload = move(frag_payload_);
				frag_opcode_.reset();
				frag_payload_.clear();
				if (final_op == Opcode::Text && !ws_detail::utf8_is_valid(final_payload)) {
					close(1007, "invalid utf-8");
					return nullopt;
				}
				return Frame{.opcode = final_op, .payload = move(final_payload)};
			}

			if (opcode_raw != 0x1U && opcode_raw != 0x2U) {
				close(1002, "reserved opcode");
				return nullopt;
			}
			if (frag_opcode_) {
				close(1002, "nested data frame");
				return nullopt;
			}
			auto const opcode = static_cast<Opcode>(opcode_raw);
			if (!fin) {
				frag_opcode_ = opcode;
				frag_payload_ = move(payload);
				continue;
			}
			if (opcode == Opcode::Text && !ws_detail::utf8_is_valid(payload)) {
				close(1007, "invalid utf-8");
				return nullopt;
			}
			return Frame{.opcode = opcode, .payload = move(payload)};
		}
	}

	void send_text(
		string_view data) {
		scoped_lock const lk{send_mtx_};
		do_send_frame(1, as_bytes(span{data}));
	}
	void send_binary(
		span<byte const> data) {
		scoped_lock const lk{send_mtx_};
		do_send_frame(2, data);
	}
	void send_ping(
		string_view data = {}) {
		if (data.size() > 125) {
			throw invalid_argument{"WsConn::send_ping: payload exceeds 125-byte control frame limit"};
		}
		scoped_lock const lk{send_mtx_};
		do_send_frame(9, as_bytes(span{data}));
	}
	void close(
		u16 code = 1000,
		string_view reason = {}) {
		if (!ws_detail::is_valid_close_code(code)) {
			throw invalid_argument{"WsConn::close: invalid close code"};
		}
		if (reason.size() > 123) {
			throw invalid_argument{"WsConn::close: reason exceeds 123-byte limit (control frame payload max 125)"};
		}
		if (!ws_detail::utf8_is_valid(reason)) {
			throw invalid_argument{"WsConn::close: reason must be valid UTF-8"};
		}
		if (closed_.test_and_set()) {
			return;
		}
		stop_keepalive();
		array<char, 2> code_bytes{static_cast<char>(code >> 8), static_cast<char>(code & 0xFF)};
		string payload{code_bytes.data(), 2};
		payload += reason;
		{
			scoped_lock const lk{send_mtx_};
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
			unique_lock lk{keepalive_mtx_};
			while (is_open()) {
				if (keepalive_cv_.wait_for(lk, st, chrono::milliseconds{interval_ms}, [this] { return !is_open(); })) {
					break;
				}
				lk.unlock();
				send_ping();
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
	condition_variable_any keepalive_cv_;
	jthread keepalive_thread_{};
	string buf_;
	optional<Opcode> frag_opcode_{};
	string frag_payload_{};

	bool fill(
		size_t n) {
		while (buf_.size() < n) {
			array<char, 4096> tmp{};
#if CONFLUX_HAS_TLS
			if (ssl_ != nullptr) {
				auto rc = SSL_read(ssl_, tmp.data(), static_cast<int>(tmp.size()));
				if (rc <= 0) {
					return false;
				}
				buf_.append(tmp.data(), static_cast<size_t>(rc));
				continue;
			}
#endif
			auto rc = ::recv(fd_, tmp.data(), tmp.size(), 0);
			if (rc <= 0) {
				return false;
			}
			buf_.append(tmp.data(), static_cast<size_t>(rc));
		}
		return true;
	}
	void consume(
		size_t n) {
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
	string accept_key;
	CloneableFunction<void(HttpRequestView const &, WsConn &)> handler;
};

// ---------------------------------------------------------------------------

struct Segment {
	string value;
	bool is_param; // true → {name} single-segment capture
	bool is_wildcard; // true → {*name} greedy tail capture (must be last segment)
};

vector<Segment> parse_pattern(
	string_view pattern) {
	vector<Segment> segs;
	size_t pos = 0;
	while (true) {
		auto next = pattern.find('/', pos);
		auto part = (next == string_view::npos) ? pattern.substr(pos) : pattern.substr(pos, next - pos);

		if (part.size() >= 3 && part.front() == '{' && part.back() == '}' && part[1] == '*') {
			segs.push_back({string{part.substr(2, part.size() - 3)}, false, true});
		} else if (part.size() >= 2 && part.front() == '{' && part.back() == '}') {
			segs.push_back({string{part.substr(1, part.size() - 2)}, true, false});
		} else {
			segs.push_back({string{part}, false, false});
		}

		if (next == string_view::npos) {
			break;
		}
		pos = next + 1;
	}
	return segs;
}

bool match_segments(
	vector<Segment> const &pattern,
	string_view path,
	HttpFieldsView &out_params) {
	// Wildcard tail: last segment {*name} matches everything remaining.
	if (!pattern.empty() && pattern.back().is_wildcard) {
		// Match all non-wildcard leading segments first.
		auto prefix_count = pattern.size() - 1;
		size_t pos = 0;
		HttpFieldsView tmp;
		for (size_t i = 0; i < prefix_count; ++i) {
			if (pos >= path.size()) {
				return false;
			}
			auto next = path.find('/', pos);
			auto part = (next == string_view::npos) ? path.substr(pos) : path.substr(pos, next - pos);
			if (next == string_view::npos && i + 1 < prefix_count) {
				return false;
			}
			if (pattern[i].is_param) {
				tmp.emplace_back_owned(string{pattern[i].value}, url_decode_path(part));
			} else if (pattern[i].value != part) {
				return false;
			}
			pos = (next == string_view::npos) ? path.size() : next + 1;
		}
		// Capture the remainder (may be empty for trailing slash).
		tmp.emplace_back_owned(string{pattern.back().value}, url_decode_path(path.substr(pos)));
		out_params = move(tmp);
		return true;
	}

	vector<string_view> parts;
	size_t pos = 0;
	while (true) {
		auto next = path.find('/', pos);
		parts.push_back((next == string_view::npos) ? path.substr(pos) : path.substr(pos, next - pos));
		if (next == string_view::npos) {
			break;
		}
		pos = next + 1;
	}

	if (parts.size() != pattern.size()) {
		return false;
	}

	HttpFieldsView tmp;
	for (size_t i = 0; i < pattern.size(); ++i) {
		if (pattern[i].is_param) {
			tmp.emplace_back_owned(string{pattern[i].value}, url_decode_path(parts[i]));
		} else if (pattern[i].value != parts[i]) {
			return false;
		}
	}
	out_params = move(tmp);
	return true;
}

// Metadata for a single registered route, exposed by Router::route_infos().
export struct RouteInfo {
	string method;
	string path_pattern; // OpenAPI-style path e.g. /users/{id}
	vector<string> path_params; // captured parameter names in order
};

// Reconstruct an OpenAPI path string from a parsed Segment vector.
// The first segment is always an empty literal (artifact of the leading '/');
// skip it so the result starts with a single '/'.
inline string segments_to_pattern(
	vector<Segment> const &segs) {
	string out;
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
	string cache_control{"max-age=3600, public"};
	// Serve pre-compressed .gz or .br sidecars when the client accepts them.
	bool precompressed{true};
	// Generate an HTML directory listing when no index.html is found.
	bool directory_listing{false};
	// When set, stat/open/mmap happen on this pool's threads via DeferredResponse,
	// keeping the io_uring thread free while slow disks resolve.
	shared_ptr<WorkPool> offload_pool{};
	// Small static file cache. Disabled by default to preserve existing memory
	// behavior unless callers opt in via Config/Router defaults or per route.
	StaticFileCacheConfig file_cache{};
	bool allow_put{false};
	bool allow_delete{false};
};

export class Router {
public:
	using Handler = CloneableFunction<HttpResponse(HttpRequestView const &)>;
	using SseHandler = CloneableFunction<void(HttpRequestView const &, shared_ptr<SseChannel>)>;
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
		string_view method,
		string_view path,
		F &&handler) {
		impl_->routes.push_back({string{method}, parse_pattern(path), make_handler(forward<F>(handler))});
		return *this;
	}
	// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

	template<typename F>
	Router &get(
		string_view path,
		F &&handler) {
		return add("GET", path, forward<F>(handler));
	}

	template<typename F>
	Router &post(
		string_view path,
		F &&handler) {
		return add("POST", path, forward<F>(handler));
	}

	template<typename F>
	Router &put(
		string_view path,
		F &&handler) {
		return add("PUT", path, forward<F>(handler));
	}

	template<typename F>
	Router &patch(
		string_view path,
		F &&handler) {
		return add("PATCH", path, forward<F>(handler));
	}

	template<typename F>
	Router &del(
		string_view path,
		F &&handler) {
		return add("DELETE", path, forward<F>(handler));
	}

	template<typename F>
	Router &options(
		string_view path,
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
		impl_->not_found_handler = make_handler(forward<F>(handler));
		return *this;
	}

	template<typename F>
	Router &on_error(
		F &&handler) {
		impl_->error_handler = make_error_handler(forward<F>(handler));
		return *this;
	}

	// Return metadata for all registered routes (regular routes only).
	[[nodiscard]] vector<RouteInfo> route_infos() const {
		vector<RouteInfo> result;
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
		string_view path,
		F &&handler) {
		impl_->sse_routes.push_back({parse_pattern(path), make_sse_handler(forward<F>(handler))});
		return *this;
	}

	Router &set_work_pool(
		shared_ptr<WorkPool> pool) {
		impl_->work_pool = move(pool);
		return *this;
	}

	[[nodiscard]] shared_ptr<WorkPool> work_pool() const { return impl_->work_pool; }

	Router &set_static_file_cache(
		StaticFileCacheConfig cfg) {
		impl_->static_file_cache = cfg;
		return *this;
	}

	// Register a WebSocket upgrade handler. GET requests with a valid Upgrade: websocket
	// handshake are upgraded to WebSocket; the handler runs on the router's work pool.
	template<typename F>
	Router &ws(
		string_view path,
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

	// Route group: scopes a set of routes under a path prefix with optional group-local middleware.
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
			string_view method,
			string_view path,
			F &&handler) {
			router_.add(method, prefix_ + string{path}, wrap(Router::make_handler(forward<F>(handler))));
			return *this;
		}
		template<typename F>
		Group &get(
			string_view path,
			F &&handler) {
			return add("GET", path, forward<F>(handler));
		}
		template<typename F>
		Group &post(
			string_view path,
			F &&handler) {
			return add("POST", path, forward<F>(handler));
		}
		template<typename F>
		Group &put(
			string_view path,
			F &&handler) {
			return add("PUT", path, forward<F>(handler));
		}
		template<typename F>
		Group &patch(
			string_view path,
			F &&handler) {
			return add("PATCH", path, forward<F>(handler));
		}
		template<typename F>
		Group &del(
			string_view path,
			F &&handler) {
			return add("DELETE", path, forward<F>(handler));
		}
		template<typename F>
		Group &options(
			string_view path,
			F &&handler) {
			return add("OPTIONS", path, forward<F>(handler));
		}

	private:
		friend class Router;
		Group(
			Router &router,
			string prefix)
			: router_(router)
			, prefix_(move(prefix))
			, middlewares_{} {}

		// Apply group middlewares around h (innermost first, so first-registered is outermost).
		// Capture mw by value: the Group object is destroyed after router.group() returns,
		// so capturing by reference would dangle.
		[[nodiscard]] Handler wrap(
			Handler h) const {
			for (int i = static_cast<int>(middlewares_.size()) - 1; i >= 0; --i) {
				auto mw = middlewares_[static_cast<size_t>(i)]; // copy: Group is destroyed after group() returns
				h = [mw = move(mw), n = move(h)](HttpRequestView const &r) { return mw(r, n); };
			}
			return h;
		}

		Router &router_;
		string prefix_;
		vector<Middleware> middlewares_;
	};

	template<typename F>
	Router &group(
		string_view prefix,
		F &&fn) {
		Group g{*this, string{prefix}};
		forward<F>(fn)(g);
		return *this;
	}

	// Serve static files from root_dir for GET/HEAD requests under url_prefix.
	// url_prefix must not end with '/'. Files are served at url_prefix/{*file}.
	// Path traversal ("..") is rejected with 403.
	// ETag based on size+mtime; Range requests (206 Partial Content) supported.
	// Pre-compressed sidecar files (.gz, .br) served when client accepts them.
	Router &serve_static(
		string_view url_prefix,
		string root_dir,
		StaticOptions const &sopts = {}) {
		// Strip trailing slash from root_dir.
		while (!root_dir.empty() && root_dir.back() == '/') {
			root_dir.pop_back();
		}

		auto pattern = string{url_prefix} + "/{*file}";
		auto effective_sopts = sopts;
		if (!effective_sopts.file_cache.enabled) {
			effective_sopts.file_cache = impl_->static_file_cache;
		}

		struct StaticReq {
			string file_param;
			string method;
			string accept_encoding;
			string if_none_match;
			string if_modified_since;
			string range;
		};

		struct StaticCacheEntry {
			string body;
			string mime;
			string etag;
			string last_modified;
			string content_encoding;
			off_t size{};
			time_t mtime{};
			dev_t dev{};
			ino_t ino{};
			u64 tick{};
		};

		struct StaticCacheStore {
			mutex mtx;
			unordered_map<string, StaticCacheEntry> entries;
			size_t total_bytes{};
			u64 tick{};

			[[nodiscard]] optional<StaticCacheEntry> get(
				string const &key,
				struct ::stat const &st) {
				scoped_lock const lk{mtx};
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
				string key,
				StaticCacheEntry entry,
				size_t max_total_bytes) {
				scoped_lock const lk{mtx};
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
				string const &key) {
				scoped_lock const lk{mtx};
				if (auto it = entries.find(key); it != entries.end()) {
					total_bytes -= it->second.body.size();
					entries.erase(it);
				}
			}

			void evict_all_encodings(
				string const &path) {
				evict(path + "|");
				evict(path + "|br");
				evict(path + "|gzip");
			}
		};

		auto static_cache = make_shared<StaticCacheStore>();

		auto do_work =
			[static_cache](string const &rd, StaticOptions const &static_options, StaticReq const &r) -> HttpResponse {
			try {
				string file_param = r.file_param;
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
						string html = format(
							"<html><head><title>Index of {}</title></head>"
							"<body><h1>Index of {}</h1><ul>",
							html_escape(file_param),
							html_escape(file_param));
						if (!file_param.empty() && file_param != "/") {
							html += "<li><a href=\"../\">..</a></li>";
						}
						struct ::dirent *ent{};
						vector<string> names;
						while ((ent = ::readdir(dir)) != nullptr) {
							// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
							string_view const n{ent->d_name};
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
				string content_encoding;
				if (static_options.precompressed) {
					auto const &accept_enc = r.accept_encoding;
					if (accept_enc.find("br") != string::npos) {
						auto br_path = full_path + ".br";
						struct ::stat br_st{};
						if (::stat(br_path.c_str(), &br_st) == 0) {
							full_path = move(br_path);
							st = br_st;
							content_encoding = "br";
						}
					}
					if (content_encoding.empty() && accept_enc.find("gzip") != string::npos) {
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
				thread_local string last_modified_cached;
				string last_modified;
				if (st.st_mtime == last_mtime_cached && !last_modified_cached.empty()) {
					last_modified = last_modified_cached;
				} else {
					tm tm_val{};
					::gmtime_r(&st.st_mtime, &tm_val);
					array<char, 64> buf{};
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
				string_view mime = "application/octet-stream";
				if (ext_pos != string::npos) {
					auto ext = string_view{file_param}.substr(ext_pos);
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

				auto file_size = static_cast<size_t>(st.st_size);

				auto base_response = [&](int status, string_view status_text) {
					HttpResponse resp{
						.status = status,
						.status_text = string{status_text},
						.content_type = string{mime}};
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
				size_t range_start = 0;
				size_t range_end = file_size - 1;
				bool is_range_request = false;
				if (content_encoding.empty()) {
					auto const &range_hdr = r.range;
					if (!range_hdr.empty() && range_hdr.starts_with("bytes=")) {
						auto spec = string_view{range_hdr}.substr(6);
						auto dash = spec.find('-');
						if (dash != string_view::npos) {
							auto start_sv = spec.substr(0, dash);
							auto end_sv = spec.substr(dash + 1);
							size_t rs = 0;
							size_t re = file_size - 1;
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
					string body(file_size, '\0');
					size_t off = 0;
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
						off += static_cast<size_t>(n);
					}
					::close(fd);
					if (off != body.size()) {
						body.resize(off);
					}
					StaticCacheEntry entry{
						.body = move(body),
						.mime = string{mime},
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
					auto const send_off = is_range_request ? range_start : size_t{0};
					auto const send_sz = is_range_request ? (range_end - range_start + 1) : file_size;
					auto terminal =
						fr->open_async(AT_FDCWD, full_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)
						| ::then([dr, base = move(base), send_off, send_sz, fs = file_size](FileHandle fh) mutable {
							  base.set_streamed_file(
								  make_shared<StreamedFile>(StreamedFile{
									  .handle = make_shared<FileHandle>(move(fh)),
									  .send_offset = send_off,
									  .send_size = send_sz,
									  .total_size = fs}));
							  dr->complete(move(base));
						  })
						| ::on_error([dr](exception_ptr const &) {
							  dr->complete(HttpResponse::not_found("async open failed"));
						  });
					(void)terminal;
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
		auto normalize_path = [](string_view raw) -> optional<string> {
			string const fp{raw};
			vector<string> parts;
			size_t pos = 0;
			bool bad = false;
			while (pos < fp.size()) {
				auto next = fp.find('/', pos);
				auto seg =
					(next == string::npos) ? string_view{fp}.substr(pos) : string_view{fp}.substr(pos, next - pos);
				if (seg.find('\0') != string_view::npos) {
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
				if (next == string::npos) {
					break;
				}
				pos = next + 1;
			}
			if (bad) {
				return nullopt;
			}
			string result;
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
						.method = string{req.method},
						.accept_encoding = string{req.headers["accept-encoding"]},
						.if_none_match = string{as_const(req.headers)["if-none-match"]},
						.if_modified_since = string{as_const(req.headers)["if-modified-since"]},
						.range = string{req.headers["range"]},
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
							auto body_owned = make_shared<string>(req.body);
							auto dr = make_shared<DeferredResponse>();
							auto fp = make_shared<string>(full_path);
							auto terminal =
								fr->open_async(
									AT_FDCWD,
									*fp,
									O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
									0644)
								| ::flat_then([fr, body_owned, fp, existed, static_cache, dr](FileHandle fh) mutable {
									  auto fh_ptr = make_shared<FileHandle>(move(fh));
									  return fr->write_into(*fh_ptr, 0, as_bytes(span{*body_owned}))
										   | ::then(
												 [dr, fh_ptr, body_owned, fp, existed, static_cache](size_t) mutable {
													 static_cache->evict_all_encodings(*fp);
													 HttpResponse resp;
													 resp.status = existed ? kHttpNoContent : kHttpCreated;
													 resp.status_text = existed ? "No Content" : "Created";
													 dr->complete(move(resp));
												 });
								  })
								| ::on_error(
									[dr](exception_ptr const &) { dr->complete(HttpResponse::internal_error()); });
							(void)terminal;
							return HttpResponse::deferred(move(dr));
						}

						if (sopts.offload_pool) {
							auto dr = make_shared<DeferredResponse>();
							auto body_owned = make_shared<string>(req.body);
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
										size_t off = 0;
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
											off += static_cast<size_t>(n);
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
						size_t off = 0;
						while (off < body.size()) {
							ssize_t const n = ::write(wfd, body.data() + off, body.size() - off);
							if (n < 0) {
								if (errno == EINTR) {
									continue;
								}
								::close(wfd);
								return HttpResponse::internal_error();
							}
							off += static_cast<size_t>(n);
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
							auto fp = make_shared<string>(full_path);
							auto terminal =
								fr->unlink_async(AT_FDCWD, full_path)
								| ::then([dr, fp, static_cache]() mutable {
									  static_cache->evict_all_encodings(*fp);
									  dr->complete(HttpResponse{.status = kHttpNoContent, .status_text = "No Content"});
								  })
								| ::on_error([dr, fp](exception_ptr const &ep) mutable {
									  try {
										  rethrow_exception(ep);
									  } catch (FileIoError const &e) {
										  dr->complete(
											  e.code().value() == ENOENT ? HttpResponse::not_found(*fp) :
																		   HttpResponse::internal_error());
									  } catch (...) { dr->complete(HttpResponse::internal_error()); }
								  });
							(void)terminal;
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
											HttpResponse{.status = kHttpNoContent, .status_text = "No Content"});
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
						return HttpResponse{.status = kHttpNoContent, .status_text = "No Content"};
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

		// Strip query string before matching.
		auto path_sv = string_view{req.path};
		if (auto q = path_sv.find('?'); q != string_view::npos) {
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
						HttpRequestView const matched_view{
							r.method,
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
							return impl_->error_handler ?
									   impl_->error_handler(matched_view, runtime_error{"unknown exception"}) :
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
								matched.params.emplace_back(string{k}, string{v});
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

	template<typename F>
	static Handler make_handler(
		F &&fn) {
		using Fn = decay_t<F>;
		if constexpr (invocable<Fn &, HttpRequestView const &>) {
			return Handler{forward<F>(fn)};
		} else if constexpr (invocable<Fn &, HttpRequest const &>) {
			return Handler{[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req) mutable -> HttpResponse {
				auto owned = req.to_owned();
				return invoke(wrapped, owned);
			}};
		} else {
			static_assert(kDependentFalse<Fn>, "Handler must accept HttpRequestView const& or HttpRequest const&");
		}
	}

	template<typename F>
	static Middleware make_middleware(
		F &&fn) {
		using Fn = decay_t<F>;
		if constexpr (invocable<Fn &, HttpRequestView const &, Handler const &>) {
			return Middleware{forward<F>(fn)};
		} else if constexpr (invocable<Fn &, HttpRequest const &, Handler const &>) {
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
		using Fn = decay_t<F>;
		if constexpr (invocable<Fn &, HttpRequestView const &, shared_ptr<SseChannel>>) {
			return SseHandler{forward<F>(fn)};
		} else if constexpr (invocable<Fn &, HttpRequest const &, shared_ptr<SseChannel>>) {
			return SseHandler{
				[wrapped = Fn(forward<F>(fn))](HttpRequestView const &req, shared_ptr<SseChannel> ch) mutable {
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
		using Fn = decay_t<F>;
		if constexpr (invocable<Fn &, HttpRequestView const &, WsConn &>) {
			return WsHandler{forward<F>(fn)};
		} else if constexpr (invocable<Fn &, HttpRequest const &, WsConn &>) {
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
		using Fn = decay_t<F>;
		if constexpr (invocable<Fn &, HttpRequestView const &, exception const &>) {
			return ErrorHandler{forward<F>(fn)};
		} else if constexpr (invocable<Fn &, HttpRequest const &, exception const &>) {
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
			string method{};
			vector<Segment> pattern{};
			Handler handler{};
		};
		struct SseRoute {
			vector<Segment> pattern{};
			SseHandler handler{};
		};
		vector<Route> routes{};
		vector<SseRoute> sse_routes{};
		vector<Middleware> middlewares{};
		Handler not_found_handler{};
		ErrorHandler error_handler{};
		shared_ptr<WorkPool> work_pool{make_shared<WorkPool>()};
		StaticFileCacheConfig static_file_cache{};
	};
	unique_ptr<Impl> impl_;

	static void launch_sse_handler(
		shared_ptr<WorkPool> const &pool,
		SseHandler handler,
		HttpRequest matched,
		shared_ptr<SseChannel> const &channel) {
		auto task = run_on(
						*pool,
						[h = move(handler), matched = move(matched), channel]() mutable {
							HttpRequestView const matched_view{matched};
							h(matched_view, channel);
							channel->close();
						})
				  | on_cancel([channel] { channel->close(); });
		spawn(move(task));
	}

	// Wrap h in the registered middleware chain. First-registered runs outermost.
	[[nodiscard]] Handler wrap_middlewares(
		Handler h) const {
		for (auto mw: impl_->middlewares | views::reverse) {
			h = [mw = move(mw), n = move(h)](HttpRequestView const &r) { return mw(r, n); };
		}
		return h;
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

	// Register a new subscriber.  Returns the shared_ptr to pass to HttpResponse::sse().
	shared_ptr<SseChannel> subscribe() {
		auto ch = make_shared<SseChannel>();
		scoped_lock const lk{mtx_};
		channels_.emplace_back(ch);
		return ch;
	}

	// Broadcast an SSE event to all active subscribers.
	void broadcast(
		string_view event,
		string_view data) {
		auto frame = format("event: {}\ndata: {}\n\n", event, data);
		broadcast_raw(frame);
	}

	// Broadcast a data-only SSE message to all active subscribers.
	void broadcast_data(
		string_view data) {
		auto frame = format("data: {}\n\n", data);
		broadcast_raw(frame);
	}

	// Number of currently-active subscribers (approximate; may include ones
	// that have just disconnected).
	[[nodiscard]] size_t subscriber_count() const {
		scoped_lock const lk{mtx_};
		return channels_.size();
	}

private:
	void broadcast_raw(
		string const &frame) {
		scoped_lock const lk{mtx_};
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
	vector<weak_ptr<SseChannel>> channels_;
};

// Returns a middleware that logs each request to `out` in the format:
//   [ISO8601] METHOD path status bytes elapsed_ms
// Thread-safe: writes are serialised with a mutex.
export Router::Middleware make_access_log_middleware(
	ostream &out) {
	auto mtx = make_shared<mutex>();
	return [&out, mtx](HttpRequestView const &req, Router::Handler const &next) {
		auto t0 = chrono::steady_clock::now();
		auto resp = next(req);
		auto elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();

		// Wall-clock timestamp (UTC).
		auto now = chrono::system_clock::now();
		auto tt = chrono::system_clock::to_time_t(now);
		array<char, 32> ts_buf{};
		string_view ts{};
		if (strftime(ts_buf.data(), ts_buf.size(), "%Y-%m-%dT%H:%M:%SZ", gmtime(&tt)) > 0) {
			ts = ts_buf.data();
		}

		scoped_lock const lk{*mtx};
		println(out, "[{}] {} {} {} {} {}ms", ts, req.method, req.path, resp.status, resp.text_body().size(), elapsed);
		out.flush();
		return resp;
	};
}
