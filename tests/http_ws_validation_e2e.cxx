#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.http.extended;
import conflux.net.config;
import conflux.net.router;
import conflux.tests.support;
import conflux.work;

using conflux::http::Config;
using conflux::tests::ScopedTestServer;
using conflux::work::WorkPool;
using conflux::work::WorkPoolOptions;

// ---------------------------------------------------------------------------
// WebSocket frame validation + fragmentation (A2)
// ---------------------------------------------------------------------------

namespace ws_test {

std::string read_http_headers(
	int fd) {
	std::string resp;
	std::array<char, 512> buf{};
	while (resp.find("\r\n\r\n") == std::string::npos) {
		auto n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		REQUIRE(n > 0);
		resp.append(buf.data(), static_cast<std::size_t>(n));
		REQUIRE(resp.size() <= 8192);
	}
	return resp;
}
Config ws_cfg() {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	return cfg;
}
int ws_handshake(
	std::uint16_t port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	std::string req = std::format(
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n");
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	auto resp = read_http_headers(fd);
	REQUIRE(resp.starts_with("HTTP/1.1 101"));
	REQUIRE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
	return fd;
}
std::vector<std::uint8_t> make_masked_frame(
	std::uint8_t b0,
	std::string_view payload,
	bool mask = true) {
	std::vector<std::uint8_t> f;
	f.push_back(b0);
	auto len = payload.size();
	std::uint8_t const mask_bit = mask ? 0x80U : 0U;
	if (len < 126) {
		f.push_back(mask_bit | static_cast<std::uint8_t>(len));
	} else if (len <= 0xFFFF) {
		f.push_back(mask_bit | 126U);
		f.push_back(static_cast<std::uint8_t>(len >> 8));
		f.push_back(static_cast<std::uint8_t>(len & 0xFFU));
	} else {
		f.push_back(mask_bit | 127U);
		for (int s = 56; s >= 0; s -= 8) {
			f.push_back(static_cast<std::uint8_t>((len >> s) & 0xFFU));
		}
	}
	if (mask) {
		std::array<std::uint8_t, 4> const key{0x01, 0x02, 0x03, 0x04};
		f.insert(f.end(), key.begin(), key.end());
		for (std::size_t i = 0; i < payload.size(); ++i) {
			f.push_back(static_cast<std::uint8_t>(payload[i]) ^ key[i & 3]);
		}
	} else {
		f.insert(f.end(), payload.begin(), payload.end());
	}
	return f;
}
struct CloseFrame {
	std::uint16_t code{};
	std::string reason;
	bool received{};
};
CloseFrame read_close(
	int fd) {
	auto read_exact = [fd](std::span<std::uint8_t> out) {
		std::size_t got = 0;
		while (got < out.size()) {
			auto n = ::recv(fd, out.data() + got, out.size() - got, 0);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				return false;
			}
			got += static_cast<std::size_t>(n);
		}
		return true;
	};

	std::array<std::uint8_t, 2> header{};
	if (!read_exact(header)) {
		return {};
	}
	std::uint8_t const b0 = header[0];
	std::uint8_t const b1 = header[1] & 0x7FU;
	if ((b0 & 0x0FU) != 0x08U) {
		return {};
	}
	if (b1 > 125) {
		return {};
	}
	std::array<std::uint8_t, 125> payload{};
	if (!read_exact(std::span{payload}.first(b1))) {
		return {};
	}
	if (b1 < 2) {
		return {.code = 0, .reason = {}, .received = true};
	}
	auto const code = static_cast<std::uint16_t>(
		(static_cast<std::uint32_t>(payload[0]) << 8U) | static_cast<std::uint32_t>(payload[1]));
	std::string reason;
	if (b1 > 2) {
		reason.assign(reinterpret_cast<char const *>(payload.data()) + 2, static_cast<std::size_t>(b1) - 2);
	}
	return {.code = code, .reason = std::move(reason), .received = true};
}

} // namespace ws_test
TEST_CASE(
	"ws: closed worker pool increments pressure metric",
	"[ws][http.lifecycle]") {
	conflux::http::Router router;
	auto pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 1});
	pool->stop();
	router.set_work_pool(pool);
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &) {});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};

	int const fd = ws_test::ws_handshake(srv.port());
	for (int i = 0; i < 50; ++i) {
		if (srv.metrics().pressure.websocket_closed_for_pressure > 0) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
	}
	::close(fd);
	srv.stop();

	auto const metrics = srv.metrics();
	CHECK(metrics.pressure.websocket_closed_for_pressure >= 1);
}

