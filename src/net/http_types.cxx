module;
#include <cctype>
#include <ctime>

export module conflux.net.http.types;
import std;
import conflux.types;
import conflux.utils;

namespace conflux::http {

[[nodiscard]] constexpr unsigned char ascii_ci_fold(
	unsigned char c) noexcept {
	return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}
[[nodiscard]] constexpr bool ascii_ci_equal(
	std::string_view lhs,
	std::string_view rhs) noexcept {
	return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, [](unsigned char x, unsigned char y) {
			   return ascii_ci_fold(x) == ascii_ci_fold(y);
		   });
}

} // namespace conflux::http

export namespace conflux::http {

using conflux::utils::append_url_percent_encoded;
using conflux::utils::ascii_lower;
using conflux::utils::url_percent_encoded_size;

namespace detail {

template<typename T>
void append_decimal(
	std::string &out,
	T value) {
	std::array<char, 32> buf{};
	auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
	if (ec == std::errc{}) {
		out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
	}
}

template<typename T>
void append_hex(
	std::string &out,
	T value) {
	using U = std::make_unsigned_t<T>;
	std::array<char, sizeof(U) * 2> buf{};
	auto const converted = static_cast<U>(value);
	auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), converted, 16);
	if (ec == std::errc{}) {
		out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
	}
}

} // namespace detail

struct FieldHash {
	using is_transparent = void;
	bool ci{false};
	[[nodiscard]] std::size_t operator ()(
		std::string_view s) const noexcept {
		auto const hash = ci ? conflux::support::fnv1a64_ascii_fold(s) : conflux::support::fnv1a64(s);
		return static_cast<std::size_t>(hash);
	}
	[[nodiscard]] std::size_t operator ()(
		std::string const &s) const noexcept {
		return operator ()(std::string_view{s});
	}
};
struct FieldEq {
	using is_transparent = void;
	bool ci{false};
	[[nodiscard]] bool operator ()(
		std::string_view a,
		std::string_view b) const noexcept {
		return ci ? ascii_ci_equal(a, b) : a == b;
	}
	[[nodiscard]] bool operator ()(
		std::string const &a,
		std::string_view b) const noexcept {
		return operator ()(std::string_view{a}, b);
	}
	[[nodiscard]] bool operator ()(
		std::string_view a,
		std::string const &b) const noexcept {
		return operator ()(a, std::string_view{b});
	}
	[[nodiscard]] bool operator ()(
		std::string const &a,
		std::string const &b) const noexcept {
		return operator ()(std::string_view{a}, std::string_view{b});
	}
};
struct HttpFieldsLookupAccessors {
protected:
	template<typename Self>
	[[nodiscard]] bool key_eq(
		this Self const &self,
		std::string_view a,
		std::string_view b) noexcept {
		return self.case_insensitive_ ? ascii_ci_equal(a, b) : a == b;
	}

	template<typename Self, class F>
	void for_each_matching(
		this Self const &self,
		std::string_view key,
		F &&fn) {
		for (auto const &[k, v]: self.data_) {
			if (self.key_eq(k, key)) {
				fn(v);
			}
		}
	}
	template<typename Self, class F>
	bool for_each_matching_until(
		this Self const &self,
		std::string_view key,
		F &&fn) {
		for (auto const &[k, v]: self.data_) {
			if (!self.key_eq(k, key)) {
				continue;
			}
			if constexpr (std::same_as<std::invoke_result_t<F &, std::string_view>, bool>) {
				if (!std::invoke(fn, v)) {
					return false;
				}
			} else {
				std::invoke(fn, v);
			}
		}
		return true;
	}

