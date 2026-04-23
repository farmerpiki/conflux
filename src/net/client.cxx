module;
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

export module conflux.net.client;
import std;
import conflux.types;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
import conflux.net.router;
import conflux.utils;
import conflux.work;

using namespace std;

export struct ClientRequest {
	string method{"GET"};
	string host{};
	u16 port{80};
	string path{"/"};
	HttpFields headers{true};
	string body{};
	int timeout_sec{10};
	bool use_tls{false};
	bool verify_peer{true};
	string server_name{};
	// When non-empty, used as the Host header value instead of host+port.
	// host is still used for the TCP connection.
	string host_override{};
};

export struct ClientResponse {
	int status{502};
	string status_text{"Bad Gateway"};
	string content_type{"application/octet-stream"};
	HttpFields headers{true};
	vector<string> set_cookies{};
	string body{};
};

export struct ClientOptions {
	string host{};
	u16 port{80};
	int timeout_sec{10};
	bool use_tls{false};
	bool verify_peer{true};
	string server_name{};
	HttpFields default_headers{true};
};

namespace client_detail {

struct Connection {
	int fd{-1};
	bool use_tls{false};
#if CONFLUX_HAS_TLS
	optional<TlsContext> tls_ctx{};
	optional<TlsStream> tls_stream{};
#endif
};

constexpr array<string_view, 8> kHopByHop{
	"connection",
	"keep-alive",
	"proxy-authenticate",
	"proxy-authorization",
	"te",
	"trailers",
	"transfer-encoding",
	"upgrade",
};

[[gnu::pure]] bool is_hop_by_hop(
	string_view name) {
	return ranges::contains(kHopByHop, name);
}

[[nodiscard]] string host_header(
	ClientRequest const &req) {
	if ((!req.use_tls && req.port == 80) || (req.use_tls && req.port == 443)) {
		return req.host;
	}
	return format("{}:{}", req.host, req.port);
}

bool connect_with_timeout(
	int fd,
	sockaddr const *addr,
	socklen_t addrlen,
	int timeout_sec) {
	if (timeout_sec <= 0) {
		return ::connect(fd, addr, addrlen) == 0;
	}

	int const flags = ::fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		return false;
	}
	if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		return false;
	}

	int const rc = ::connect(fd, addr, addrlen);
	if (rc == 0) {
		::fcntl(fd, F_SETFL, flags);
		return true;
	}
	if (errno != EINPROGRESS) {
		::fcntl(fd, F_SETFL, flags);
		return false;
	}
	if (!wait_fd(fd, POLLOUT, timeout_sec)) {
		::fcntl(fd, F_SETFL, flags);
		return false;
	}

	int so_error = 0;
	socklen_t so_error_len = sizeof(so_error);
	bool const ok = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) == 0 && so_error == 0;
	::fcntl(fd, F_SETFL, flags);
	return ok;
}

void close_connection(
	Connection &conn) noexcept {
#if CONFLUX_HAS_TLS
	if (conn.tls_stream.has_value()) {
		conn.tls_stream->shutdown_safe();
		conn.tls_stream.reset();
	}
	conn.tls_ctx.reset();
#endif
	if (conn.fd >= 0) {
		::close(conn.fd);
		conn.fd = -1;
	}
}

#if CONFLUX_HAS_TLS
bool enable_tls(
	Connection &conn,
	string_view server_name,
	int timeout_sec,
	bool verify_peer) {
	try {
		conn.tls_ctx.emplace();
	} catch (TlsError const &) { return false; }
	conn.tls_ctx->set_verify_peer(verify_peer);
	if (verify_peer) {
		if (!conn.tls_ctx->set_default_verify_paths()) {
			return false;
		}
	}

	try {
		conn.tls_stream.emplace(*conn.tls_ctx, conn.fd);
	} catch (TlsError const &) { return false; }
	if (!conn.tls_stream->set_server_name(server_name)) {
		return false;
	}
	if (verify_peer && !conn.tls_stream->set_verify_hostname(server_name)) {
		return false;
	}

	if (!conn.tls_stream->handshake_connect(timeout_sec)) {
		return false;
	}
	conn.use_tls = true;
	return true;
}
#endif

