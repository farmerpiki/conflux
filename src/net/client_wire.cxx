export module conflux.net.client_wire;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.request;
import conflux.utils;

namespace client_wire_detail {

using namespace conflux::http;

[[nodiscard]] bool default_header_key_eq(
	HttpFields const &defaults,
	std::string_view lhs,
	std::string_view rhs) noexcept {
	return defaults.case_insensitive() ? ascii_iequals(lhs, rhs) : lhs == rhs;
}

[[nodiscard]] bool serializable_request_header(
	std::string_view name) noexcept {
	return !ascii_iequals(name, "host") && !is_hop_by_hop_header(name);
}

void append_decimal(
	std::string &out,
	std::size_t value) {
	std::array<char, 32> buf{};
	auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
	if (ec == std::errc{}) {
		out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
	}
}

[[nodiscard]] std::size_t decimal_size(
	std::size_t value) noexcept {
	std::size_t n = 1;
	while (value >= 10) {
		value /= 10;
		++n;
	}
	return n;
}

[[nodiscard]] bool url_uses_default_port(
	Url const &url) noexcept {
	return (url.scheme == "http" && url.port == 80) || (url.scheme == "https" && url.port == 443);
}

[[nodiscard]] std::size_t host_header_value_size(
	Url const &url,
	std::string_view caller_host) noexcept {
	if (!caller_host.empty()) {
		return caller_host.size();
	}
	if (url_uses_default_port(url)) {
		return url.host.size();
	}
	return url.host.size() + 1 + decimal_size(url.port);
}

void append_host_header_value(
	std::string &out,
	Url const &url,
	std::string_view caller_host) {
	if (!caller_host.empty()) {
		out += caller_host;
		return;
	}
	out += url.host;
	if (!url_uses_default_port(url)) {
		out += ':';
		append_decimal(out, url.port);
	}
}

[[nodiscard]] std::size_t request_target_size(
	Url const &url) noexcept {
	std::size_t n = url.path.empty() ? 1 : url.path.size();
	if (!url.query.empty()) {
		n += 1 + url.query.size();
	}
	return n;
}

void append_request_target(
	std::string &out,
	Url const &url) {
	if (url.path.empty()) {
		out += '/';
	} else {
		out += url.path;
	}
	if (!url.query.empty()) {
		out += '?';
		out += url.query;
	}
}

void append_header_line(
	std::string &out,
	std::string_view name,
	std::string_view value) {
	out += name;
	out += ": ";
	out += value;
	out += "\r\n";
}

[[nodiscard]] std::optional<std::string_view> find_request_override(
	HttpFields const &defaults,
	HttpFields const &headers,
	std::string_view default_name) noexcept {
	std::optional<std::string_view> found{};
	for (auto const &[k, v]: headers) {
		if (serializable_request_header(k) && default_header_key_eq(defaults, k, default_name)) {
			found = std::string_view{v};
		}
	}
	return found;
}

[[nodiscard]] bool default_has_prior_key(
	HttpFields const &defaults,
	std::size_t index) noexcept {
	std::string_view current_name;
	bool current_found = false;
	std::size_t i = 0;
	for (auto const &[k, v]: defaults) {
		(void)v;
		if (i == index) {
			current_name = k;
			current_found = true;
			break;
		}
		++i;
	}
	if (!current_found) {
		return false;
	}
	i = 0;
	for (auto const &[k, v]: defaults) {
		(void)v;
		if (i == index) {
			return false;
		}
		if (serializable_request_header(k) && default_header_key_eq(defaults, k, current_name)) {
			return true;
		}
		++i;
	}
	return false;
}

[[nodiscard]] bool request_matches_default(
	HttpFields const &defaults,
	std::string_view request_name) noexcept {
	for (auto const &[k, v]: defaults) {
		(void)v;
		if (serializable_request_header(k) && default_header_key_eq(defaults, k, request_name)) {
			return true;
		}
	}
	return false;
}

template<class F>
void for_each_effective_request_header(
	HttpFields const &default_headers,
	HttpFields const &request_headers,
	F &&fn) {
	std::size_t default_index = 0;
	for (auto const &[k, v]: default_headers) {
		if (!serializable_request_header(k)) {
			++default_index;
			continue;
		}
		auto const override = find_request_override(default_headers, request_headers, k);
		if (override) {
			if (!default_has_prior_key(default_headers, default_index)) {
				fn(k, *override);
			}
		} else {
			fn(k, v);
		}
		++default_index;
	}
	for (auto const &[k, v]: request_headers) {
		if (serializable_request_header(k) && !request_matches_default(default_headers, k)) {
			fn(k, v);
		}
	}
}

[[nodiscard]] std::size_t estimate_request_wire_size(
	ClientRequest const &req,
	HttpFields const &default_headers,
	std::string_view caller_host) noexcept {
	auto const &url = req.url();
	std::size_t n = req.method().size()
				  + 1
				  + request_target_size(url)
				  + sizeof(" HTTP/1.1\r\nHost: ")
				  - 1
				  + host_header_value_size(url, caller_host)
				  + 2;
	for_each_effective_request_header(default_headers, req.headers(), [&n](std::string_view k, std::string_view v) {
		n += k.size() + 2 + v.size() + 2;
	});
	n += sizeof("Connection: close\r\n") - 1;
	if (!req.body().empty()) {
		n += sizeof("Content-Length: \r\n") - 1 + decimal_size(req.body().size());
	}
	n += 2;
	return n;
}

} // namespace client_wire_detail

