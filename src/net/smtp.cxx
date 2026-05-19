module;
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

export module conflux.net.smtp;
import std;
import conflux.types;
import conflux.crypto;
import conflux.net.tls;
import conflux.utils;
import conflux.net.dns;
import conflux.work;
export struct SmtpError : std::runtime_error {
	explicit SmtpError(
		std::string const &msg)
		: std::runtime_error{msg} {}
};
export struct SmtpReply {
	int code{0};
	std::string text{};
};
export struct SmtpEnvelope {
	std::string from{};
	std::vector<std::string> to{};
	std::string data{};
};
namespace smtp_detail {

inline int try_connect_addr(
	::sockaddr const *sa,
	::socklen_t sa_len,
	int family,
	int timeout_sec) {
	int const fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
	if (fd < 0) {
		return -1;
	}
	int const flags = ::fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
	int const rc = ::connect(fd, sa, sa_len);
	bool ok = rc == 0;
	if (!ok && errno == EINPROGRESS) {
		if (wait_fd(fd, POLLOUT, timeout_sec)) {
			int so_error = 0;
			socklen_t so_len = sizeof(so_error);
			ok = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0 && so_error == 0;
		}
	}
	if (flags >= 0) {
		::fcntl(fd, F_SETFL, flags);
	}
	if (!ok) {
		::close(fd);
		return -1;
	}
	return fd;
}
inline int open_connected_socket(
	std::string_view host,
	std::uint16_t port,
	int timeout_sec,
	conflux::net::dns::Resolver *resolver = nullptr) {
	namespace dns = conflux::net::dns;

	dns::ResolveOptions resolve_opts{};
	if (timeout_sec > 0) {
		resolve_opts.query_timeout = std::chrono::seconds{timeout_sec};
		resolve_opts.total_timeout = std::chrono::seconds{timeout_sec};
	}

	auto *effective_resolver = resolver != nullptr ? resolver : dns::current_resolver();
	std::optional<WorkPool> fallback_pool{};
	std::optional<dns::Resolver> fallback_resolver{};
	if (effective_resolver == nullptr) {
		fallback_pool.emplace(WorkPoolOptions{.threads = 1});
		fallback_resolver.emplace(*fallback_pool);
		effective_resolver = &*fallback_resolver;
	}

	auto result = effective_resolver->resolve_blocking(host, port, resolve_opts);
	if (!result.has_value()) {
		return -1;
	}
	for (auto const &ep: result->endpoints) {
		int const family = ep.family == dns::AddressFamily::v4 ? AF_INET : AF_INET6;
		int const fd =
			try_connect_addr(reinterpret_cast<::sockaddr const *>(&ep.addr), ep.addr_len, family, timeout_sec);
		if (fd >= 0) {
			return fd;
		}
	}
	return -1;
}
inline bool raw_send_all(
	int fd,
	std::string_view data,
	int timeout_sec) {
	std::size_t sent = 0;
	while (sent < data.size()) {
		if (!wait_fd(fd, POLLOUT, timeout_sec)) {
			return false;
		}
		auto const n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
		if (n <= 0) {
			return false;
		}
		sent += static_cast<std::size_t>(n);
	}
	return true;
}
inline bool raw_read_some(
	int fd,
	std::string &out,
	int timeout_sec) {
	std::array<char, 4096> tmp{};
	if (!wait_fd(fd, POLLIN, timeout_sec)) {
		return false;
	}
	auto const n = ::recv(fd, tmp.data(), tmp.size(), 0);
	if (n <= 0) {
		return false;
	}
	out.append(tmp.data(), static_cast<std::size_t>(n));
	return true;
}
// Dot-stuff a DATA payload and terminate with CRLF.CRLF per RFC 5321 §4.5.2.
inline std::string dot_stuff(
	std::string_view body) {
	std::string out;
	out.reserve(body.size() + 8);
	std::size_t pos = 0;
	bool at_line_start = true;
	char prev = 0;
	while (pos < body.size()) {
		char const c = body[pos];
		if (at_line_start && c == '.') {
			out.push_back('.');
		}
		out.push_back(c);
		at_line_start = (prev == '\r' && c == '\n');
		prev = c;
		++pos;
	}
	if (out.size() < 2 || out[out.size() - 2] != '\r' || out[out.size() - 1] != '\n') {
		out += "\r\n";
	}
	out += ".\r\n";
	return out;
}

} // namespace smtp_detail
export class SmtpClient {
	int fd_{-1};
	int timeout_sec_{30};
	bool tls_active_{false};
	std::optional<TlsContext> tls_ctx_{};
	std::optional<TlsStream> tls_stream_{};
	std::string rx_buf_{};
	std::string ehlo_caps_{};
	conflux::net::dns::Resolver *resolver_{nullptr};

public:
	SmtpClient() = default;