	template<typename Self>
	[[nodiscard]] std::optional<std::string_view> first_matching(
		this Self const &self,
		std::string_view key) noexcept {
		for (auto const &[k, v]: self.data_) {
			if (self.key_eq(k, key)) {
				return std::string_view{v};
			}
		}
		return std::nullopt;
	}

public:
	template<typename Self>
	[[nodiscard]] bool case_insensitive(
		this Self const &self) noexcept {
		return self.case_insensitive_;
	}
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	template<typename Self>
	[[nodiscard]] std::string_view operator [](
		this Self const &self,
		std::string_view key) noexcept {
		return self.get(key).value_or(std::string_view{});
	}
	template<typename Self>
	[[nodiscard]] std::optional<std::string_view> get(
		this Self const &self,
		std::string_view key) noexcept {
		return self.first_matching(key);
	}
	template<typename Self>
	[[nodiscard]] std::size_t count(
		this Self const &self,
		std::string_view key) noexcept {
		std::size_t n = 0;
		self.for_each_matching(key, [&n](auto const &) { ++n; });
		return n;
	}
	template<typename Self>
	[[nodiscard]] std::vector<std::string_view> values(
		this Self const &self,
		std::string_view key) {
		std::vector<std::string_view> out;
		self.for_each_matching(key, [&out](auto const &value) { out.push_back(value); });
		return out;
	}
	template<typename Self, class F>
	void for_each_value(
		this Self const &self,
		std::string_view key,
		F &&fn) {
		self.for_each_matching(key, std::forward<F>(fn));
	}
	template<typename Self, class F>
	bool for_each_value_until(
		this Self const &self,
		std::string_view key,
		F &&fn) {
		return self.for_each_matching_until(key, std::forward<F>(fn));
	}
	template<typename Self, class Pred>
	[[nodiscard]] bool any_value(
		this Self const &self,
		std::string_view key,
		Pred &&pred) {
		bool matched = false;
		self.for_each_matching_until(key, [&](std::string_view value) {
			if (std::invoke(pred, value)) {
				matched = true;
				return false;
			}
			return true;
		});
		return matched;
	}
	template<typename Self>
	[[nodiscard]] bool contains(
		this Self const &self,
		std::string_view key) noexcept {
		return self.first_matching(key).has_value();
	}
	template<typename Self>
	[[nodiscard]] std::string_view value_or(
		this Self const &self,
		std::string_view key,
		std::string_view def = {}) noexcept {
		return self.get(key).value_or(def);
	}
	template<typename Self>
	void reserve(
		this Self &self,
		std::size_t n) {
		self.data_.reserve(n);
	}
	template<typename Self>
	[[nodiscard]] bool empty(
		this Self const &self) noexcept {
		return self.data_.empty();
	}
	template<typename Self>
	[[nodiscard]] std::size_t size(
		this Self const &self) noexcept {
		return self.data_.size();
	}
	[[nodiscard]] auto begin(
		this auto &&self) {
		return std::forward<decltype(self)>(self).data_.begin();
	}
	[[nodiscard]] auto end(
		this auto &&self) {
		return std::forward<decltype(self)>(self).data_.end();
	}
};

// Vector-backed string map. Linear scan — sufficient for HTTP header counts (<100).
class HttpFields : public HttpFieldsLookupAccessors {
	friend struct HttpFieldsLookupAccessors;
	std::vector<std::pair<std::string, std::string>> data_;
	bool case_insensitive_{false};

public:
	using HttpFieldsLookupAccessors::operator [];

