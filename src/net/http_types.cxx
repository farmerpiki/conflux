module;
#include <cctype>

export module conflux.net.http.types;
import std;
import conflux.types;

using namespace std;

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
	string verify_reason{};
	string message{};
};

// ─── timeouts ────────────────────────────────────────────────────────────────

struct HttpTimeouts {
	chrono::milliseconds resolve{5'000};
	chrono::milliseconds connect{5'000};
	chrono::milliseconds tls{5'000};
	chrono::milliseconds write{30'000};
	chrono::milliseconds first_byte{30'000};
	chrono::milliseconds between_bytes{30'000};
};

// ─── telemetry ───────────────────────────────────────────────────────────────

struct HttpTelemetry {
	chrono::nanoseconds dns{};
	chrono::nanoseconds connect{};
	chrono::nanoseconds tls{};
	chrono::nanoseconds ttfb{};
	chrono::nanoseconds body{};
	optional<chrono::nanoseconds> pool_wait{};
	u64 bytes_sent{0};
	u64 bytes_received{0};
	bool reused_connection{false};
	string negotiated_protocol{};
	string tls_cipher{};
	string tls_version{};
	bool tls_verified{false};
	string peer_addr{};
	optional<string> decoded_encoding{};
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
	string message{};
};

struct Url {
	string scheme{};
	string host{};
	u16 port{80};
	string path{"/"};
	string query{}; // raw, without leading '?'

	[[nodiscard]] static expected<Url, UrlError> parse(string_view input);

	[[nodiscard]] string str() const {
		string out;
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
		string_view name,
		string_view value) {
		auto encode = [](string_view s) {
			string out;
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
	string_view input) {
	if (input.empty()) {
		return unexpected(UrlError{UrlErrorKind::empty, "empty URL"});
	}
	constexpr size_t kMaxUrl = 8192;
	if (input.size() > kMaxUrl) {
		return unexpected(UrlError{UrlErrorKind::too_long, "URL exceeds 8192 bytes"});
	}

	auto const scheme_end = input.find("://");
	if (scheme_end == string_view::npos) {
		return unexpected(UrlError{UrlErrorKind::missing_scheme, "missing '://'"});
	}

	Url url;
	url.scheme.resize(scheme_end);
	for (size_t i = 0; i < scheme_end; ++i) {
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
	auto const authority = (authority_end == string_view::npos) ? rest : rest.substr(0, authority_end);

	if (authority.empty()) {
		return unexpected(UrlError{UrlErrorKind::missing_host, "missing host"});
	}

	if (authority.starts_with('[')) {
		auto const bracket_end = authority.find(']');
		if (bracket_end == string_view::npos) {
			return unexpected(UrlError{UrlErrorKind::missing_host, "unterminated IPv6 literal"});
		}
		url.host = string{authority.substr(0, bracket_end + 1)};
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
		if (colon == string_view::npos) {
			url.host = string{authority};
		} else {
			url.host = string{authority.substr(0, colon)};
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

	if (authority_end == string_view::npos) {
		url.path = "/";
	} else {
		auto const path_and_query = rest.substr(authority_end);
		auto const qmark = path_and_query.find('?');
		if (qmark == string_view::npos) {
			url.path = string{path_and_query};
			if (url.path.empty()) {
				url.path = "/";
			}
		} else {
			url.path = string{path_and_query.substr(0, qmark)};
			if (url.path.empty()) {
				url.path = "/";
			}
			url.query = string{path_and_query.substr(qmark + 1)};
		}
	}

	return url;
}

} // namespace conflux::http
