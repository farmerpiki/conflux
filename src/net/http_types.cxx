module;
#include <cctype>

export module conflux.net.http.types;
import std;
import conflux.types;
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
export struct FieldHash {
	using is_transparent = void;
	bool ci{false};
	[[nodiscard]] std::size_t operator ()(
		std::string_view s) const noexcept {
		std::size_t h = 14695981039346656037ULL;
		for (char const ch: s) {
			auto const c = static_cast<unsigned char>(ch);
			unsigned char const k = ci ? ascii_ci_fold(c) : c;
			h ^= k;
			h *= 1099511628211ULL;
		}
		return h;
	}
	[[nodiscard]] std::size_t operator ()(
		std::string const &s) const noexcept {
		return operator ()(std::string_view{s});
	}
};
export struct FieldEq {
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
		for (auto const &[k, v]: self.data_) {
			if (self.key_eq(k, key)) {
				return std::string_view{v};
			}
		}
		return std::nullopt;
	}
	template<typename Self>
	[[nodiscard]] std::size_t count(
		this Self const &self,
		std::string_view key) noexcept {
		std::size_t n = 0;
		for (auto const &[k, v]: self.data_) {
			(void)v;
			if (self.key_eq(k, key)) {
				++n;
			}
		}
		return n;
	}
	template<typename Self>
	[[nodiscard]] std::vector<std::string_view> values(
		this Self const &self,
		std::string_view key) {
		std::vector<std::string_view> out;
		for (auto const &[k, v]: self.data_) {
			if (self.key_eq(k, key)) {
				out.push_back(v);
			}
		}
		return out;
	}
	template<typename Self>
	[[nodiscard]] bool contains(
		this Self const &self,
		std::string_view key) noexcept {
		for (auto const &[k, v]: self.data_) {
			if (self.key_eq(k, key)) {
				return true;
			}
		}
		return false;
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
	template<typename Self>
	[[nodiscard]] auto begin(
		this Self &self) {
		return self.data_.begin();
	}
	template<typename Self>
	[[nodiscard]] auto end(
		this Self &self) {
		return self.data_.end();
	}
	template<typename Self>
	[[nodiscard]] auto begin(
		this Self const &self) {
		return self.data_.begin();
	}
	template<typename Self>
	[[nodiscard]] auto end(
		this Self const &self) {
		return self.data_.end();
	}
};

// Vector-backed string map. Linear scan — sufficient for HTTP header counts (<100).
export class HttpFields : public HttpFieldsLookupAccessors {
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
export class HttpFieldsView : public HttpFieldsLookupAccessors {
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
export namespace conflux::http {

[[nodiscard]] constexpr std::string_view trim_http_whitespace(
	std::string_view s) noexcept {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
		s.remove_prefix(1);
	}
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
		s.remove_suffix(1);
	}
	return s;
}

template<class Fn>
bool for_each_comma_token(
	std::string_view header_value,
	Fn &&fn) {
	for (std::size_t pos = 0; pos <= header_value.size();) {
		auto const comma = header_value.find(',', pos);
		auto const token = trim_http_whitespace(
			comma == std::string_view::npos ? header_value.substr(pos) : header_value.substr(pos, comma - pos));
		if (!std::invoke(fn, token)) {
			return false;
		}
		if (comma == std::string_view::npos) {
			return true;
		}
		pos = comma + 1;
	}
	return true;
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
	[[nodiscard]] std::string str() const {
		std::string out;
		out.reserve(scheme.size() + 3 + host.size() + 7 + path.size() + query.size() + 1);
		out += scheme;
		out += "://";
		out += host;
		bool const default_port = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
		if (!default_port) {
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
		auto is_query_unreserved = [](unsigned char c) noexcept {
			return (c >= 'A' && c <= 'Z')
				|| (c >= 'a' && c <= 'z')
				|| (c >= '0' && c <= '9')
				|| c == '-'
				|| c == '_'
				|| c == '.'
				|| c == '~';
		};
		auto encoded_size = [&](std::string_view s) noexcept {
			std::size_t out = 0;
			for (auto const raw_c: s) {
				out += is_query_unreserved(static_cast<unsigned char>(raw_c)) ? std::size_t{1} : std::size_t{3};
			}
			return out;
		};
		auto append_encoded = [&](std::string_view s) {
			static constexpr std::array<char, 16> kHex =
				{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
			for (auto const raw_c: s) {
				unsigned char const c = static_cast<unsigned char>(raw_c);
				if (is_query_unreserved(c)) {
					query += static_cast<char>(c);
				} else {
					query += '%';
					query += kHex[c >> 4U];
					query += kHex[c & 0x0FU];
				}
			}
		};
		query.reserve(
			query.size()
			+ (query.empty() ? std::size_t{0} : std::size_t{1})
			+ encoded_size(name)
			+ 1
			+ encoded_size(value));
		if (!query.empty()) {
			query += '&';
		}
		append_encoded(name);
		query += '=';
		append_encoded(value);
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
	url.scheme.resize(scheme_end);
	for (std::size_t i = 0; i < scheme_end; ++i) {
		url.scheme[i] = static_cast<char>(tolower(static_cast<unsigned char>(input[i])));
	}

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

	if (authority.starts_with('[')) {
		auto const bracket_end = authority.find(']');
		if (bracket_end == std::string_view::npos) {
			return std::unexpected(UrlError{UrlErrorKind::missing_host, "unterminated IPv6 literal"});
		}
		url.host = std::string{authority.substr(0, bracket_end + 1)};
		auto const after = authority.substr(bracket_end + 1);
		if (!after.empty()) {
			if (after[0] != ':') {
				return std::unexpected(UrlError{UrlErrorKind::invalid_port, "std::unexpected character after ']'"});
			}
			auto const port_sv = after.substr(1);
			std::uint16_t p = 0;
			auto const [ptr, ec] = std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), p);
			if (ec != std::errc{} || ptr != port_sv.data() + port_sv.size() || p == 0) {
				return std::unexpected(UrlError{UrlErrorKind::invalid_port, std::format("invalid port '{}'", port_sv)});
			}
			url.port = p;
		}
	} else {
		auto const colon = authority.rfind(':');
		if (colon == std::string_view::npos) {
			url.host = std::string{authority};
		} else {
			url.host = std::string{authority.substr(0, colon)};
			auto const port_sv = authority.substr(colon + 1);
			std::uint16_t p = 0;
			auto const [ptr, ec] = std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), p);
			if (ec != std::errc{} || ptr != port_sv.data() + port_sv.size() || p == 0) {
				return std::unexpected(UrlError{UrlErrorKind::invalid_port, std::format("invalid port '{}'", port_sv)});
			}
			url.port = p;
		}
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
constexpr std::array<std::string_view, 8> kHopByHopHeaders{
	"connection",
	"keep-alive",
	"proxy-authenticate",
	"proxy-authorization",
	"te",
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
[[nodiscard]] bool header_token_contains(
	std::string_view header,
	std::string_view token) noexcept {
	if (header.empty()) {
		return false;
	}
	bool found = false;
	for_each_comma_token(header, [&](std::string_view part) {
		if (ascii_iequals(part, token)) {
			found = true;
			return false;
		}
		return true;
	});
	return found;
}

} // namespace conflux::http