export namespace conflux::http::client_wire {

struct ParsedResponseHead {
	int status{502};
	std::string status_text{};
	HttpFields headers = HttpFields(true);
	std::vector<std::string> set_cookies{};
	std::size_t content_length{0};
	bool has_content_length{false};
	bool chunked{false};
};

[[nodiscard]] bool is_redirect_status(
	int status) noexcept {
	return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

[[nodiscard]] bool same_origin(
	Url const &a,
	Url const &b) noexcept {
	return a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}

[[nodiscard]] std::optional<Url> resolve_redirect_target(
	Url const &base,
	std::string_view location) {
	if (location.empty() || location.find_first_of("\r\n") != std::string_view::npos) {
		return std::nullopt;
	}
	std::string loc{location};
	auto const frag = loc.find('#');
	if (frag != std::string::npos) {
		loc.erase(frag);
	}
	if (loc.empty()) {
		return std::nullopt;
	}
	if (loc.starts_with("//")) {
		std::string abs_url;
		abs_url.reserve(base.scheme.size() + 1 + loc.size());
		abs_url += base.scheme;
		abs_url += ':';
		abs_url += loc;
		auto abs = Url::parse(abs_url);
		return abs ? std::optional<Url>{std::move(*abs)} : std::nullopt;
	}
	if (auto abs = Url::parse(loc); abs) {
		return std::move(*abs);
	}
	Url next = base;
	auto const q = loc.find('?');
	if (q != std::string::npos) {
		next.query = loc.substr(q + 1);
		loc.erase(q);
		if (loc.empty()) {
			return next;
		}
	} else {
		next.query.clear();
	}
	if (loc.starts_with('/')) {
		next.path = std::move(loc);
		return next;
	}
	std::string_view const base_path = next.path.empty() ? std::string_view{"/"} : std::string_view{next.path};
	auto const slash = base_path.rfind('/');
	if (slash == std::string_view::npos) {
		next.path.clear();
		next.path.reserve(1 + loc.size());
		next.path.push_back('/');
		next.path += loc;
	} else {
		std::string new_path;
		new_path.reserve(slash + 1 + loc.size());
		new_path.append(base_path.data(), slash + 1);
		new_path += loc;
		next.path = std::move(new_path);
	}
	return next;
}

void accumulate_telemetry(
	HttpTelemetry &total,
	HttpTelemetry const &hop) {
	total.dns += hop.dns;
	total.connect += hop.connect;
	total.tls += hop.tls;
	total.ttfb += hop.ttfb;
	total.body += hop.body;
	if (hop.pool_wait) {
		total.pool_wait = total.pool_wait ? *total.pool_wait + *hop.pool_wait : hop.pool_wait;
	}
	total.bytes_sent += hop.bytes_sent;
	total.bytes_received += hop.bytes_received;
	total.reused_connection = total.reused_connection || hop.reused_connection;
	if (!hop.negotiated_protocol.empty()) {
		total.negotiated_protocol = hop.negotiated_protocol;
	}
	if (!hop.tls_cipher.empty()) {
		total.tls_cipher = hop.tls_cipher;
	}
	if (!hop.tls_version.empty()) {
		total.tls_version = hop.tls_version;
	}
	total.tls_verified = total.tls_verified || hop.tls_verified;
	if (!hop.peer_addr.empty()) {
		total.peer_addr = hop.peer_addr;
	}
	if (hop.decoded_encoding) {
		total.decoded_encoding = hop.decoded_encoding;
	}
}

enum class ChunkedDecodeStatus : std::uint8_t {
	complete,
	incomplete,
	invalid,
};

ChunkedDecodeStatus decode_chunked_prefix(
	std::string_view encoded,
	std::string &decoded,
	std::size_t &consumed) {
	for (;;) {
		auto const line_end = encoded.find("\r\n", consumed);
		if (line_end == std::string_view::npos) {
			return ChunkedDecodeStatus::incomplete;
		}
		auto size_str = trim(encoded.substr(consumed, line_end - consumed));
		if (auto const semi = size_str.find(';'); semi != std::string_view::npos) {
			size_str = trim(size_str.substr(0, semi));
		}
		if (size_str.empty()) {
			return ChunkedDecodeStatus::invalid;
		}
		std::size_t chunk_size = 0;
		auto const parsed = std::from_chars(size_str.data(), size_str.data() + size_str.size(), chunk_size, 16);
		if (parsed.ec != std::errc{} || parsed.ptr != size_str.data() + size_str.size()) {
			return ChunkedDecodeStatus::invalid;
		}
		consumed = line_end + 2;
		if (chunk_size == 0) {
			for (;;) {
				auto const eol = encoded.find("\r\n", consumed);
				if (eol == std::string_view::npos) {
					return ChunkedDecodeStatus::incomplete;
				}
				bool const empty = (eol == consumed);
				consumed = eol + 2;
				if (empty) {
					return ChunkedDecodeStatus::complete;
				}
			}
		}
		if (encoded.size() < consumed + chunk_size + 2) {
			return ChunkedDecodeStatus::incomplete;
		}
		decoded.append(encoded.substr(consumed, chunk_size));
		consumed += chunk_size;
		if (encoded.substr(consumed, 2) != "\r\n") {
			return ChunkedDecodeStatus::invalid;
		}
		consumed += 2;
	}
}

[[nodiscard]] std::expected<ParsedResponseHead, HttpError> parse_http1_response_head(
	std::string_view headers_str,
	std::size_t max_body_bytes) {
	ParsedResponseHead parsed;
	auto const nl = headers_str.find("\r\n");
	auto const status_line = (nl != std::string_view::npos) ? headers_str.substr(0, nl) : headers_str;
	auto const sp1 = status_line.find(' ');
	if (sp1 == std::string_view::npos) {
		return std::unexpected(HttpError{.kind = HttpErrorKind::protocol, .message = "malformed status line"});
	}
	auto const rest = status_line.substr(sp1 + 1);
	auto const sp2 = rest.find(' ');
	auto const code_sv = (sp2 != std::string_view::npos) ? rest.substr(0, sp2) : rest;
	int status = 0;
	auto const [ptr, ec] = std::from_chars(code_sv.data(), code_sv.data() + code_sv.size(), status);
	if (ec != std::errc{} || status < 100 || status > 999) {
		return std::unexpected(
			HttpError{.kind = HttpErrorKind::protocol, .message = std::format("invalid status code '{}'", code_sv)});
	}
	parsed.status = status;
	if (sp2 != std::string_view::npos) {
		parsed.status_text = std::string{rest.substr(sp2 + 1)};
	}
	std::size_t pos = (nl != std::string_view::npos) ? nl + 2 : headers_str.size();
	while (pos < headers_str.size()) {
		auto const end = headers_str.find("\r\n", pos);
		auto const hdr = (end != std::string_view::npos) ? headers_str.substr(pos, end - pos) : headers_str.substr(pos);
		auto const colon = hdr.find(':');
		if (colon != std::string_view::npos) {
			auto k = hdr.substr(0, colon);
			auto v = hdr.substr(colon + 1);
			while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) {
				v.remove_prefix(1);
			}
			if (ascii_iequals(k, "content-length")) {
				std::from_chars(v.data(), v.data() + v.size(), parsed.content_length);
				parsed.has_content_length = true;
			} else if (ascii_iequals(k, "transfer-encoding") && header_token_contains(v, "chunked")) {
				parsed.chunked = true;
			} else if (ascii_iequals(k, "set-cookie")) {
				parsed.set_cookies.push_back(std::string{v});
			} else if (!conflux::http::is_hop_by_hop_header(k)) {
				parsed.headers.set(std::string{k}, std::string{v});
			}
		}
		pos = (end != std::string_view::npos) ? end + 2 : headers_str.size();
	}
	if (parsed.has_content_length && parsed.content_length > max_body_bytes) {
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::body_too_large,
				.message = std::format("Content-Length {} exceeds limit {}", parsed.content_length, max_body_bytes)});
	}
	return parsed;
}

[[nodiscard]] std::string build_http1_request_wire(
	ClientRequest const &req,
	HttpFields const &default_headers) {
	using namespace client_wire_detail;
	auto const &url = req.url();
	auto const caller_host = req.headers()["host"];
	std::string wire;
	wire.reserve(estimate_request_wire_size(req, default_headers, caller_host));
	wire += req.method();
	wire += ' ';
	append_request_target(wire, url);
	wire += " HTTP/1.1\r\nHost: ";
	append_host_header_value(wire, url, caller_host);
	wire += "\r\n";

	for_each_effective_request_header(default_headers, req.headers(), [&wire](std::string_view k, std::string_view v) {
		append_header_line(wire, k, v);
	});
	wire += "Connection: close\r\n";
	if (!req.body().empty()) {
		wire += "Content-Length: ";
		append_decimal(wire, req.body().size());
		wire += "\r\n";
	}
	wire += "\r\n";
	return wire;
}

} // namespace conflux::http::client_wire
