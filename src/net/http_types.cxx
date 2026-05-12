module;
#include <cctype>
#include <memory>

export module conflux.net.http.types;
import std;
import conflux.types;
[[nodiscard]] constexpr unsigned char ascii_ci_fold(
	unsigned char c) noexcept {
	return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}
[[nodiscard]] static SV http_trim(
	SV s) noexcept {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
		s.remove_prefix(1);
	}
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
		s.remove_suffix(1);
	}
	return s;
}
export struct FieldHash {
	using is_transparent = void;
	bool ci{false};
	[[nodiscard]] SZ operator ()(
		SV s) const noexcept {
		SZ h = 14695981039346656037ULL;
		for (char const ch: s) {
			auto const c = static_cast<unsigned char>(ch);
			unsigned char const k = ci ? ascii_ci_fold(c) : c;
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
		return ranges::equal(a, b, [](unsigned char x, unsigned char y) {
			return ascii_ci_fold(x) == ascii_ci_fold(y);
		});
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
// Vector-backed string map. Linear scan — sufficient for HTTP header counts (<100).
export class HttpFields {
	V<P<S, S>> data_;
	bool case_insensitive_{false};
	[[nodiscard]] bool key_eq(
		SV a,
		SV b) const noexcept {
		if (a.size() != b.size()) {
			return false;
		}
		if (!case_insensitive_) {
			return a == b;
		}
		return ranges::equal(a, b, [](unsigned char x, unsigned char y) {
			return ascii_ci_fold(x) == ascii_ci_fold(y);
		});
	}

public:
	HttpFields(
		bool case_insensitive = false)
		: case_insensitive_(case_insensitive) {}
	HttpFields(
		std::initializer_list<P<S, S>> init)
		: data_(init) {}
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	SV operator [](
		SV key) const noexcept {
		return get(key).value_or(SV{});
	}
	// NOLINTNEXTLINE(fuchsia-overloaded-operator)
	S &operator [](
		SV key) {
		for (auto &[k, v]: data_) {
			if (key_eq(k, key)) {
				return v;
			}
		}
		data_.emplace_back(S{key}, S{});
		return data_.back().second;
	}
	[[nodiscard]] Opt<SV> get(
		SV key) const noexcept {
		for (auto const &[k, v]: data_) {
			if (key_eq(k, key)) {
				return SV{v};
			}
		}
		return nullopt;
	}
	[[nodiscard]] V<SV> values(
		SV key) const {
		V<SV> out;
		for (auto const &[k, v]: data_) {
			if (key_eq(k, key)) {
				out.push_back(v);
			}
		}
		return out;
	}
	[[nodiscard]] bool contains(
		SV key) const noexcept {
		for (auto const &[k, v]: data_) {
			if (key_eq(k, key)) {
				return true;
			}
		}
		return false;
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
	}
	void append(
		S k,
		S v) {
		emplace_back(move(k), move(v));
	}
	void set(
		S key,
		S field_value) {
		SZ keep_idx = SZ(-1);
		for (SZ i = 0; i < data_.size(); ++i) {
			if (key_eq(data_[i].first, key) && keep_idx == SZ(-1)) {
				keep_idx = i;
			}
		}
		if (keep_idx == SZ(-1)) {
			data_.emplace_back(move(key), move(field_value));
			return;
		}
		data_[keep_idx].second = move(field_value);
		S const keep_key = data_[keep_idx].first;
		SZ cursor = 0;
		erase_if(data_, [&](auto const &pair) {
			SZ const i = cursor++;
			return i != keep_idx && key_eq(SV{pair.first}, SV{keep_key});
		});
	}
	SZ erase(
		SV key) {
		SZ cursor = 0;
		return erase_if(data_, [&](auto const &pair) {
			++cursor;
			return key_eq(SV{pair.first}, key);
		});
	}
	void clear() noexcept { data_.clear(); }
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
	SP<deque<S>> owned_storage_{};
	bool case_insensitive_{false};
	[[nodiscard]] bool key_eq(
		SV a,
		SV b) const noexcept {
		if (a.size() != b.size()) {
			return false;
		}
		if (!case_insensitive_) {
			return a == b;
		}
		return ranges::equal(a, b, [](unsigned char x, unsigned char y) {
			return ascii_ci_fold(x) == ascii_ci_fold(y);
		});
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
		: case_insensitive_(case_insensitive) {}
	HttpFieldsView(
		HttpFields const &fields)
		: case_insensitive_(fields.case_insensitive()) {
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
		for (auto const &[k, v]: data_) {
			if (key_eq(k, key)) {
				return v;
			}
		}
		return nullopt;
	}
	[[nodiscard]] V<SV> values(
		SV key) const {
		V<SV> out;
		for (auto const &[k, v]: data_) {
			if (key_eq(k, key)) {
				out.push_back(v);
			}
		}
		return out;
	}
	[[nodiscard]] bool contains(
		SV key) const noexcept {
		for (auto const &[k, v]: data_) {
			if (key_eq(k, key)) {
				return true;
			}
		}
		return false;
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
	}
	void emplace_back_owned(
		S k,
		S v) {
		data_.emplace_back(store_owned(move(k)), store_owned(move(v)));
	}
	void clear() noexcept {
		data_.clear();
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
export namespace conflux::http {

// ─── errors ──────────────────────────────────────────────────────────────────

enum class HttpErrorKind : u8 {
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

enum class HttpPhase : u8 {
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
	S verify_reason{};
	S message{};
};
// ─── timeouts ────────────────────────────────────────────────────────────────

struct HttpTimeouts {
	chrono::milliseconds resolve{5000};
	chrono::milliseconds connect{5000};
	chrono::milliseconds tls{5000};
	chrono::milliseconds write{30000};
	chrono::milliseconds first_byte{30000};
	chrono::milliseconds between_bytes{30000};
};
// ─── telemetry ───────────────────────────────────────────────────────────────

struct HttpTelemetry {
	chrono::nanoseconds dns{};
	chrono::nanoseconds connect{};
	chrono::nanoseconds tls{};
	chrono::nanoseconds ttfb{};
	chrono::nanoseconds body{};
	Opt<chrono::nanoseconds> pool_wait{};
	u64 bytes_sent{0};
	u64 bytes_received{0};
	bool reused_connection{false};
	S negotiated_protocol{};
	S tls_cipher{};
	S tls_version{};
	bool tls_verified{false};
	S peer_addr{};
	Opt<S> decoded_encoding{};
};

// ─── URL ─────────────────────────────────────────────────────────────────────

enum class UrlErrorKind : u8 {
	empty,
	missing_scheme,
	unsupported_scheme,
	missing_host,
	invalid_port,
	too_long,
};
struct UrlError {
	UrlErrorKind kind{UrlErrorKind::empty};
	S message{};
};
struct Url {
	S scheme{};
	S host{};
	u16 port{80};
	S path{"/"};
	S query{}; // raw, without leading '?'

	[[nodiscard]] static expected<Url, UrlError> parse(SV input);
	[[nodiscard]] S str() const {
		S out;
		out.reserve(scheme.size() + 3 + host.size() + 7 + path.size() + query.size() + 1);
		out += scheme;
		out += "://";
		out += host;
		bool const default_port = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
		if (!default_port) {
			out += ':';
			out += to_string(port);
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
		SV name,
		SV value) {
		auto encode = [](SV s) {
			S out;
			out.reserve(s.size());
			for (auto const raw_c: s) {
				unsigned char const c = static_cast<unsigned char>(raw_c);
				if ((c >= 'A' && c <= 'Z')
					|| (c >= 'a' && c <= 'z')
					|| (c >= '0' && c <= '9')
					|| c == '-'
					|| c == '_'
					|| c == '.'
					|| c == '~') {
					out += static_cast<char>(c);
				} else {
					out += format("%{:02X}", c);
				}
			}
			return out;
		};
		if (!query.empty()) {
			query += '&';
		}
		query += encode(name);
		query += '=';
		query += encode(value);
	}
};
expected<Url, UrlError> Url::parse(
	SV input) {
	if (input.empty()) {
		return unexpected(UrlError{UrlErrorKind::empty, "empty URL"});
	}
	constexpr SZ kMaxUrl = 8192;
	if (input.size() > kMaxUrl) {
		return unexpected(UrlError{UrlErrorKind::too_long, "URL exceeds 8192 bytes"});
	}

	auto const scheme_end = input.find("://");
	if (scheme_end == SV::npos) {
		return unexpected(UrlError{UrlErrorKind::missing_scheme, "missing '://'"});
	}

	Url url;
	url.scheme.resize(scheme_end);
	for (SZ i = 0; i < scheme_end; ++i) {
		url.scheme[i] = static_cast<char>(tolower(static_cast<unsigned char>(input[i])));
	}

	if (url.scheme != "http" && url.scheme != "https") {
		return unexpected(UrlError{UrlErrorKind::unsupported_scheme, format("unsupported scheme '{}'", url.scheme)});
	}
	url.port = (url.scheme == "https") ? u16{443} : u16{80};

	auto rest = input.substr(scheme_end + 3);
	if (rest.empty()) {
		return unexpected(UrlError{UrlErrorKind::missing_host, "missing host"});
	}

	auto const authority_end = rest.find_first_of("/?");
	auto const authority = (authority_end == SV::npos) ? rest : rest.substr(0, authority_end);

	if (authority.empty()) {
		return unexpected(UrlError{UrlErrorKind::missing_host, "missing host"});
	}

	if (authority.starts_with('[')) {
		auto const bracket_end = authority.find(']');
		if (bracket_end == SV::npos) {
			return unexpected(UrlError{UrlErrorKind::missing_host, "unterminated IPv6 literal"});
		}
		url.host = S{authority.substr(0, bracket_end + 1)};
		auto const after = authority.substr(bracket_end + 1);
		if (!after.empty()) {
			if (after[0] != ':') {
				return unexpected(UrlError{UrlErrorKind::invalid_port, "unexpected character after ']'"});
			}
			auto const port_sv = after.substr(1);
			u16 p = 0;
			auto const [ptr, ec] = from_chars(port_sv.data(), port_sv.data() + port_sv.size(), p);
			if (ec != errc{} || ptr != port_sv.data() + port_sv.size() || p == 0) {
				return unexpected(UrlError{UrlErrorKind::invalid_port, format("invalid port '{}'", port_sv)});
			}
			url.port = p;
		}
	} else {
		auto const colon = authority.rfind(':');
		if (colon == SV::npos) {
			url.host = S{authority};
		} else {
			url.host = S{authority.substr(0, colon)};
			auto const port_sv = authority.substr(colon + 1);
			u16 p = 0;
			auto const [ptr, ec] = from_chars(port_sv.data(), port_sv.data() + port_sv.size(), p);
			if (ec != errc{} || ptr != port_sv.data() + port_sv.size() || p == 0) {
				return unexpected(UrlError{UrlErrorKind::invalid_port, format("invalid port '{}'", port_sv)});
			}
			url.port = p;
		}
	}

	if (url.host.empty()) {
		return unexpected(UrlError{UrlErrorKind::missing_host, "empty host"});
	}

	if (authority_end == SV::npos) {
		url.path = "/";
	} else {
		auto const path_and_query = rest.substr(authority_end);
		auto const qmark = path_and_query.find('?');
		if (qmark == SV::npos) {
			url.path = S{path_and_query};
			if (url.path.empty()) {
				url.path = "/";
			}
		} else {
			url.path = S{path_and_query.substr(0, qmark)};
			if (url.path.empty()) {
				url.path = "/";
			}
			url.query = S{path_and_query.substr(qmark + 1)};
		}
	}

	return url;
}
[[nodiscard]] bool ascii_iequals(
	SV lhs,
	SV rhs) noexcept {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	return ranges::equal(lhs, rhs, [](unsigned char x, unsigned char y) {
		return ascii_ci_fold(x) == ascii_ci_fold(y);
	});
}
constexpr A<SV, 8> kHopByHopHeaders{
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
	SV name) noexcept {
	return ranges::contains(kHopByHopHeaders, name);
}
[[nodiscard]] bool header_token_contains(
	SV header,
	SV token) noexcept {
	while (!header.empty()) {
		auto const comma = header.find(',');
		auto const part = http_trim((comma == SV::npos) ? header : header.substr(0, comma));
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

} // namespace conflux::http