	HttpFields(
		bool case_insensitive = false)
		: case_insensitive_(case_insensitive) {}
	HttpFields(
		std::initializer_list<std::pair<std::string, std::string>> init)
		: data_(init) {}
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	std::string &operator [](
		std::string_view key) {
		for (auto &[k, v]: data_) {
			if (key_eq(k, key)) {
				return v;
			}
		}
		data_.emplace_back(std::string{key}, std::string{});
		return data_.back().second;
	}
	void emplace_back(
		std::string k,
		std::string v) {
		data_.emplace_back(std::move(k), std::move(v));
	}
	void append(
		std::string k,
		std::string v) {
		emplace_back(std::move(k), std::move(v));
	}
	void set(
		std::string key,
		std::string field_value) {
		bool found = false;
		std::size_t write = 0;
		for (std::size_t read = 0; read < data_.size(); ++read) {
			auto &field = data_[read];
			if (key_eq(field.first, key)) {
				if (found) {
					continue;
				}
				found = true;
				field.second = std::move(field_value);
			}
			if (write != read) {
				data_[write] = std::move(field);
			}
			++write;
		}
		if (!found) {
			data_.emplace_back(std::move(key), std::move(field_value));
			return;
		}
		data_.resize(write);
	}
	std::size_t erase(
		std::string_view key) {
		std::size_t cursor = 0;
		return erase_if(data_, [&](auto const &pair) {
			++cursor;
			return key_eq(std::string_view{pair.first}, key);
		});
	}
	void clear() noexcept { data_.clear(); }
};
class HttpFieldsView : public HttpFieldsLookupAccessors {
	friend struct HttpFieldsLookupAccessors;
	struct OwnedStorage {
		std::atomic<std::size_t> refs{1};
		std::deque<std::string> values;
	};
	std::vector<std::pair<std::string_view, std::string_view>> data_;
	OwnedStorage *owned_storage_{};
	bool case_insensitive_{false};
	static void retain(
		OwnedStorage *storage) noexcept {
		if (storage != nullptr) {
			storage->refs.fetch_add(1, std::memory_order_relaxed);
		}
	}
	static void release(
		OwnedStorage *storage) noexcept {
		if (storage != nullptr && storage->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			delete storage;
		}
	}
	[[nodiscard]] std::string_view store_owned(
		std::string owned_value) {
		if (owned_storage_ == nullptr) {
			owned_storage_ = new OwnedStorage;
		}
		owned_storage_->values.push_back(std::move(owned_value));
		return owned_storage_->values.back();
	}

public:
	HttpFieldsView(
		bool case_insensitive = false)
		: case_insensitive_(case_insensitive) {}
	HttpFieldsView(
		HttpFields const &fields)
		: case_insensitive_(fields.case_insensitive()) {
		data_.reserve(fields.size());
		for (auto const &[k, v]: fields) {
			emplace_back(k, v);
		}
	}
	HttpFieldsView(
		HttpFieldsView const &other)
		: data_(other.data_)
		, owned_storage_(other.owned_storage_)
		, case_insensitive_(other.case_insensitive_) {
		retain(owned_storage_);
	}
	HttpFieldsView(
		HttpFieldsView &&other) noexcept
		: data_(std::move(other.data_))
		, owned_storage_(std::exchange(other.owned_storage_, nullptr))
		, case_insensitive_(std::exchange(other.case_insensitive_, false)) {}
	~HttpFieldsView() { release(owned_storage_); }
	HttpFieldsView &operator =(
		HttpFieldsView const &other) {
		if (this == &other) {
			return *this;
		}
		retain(other.owned_storage_);
		release(owned_storage_);
		data_ = other.data_;
		owned_storage_ = other.owned_storage_;
		case_insensitive_ = other.case_insensitive_;
		return *this;
	}
	HttpFieldsView &operator =(
		HttpFieldsView &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		release(owned_storage_);
		data_ = std::move(other.data_);
		owned_storage_ = std::exchange(other.owned_storage_, nullptr);
		case_insensitive_ = std::exchange(other.case_insensitive_, false);
		return *this;
	}
	void emplace_back(
		std::string_view k,
		std::string_view v) {
		data_.emplace_back(k, v);
	}
	void emplace_back_owned(
		std::string k,
		std::string v) {
		data_.emplace_back(store_owned(std::move(k)), store_owned(std::move(v)));
	}
	void emplace_back_owned_value(
		std::string_view k,
		std::string v) {
		data_.emplace_back(k, store_owned(std::move(v)));
	}
	void clear() noexcept {
		data_.clear();
		release(owned_storage_);
		owned_storage_ = nullptr;
	}
	[[nodiscard]] HttpFields to_owned() const {
		HttpFields out{case_insensitive_};
		out.reserve(data_.size());
		for (auto const &[k, v]: data_) {
			out.emplace_back(std::string{k}, std::string{v});
		}
		return out;
	}
};
template<class Fields, class F>
void for_each_header_value(
	Fields const &fields,
	std::string_view key,
	F &&fn) {
	fields.for_each_value(key, std::forward<F>(fn));
}
template<class Fields, class F>
bool for_each_header_value_until(
	Fields const &fields,
	std::string_view key,
	F &&fn) {
	return fields.for_each_value_until(key, std::forward<F>(fn));
}
template<class Fields, class Pred>
[[nodiscard]] bool any_header_value(
	Fields const &fields,
	std::string_view key,
	Pred &&pred) {
	return fields.any_value(key, std::forward<Pred>(pred));
}

