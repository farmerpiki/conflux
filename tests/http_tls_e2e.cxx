#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>
#if CONFLUX_HAS_TLS
	#include <openssl/ssl.h>
#endif
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.net.config;
import conflux.net.http.client;
import conflux.net.router;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
import conflux.tests.support;

using conflux::http::Config;
using conflux::http::HttpClient;
using conflux::http::HttpClientOptions;
using namespace conflux::tests;

#if CONFLUX_HAS_TLS
static_assert(!std::is_move_constructible_v<conflux::net_tls::TlsServerContext>);
static_assert(!std::is_move_assignable_v<conflux::net_tls::TlsServerContext>);
#endif

namespace {
namespace chttp = conflux::http;

#if CONFLUX_HAS_TLS
std::uint16_t g_tls_port = 0;

void ensure_tls_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		Config cfg = mw_config();
		configure_test_tls(cfg);

		conflux::http::Router router;
		router.get("/ping", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"tls":true})");
		});
		router.get("/hello/{name}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::format("hello {}", req.params["name"]));
		});
		router.post("/echo", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(req.body);
		});
		router.put("/put/{id}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(std::format(R"({{"id":"{}"}})", req.params["id"]));
		});
		router.get("/notfound-test", [](conflux::http::OwnedRequest const &) -> conflux::http::Response {
			return conflux::http::Response::not_found("notfound-test");
		});

		g_tls_port = start_mw_server(cfg, std::move(router));
	});
}

std::string tls_raw(
	std::uint16_t port,
	std::string_view raw_request) {
	conflux::net_tls::UniqueSslCtx const ctx{SSL_CTX_new(TLS_client_method())};
	SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));

	conflux::net_tls::UniqueSsl const ssl{SSL_new(ctx.get())};
	SSL_set_fd(ssl.get(), fd);
	SSL_connect(ssl.get());

	SSL_write(ssl.get(), raw_request.data(), static_cast<int>(raw_request.size()));

	std::string response;
	std::array<char, 4096> buf{};
	for (;;) {
		int const n = SSL_read(ssl.get(), buf.data(), static_cast<int>(buf.size()));
		if (n <= 0) {
			break;
		}
		response.append(buf.data(), static_cast<std::size_t>(n));
		auto hdr_end = response.find("\r\n\r\n");
		if (hdr_end == std::string::npos) {
			continue;
		}
		auto cl_pos = response.find("Content-Length: ");
		if (cl_pos == std::string::npos || cl_pos > hdr_end) {
			break;
		}
		cl_pos += 16;
		auto cl_end = response.find("\r\n", cl_pos);
		std::size_t body_len = 0;
		std::from_chars(response.data() + cl_pos, response.data() + cl_end, body_len);
		if (response.size() >= hdr_end + 4 + body_len) {
			break;
		}
	}

	SSL_shutdown(ssl.get());
	::close(fd);
	return response;
}

std::string tls_get(
	std::string_view path,
	std::string_view extra = "") {
	ensure_tls_server();
	return tls_raw(
		g_tls_port,
		std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}\r\n", path, extra));
}

std::string tls_post(
	std::string_view path,
	std::string_view body,
	std::string_view ct = "text/plain") {
	ensure_tls_server();
	return tls_raw(
		g_tls_port,
		std::format(
			"POST {} HTTP/1.1\r\nHost: localhost\r\nContent-Type: {}\r\n"
			"Content-Length: {}\r\nConnection: close\r\n\r\n{}",
			path,
			ct,
			body.size(),
			body));
}
#endif

} // namespace

#if CONFLUX_HAS_TLS
TEST_CASE(
	"TLS: GET returns JSON response") {
	auto resp = tls_get("/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"tls":true})");
}

