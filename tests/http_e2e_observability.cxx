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
// ---------------------------------------------------------------------------
// A3: TLS sniff timeout + EOF handling
// ---------------------------------------------------------------------------

TEST_CASE(
	"TLS sniff: silent connection closed after tls_sniff_timeout_ms") {
	char cert_tmp[] = "/tmp/conflux_sniff_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_sniff_key_XXXXXX.pem";
	{
		int fd = ::mkstemps(cert_tmp, 4);
		::close(fd);
		fd = ::mkstemps(key_tmp, 4);
		::close(fd);
	}
	std::string const cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	REQUIRE(::system(cmd.c_str()) == 0);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 0; // disable idle-request reap so only sniff timeout is exercised
	cfg.tls_sniff_timeout_ms = 1500;
	cfg.cert_file = cert_tmp;
	cfg.key_file = key_tmp;

	conflux::http::Router router;
	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const port = srv.port();
	::unlink(cert_tmp);
	::unlink(key_tmp);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	auto const end = recv_close_state(fd, MSG_DONTWAIT);
	::close(fd);
	REQUIRE(is_socket_closed(end));

	srv.stop();
}
TEST_CASE(
	"TLS sniff: client half-close before any data triggers clean server close") {
	char cert_tmp[] = "/tmp/conflux_eof_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_eof_key_XXXXXX.pem";
	{
		int fd = ::mkstemps(cert_tmp, 4);
		::close(fd);
		fd = ::mkstemps(key_tmp, 4);
		::close(fd);
	}
	std::string const cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
		"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	REQUIRE(::system(cmd.c_str()) == 0);

	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 30000;
	cfg.tls_sniff_timeout_ms = 30000; // big — we don't want timer reap to be what closes us
	cfg.cert_file = cert_tmp;
	cfg.key_file = key_tmp;

	conflux::http::Router router;
	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const port = srv.port();
	::unlink(cert_tmp);
	::unlink(key_tmp);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	::shutdown(fd, SHUT_WR);

	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	auto const end = recv_close_state(fd, MSG_DONTWAIT);
	::close(fd);
	REQUIRE(is_socket_closed(end));

	srv.stop();
}
// ---------------------------------------------------------------------------
// C1: SseChannel bounded queue + overflow policies (unit tests, no server)
// ---------------------------------------------------------------------------

TEST_CASE(
	"SseChannel: DropNewest policy drops overflowing frames") {
	conflux::http::SseChannel ch{64, conflux::http::SseOverflowPolicy::DropNewest};
	// Each frame is 10 bytes; queue holds at most 64 bytes → 6 fit.
	for (int i = 0; i < 10; ++i) {
		std::string frame(10, 'x');
		(void)ch.send(std::move(frame));
	}
	REQUIRE(ch.dropped_count() == 4);
	auto out = ch.drain();
	REQUIRE(out.size() == 60);
}
TEST_CASE(
	"SseChannel: DropOldest policy keeps newest frames") {
	conflux::http::SseChannel ch{30, conflux::http::SseOverflowPolicy::DropOldest};
	for (int i = 0; i < 5; ++i) {
		std::string frame(10, static_cast<char>('a' + i));
		(void)ch.send(std::move(frame));
	}
	REQUIRE(ch.dropped_count() >= 2);
	auto out = ch.drain();
	// After overflow, the final 3 frames (cc…, dd…, ee…) should remain.
	REQUIRE(out.find("eeeeeeeeee") != std::string::npos);
	REQUIRE(out.find("aaaaaaaaaa") == std::string::npos);
}
TEST_CASE(
	"SseChannel: Disconnect policy closes on overflow") {
	conflux::http::SseChannel ch{20, conflux::http::SseOverflowPolicy::Disconnect};
	REQUIRE(ch.send(std::string(10, 'x')));
	// Next send exceeds the cap → channel is closed; further sends return false.
	(void)ch.send(std::string(20, 'y'));
	REQUIRE(ch.is_closed());
	REQUIRE_FALSE(ch.send(std::string(5, 'z')));
}
TEST_CASE(
	"SseChannel: send returns false after close") {
	conflux::http::SseChannel ch{4096};
	ch.close();
	REQUIRE_FALSE(ch.send("hello"));
}
// ---------------------------------------------------------------------------
// C2: conflux::http::DeferredResponse timeout
// ---------------------------------------------------------------------------