TEST_CASE(
	"ws: frame with RSV bit set triggers close 1002") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto frame = ws_test::make_masked_frame(0xC1U, "x"); // FIN | RSV1 | text
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: unmasked client frame triggers close 1002") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto frame = ws_test::make_masked_frame(0x81U, "x", /*mask=*/false);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: non-minimal extended payload length triggers close 1002") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	std::array<std::uint8_t, 9> frame{
		0x81U, // FIN | text
		0xFEU, // MASK | 126 extended length marker
		0x00U,
		0x01U, // non-minimal encoding for length 1
		0x01U,
		0x02U,
		0x03U,
		0x04U,
		static_cast<std::uint8_t>('x' ^ 0x01U)};
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: oversized control frame (ping with 126-std::byte payload) triggers close 1002") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	std::string big(126, 'p');
	auto frame = ws_test::make_masked_frame(0x89U, big); // FIN | ping
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: handshake without Upgrade header is rejected") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &) {});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	std::string const req =
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Connection: keep-alive\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	auto resp = ws_test::read_http_headers(fd);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: handshake with invalid Sec-WebSocket-Key is rejected") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &) {});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(srv.port());
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	std::string const req =
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: keep-alive, Upgrade\r\n"
		"Sec-WebSocket-Key: not-a-valid-key\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	auto resp = ws_test::read_http_headers(fd);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: one-std::byte close payload triggers close 1002") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto frame = ws_test::make_masked_frame(0x88U, "x"); // FIN | close, invalid payload length 1
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: invalid close code is rejected instead of echoed") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	std::string code_payload;
	code_payload.push_back('\x03');
	code_payload.push_back('\xED'); // 1005 (reserved/invalid close code)
	auto frame = ws_test::make_masked_frame(0x88U, code_payload);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1002);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: invalid close reason UTF-8 from peer triggers close 1007") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	std::string close_payload;
	close_payload.push_back('\x03');
	close_payload.push_back('\xE8'); // 1000
	close_payload.append("\xC0\xAF", 2);
	auto frame = ws_test::make_masked_frame(0x88U, close_payload);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1007);
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: close rejects invalid status code via public API") {
	auto result = std::make_shared<std::promise<bool>>();
	auto done = result->get_future();
	conflux::http::Router router;
	router.ws("/ws", [result](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		try {
			ws.close(1005);
			result->set_value(false);
		} catch (std::invalid_argument const &) { result->set_value(true); }
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	REQUIRE(done.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
	REQUIRE(done.get());
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: close rejects invalid UTF-8 reason via public API") {
	auto result = std::make_shared<std::promise<bool>>();
	auto done = result->get_future();
	conflux::http::Router router;
	router.ws("/ws", [result](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		try {
			ws.close(1000, std::string_view{"\xC0\xAF", 2});
			result->set_value(false);
		} catch (std::invalid_argument const &) { result->set_value(true); }
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	REQUIRE(done.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
	REQUIRE(done.get());
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: fragmented text message is reassembled before handler sees it") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (auto f = ws.recv()) {
			if (f->opcode == conflux::http::WsConn::Opcode::Text) {
				if (!ws.send_text(f->payload)) {
					break;
				}
			}
		}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	auto part1 = ws_test::make_masked_frame(0x01U, "hel"); // FIN=0 | text
	auto part2 = ws_test::make_masked_frame(0x80U, "lo"); // FIN=1 | continuation
	::send(fd, part1.data(), part1.size(), MSG_NOSIGNAL);
	::send(fd, part2.data(), part2.size(), MSG_NOSIGNAL);
	std::array<std::uint8_t, 64> rx{};
	auto n = ::recv(fd, rx.data(), rx.size(), 0);
	REQUIRE(n >= 7);
	REQUIRE(rx[0] == 0x81U); // FIN | text
	REQUIRE((rx[1] & 0x7FU) == 5U);
	std::string echo{reinterpret_cast<char const *>(rx.data()) + 2, 5};
	REQUIRE(echo == "hello");
	::close(fd);
	srv.stop();
}
TEST_CASE(
	"ws: invalid UTF-8 in text frame triggers close 1007") {
	conflux::http::Router router;
	router.ws("/ws", [](conflux::http::OwnedRequest const &, conflux::http::WsConn &ws) {
		while (ws.recv()) {}
	});
	ScopedTestServer srv{ws_test::ws_cfg(), std::move(router)};
	int const fd = ws_test::ws_handshake(srv.port());
	std::string bad{"\xC0\xAF"}; // overlong / illegal sequence
	auto frame = ws_test::make_masked_frame(0x81U, bad);
	::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
	auto close = ws_test::read_close(fd);
	REQUIRE(close.received);
	REQUIRE(close.code == 1007);
	::close(fd);
	srv.stop();
}
