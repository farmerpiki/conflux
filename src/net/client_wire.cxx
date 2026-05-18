export module conflux.net.client_wire;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.request;

namespace client_wire_detail {
using namespace conflux::http;

[[nodiscard]] bool default_header_key_eq(
	HttpFields const &defaults,
	SV lhs,
	SV rhs) noexcept {
	return defaults.case_insensitive() ? ascii_iequals(lhs, rhs) : lhs == rhs;
}

[[nodiscard]] bool serializable_request_header(
	SV name) noexcept {
	return !ascii_iequals(name, "host") && !is_hop_by_hop_header(name);
}

void append_decimal(
	S &out,
	SZ value) {
	A<char, 32> buf{};
	auto const [ptr, ec] = to_chars(buf.data(), buf.data() + buf.size(), value);
	if (ec == errc{}) {
		out.append(buf.data(), static_cast<SZ>(ptr - buf.data()));
	}
}

[[nodiscard]] SZ decimal_size(
	SZ value) noexcept {
	SZ n = 1;
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

[[nodiscard]] SZ host_header_value_size(
	Url const &url,
	SV caller_host) noexcept {
	if (!caller_host.empty()) {
		return caller_host.size();
	}
	if (url_uses_default_port(url)) {
		return url.host.size();
	}
	return url.host.size() + 1 + decimal_size(url.port);
}

void append_host_header_value(
	S &out,
	Url const &url,
	SV caller_host) {
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

[[nodiscard]] SZ request_target_size(
	Url const &url) noexcept {
	SZ n = url.path.empty() ? 1 : url.path.size();
	if (!url.query.empty()) {
		n += 1 + url.query.size();
	}
	return n;
}

void append_request_target(
	S &out,
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
	S &out,
	SV name,
	SV value) {
	out += name;
	out += ": ";
	out += value;
	out += "\r\n";
}

[[nodiscard]] Opt<SV> find_request_override(
	HttpFields const &defaults,
	HttpFields const &headers,
	SV default_name) noexcept {
	Opt<SV> found{};
	for (auto const &[k, v]: headers) {
		if (serializable_request_header(k) && default_header_key_eq(defaults, k, default_name)) {
			found = SV{v};
		}
	}
	return found;
}

[[nodiscard]] bool default_has_prior_key(
	HttpFields const &defaults,
	SZ index) noexcept {
	SV current_name;
	bool current_found = false;
	SZ i = 0;
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
	SV request_name) noexcept {
	for (auto const &[k, v]: defaults) {
		(void)v;
		if (serializable_request_header(k) && default_header_key_eq(defaults, k, request_name)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] SZ estimate_request_wire_size(
	ClientRequest const &req,
	HttpFields const &default_headers,
	SV caller_host) noexcept {
	auto const &url = req.url();
	SZ n = req.method().size() + 1 + request_target_size(url) + sizeof(" HTTP/1.1\r\nHost: ") - 1
		   + host_header_value_size(url, caller_host) + 2;
	for (auto const &[k, v]: default_headers) {
		if (serializable_request_header(k)) {
			n += k.size() + 2 + v.size() + 2;
		}
	}
	for (auto const &[k, v]: req.headers()) {
		if (serializable_request_header(k)) {
			n += k.size() + 2 + v.size() + 2;
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

[[nodiscard]] S build_http1_request_wire(
	ClientRequest const &req,
	HttpFields const &default_headers) {
	using namespace client_wire_detail;
	auto const &url = req.url();
	auto const caller_host = req.headers()["host"];
	S wire;
	wire.reserve(estimate_request_wire_size(req, default_headers, caller_host));
	wire += req.method();
	wire += ' ';
	append_request_target(wire, url);
	wire += " HTTP/1.1\r\nHost: ";
	append_host_header_value(wire, url, caller_host);
	wire += "\r\n";

	SZ default_index = 0;
	for (auto const &[k, v]: default_headers) {
		if (!serializable_request_header(k)) {
			++default_index;
			continue;
		}
		auto const override = find_request_override(default_headers, req.headers(), k);
		if (override) {
			if (!default_has_prior_key(default_headers, default_index)) {
				append_header_line(wire, k, *override);
			}
		} else {
			append_header_line(wire, k, v);
		}
		++default_index;
	}
	for (auto const &[k, v]: req.headers()) {
		if (serializable_request_header(k) && !request_matches_default(default_headers, k)) {
			append_header_line(wire, k, v);
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