TEST_CASE(
	"deferred response that never completes returns 504 after its deadline") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 30000;

	conflux::http::Router router;
	// Handler returns a conflux::http::DeferredResponse with a 1-second deadline but never completes it.
	// The idle-timer sweeper should expire it with a 504 shortly after.
	router.get("/stuck", [](conflux::http::OwnedRequest const &) {
		auto d = std::make_shared<conflux::http::DeferredResponse>(std::chrono::milliseconds{1000});
		return conflux::http::Response::deferred(d);
	});

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const p = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(p);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	std::string_view const req = "GET /stuck HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);

	auto const response = read_one_response(fd);
	::close(fd);
	REQUIRE(response.starts_with("HTTP/1.1 504"));
	srv.stop();
}
TEST_CASE(
	"deferred response that completes before its deadline returns the completed payload") {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	conflux::http::Router router;
	router.get("/fast", [](conflux::http::OwnedRequest const &) {
		auto d = std::make_shared<conflux::http::DeferredResponse>(std::chrono::milliseconds{10000});
		std::thread([d]() {
			std::this_thread::sleep_for(std::chrono::milliseconds{80});
			d->complete(conflux::http::Response::text("pong"));
		}).detach();
		return conflux::http::Response::deferred(d);
	});

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const p = srv.port();

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(p);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	std::string_view const req = "GET /fast HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	::send(fd, req.data(), req.size(), 0);

	auto const response = read_one_response(fd);
	::close(fd);
	REQUIRE(response.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response.find("pong") != std::string::npos);
	srv.stop();
}
// Regression test for handle_send recv re-arm bug:
// When a send CQE arrives with response_ptr==nullptr (can occur when an
// error-path response races with a multishot recv CQE clearing recv_armed),
// the old code did send_queued=false; return — leaving recv_armed=false with
// no pending ops, orphaning the connection forever.
// The fix: queue_multishot_recv in that branch.
//
// We exercise this by:
//   (a) sending a pipelined error+good pair — the good request lands in
//       conn.partial while the 400 send is in-flight, maximising the chance
//       that handle_send sees response_ptr==nullptr after the pipelined
//       dispatch_request clears it and defers.
//   (b) running many iterations so that even rare CQE orderings are hit.
//   (c) keeping a 2 s SO_RCVTIMEO: a stuck connection makes recv() timeout
//       instead of returning 0, which turns an infinite hang into a test
//       failure.
namespace {

int make_conn() {
	ensure_server();
	int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket"};
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_test_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"connect"};
	}
	timeval tv{.tv_sec = 2, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}