[[nodiscard]] std::string http_date(
	time_t epoch) {
	tm gmt{};
	if (::gmtime_r(&epoch, &gmt) == nullptr) {
		return "Thu, 01 Jan 1970 00:00:00 GMT";
	}
	std::array<char, 32> buf{};
	std::size_t const n = ::strftime(buf.data(), buf.size(), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
	return n != 0 ? std::string{buf.data(), n} : std::string{"Thu, 01 Jan 1970 00:00:00 GMT"};
}

[[nodiscard]] std::string http_date(
	std::chrono::system_clock::time_point tp) {
	return http_date(std::chrono::system_clock::to_time_t(tp));
}

class HeaderTokenView {
	std::string_view header_{};

public:
	class Iterator {
		std::string_view header_{};
		std::size_t pos_{0};
		std::string_view token_{};
		bool done_{true};

		void read_current() noexcept {
			if (pos_ > header_.size()) {
				done_ = true;
				token_ = {};
				return;
			}
			auto const comma = header_.find(',', pos_);
			token_ = conflux::utils::trim(
				comma == std::string_view::npos ? header_.substr(pos_) : header_.substr(pos_, comma - pos_));
			done_ = false;
		}

	public:
		using value_type = std::string_view;
		using difference_type = std::ptrdiff_t;
		using iterator_concept = std::input_iterator_tag;
		using iterator_category = std::input_iterator_tag;

		Iterator() = default;
		Iterator(
			std::string_view header,
			std::size_t pos) noexcept
			: header_{header}
			, pos_{pos} {
			read_current();
		}

		[[nodiscard]] std::string_view operator *() const noexcept { return token_; }
		Iterator &operator ++() noexcept {
			if (done_) {
				return *this;
			}
			auto const comma = header_.find(',', pos_);
			if (comma == std::string_view::npos) {
				done_ = true;
				token_ = {};
			} else {
				pos_ = comma + 1;
				read_current();
			}
			return *this;
		}
		void operator ++(
			int) noexcept {
			++(*this);
		}
		[[nodiscard]] friend bool operator ==(
			Iterator const &lhs,
			Iterator const &rhs) noexcept {
			return lhs.done_ == rhs.done_
				&& (lhs.done_
					|| (lhs.header_.data() == rhs.header_.data()
						&& lhs.header_.size() == rhs.header_.size()
						&& lhs.pos_ == rhs.pos_));
		}
	};

	explicit constexpr HeaderTokenView(
		std::string_view header) noexcept
		: header_{header} {}

	[[nodiscard]] Iterator begin() const noexcept { return Iterator{header_, 0}; }
	[[nodiscard]] Iterator end() const noexcept { return {}; }
};

[[nodiscard]] constexpr HeaderTokenView header_tokens(
	std::string_view header_value) noexcept {
	return HeaderTokenView{header_value};
}

[[nodiscard]] constexpr std::size_t find_unquoted_header_delim(
	std::string_view s,
	char needle,
	std::size_t pos = 0) noexcept {
	bool quoted = false;
	bool escaped = false;
	for (std::size_t i = pos; i < s.size(); ++i) {
		char const c = s[i];
		if (escaped) {
			escaped = false;
			continue;
		}
		if (quoted && c == '\\') {
			escaped = true;
			continue;
		}
		if (c == '"') {
			quoted = !quoted;
			continue;
		}
		if (!quoted && c == needle) {
			return i;
		}
	}
	return std::string_view::npos;
}

struct HeaderParam {
	std::string_view name{};
	std::string_view value{};
	bool has_value{};
};

class HeaderParamView {
	std::string_view params_{};

	[[nodiscard]] static constexpr std::size_t find_unquoted(
		std::string_view s,
		char needle,
		std::size_t pos = 0) noexcept {
		return find_unquoted_header_delim(s, needle, pos);
	}

	class Iterator {
		std::string_view params_{};
		std::size_t pos_{0};
		HeaderParam param_{};
		bool done_{true};

		void read_current() noexcept {
			while (pos_ <= params_.size()) {
				auto const semi = find_unquoted(params_, ';', pos_);
				auto segment = conflux::utils::trim(
					semi == std::string_view::npos ? params_.substr(pos_) : params_.substr(pos_, semi - pos_));
				if (semi == std::string_view::npos) {
					pos_ = params_.size() + 1;
				} else {
					pos_ = semi + 1;
				}
				if (segment.empty()) {
					continue;
				}
				auto const eq = find_unquoted(segment, '=');
				if (eq == std::string_view::npos) {
					param_ = HeaderParam{.name = conflux::utils::trim(segment)};
				} else {
					param_ = HeaderParam{
						.name = conflux::utils::trim(segment.substr(0, eq)),
						.value = conflux::utils::trim(segment.substr(eq + 1)),
						.has_value = true};
				}
				done_ = false;
				return;
			}
			done_ = true;
			param_ = {};
		}

	public:
		using value_type = HeaderParam;
		using difference_type = std::ptrdiff_t;
		using iterator_concept = std::input_iterator_tag;
		using iterator_category = std::input_iterator_tag;

		Iterator() = default;
		Iterator(
			std::string_view params,
			std::size_t pos) noexcept
			: params_{params}
			, pos_{pos} {
			read_current();
		}

		[[nodiscard]] HeaderParam operator *() const noexcept { return param_; }
		Iterator &operator ++() noexcept {
			if (!done_) {
				read_current();
			}
			return *this;
		}
		void operator ++(
			int) noexcept {
			++(*this);
		}
		[[nodiscard]] friend bool operator ==(
			Iterator const &lhs,
			Iterator const &rhs) noexcept {
			return lhs.done_ == rhs.done_
				&& (lhs.done_
					|| (lhs.params_.data() == rhs.params_.data()
						&& lhs.params_.size() == rhs.params_.size()
						&& lhs.pos_ == rhs.pos_));
		}
	};

public:
	constexpr HeaderParamView() noexcept = default;
	explicit constexpr HeaderParamView(
		std::string_view params) noexcept
		: params_{params} {}

	[[nodiscard]] Iterator begin() const noexcept { return Iterator{params_, 0}; }
	[[nodiscard]] Iterator end() const noexcept { return {}; }
};

[[nodiscard]] constexpr HeaderParamView header_params(
	std::string_view params) noexcept {
	return HeaderParamView{params};
}

struct HeaderItem {
	std::string_view name{};
	std::string_view value{};
	bool has_value{};
	HeaderParamView params{};
};

class HeaderItemView {
	std::string_view header_{};

	class Iterator {
		HeaderTokenView::Iterator it_{};
		HeaderTokenView::Iterator end_{};
		HeaderItem item_{};

		void read_current() noexcept {
			if (it_ == end_) {
				item_ = {};
				return;
			}
			auto const token = *it_;
			auto const semi = find_unquoted_header_delim(token, ';');
			auto const first = conflux::utils::trim(semi == std::string_view::npos ? token : token.substr(0, semi));
			auto const eq = find_unquoted_header_delim(first, '=');
			std::string_view name = first;
			std::string_view value{};
			bool has_value = false;
			if (eq != std::string_view::npos) {
				name = conflux::utils::trim(first.substr(0, eq));
				value = conflux::utils::trim(first.substr(eq + 1));
				has_value = true;
			}
			item_ = HeaderItem{
				.name = name,
				.value = value,
				.has_value = has_value,
				.params = header_params(semi == std::string_view::npos ? std::string_view{} : token.substr(semi + 1))};
		}

	public:
		using value_type = HeaderItem;
		using difference_type = std::ptrdiff_t;
		using iterator_concept = std::input_iterator_tag;
		using iterator_category = std::input_iterator_tag;

		Iterator() = default;
		explicit Iterator(
			std::string_view header)
			: it_{header_tokens(header).begin()}
			, end_{header_tokens(header).end()} {
			read_current();
		}

		[[nodiscard]] HeaderItem operator *() const noexcept { return item_; }
		Iterator &operator ++() noexcept {
			if (it_ != end_) {
				++it_;
				read_current();
			}
			return *this;
		}
		void operator ++(
			int) noexcept {
			++(*this);
		}
		[[nodiscard]] friend bool operator ==(
			Iterator const &lhs,
			Iterator const &rhs) noexcept {
			return lhs.it_ == rhs.it_;
		}
	};

public:
	explicit constexpr HeaderItemView(
		std::string_view header) noexcept
		: header_{header} {}

	[[nodiscard]] Iterator begin() const noexcept { return Iterator{header_}; }
	[[nodiscard]] Iterator end() const noexcept { return {}; }
};

[[nodiscard]] constexpr HeaderItemView header_items(
	std::string_view header_value) noexcept {
	return HeaderItemView{header_value};
}

[[nodiscard]] inline std::optional<float> parse_http_q(
	HeaderParamView params) noexcept {
	for (auto const param: params) {
		if (!param.has_value || !ascii_ci_equal(param.name, "q")) {
			continue;
		}
		float q = 1.0F;
		auto const value = conflux::utils::trim(param.value);
		auto const [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), q);
		if (ec == std::errc{} && ptr == value.data() + value.size()) {
			return q;
		}
		return std::nullopt;
	}
	return std::nullopt;
}