	SmtpClient(SmtpClient const &) = delete;
	SmtpClient &operator =(SmtpClient const &) = delete;
	SmtpClient(
		SmtpClient &&other) noexcept
		: fd_{other.fd_}
		, timeout_sec_{other.timeout_sec_}
		, tls_active_{other.tls_active_}
		, tls_ctx_{std::move(other.tls_ctx_)}
		, tls_stream_{std::move(other.tls_stream_)}
		, rx_buf_{std::move(other.rx_buf_)}
		, ehlo_caps_{std::move(other.ehlo_caps_)}
		, resolver_{other.resolver_} {
		other.fd_ = -1;
		other.tls_active_ = false;
		other.resolver_ = nullptr;
	}
	SmtpClient &operator =(
		SmtpClient &&other) noexcept {
		if (this != &other) {
			close_all();
			fd_ = other.fd_;
			timeout_sec_ = other.timeout_sec_;
			tls_active_ = other.tls_active_;
			tls_ctx_ = std::move(other.tls_ctx_);
			tls_stream_ = std::move(other.tls_stream_);
			rx_buf_ = std::move(other.rx_buf_);
			ehlo_caps_ = std::move(other.ehlo_caps_);
			resolver_ = other.resolver_;
			other.fd_ = -1;
			other.tls_active_ = false;
			other.resolver_ = nullptr;
		}
		return *this;
	}
	~SmtpClient() { close_all(); }
	void set_timeout(
		int seconds) noexcept {
		timeout_sec_ = seconds;
	}
	void set_resolver(
		conflux::net::dns::Resolver *r) noexcept {
		resolver_ = r;
	}
	// Plain TCP connect. Reads the server greeting (220).
	bool connect(
		std::string_view host,
		std::uint16_t port) {
		close_all();
		fd_ = smtp_detail::open_connected_socket(host, port, timeout_sec_, resolver_);
		if (fd_ < 0) {
			return false;
		}
		auto reply = read_reply();
		return reply.has_value() && reply->code == 220;
	}
	// Implicit TLS connect (port 465 style). Reads greeting over TLS.
	bool connect_tls(
		std::string_view host,
		std::uint16_t port,
		bool verify_peer = true) {
		close_all();
		fd_ = smtp_detail::open_connected_socket(host, port, timeout_sec_, resolver_);
		if (fd_ < 0) {
			return false;
		}
		if (!start_tls_handshake(host, verify_peer)) {
			return false;
		}
		auto reply = read_reply();
		return reply.has_value() && reply->code == 220;
	}
	// EHLO / HELO. Returns parsed multi-line reply.
	std::optional<SmtpReply> ehlo(
		std::string_view domain) {
		if (!write_line(std::format("EHLO {}\r\n", domain))) {
			return std::nullopt;
		}
		auto reply = read_reply();
		if (reply) {
			ehlo_caps_ = reply->text;
		}
		return reply;
	}
	std::optional<SmtpReply> helo(
		std::string_view domain) {
		if (!write_line(std::format("HELO {}\r\n", domain))) {
			return std::nullopt;
		}
		return read_reply();
	}
	// STARTTLS upgrade (RFC 3207). After 220, drive the TLS handshake and
	// discard any buffered plaintext state per spec.
	bool starttls(
		std::string_view host,
		bool verify_peer = true) {
		if (tls_active_) {
			return false;
		}
		if (ehlo_caps_.find("STARTTLS") == std::string::npos) {
			return false;
		}
		if (!write_line("STARTTLS\r\n")) {
			return false;
		}
		auto reply = read_reply();
		if (!reply.has_value() || reply->code != 220) {
			return false;
		}
		rx_buf_.clear();
		return start_tls_handshake(host, verify_peer);
	}
	// AUTH PLAIN with base64(\0user\0pass).
	bool auth_plain(
		std::string_view user,
		std::string_view pass) {
		std::string raw;
		raw.push_back('\0');
		raw.append(user);
		raw.push_back('\0');
		raw.append(pass);
		auto encoded = base64_encode(to_unsigned_span(raw));
		if (!write_line(std::format("AUTH PLAIN {}\r\n", encoded))) {
			return false;
		}
		auto reply = read_reply();
		return reply.has_value() && reply->code == 235;
	}
	// AUTH LOGIN challenge/response.
	bool auth_login(
		std::string_view user,
		std::string_view pass) {
		if (!write_line("AUTH LOGIN\r\n")) {
			return false;
		}
		auto r1 = read_reply();
		if (!r1.has_value() || r1->code != 334) {
			return false;
		}
		auto u = base64_encode(to_unsigned_span(user));
		if (!write_line(u + "\r\n")) {
			return false;
		}
		auto r2 = read_reply();
		if (!r2.has_value() || r2->code != 334) {
			return false;
		}
		auto p = base64_encode(to_unsigned_span(pass));
		if (!write_line(p + "\r\n")) {
			return false;
		}
		auto r3 = read_reply();
		return r3.has_value() && r3->code == 235;
	}
	bool mail_from(
		std::string_view addr) {
		if (!write_line(std::format("MAIL FROM:<{}>\r\n", addr))) {
			return false;
		}
		auto reply = read_reply();
		return reply.has_value() && reply->code == 250;
	}
	bool rcpt_to(
		std::string_view addr) {
		if (!write_line(std::format("RCPT TO:<{}>\r\n", addr))) {
			return false;
		}
		auto reply = read_reply();
		return reply.has_value() && (reply->code == 250 || reply->code == 251);
	}
	// DATA. Sends body with dot-stuffing and CRLF.CRLF terminator.
	bool data(
		std::string_view body) {
		if (!write_line("DATA\r\n")) {
			return false;
		}
		auto r1 = read_reply();
		if (!r1.has_value() || r1->code != 354) {
			return false;
		}
		std::string const stuffed = smtp_detail::dot_stuff(body);
		if (!write_line(stuffed)) {
			return false;
		}
		auto r2 = read_reply();
		return r2.has_value() && r2->code == 250;
	}
	// Composite send: MAIL FROM / RCPT TO* / DATA.
	bool send(
		SmtpEnvelope const &env) {
		if (!mail_from(env.from)) {
			return false;
		}
		for (auto const &rcpt: env.to) {
			if (!rcpt_to(rcpt)) {
				return false;
			}
		}
		return data(env.data);
	}
	bool quit() {
		if (!write_line("QUIT\r\n")) {
			return false;
		}
		auto reply = read_reply();
		close_all();
		return reply.has_value() && reply->code == 221;
	}
	[[nodiscard]] bool tls_active() const noexcept { return tls_active_; }
	[[nodiscard]] int fd() const noexcept { return fd_; }

private:
	bool start_tls_handshake(
		std::string_view host,
		bool verify_peer) {
		try {
			tls_ctx_.emplace();
		} catch (TlsError const &) { return false; }
		if (verify_peer) {
			tls_ctx_->set_verify_peer(true);
			if (!tls_ctx_->set_default_verify_paths()) {
				return false;
			}
		}
		try {
			tls_stream_.emplace(*tls_ctx_, fd_);
		} catch (TlsError const &) { return false; }
		if (!tls_stream_->set_server_name(host)) {
			return false;
		}
		if (verify_peer && !tls_stream_->set_verify_hostname(host)) {
			return false;
		}
		if (!tls_stream_->handshake_connect(timeout_sec_)) {
			return false;
		}
		tls_active_ = true;
		return true;
	}
	bool write_line(
		std::string_view line) {
		if (tls_active_) {
			return tls_stream_->write_all(line, timeout_sec_);
		}
		return smtp_detail::raw_send_all(fd_, line, timeout_sec_);
	}
	bool read_some(
		std::string &out) {
		if (tls_active_) {
			return tls_stream_->read_some(out, timeout_sec_);
		}
		return smtp_detail::raw_read_some(fd_, out, timeout_sec_);
	}
	// Parse a multi-line reply: lines "NNN-text" continue, final "NNN text".
	std::optional<SmtpReply> read_reply() {
		SmtpReply reply;
		for (;;) {
			std::size_t const nl = rx_buf_.find("\r\n");
			if (nl == std::string::npos) {
				if (!read_some(rx_buf_)) {
					return std::nullopt;
				}
				continue;
			}
			if (nl < 4) {
				return std::nullopt;
			}
			std::string_view const line{rx_buf_.data(), nl};
			int code = 0;
			auto const res = std::from_chars(line.data(), line.data() + 3, code);
			if (res.ec != std::errc{}) {
				return std::nullopt;
			}
			if (reply.code == 0) {
				reply.code = code;
			} else if (reply.code != code) {
				return std::nullopt;
			}
			char const sep = line[3];
			if (!reply.text.empty()) {
				reply.text.push_back('\n');
			}
			reply.text.append(line.substr(4));
			rx_buf_.erase(0, nl + 2);
			if (sep == ' ') {
				return reply;
			}
			if (sep != '-') {
				return std::nullopt;
			}
		}
	}
	void close_all() noexcept {
		if (tls_stream_.has_value()) {
			tls_stream_->shutdown_safe();
			tls_stream_.reset();
		}
		tls_ctx_.reset();
		tls_active_ = false;
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
		rx_buf_.clear();
	}
};