bool send_all(
	Connection &conn,
	string_view data,
	int timeout_sec) {
#if CONFLUX_HAS_TLS
	if (conn.use_tls) {
		return conn.tls_stream->write_all(data, timeout_sec);
	}
#endif
	size_t sent = 0;
	while (sent < data.size()) {
		if (!wait_fd(conn.fd, POLLOUT, timeout_sec)) {
			return false;
		}
		auto const n = ::send(conn.fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
		if (n <= 0) {
			return false;
		}
		sent += static_cast<size_t>(n);
	}
	return true;
}

bool recv_some(
	Connection &conn,
	string &out,
	int timeout_sec) {
#if CONFLUX_HAS_TLS
	if (conn.use_tls) {
		return conn.tls_stream->read_some(out, timeout_sec);
	}
#endif
	array<char, 4096> tmp{};
	if (!wait_fd(conn.fd, POLLIN, timeout_sec)) {
		return false;
	}
	auto const n = ::recv(conn.fd, tmp.data(), tmp.size(), 0);
	if (n <= 0) {
		return false;
	}
	out.append(tmp.data(), static_cast<size_t>(n));
	return true;
}

string recv_until(
	Connection &conn,
	string_view delim,
	int timeout_sec,
	size_t max = 65536) {
	string buf;
	buf.reserve(4096);
	while (buf.size() < max) {
		if (!recv_some(conn, buf, timeout_sec)) {
			break;
		}
		if (buf.find(delim) != string::npos) {
			break;
		}
	}
	return buf;
}

bool recv_exact(
	Connection &conn,
	string &out,
	int timeout_sec,
	size_t target_size) {
	while (out.size() < target_size) {
		if (!recv_some(conn, out, timeout_sec)) {
			return false;
		}
	}
	return true;
}

void recv_to_eof(
	Connection &conn,
	string &out,
	int timeout_sec) {
	while (recv_some(conn, out, timeout_sec)) {}
}

// Resolves `host` as AF_UNSPEC, walks the address list in order (v6 first on
// most systems via /etc/gai.conf), creates a socket, connects with timeout.
// Returns the connected fd, or -1 on failure.
int resolve_and_connect(
	string_view host,
	u16 port,
	int timeout_sec) {
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo *res = nullptr;
	string const h{host};
	string const p = to_string(port);
	if (::getaddrinfo(h.c_str(), p.c_str(), &hints, &res) != 0 || res == nullptr) {
		return -1;
	}
	int fd = -1;
	for (auto *rp = res; rp != nullptr; rp = rp->ai_next) {
		fd = ::socket(rp->ai_family, rp->ai_socktype | SOCK_CLOEXEC, rp->ai_protocol);
		if (fd < 0) {
			continue;
		}
		if (connect_with_timeout(fd, rp->ai_addr, rp->ai_addrlen, timeout_sec)) {
			break;
		}
		::close(fd);
		fd = -1;
	}
	::freeaddrinfo(res);
	return fd;
}

enum class ChunkedDecodeStatus : u8 {
	complete,
	incomplete,
	invalid,
};

ChunkedDecodeStatus decode_chunked_prefix(
	string_view encoded,
	string &decoded,
	size_t &consumed) {
	decoded.clear();
	consumed = 0;

	for (;;) {
		auto const line_end = encoded.find("\r\n", consumed);
		if (line_end == string_view::npos) {
			return ChunkedDecodeStatus::incomplete;
		}

		auto size_str = trim(encoded.substr(consumed, line_end - consumed));
		if (auto const semi = size_str.find(';'); semi != string_view::npos) {
			size_str = trim(size_str.substr(0, semi));
		}
		if (size_str.empty()) {
			return ChunkedDecodeStatus::invalid;
		}

		size_t chunk_size = 0;
		auto const parsed = from_chars(size_str.data(), size_str.data() + size_str.size(), chunk_size, 16);
		if (parsed.ec != errc{} || parsed.ptr != size_str.data() + size_str.size()) {
			return ChunkedDecodeStatus::invalid;
		}

		consumed = line_end + 2;
		if (chunk_size == 0) {
			// Consume optional trailer lines until the terminating empty CRLF.
			for (;;) {
				auto const eol = encoded.find("\r\n", consumed);
				if (eol == string_view::npos) {
					return ChunkedDecodeStatus::incomplete;
				}
				bool const is_empty_line = (eol == consumed);
				consumed = eol + 2;
				if (is_empty_line) {
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

bool recv_chunked(
	Connection &conn,
	string &encoded,
	string &decoded,
	int timeout_sec) {
	for (;;) {
		size_t consumed = 0;
		switch (decode_chunked_prefix(encoded, decoded, consumed)) {
		case ChunkedDecodeStatus::complete: return true;
		case ChunkedDecodeStatus::invalid : return false;
		case ChunkedDecodeStatus::incomplete:
			if (!recv_some(conn, encoded, timeout_sec)) {
				return false;
			}
			break;
		}
	}
}

} // namespace client_detail

export [[nodiscard]] optional<string> decode_chunked_body(
	string_view encoded) {
	string decoded;
	size_t consumed = 0;
	if (client_detail::decode_chunked_prefix(encoded, decoded, consumed)
		!= client_detail::ChunkedDecodeStatus::complete) {
		return nullopt;
	}
	if (consumed != encoded.size()) {
		return nullopt;
	}
	return decoded;
}

export [[nodiscard]] expected<ClientResponse, string> http_request(
	ClientRequest const &req) {
	client_detail::Connection conn{
		.fd = client_detail::resolve_and_connect(req.host, req.port, req.timeout_sec),
	};
	if (conn.fd < 0) {
		return unexpected{"connect"};
	}
	if (req.use_tls) {
#if CONFLUX_HAS_TLS
		auto const server_name = req.server_name.empty() ? string_view{req.host} : string_view{req.server_name};
		if (!client_detail::enable_tls(conn, server_name, req.timeout_sec, req.verify_peer)) {
			client_detail::close_connection(conn);
			return unexpected{"tls connect"};
		}
#else
		client_detail::close_connection(conn);
		return unexpected{"tls unavailable"};
#endif
	}

	string const host_hdr = req.host_override.empty() ? client_detail::host_header(req) : string{req.host_override};
	string encoded_request = format("{} {} HTTP/1.1\r\nHost: {}\r\n", req.method, req.path, host_hdr);
	for (auto const &[name, header_value]: req.headers) {
		auto const lower_name = ascii_lower(name);
		if (lower_name == "host" || client_detail::is_hop_by_hop(lower_name)) {
			continue;
		}
		encoded_request += format("{}: {}\r\n", name, header_value);
	}
	encoded_request += "Connection: close\r\n";
	if (!req.body.empty()) {
		encoded_request += format("Content-Length: {}\r\n", req.body.size());
	}
	encoded_request += "\r\n";

	if (!client_detail::send_all(conn, encoded_request, req.timeout_sec)) {
		client_detail::close_connection(conn);
		return unexpected{"send headers"};
	}
	if (!req.body.empty() && !client_detail::send_all(conn, req.body, req.timeout_sec)) {
		client_detail::close_connection(conn);
		return unexpected{"send body"};
	}

	auto raw = client_detail::recv_until(conn, "\r\n\r\n", req.timeout_sec);
	auto header_end = raw.find("\r\n\r\n");
	ClientResponse response;

	if (header_end != string::npos) {
		auto headers_str = string_view{raw}.substr(0, header_end);
		auto nl = headers_str.find("\r\n");
		auto status_line = (nl != string_view::npos) ? headers_str.substr(0, nl) : headers_str;
		auto sp1 = status_line.find(' ');
		if (sp1 != string_view::npos) {
			auto rest = status_line.substr(sp1 + 1);
			auto sp2 = rest.find(' ');
			auto code_sv = (sp2 != string_view::npos) ? rest.substr(0, sp2) : rest;
			from_chars(code_sv.data(), code_sv.data() + code_sv.size(), response.status);
			if (sp2 != string_view::npos) {
				response.status_text = string{rest.substr(sp2 + 1)};
			}
		}

		size_t content_length = 0;
		bool chunked = false;
		auto pos = (nl != string_view::npos) ? nl + 2 : headers_str.size();
		while (pos < headers_str.size()) {
			auto end = headers_str.find("\r\n", pos);
			auto hdr = (end != string_view::npos) ? headers_str.substr(pos, end - pos) : headers_str.substr(pos);
			auto colon = hdr.find(':');
			if (colon != string_view::npos) {
				auto k = hdr.substr(0, colon);
				auto v = hdr.substr(colon + 1);
				while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
					v.remove_prefix(1);
				}
				auto kl = ascii_lower(k);
				auto const vl = ascii_lower(v);
				if (kl == "content-type") {
					response.content_type = string{v};
				} else if (kl == "content-length") {
					from_chars(v.data(), v.data() + v.size(), content_length);
					static constexpr size_t kMaxResponseBody = 16ULL * 1024 * 1024;
					if (content_length > kMaxResponseBody) {
						return unexpected{"response body too large"};
					}
				} else if (kl == "transfer-encoding" && vl.find("chunked") != string::npos) {
					chunked = true;
				} else if (!client_detail::is_hop_by_hop(kl)) {
					if (kl == "set-cookie") {
						response.set_cookies.push_back(string{v});
					} else {
						response.headers[string{k}] = string{v};
					}
				}
			}
			pos = (end != string_view::npos) ? end + 2 : headers_str.size();
		}

		response.body = raw.substr(header_end + 4);
		if (req.method == "HEAD") {
			response.body.clear();
		} else if (!chunked && content_length > response.body.size()) {
			if (!client_detail::recv_exact(conn, response.body, req.timeout_sec, content_length)) {
				client_detail::close_connection(conn);
				return unexpected{"recv body"};
			}
		} else if (chunked) {
			string decoded;
			if (!client_detail::recv_chunked(conn, response.body, decoded, req.timeout_sec)) {
				client_detail::close_connection(conn);
				return unexpected{"recv chunked body"};
			}
			response.body = move(decoded);
		} else if (!chunked && content_length == 0) {
			client_detail::recv_to_eof(conn, response.body, req.timeout_sec);
		}
	}

	client_detail::close_connection(conn);
	return response;
}

export template<typename Target>
[[nodiscard]] auto http_request_in(
	Target &target,
	ClientRequest req) {
	return ::run_on(target, [req = move(req)] { return http_request(req); });
}

export class HttpClient {
	ClientOptions options_{};

	[[nodiscard]] ClientRequest make_request(
		string method,
		string path,
		string body,
		HttpFields const &headers) const {
		ClientRequest req{
			.method = move(method),
			.host = options_.host,
			.port = options_.port,
			.path = move(path),
			.headers = options_.default_headers,
			.body = move(body),
			.timeout_sec = options_.timeout_sec,
			.use_tls = options_.use_tls,
			.verify_peer = options_.verify_peer,
			.server_name = options_.server_name,
		};
		for (auto const &[name, header_value]: headers) {
			req.headers.set(name, header_value);
		}
		return req;
	}

public:
	explicit HttpClient(
		ClientOptions options)
		: options_{move(options)} {}

	[[nodiscard]] ClientOptions const &options() const noexcept { return options_; }

	[[nodiscard]] expected<ClientResponse, string> request(
		string method,
		string path,
		string body = {},
		HttpFields const &headers = {}) const {
		return http_request(make_request(move(method), move(path), move(body), headers));
	}

	[[nodiscard]] expected<ClientResponse, string> get(
		string path,
		HttpFields const &headers = {}) const {
		return request("GET", move(path), {}, headers);
	}

	[[nodiscard]] expected<ClientResponse, string> post(
		string path,
		string body,
		string content_type = "application/octet-stream",
		HttpFields headers = {}) const {
		if (!content_type.empty()) {
			headers.set("Content-Type", move(content_type));
		}
		return request("POST", move(path), move(body), headers);
	}

	[[nodiscard]] expected<ClientResponse, string> put(
		string path,
		string body,
		string content_type = "application/octet-stream",
		HttpFields headers = {}) const {
		if (!content_type.empty()) {
			headers.set("Content-Type", move(content_type));
		}
		return request("PUT", move(path), move(body), headers);
	}

	[[nodiscard]] expected<ClientResponse, string> patch(
		string path,
		string body,
		string content_type = "application/octet-stream",
		HttpFields headers = {}) const {
		if (!content_type.empty()) {
			headers.set("Content-Type", move(content_type));
		}
		return request("PATCH", move(path), move(body), headers);
	}

	[[nodiscard]] expected<ClientResponse, string> del(
		string path,
		HttpFields const &headers = {}) const {
		return request("DELETE", move(path), {}, headers);
	}

	[[nodiscard]] expected<ClientResponse, string> head(
		string path,
		HttpFields const &headers = {}) const {
		return request("HEAD", move(path), {}, headers);
	}

	template<typename Target>
	[[nodiscard]] auto request_in(
		Target &target,
		string method,
		string path,
		string body = {},
		HttpFields const &headers = {}) const {
		return http_request_in(target, make_request(move(method), move(path), move(body), headers));
	}

	template<typename Target>
	[[nodiscard]] auto get_in(
		Target &target,
		string path,
		HttpFields headers = {}) const {
		return request_in(target, "GET", move(path), {}, move(headers));
	}

	template<typename Target>
	[[nodiscard]] auto post_in(
		Target &target,
		string path,
		string body,
		string content_type = "application/octet-stream",
		HttpFields headers = {}) const {
		if (!content_type.empty()) {
			headers.set("Content-Type", move(content_type));
		}
		return request_in(target, "POST", move(path), move(body), move(headers));
	}

	template<typename Target>
	[[nodiscard]] auto put_in(
		Target &target,
		string path,
		string body,
		string content_type = "application/octet-stream",
		HttpFields headers = {}) const {
		if (!content_type.empty()) {
			headers.set("Content-Type", move(content_type));
		}
		return request_in(target, "PUT", move(path), move(body), move(headers));
	}

	template<typename Target>
	[[nodiscard]] auto patch_in(
		Target &target,
		string path,
		string body,
		string content_type = "application/octet-stream",
		HttpFields headers = {}) const {
		if (!content_type.empty()) {
			headers.set("Content-Type", move(content_type));
		}
		return request_in(target, "PATCH", move(path), move(body), move(headers));
	}

	template<typename Target>
	[[nodiscard]] auto del_in(
		Target &target,
		string path,
		HttpFields headers = {}) const {
		return request_in(target, "DELETE", move(path), {}, move(headers));
	}

	template<typename Target>
	[[nodiscard]] auto head_in(
		Target &target,
		string path,
		HttpFields headers = {}) const {
		return request_in(target, "HEAD", move(path), {}, move(headers));
	}
};