enum class AcceptEncodingMerge : std::uint8_t {
	first,
	max,
};

struct AcceptEncodingQs {
	float br{-1.0F};
	float gzip{-1.0F};
	float zstd{-1.0F};
	float wildcard{-1.0F};
};

[[nodiscard]] constexpr float merged_accept_encoding_q(
	float current,
	float q,
	AcceptEncodingMerge merge) noexcept {
	if (current < 0.0F) {
		return q;
	}
	if (merge == AcceptEncodingMerge::max) {
		return std::max(current, q);
	}
	return current;
}

[[nodiscard]] inline AcceptEncodingQs parse_accept_encoding_qs(
	std::string_view header,
	AcceptEncodingMerge merge = AcceptEncodingMerge::max) noexcept {
	AcceptEncodingQs qs{};
	for (auto const item: header_items(header)) {
		float const q = parse_http_q(item.params).value_or(1.0F);
		if (item.name == "*") {
			qs.wildcard = merged_accept_encoding_q(qs.wildcard, q, merge);
		} else if (ascii_ci_equal(item.name, "br")) {
			qs.br = merged_accept_encoding_q(qs.br, q, merge);
		} else if (ascii_ci_equal(item.name, "gzip")) {
			qs.gzip = merged_accept_encoding_q(qs.gzip, q, merge);
		} else if (ascii_ci_equal(item.name, "zstd")) {
			qs.zstd = merged_accept_encoding_q(qs.zstd, q, merge);
		}
	}
	return qs;
}