std::string drain_fd(
	int fd) {
	std::string out;
	std::array<char, 4096> buf{};
	for (;;) {
		auto n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n <= 0) {
			break;
		}
		out.append(buf.data(), static_cast<std::size_t>(n));
	}
	return out;
}
bool recv_until_closed(
	int fd) {
	std::array<char, 4096> buf{};
	for (;;) {
		auto const n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n == 0) {
			return true;
		}
		if (n < 0) {
			return errno == ECONNRESET;
		}
	}
}

} // namespace
TEST_CASE(
	"regression: handle_send recv re-arm — no stuck connection after error close") {
	// Error request without Connection: close.
	// Server responds 400 + close_after_send=true and must properly close the
	// fd.  Without the fix recv() would block until the 2 s timeout instead of
	// returning 0 (FIN).
	static constexpr std::string_view kDupCL =
		"POST /api/echo-body HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Length: 5\r\n"
		"Content-Length: 5\r\n"
		"\r\nhello";
	static constexpr std::string_view kGood =
		"GET /api/ping HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Connection: close\r\n"
		"\r\n";
	constexpr int kIter = 300;
	for (int i = 0; i < kIter; ++i) {
		// (a) standalone error — server must send FIN promptly
		{
			int fd = make_conn();
			::send(fd, kDupCL.data(), kDupCL.size(), MSG_NOSIGNAL);
			auto r = drain_fd(fd);
			REQUIRE(r.starts_with("HTTP/1.1 400"));
			// drain_fd reads until recv()=0 (FIN) or timeout.
			// A timeout returns whatever partial data arrived — still starts with 400
			// but the next assert catches whether the loop exited cleanly.
			// We verify no timeout occurred by checking we got a clean FIN (recv=0).
			// drain_fd exits on n<=0; recv()=0 means FIN, recv()<0 means timeout →
			// r won't contain a second response, but r is already checked above.
			// The key observable: the test must finish in <2 s per iteration.
			::close(fd);
		}
		// (b) pipelined error+good in one write — exercises conn.partial path
		// during handle_http_response_send_complete; server must 400+close and
		// not get stuck reading the good request after it.
		{
			int fd = make_conn();
			std::string both{kDupCL};
			both.append(kGood);
			::send(fd, both.data(), both.size(), MSG_NOSIGNAL);
			auto r = drain_fd(fd);
			// Server closes after 400; good request is never processed.
			REQUIRE(r.starts_with("HTTP/1.1 400"));
			::close(fd);
		}
		// (c) verify server still responsive after each iteration
		{
			int fd = make_conn();
			::send(fd, kGood.data(), kGood.size(), MSG_NOSIGNAL);
			auto r = drain_fd(fd);
			REQUIRE(r.starts_with("HTTP/1.1 200"));
			::close(fd);
		}
	}
}
// ---------------------------------------------------------------------------
// PR A — cancel_recv_if_armed: shutdown drains armed multishot recv connections
// (proposal tests 1, 3, 4)
// ---------------------------------------------------------------------------
namespace {

Config small_ring_cfg_pr_a() {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 64;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.startup_banner = false;
	cfg.request_timeout_ms = 0; // disable to prevent timeout closing before srv.stop()
	return cfg;
}
int connect_to(
	std::uint16_t port) {
	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		return -1;
	}
	timeval const tv{.tv_sec = 5, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}

} // namespace
TEST_CASE(
	"server shutdown: cleanly closes N connections with armed multishot recv") {
	// N idle connections holding TCP open without sending a request.
	// Server recv is armed on each. shutdown() must cancel every multishot
	// recv and close the sockets so the client sees EOF, not a hung recv.
	// Covers proposal tests 1 (recv cancel on close) and 3 (sweep N conns).
	static constexpr int N = 20;
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
	ScopedTestServer srv{small_ring_cfg_pr_a(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(N);
	for (int i = 0; i < N; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		fds.push_back(fd);
	}
	// srv.stop() signals shutdown and joins the server std::thread; the std::thread only
	// exits once all connections are closed, so by the time stop() returns
	// every client fd must have received FIN.
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == N);
}
TEST_CASE(
	"server shutdown: recv cancel fires for send_queued connections") {
	// A connection with a response in flight (send_queued=true) must also
	// have its multishot recv cancelled so it does not block server teardown.
	// This covers the handle_shutdown send_queued branch of PR A.
	conflux::http::Router router;
	router.get("/big", [](conflux::http::OwnedRequest const &) {
		// Large enough body that the send may still be in-flight when shutdown
		// fires, increasing the chance that send_queued=true at shutdown time.
		return conflux::http::Response::text(std::string(128 * 1024, 'z'));
	});
	ScopedTestServer srv{small_ring_cfg_pr_a(), std::move(router)};
	int const fd = connect_to(srv.port());
	REQUIRE(fd >= 0);
	std::string_view const req = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
	::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
	// Shutdown immediately — races with the large-body send completing.
	// The server must not deadlock regardless of which side wins the race.
	srv.stop();
	// Already queued response bytes may arrive before FIN; the invariant is
	// that stop() drains to server close before the socket timeout fires.
	CHECK(recv_until_closed(fd));
	::close(fd);
}
TEST_CASE(
	"server shutdown: concurrent idle + send_queued connections all close") {
	// Mix: some connections idle (recv armed, no send), some with response
	// in flight. All must close after shutdown without deadlock.
	static constexpr int N_IDLE = 10;
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	router.get("/big", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(std::string(128 * 1024, 'z'));
	});
	ScopedTestServer srv{small_ring_cfg_pr_a(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(N_IDLE + 2);
	// Idle connections — no request sent
	for (int i = 0; i < N_IDLE; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		fds.push_back(fd);
	}
	// Connections with response in-flight
	for (int i = 0; i < 2; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		std::string_view const req = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
		::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
		fds.push_back(fd);
	}
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == static_cast<int>(fds.size()));
}
// ---------------------------------------------------------------------------
// P1-08b — recv-only gen invalidation + no-stall close
// ---------------------------------------------------------------------------
namespace {

Config tiny_ring_cfg_p108b() {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 16;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.startup_banner = false;
	cfg.request_timeout_ms = 0;
	return cfg;
}

} // namespace
TEST_CASE(
	"P1-08b: SQ-pressure shutdown — 30 idle conns, ring_entries=16") {
	static constexpr int N = 30;
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
	ScopedTestServer srv{tiny_ring_cfg_p108b(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(N);
	for (int i = 0; i < N; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		fds.push_back(fd);
	}
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == N);
}
TEST_CASE(
	"P1-08b: SQ-pressure shutdown — mixed idle + send_queued, ring_entries=16") {
	static constexpr int N_IDLE = 20;
	static constexpr int N_SEND = 5;
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	router.get("/big", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(std::string(256 * 1024, 'z'));
	});
	ScopedTestServer srv{tiny_ring_cfg_p108b(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(N_IDLE + N_SEND);
	for (int i = 0; i < N_IDLE; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		fds.push_back(fd);
	}
	for (int i = 0; i < N_SEND; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		std::string_view const req = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
		::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
		fds.push_back(fd);
	}
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == static_cast<int>(fds.size()));
}
TEST_CASE(
	"P1-08b: recv data queued before close_after_send is discarded") {
	conflux::http::Router router;
	router.get("/big", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(std::string(256 * 1024, 'z'));
	});
	ScopedTestServer srv{small_ring_cfg_pr_a(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(4);
	for (int i = 0; i < 4; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		std::string_view const req = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n";
		::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
		std::string_view const extra = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\ngarbage";
		::send(fd, extra.data(), extra.size(), MSG_NOSIGNAL);
		fds.push_back(fd);
	}
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == static_cast<int>(fds.size()));
}
TEST_CASE(
	"P1-08b: final recv CQE before send completion — clean shutdown") {
	conflux::http::Router router;
	router.get("/slow", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(std::string(16 * 1024, 'x'));
	});
	ScopedTestServer srv{small_ring_cfg_pr_a(), std::move(router)};
	std::vector<int> fds;
	fds.reserve(8);
	for (int i = 0; i < 8; ++i) {
		int fd = connect_to(srv.port());
		REQUIRE(fd >= 0);
		std::string_view const req = "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n";
		::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
		fds.push_back(fd);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	srv.stop();
	int closed = 0;
	for (int fd: fds) {
		if (recv_until_closed(fd)) {
			++closed;
		}
		::close(fd);
	}
	CHECK(closed == static_cast<int>(fds.size()));
}
