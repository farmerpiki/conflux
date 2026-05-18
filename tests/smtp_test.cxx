#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.crypto;
import conflux.net.smtp;
namespace {

struct ScriptStep {
	std::string expect_starts_with;
	std::vector<std::string> send_lines;
};
int make_listener(
	std::uint16_t &port) {
	int const s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(s >= 0);
	int yes = 1;
	::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;
	REQUIRE(::bind(s, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0);
	socklen_t len = sizeof(sa);
	REQUIRE(::getsockname(s, reinterpret_cast<sockaddr *>(&sa), &len) == 0);
	port = ntohs(sa.sin_port);
	REQUIRE(::listen(s, 1) == 0);
	return s;
}
std::string recv_line(
	int fd) {
	std::string line;
	for (;;) {
		char c = 0;
		auto const n = ::recv(fd, &c, 1, 0);
		if (n <= 0) {
			return line;
		}
		line.push_back(c);
		if (line.size() >= 2 && line[line.size() - 2] == '\r' && line.back() == '\n') {
			return line;
		}
	}
}
std::string recv_until_dot(
	int fd) {
	std::string body;
	std::string line;
	for (;;) {
		line = recv_line(fd);
		if (line.empty()) {
			return body;
		}
		body += line;
		if (line == ".\r\n") {
			return body;
		}
	}
}
void send_all(
	int fd,
	std::string_view s) {
	std::size_t off = 0;
	while (off < s.size()) {
		auto const n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
		if (n <= 0) {
			return;
		}
		off += static_cast<std::size_t>(n);
	}
}

} // namespace
TEST_CASE(
	"SMTP envelope round-trip AUTH PLAIN",
	"[smtp]") {
	std::uint16_t port = 0;
	int const listen_fd = make_listener(port);

	std::string received_auth;
	std::string received_body;
	std::string received_from;
	std::string received_rcpt;

	auto srv = thread([&] {
		int const c = ::accept(listen_fd, nullptr, nullptr);
		REQUIRE(c >= 0);
		send_all(c, "220 dummy ESMTP\r\n");

		auto ehlo = recv_line(c);
		REQUIRE(ehlo.starts_with("EHLO "));
		send_all(c, "250-dummy\r\n250-AUTH PLAIN LOGIN\r\n250 OK\r\n");

		auto auth = recv_line(c);
		REQUIRE(auth.starts_with("AUTH PLAIN "));
		received_auth = auth.substr(11, auth.size() - 13);
		send_all(c, "235 Authenticated\r\n");

		auto mf = recv_line(c);
		received_from = mf;
		send_all(c, "250 OK\r\n");

		auto rt = recv_line(c);
		received_rcpt = rt;
		send_all(c, "250 OK\r\n");

		auto data = recv_line(c);
		REQUIRE(data == "DATA\r\n");
		send_all(c, "354 Go\r\n");
		received_body = recv_until_dot(c);
		send_all(c, "250 Queued\r\n");

		auto q = recv_line(c);
		REQUIRE(q == "QUIT\r\n");
		send_all(c, "221 Bye\r\n");

		::close(c);
	});

	SmtpClient cli;
	cli.set_timeout(5);
	REQUIRE(cli.connect("127.0.0.1", port));
	auto eh = cli.ehlo("localhost");
	REQUIRE(eh.has_value());
	REQUIRE(eh->code == 250);
	REQUIRE(cli.auth_plain("alice", "s3cret"));
	REQUIRE(cli.mail_from("alice@example.com"));
	REQUIRE(cli.rcpt_to("bob@example.com"));
	REQUIRE(cli.data("Subject: hi\r\n\r\n.leading dot\r\nnormal line\r\n"));
	REQUIRE(cli.quit());

	srv.join();
	::close(listen_fd);

	auto raw = base64_decode(received_auth);
	REQUIRE(raw.size() == 1 + 5 + 1 + 6);
	REQUIRE(raw[0] == '\0');
	REQUIRE(raw.substr(1, 5) == "alice");
	REQUIRE(raw[6] == '\0');
	REQUIRE(raw.substr(7) == "s3cret");

	REQUIRE(received_from == "MAIL FROM:<alice@example.com>\r\n");
	REQUIRE(received_rcpt == "RCPT TO:<bob@example.com>\r\n");

	// Dot-stuffing: leading "." on a line must be doubled on the wire.
	REQUIRE(received_body.find("..leading dot\r\n") != std::string::npos);
	REQUIRE(received_body.ends_with(".\r\n"));
}
TEST_CASE(
	"SMTP AUTH LOGIN base64 challenge/response",
	"[smtp]") {
	std::uint16_t port = 0;
	int const listen_fd = make_listener(port);

	std::string got_user;
	std::string got_pass;

	auto srv = thread([&] {
		int const c = ::accept(listen_fd, nullptr, nullptr);
		REQUIRE(c >= 0);
		send_all(c, "220 dummy\r\n");
		auto ehlo = recv_line(c);
		REQUIRE(ehlo.starts_with("EHLO "));
		send_all(c, "250-dummy\r\n250 AUTH LOGIN\r\n");
		auto cmd = recv_line(c);
		REQUIRE(cmd == "AUTH LOGIN\r\n");
		send_all(c, "334 VXNlcm5hbWU6\r\n");
		auto u = recv_line(c);
		got_user = u.substr(0, u.size() - 2);
		send_all(c, "334 UGFzc3dvcmQ6\r\n");
		auto p = recv_line(c);
		got_pass = p.substr(0, p.size() - 2);
		send_all(c, "235 OK\r\n");
		auto q = recv_line(c);
		REQUIRE(q == "QUIT\r\n");
		send_all(c, "221 Bye\r\n");
		::close(c);
	});

	SmtpClient cli;
	cli.set_timeout(5);
	REQUIRE(cli.connect("127.0.0.1", port));
	REQUIRE(cli.ehlo("localhost").has_value());
	REQUIRE(cli.auth_login("alice", "s3cret"));
	REQUIRE(cli.quit());

	srv.join();
	::close(listen_fd);

	REQUIRE(base64_decode(got_user) == "alice");
	REQUIRE(base64_decode(got_pass) == "s3cret");
}
TEST_CASE(
	"SMTP multi-line 250 reply is parsed",
	"[smtp]") {
	std::uint16_t port = 0;
	int const listen_fd = make_listener(port);

	auto srv = thread([&] {
		int const c = ::accept(listen_fd, nullptr, nullptr);
		REQUIRE(c >= 0);
		send_all(c, "220 dummy\r\n");
		auto ehlo = recv_line(c);
		REQUIRE(ehlo.starts_with("EHLO "));
		send_all(c, "250-dummy\r\n250-SIZE 10240000\r\n250-8BITMIME\r\n250 PIPELINING\r\n");
		auto q = recv_line(c);
		REQUIRE(q == "QUIT\r\n");
		send_all(c, "221 Bye\r\n");
		::close(c);
	});

	SmtpClient cli;
	cli.set_timeout(5);
	REQUIRE(cli.connect("127.0.0.1", port));
	auto reply = cli.ehlo("localhost");
	REQUIRE(reply.has_value());
	REQUIRE(reply->code == 250);
	REQUIRE(reply->text.find("SIZE 10240000") != std::string::npos);
	REQUIRE(reply->text.find("8BITMIME") != std::string::npos);
	REQUIRE(reply->text.find("PIPELINING") != std::string::npos);
	REQUIRE(cli.quit());

	srv.join();
	::close(listen_fd);
}
TEST_CASE(
	"SMTP move constructor preserves ehlo_caps (STARTTLS check)",
	"[smtp]") {
	std::uint16_t port = 0;
	int const listen_fd = make_listener(port);

	bool starttls_received = false;

	auto srv = thread([&] {
		int const c = ::accept(listen_fd, nullptr, nullptr);
		if (c < 0) {
			return;
		}
		send_all(c, "220 dummy\r\n");
		auto ehlo = recv_line(c);
		(void)ehlo;
		send_all(c, "250-dummy\r\n250 STARTTLS\r\n");
		// After EHLO the moved client calls starttls() which sends "STARTTLS\r\n".
		// Without the fix ehlo_caps_ is empty after move and starttls() bails immediately.
		auto cmd = recv_line(c);
		starttls_received = (cmd == "STARTTLS\r\n");
		// Reply 500 so the client does not attempt a TLS handshake on this plain socket.
		send_all(c, "500 No TLS here\r\n");
		::close(c);
	});

	SmtpClient cli;
	cli.set_timeout(5);
	REQUIRE(cli.connect("127.0.0.1", port));
	auto eh = cli.ehlo("localhost");
	REQUIRE(eh.has_value());
	REQUIRE(eh->code == 250);

	SmtpClient moved = move(cli);
	// Return value is false because TLS handshake to a plain socket fails;
	// what matters is whether the STARTTLS command was sent (i.e. ehlo_caps_ survived the move).
	(void)moved.starttls("localhost", false);

	srv.join();
	::close(listen_fd);

	CHECK(starttls_received);
}