TEST_CASE(
	"http client: HTTPS GET /ping returns parsed response") {
	ensure_tls_server();
	HttpClientOptions tls_opts{};
	tls_opts.verify_peer = false;
	HttpClient tls_client{std::move(tls_opts)};
	auto response = tls_client.blocking_send(
		chttp::ClientRequest::get(std::format("https://127.0.0.1:{}/ping", g_tls_port))
			.server_name("localhost")
			.build());
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]}.find("application/json") != std::string::npos);
	CHECK(response->body == R"({"tls":true})");
}

TEST_CASE(
	"TLS: GET with path parameter") {
	auto resp = tls_get("/hello/world");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "hello world");
}

TEST_CASE(
	"TLS: POST body is echoed back") {
	auto resp = tls_post("/echo", "hello TLS");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == "hello TLS");
}

TEST_CASE(
	"TLS: POST with binary-safe body") {
	std::string body(256, '\x00');
	for (int i = 0; i < 256; ++i) {
		body[static_cast<std::size_t>(i)] = static_cast<char>(i);
	}
	auto resp = tls_post("/echo", body, "application/octet-stream");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == body);
}

TEST_CASE(
	"TLS: PUT with path param returns JSON") {
	ensure_tls_server();
	auto resp = tls_raw(
		g_tls_port,
		"PUT /put/42 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(resp.substr(hdr_end + 4) == R"({"id":"42"})");
}

TEST_CASE(
	"TLS: unknown route returns 404") {
	auto resp = tls_get("/does-not-exist");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}

TEST_CASE(
	"TLS: pipelined requests on one connection both succeed") {
	ensure_tls_server();
	conflux::net_tls::UniqueSslCtx const ctx{SSL_CTX_new(TLS_client_method())};
	SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_tls_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	conflux::net_tls::UniqueSsl const ssl{SSL_new(ctx.get())};
	SSL_set_fd(ssl.get(), fd);
	REQUIRE(SSL_connect(ssl.get()) == 1);

	std::string_view const r1 = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
	std::string_view const r2 = "GET /hello/pipe HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	SSL_write(ssl.get(), r1.data(), static_cast<int>(r1.size()));
	SSL_write(ssl.get(), r2.data(), static_cast<int>(r2.size()));

	auto read_one_tls = [&]() {
		std::string resp;
		std::array<char, 4096> buf{};
		while (true) {
			int const n = SSL_read(ssl.get(), buf.data(), static_cast<int>(buf.size()));
			if (n <= 0) {
				break;
			}
			resp.append(buf.data(), static_cast<std::size_t>(n));
			auto hdr_end = resp.find("\r\n\r\n");
			if (hdr_end == std::string::npos) {
				continue;
			}
			auto cl_pos = resp.find("Content-Length: ");
			if (cl_pos == std::string::npos || cl_pos > hdr_end) {
				break;
			}
			cl_pos += 16;
			auto cl_end = resp.find("\r\n", cl_pos);
			std::size_t body_len = 0;
			std::from_chars(resp.data() + cl_pos, resp.data() + cl_end, body_len);
			if (resp.size() >= hdr_end + 4 + body_len) {
				resp.resize(hdr_end + 4 + body_len);
				break;
			}
		}
		return resp;
	};

	auto resp1 = read_one_tls();
	auto resp2 = read_one_tls();

	SSL_shutdown(ssl.get());
	::close(fd);

	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	auto h1 = resp1.find("\r\n\r\n");
	REQUIRE(resp1.substr(h1 + 4) == R"({"tls":true})");

	REQUIRE(resp2.starts_with("HTTP/1.1 200 OK"));
	auto h2 = resp2.find("\r\n\r\n");
	REQUIRE(resp2.substr(h2 + 4) == "hello pipe");
}

TEST_CASE(
	"same-port: HTTP and HTTPS on same port both serve correctly") {
	ensure_tls_server();
	auto plain_resp = http_get_on(g_tls_port, "/ping");
	REQUIRE(plain_resp.starts_with("HTTP/1.1 200 OK"));
	auto hdr_end = plain_resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(plain_resp.substr(hdr_end + 4) == R"({"tls":true})");

	auto tls_resp = tls_get("/ping");
	REQUIRE(tls_resp.starts_with("HTTP/1.1 200 OK"));
	auto tls_hdr_end = tls_resp.find("\r\n\r\n");
	REQUIRE(tls_hdr_end != std::string::npos);
	REQUIRE(tls_resp.substr(tls_hdr_end + 4) == R"({"tls":true})");
}

TEST_CASE(
	"TLS: WebSocket upgrade over TLS (wss://) works end-to-end") {
	Config cfg = mw_config();
	configure_test_tls(cfg);

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

	ScopedTestServer srv{cfg, std::move(router)};
	std::uint16_t const wss_port = srv.port();

	conflux::net_tls::UniqueSslCtx const ctx{SSL_CTX_new(TLS_client_method())};
	SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);

	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(wss_port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	conflux::net_tls::UniqueSsl const ssl{SSL_new(ctx.get())};
	SSL_set_fd(ssl.get(), fd);
	REQUIRE(SSL_connect(ssl.get()) == 1);

	std::string_view const upgrade =
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n";
	REQUIRE(SSL_write(ssl.get(), upgrade.data(), static_cast<int>(upgrade.size())) > 0);

	std::string resp;
	std::array<char, 4096> buf{};
	for (;;) {
		int const n = SSL_read(ssl.get(), buf.data(), static_cast<int>(buf.size()));
		if (n <= 0) {
			break;
		}
		resp.append(buf.data(), static_cast<std::size_t>(n));
		if (resp.find("\r\n\r\n") != std::string::npos) {
			break;
		}
	}

	REQUIRE(resp.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
	REQUIRE(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);

	std::string_view const payload = "hello";
	std::array<std::uint8_t, 4> mask_key{0xAB, 0xCD, 0xEF, 0x01};
	std::array<std::uint8_t, 2 + 4 + 5> frame_buf{};
	frame_buf[0] = 0x81U;
	frame_buf[1] = 0x80U | static_cast<std::uint8_t>(payload.size());
	frame_buf[2] = mask_key[0];
	frame_buf[3] = mask_key[1];
	frame_buf[4] = mask_key[2];
	frame_buf[5] = mask_key[3];
	for (std::size_t i = 0; i < payload.size(); ++i) {
		frame_buf[6 + i] = static_cast<std::uint8_t>(payload[i]) ^ mask_key[i & 3];
	}
	REQUIRE(SSL_write(ssl.get(), frame_buf.data(), static_cast<int>(frame_buf.size())) > 0);

	std::array<char, 32> echo_buf{};
	int const n = SSL_read(ssl.get(), echo_buf.data(), static_cast<int>(echo_buf.size()));
	REQUIRE(n >= 7);
	REQUIRE((static_cast<std::uint8_t>(echo_buf[0]) & 0x8FU) == 0x81U);
	REQUIRE(static_cast<std::uint8_t>(echo_buf[1]) == 5);
	REQUIRE(std::string_view{echo_buf.data() + 2, 5} == "hello");

	std::array<std::uint8_t, 2 + 4 + 2> close_frame{};
	close_frame[0] = 0x88U;
	close_frame[1] = 0x82U;
	close_frame[2] = 0x11;
	close_frame[3] = 0x22;
	close_frame[4] = 0x33;
	close_frame[5] = 0x44;
	close_frame[6] = static_cast<std::uint8_t>(0x03U ^ 0x11U);
	close_frame[7] = static_cast<std::uint8_t>(0xE8U ^ 0x22U);
	SSL_write(ssl.get(), close_frame.data(), static_cast<int>(close_frame.size()));

	for (int i = 0; i < 10; ++i) {
		char drain[64]{};
		int const dr = SSL_read(ssl.get(), drain, sizeof(drain));
		if (dr <= 0) {
			break;
		}
	}

	::close(fd);

	srv.stop();
}
#endif
