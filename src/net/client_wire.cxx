export module conflux.net.client_wire;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.request;
import conflux.net.http.parse_helpers;
import conflux.utils;

namespace client_wire_detail {

using namespace conflux::http;

[[nodiscard]] bool default_header_key_eq(
	conflux::http::HttpFields const &defaults,
	std::string_view lhs,
	std::string_view rhs) noexcept {
	return defaults.case_insensitive() ? ascii_iequals(lhs, rhs) : lhs == rhs;
}

[[nodiscard]] bool serializable_request_header(
	std::string_view name) noexcept {
	return !is_request_controlled_header(name);
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

void append_header_line(
	std::string &out,
	std::string_view name,
	std::string_view value) {
	out += name;
	out += ": ";
	out += value;
	out += "\r\n";
}

struct EffectiveRequestHeader {
	std::string_view name;
	std::string_view value;
	bool default_header{};
	bool emit{true};
};

[[nodiscard]] std::vector<EffectiveRequestHeader> make_effective_request_headers(
	conflux::http::HttpFields const &defaults,
	conflux::http::HttpFields const &headers) {
	std::vector<EffectiveRequestHeader> out;
	out.reserve(defaults.size() + headers.size());
	for (auto const &[k, v]: defaults) {
		if (serializable_request_header(k)) {
			out.push_back(EffectiveRequestHeader{.name = k, .value = v, .default_header = true});
		}
	}
	for (auto const &[k, v]: headers) {
		if (!serializable_request_header(k)) {
			continue;
		}
		auto match = std::ranges::find_if(out, [&](EffectiveRequestHeader const &candidate) {
			return candidate.default_header && default_header_key_eq(defaults, candidate.name, k);
		});
		if (match == out.end()) {
			out.push_back(EffectiveRequestHeader{.name = k, .value = v});
			continue;
		}
		match->value = v;
		auto const first_name = match->name;
		for (auto it = std::next(match); it != out.end(); ++it) {
			if (it->default_header && default_header_key_eq(defaults, it->name, first_name)) {
				it->emit = false;
			}
		}
	}
	return out;
}

[[nodiscard]] std::expected<std::size_t, HttpError> parse_content_length(
	std::string_view value,
	std::size_t max_body_bytes) {
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
		value.remove_prefix(1);
	}
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
		value.remove_suffix(1);
	}
	if (value.empty()) {
		return std::unexpected(HttpError{.kind = HttpErrorKind::protocol, .message = "invalid Content-Length"});
	}
	auto parsed = conflux::http::parse_content_length_value(value);
	if (!parsed) {
		return std::unexpected(
			HttpError{.kind = HttpErrorKind::protocol, .message = std::format("invalid Content-Length '{}'", value)});
	}
	if (*parsed > max_body_bytes) {
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::body_too_large,
				.message = std::format("Content-Length {} exceeds limit {}", *parsed, max_body_bytes)});
	}
	return *parsed;
}

[[nodiscard]] std::size_t estimate_request_wire_size(
	ClientRequest const &req,
	std::span<EffectiveRequestHeader const> effective_headers,
	std::string_view caller_host) noexcept {
	auto const &url = req.url();
	std::size_t n = req.method().size()
				  + 1
				  + url.origin_form_target_size()
				  + sizeof(" HTTP/1.1\r\nHost: ")
				  - 1
				  + url.host_header_value_size(caller_host)
				  + 2;
	for (auto const &header: effective_headers) {
		if (header.emit) {
			n += header.name.size() + 2 + header.value.size() + 2;
		}
	}
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
	conflux::http::HttpFields headers = conflux::http::HttpFields(true);
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

[[nodiscard]] std::expected<std::optional<ClientRequest>, HttpError> follow_redirect_request(
	ClientRequest const &req,
	int status,
	conflux::http::HttpFields const &response_headers) {
	if (!is_redirect_status(status)) {
		return std::nullopt;
	}
	auto const location = response_headers["location"];
	if (location.empty()) {
		return std::nullopt;
	}
	if (!req.follows_redirects()) {
		return std::nullopt;
	}
	if (req.max_redirects() <= 0) {
		return std::unexpected(HttpError{.kind = HttpErrorKind::redirect_limit, .message = "redirect limit exceeded"});
	}
	auto next_url = resolve_redirect_target(req.url(), location);
	if (!next_url) {
		return std::nullopt;
	}
	bool const cross_origin = !same_origin(req.url(), *next_url);
	bool const drop_body = status == 303;
	conflux::http::HttpFields next_headers{req.headers().case_insensitive()};
	next_headers.clear();
	for (auto const &[k, v]: req.headers()) {
		if (ascii_iequals(k, "host")) {
			continue;
		}
		if (cross_origin
			&& (ascii_iequals(k, "authorization")
				|| ascii_iequals(k, "cookie")
				|| ascii_iequals(k, "proxy-authorization"))) {
			continue;
		}
		if (drop_body && (ascii_iequals(k, "content-length") || ascii_iequals(k, "content-type"))) {
			continue;
		}
		next_headers.set(k, v);
	}
	auto builder = ClientRequest::method(drop_body ? "GET" : req.method(), next_url->str())
					   .headers(next_headers)
					   .timeouts(req.timeouts())
					   .verify_peer(req.verify_peer());
	if (!drop_body && !req.body().empty()) {
		builder.body(req.body());
	}
	if (!req.server_name().empty()) {
		builder.server_name(req.server_name());
	}
	builder.follow_redirects(req.max_redirects() - 1);
	return std::move(builder).build();
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
				auto parsed_content_length = client_wire_detail::parse_content_length(v, max_body_bytes);
				if (!parsed_content_length) {
					return std::unexpected(std::move(parsed_content_length).error());
				}
				if (parsed.has_content_length && parsed.content_length != *parsed_content_length) {
					return std::unexpected(
						HttpError{.kind = HttpErrorKind::protocol, .message = "conflicting Content-Length headers"});
				}
				parsed.content_length = *parsed_content_length;
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
	if (parsed.has_content_length && parsed.chunked) {
		return std::unexpected(
			HttpError{
				.kind = HttpErrorKind::protocol,
				.message = "response has both Content-Length and chunked Transfer-Encoding"});
	}
	return parsed;
}

[[nodiscard]] std::string build_http1_request_wire(
	ClientRequest const &req,
	conflux::http::HttpFields const &default_headers) {
	using namespace client_wire_detail;
	auto const &url = req.url();
	auto const caller_host = req.headers()["host"];
	auto const effective_headers = make_effective_request_headers(default_headers, req.headers());
	std::string wire;
	wire.reserve(estimate_request_wire_size(req, effective_headers, caller_host));
	wire += req.method();
	wire += ' ';
	url.append_origin_form_target(wire);
	wire += " HTTP/1.1\r\nHost: ";
	url.append_host_header_value(wire, caller_host);
	wire += "\r\n";

	for (auto const &header: effective_headers) {
		if (header.emit) {
			append_header_line(wire, header.name, header.value);
		}
	}
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