template<class Fn>
bool for_each_comma_token(
	std::string_view header_value,
	Fn &&fn) {
	for (auto const token: header_tokens(header_value)) {
		if (!std::invoke(fn, token)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] constexpr std::optional<std::string_view> credentials_for_auth_scheme(
	std::string_view authorization,
	std::string_view scheme) noexcept {
	if (authorization.size() <= scheme.size() || authorization[scheme.size()] != ' ') {
		return std::nullopt;
	}
	if (!ascii_ci_equal(authorization.substr(0, scheme.size()), scheme)) {
		return std::nullopt;
	}
	return conflux::utils::trim(authorization.substr(scheme.size() + 1));
}

// ─── errors ──────────────────────────────────────────────────────────────────

enum class HttpErrorKind : std::uint8_t {
	dns,
	connect,
	tls,
	write,
	read,
	timeout,
	protocol,
	header_too_large,
	body_too_large,
	decompression,
	redirect_limit,
};

enum class HttpPhase : std::uint8_t {
	resolve,
	connect,
	tls,
	write,
	first_byte,
	between_bytes,
};
struct HttpError {
	HttpErrorKind kind{HttpErrorKind::protocol};
	HttpPhase phase{};
	int os_errno{0};
	int tls_alert{0};
	std::string verify_reason{};
	std::string message{};
};
// ─── timeouts ────────────────────────────────────────────────────────────────

struct HttpTimeouts {
	std::chrono::milliseconds resolve{5000};
	std::chrono::milliseconds connect{5000};
	std::chrono::milliseconds tls{5000};
	std::chrono::milliseconds write{30000};
	std::chrono::milliseconds first_byte{30000};
	std::chrono::milliseconds between_bytes{30000};
};

namespace detail {

[[nodiscard]] constexpr HttpTimeouts effective_http_timeouts(
	HttpTimeouts request,
	HttpTimeouts defaults) noexcept {
	constexpr HttpTimeouts built_in{};
	return HttpTimeouts{
		.resolve = request.resolve != built_in.resolve ? request.resolve : defaults.resolve,
		.connect = request.connect != built_in.connect ? request.connect : defaults.connect,
		.tls = request.tls != built_in.tls ? request.tls : defaults.tls,
		.write = request.write != built_in.write ? request.write : defaults.write,
		.first_byte = request.first_byte != built_in.first_byte ? request.first_byte : defaults.first_byte,
		.between_bytes =
			request.between_bytes != built_in.between_bytes ? request.between_bytes : defaults.between_bytes,
	};
}

} // namespace detail
// ─── telemetry ───────────────────────────────────────────────────────────────

struct HttpTelemetry {
	std::chrono::nanoseconds dns{};
	std::chrono::nanoseconds connect{};
	std::chrono::nanoseconds tls{};
	std::chrono::nanoseconds ttfb{};
	std::chrono::nanoseconds body{};
	std::optional<std::chrono::nanoseconds> pool_wait{};
	std::uint64_t bytes_sent{0};
	std::uint64_t bytes_received{0};
	bool reused_connection{false};
	std::string negotiated_protocol{};
	std::string tls_cipher{};
	std::string tls_version{};
	bool tls_verified{false};
	std::string peer_addr{};
	std::optional<std::string> decoded_encoding{};
};

// ─── request-target helpers ──────────────────────────────────────────────────

struct PathQueryView {
	std::string_view path{}; // without '?' and query
	std::string_view query{}; // without leading '?'
	std::string_view query_suffix{}; // including leading '?', empty when absent
};

[[nodiscard]] constexpr PathQueryView split_path_query(
	std::string_view target) noexcept {
	auto const qmark = target.find('?');
	if (qmark == std::string_view::npos) {
		return {.path = target};
	}
	return {.path = target.substr(0, qmark), .query = target.substr(qmark + 1), .query_suffix = target.substr(qmark)};
}

[[nodiscard]] constexpr std::string_view path_without_query(
	std::string_view target) noexcept {
	return split_path_query(target).path;
}

[[nodiscard]] constexpr std::string_view origin_form_path_from_target(
	std::string_view target_without_query) noexcept {
	if (target_without_query.starts_with("https://")) {
		auto const slash = target_without_query.find('/', 8);
		return (slash != std::string_view::npos) ? target_without_query.substr(slash) : std::string_view{"/"};
	}
	if (target_without_query.starts_with("http://")) {
		auto const slash = target_without_query.find('/', 7);
		return (slash != std::string_view::npos) ? target_without_query.substr(slash) : std::string_view{"/"};
	}
	return target_without_query;
}

// ─── Host/authority helpers ─────────────────────────────────────────────────

struct AuthorityHostPortView {
	std::string_view host{}; // preserves IPv6 brackets when present
	std::string_view port{}; // without leading ':', empty when absent
	bool has_port{false};
	bool bracketed_ipv6{false};
	bool invalid_bracket{false};
	bool invalid_suffix{false};
};

[[nodiscard]] constexpr AuthorityHostPortView split_authority_host_port(
	std::string_view authority) noexcept {
	if (authority.starts_with('[')) {
		auto const bracket = authority.find(']');
		if (bracket == std::string_view::npos) {
			return {.host = authority, .bracketed_ipv6 = true, .invalid_bracket = true};
		}
		auto const after = authority.substr(bracket + 1);
		if (after.empty()) {
			return {.host = authority, .bracketed_ipv6 = true};
		}
		if (!after.starts_with(':')) {
			return {
				.host = authority.substr(0, bracket + 1),
				.bracketed_ipv6 = true,
				.invalid_suffix = true,
			};
		}
		return {
			.host = authority.substr(0, bracket + 1),
			.port = after.substr(1),
			.has_port = true,
			.bracketed_ipv6 = true,
		};
	}
	auto const colon = authority.rfind(':');
	if (colon == std::string_view::npos) {
		return {.host = authority};
	}
	return {.host = authority.substr(0, colon), .port = authority.substr(colon + 1), .has_port = true};
}

[[nodiscard]] constexpr std::string_view host_without_port(
	std::string_view authority) noexcept {
	auto const parts = split_authority_host_port(authority);
	return (parts.invalid_bracket || parts.invalid_suffix) ? authority : parts.host;
}

[[nodiscard]] constexpr std::string_view host_without_port_or_ipv6_brackets(
	std::string_view authority) noexcept {
	auto parts = split_authority_host_port(authority);
	if (parts.invalid_bracket || parts.invalid_suffix) {
		return authority;
	}
	auto host = parts.host;
	if (parts.bracketed_ipv6 && host.size() >= 2 && host.front() == '[' && host.back() == ']') {
		host.remove_prefix(1);
		host.remove_suffix(1);
	}
	return host;
}

// ─── URL ─────────────────────────────────────────────────────────────────────

enum class UrlErrorKind : std::uint8_t {
	empty,
	missing_scheme,
	unsupported_scheme,
	missing_host,
	invalid_port,
	too_long,
};
struct UrlError {
	UrlErrorKind kind{UrlErrorKind::empty};
	std::string message{};
};
struct Url {
	std::string scheme{};
	std::string host{};
	std::uint16_t port{80};
	std::string path{"/"};
	std::string query{}; // raw, without leading '?'

	[[nodiscard]] static std::expected<Url, UrlError> parse(std::string_view input);
	[[nodiscard]] bool uses_default_port() const noexcept {
		return (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
	}
	[[nodiscard]] std::size_t origin_form_target_size() const noexcept {
		std::size_t n = path.empty() ? 1 : path.size();
		if (!query.empty()) {
			n += 1 + query.size();
		}
		return n;
	}
	void append_origin_form_target(
		std::string &out) const {
		if (path.empty()) {
			out += '/';
		} else {
			out += path;
		}
		if (!query.empty()) {
			out += '?';
			out += query;
		}
	}
	[[nodiscard]] std::size_t host_header_value_size(
		std::string_view override_host = {}) const noexcept {
		if (!override_host.empty()) {
			return override_host.size();
		}
		if (uses_default_port()) {
			return host.size();
		}
		std::size_t port_digits = 1;
		for (auto value = port; value >= 10; value /= 10) {
			++port_digits;
		}
		return host.size() + 1 + port_digits;
	}
	void append_host_header_value(
		std::string &out,
		std::string_view override_host = {}) const {
		if (!override_host.empty()) {
			out += override_host;
			return;
		}
		out += host;
		if (!uses_default_port()) {
			out += ':';
			std::array<char, 5> port_buf{};
			auto const [ptr, ec] = std::to_chars(port_buf.data(), port_buf.data() + port_buf.size(), port);
			if (ec == std::errc{}) {
				out.append(port_buf.data(), static_cast<std::size_t>(ptr - port_buf.data()));
			}
		}
	}
	[[nodiscard]] std::string str() const {
		std::string out;
		out.reserve(scheme.size() + 3 + host.size() + 7 + path.size() + query.size() + 1);
		out += scheme;
		out += "://";
		out += host;
		if (!uses_default_port()) {
			out += ':';
			std::array<char, 5> port_buf{};
			auto const [ptr, ec] = std::to_chars(port_buf.data(), port_buf.data() + port_buf.size(), port);
			if (ec == std::errc{}) {
				out.append(port_buf.data(), static_cast<std::size_t>(ptr - port_buf.data()));
			}
		}
		if (path.empty() || path[0] != '/') {
			out += '/';
		}
		out += path;
		if (!query.empty()) {
			out += '?';
			out += query;
		}
		return out;
	}
	void set_query_param(
		std::string_view name,
		std::string_view value) {
		query.reserve(
			query.size()
			+ (query.empty() ? std::size_t{0} : std::size_t{1})
			+ url_percent_encoded_size(name)
			+ 1
			+ url_percent_encoded_size(value));
		if (!query.empty()) {
			query += '&';
		}
		append_url_percent_encoded(query, name);
		query += '=';
		append_url_percent_encoded(query, value);
	}
};
std::expected<Url, UrlError> Url::parse(
	std::string_view input) {
	if (input.empty()) {
		return std::unexpected(UrlError{UrlErrorKind::empty, "empty URL"});
	}
	constexpr std::size_t kMaxUrl = 8192;
	if (input.size() > kMaxUrl) {
		return std::unexpected(UrlError{UrlErrorKind::too_long, "URL exceeds 8192 bytes"});
	}

	auto const scheme_end = input.find("://");
	if (scheme_end == std::string_view::npos) {
		return std::unexpected(UrlError{UrlErrorKind::missing_scheme, "missing '://'"});
	}

	Url url;
	url.scheme = ascii_lower(input.substr(0, scheme_end));

	if (url.scheme != "http" && url.scheme != "https") {
		return std::unexpected(
			UrlError{UrlErrorKind::unsupported_scheme, std::format("unsupported scheme '{}'", url.scheme)});
	}
	url.port = (url.scheme == "https") ? std::uint16_t{443} : std::uint16_t{80};

	auto rest = input.substr(scheme_end + 3);
	if (rest.empty()) {
		return std::unexpected(UrlError{UrlErrorKind::missing_host, "missing host"});
	}

	auto const authority_end = rest.find_first_of("/?");
	auto const authority = (authority_end == std::string_view::npos) ? rest : rest.substr(0, authority_end);

	if (authority.empty()) {
		return std::unexpected(UrlError{UrlErrorKind::missing_host, "missing host"});
	}

	auto const authority_parts = split_authority_host_port(authority);
	if (authority_parts.invalid_bracket) {
		return std::unexpected(UrlError{UrlErrorKind::missing_host, "unterminated IPv6 literal"});
	}
	if (authority_parts.invalid_suffix) {
		return std::unexpected(UrlError{UrlErrorKind::invalid_port, "std::unexpected character after ']'"});
	}
	url.host = std::string{authority_parts.host};
	if (authority_parts.has_port) {
		auto const port_sv = authority_parts.port;
		std::uint16_t p = 0;
		auto const [ptr, ec] = std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), p);
		if (ec != std::errc{} || ptr != port_sv.data() + port_sv.size() || p == 0) {
			return std::unexpected(UrlError{UrlErrorKind::invalid_port, std::format("invalid port '{}'", port_sv)});
		}
		url.port = p;
	}

	if (url.host.empty()) {
		return std::unexpected(UrlError{UrlErrorKind::missing_host, "empty host"});
	}

	if (authority_end == std::string_view::npos) {
		url.path = "/";
	} else {
		auto const path_query = split_path_query(rest.substr(authority_end));
		url.path = std::string{path_query.path};
		if (url.path.empty()) {
			url.path = "/";
		}
		url.query = std::string{path_query.query};
	}

	return url;
}
[[nodiscard]] bool ascii_iequals(
	std::string_view lhs,
	std::string_view rhs) noexcept {
	return ascii_ci_equal(lhs, rhs);
}
constexpr std::array<std::string_view, 9> kHopByHopHeaders{
	"connection",
	"keep-alive",
	"proxy-authenticate",
	"proxy-authorization",
	"te",
	"trailer",
	"trailers",
	"transfer-encoding",
	"upgrade",
};
[[nodiscard]] bool is_hop_by_hop_header(
	std::string_view name) noexcept {
	return std::ranges::any_of(kHopByHopHeaders, [&](std::string_view candidate) {
		return ascii_iequals(name, candidate);
	});
}
[[nodiscard]] bool is_request_controlled_header(
	std::string_view name) noexcept {
	return ascii_iequals(name, "host") || is_hop_by_hop_header(name);
}
[[nodiscard]] bool is_response_framing_header(
	std::string_view name) noexcept {
	return ascii_iequals(name, "content-length") || is_hop_by_hop_header(name);
}
[[nodiscard]] bool header_token_contains(
	std::string_view header,
	std::string_view token) noexcept {
	if (header.empty()) {
		return false;
	}
	return std::ranges::any_of(header_tokens(header), [&](std::string_view part) {
		return ascii_iequals(part, token);
	});
}

} // namespace conflux::http

template<>
inline constexpr bool std::ranges::enable_borrowed_range<conflux::http::HeaderTokenView> = true;
template<>
inline constexpr bool std::ranges::enable_borrowed_range<conflux::http::HeaderParamView> = true;
template<>
inline constexpr bool std::ranges::enable_borrowed_range<conflux::http::HeaderItemView> = true;

static_assert(std::ranges::borrowed_range<conflux::http::HeaderTokenView>);
static_assert(std::ranges::borrowed_range<conflux::http::HeaderParamView>);
static_assert(std::ranges::borrowed_range<conflux::http::HeaderItemView>);
