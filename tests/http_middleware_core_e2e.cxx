#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.router;
import conflux.tests.support;

using conflux::http::Config;
using conflux::tests::http_get_on;
using conflux::tests::LocalTcpClient;
using conflux::tests::mw_config;
using conflux::tests::ScopedTestServer;

namespace {

std::string body_of(
	std::string_view response) {
	auto const hdr_end = response.find("\r\n\r\n");
	if (hdr_end == std::string_view::npos) {
		return {};
	}
	return std::string{response.substr(hdr_end + 4)};
}

conflux::http::Router echo_body_router() {
	conflux::http::Router router;
	router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(req.body);
	});
	return router;
}

conflux::http::Router cookie_and_group_router() {
	conflux::http::Router router;
	router.group("/api/v2", [](conflux::http::Router::Group &g) {
		g.use([](conflux::http::OwnedRequest const &req, conflux::http::Router::Handler const &next) {
			auto resp = next(req);
			resp.headers["X-Api-Version"] = "2";
			return resp;
		});
		g.get("/status", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"v":"2","status":"ok"})");
		});
		g.get("/item/{id}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(std::format(R"({{"id":"{}"}})", req.params["id"]));
		});
	});
	router.get("/api/v1/status", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::json(R"({"v":"1","status":"ok"})");
	});
	router.get("/api/echo-cookie", [](conflux::http::OwnedRequest const &req) {
		auto value = req.cookies["name"];
		if (value.empty()) {
			return conflux::http::Response::not_found("name");
		}
		return conflux::http::Response::text(std::string{value});
	});
	router.get("/api/set-cookie", [](conflux::http::OwnedRequest const &) {
		auto response = conflux::http::Response::text("ok");
		response.set_cookie("session", "abc123", "Path=/; HttpOnly");
		response.set_cookie("theme", "dark");
		return response;
	});
	return router;
}

std::string chunked_post(
	std::uint16_t port,
	std::string_view raw_request) {
	LocalTcpClient client{port};
	auto const sent = client.send(raw_request);
	REQUIRE(sent >= 0);
	REQUIRE(static_cast<std::size_t>(sent) == raw_request.size());
	return client.read_one_response();
}

} // namespace

TEST_CASE(
	"custom on_not_found and on_error handlers") {
	Config cfg = mw_config();
	conflux::http::Router router;

	router.on_not_found([](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::json(std::format(R"({{"error":"not_found","path":"{}"}})", req.path));
	});

	router.on_error([](conflux::http::OwnedRequest const &, std::exception const &ex) {
		return conflux::http::Response::json(
			std::format(R"({{"error":"internal","detail":"{}"}})", ex.what()),
			500,
			"Internal Server Error");
	});

	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("all good"); });
	router.get("/boom", [](conflux::http::OwnedRequest const &) -> conflux::http::Response {
		throw std::runtime_error{"something exploded"};
	});

	ScopedTestServer srv{cfg, std::move(router)};
	auto const port = srv.port();

	SECTION("existing route still responds normally") {
		auto resp = http_get_on(port, "/ok");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(body_of(resp) == "all good");
	}

	SECTION("custom not_found handler returns JSON 404") {
		auto resp = http_get_on(port, "/missing");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(resp.find("application/json") != std::string::npos);
		REQUIRE(body_of(resp) == R"({"error":"not_found","path":"/missing"})");
	}

	SECTION("throwing handler returns custom error response with std::exception message") {
		auto resp = http_get_on(port, "/boom");
		REQUIRE(resp.starts_with("HTTP/1.1 500 Internal Server Error"));
		REQUIRE(resp.find("application/json") != std::string::npos);
		REQUIRE(body_of(resp) == R"({"error":"internal","detail":"something exploded"})");
	}

	srv.stop();
}

TEST_CASE(
	"throwing middleware returns per-request 500") {
	Config cfg = mw_config();
	conflux::http::Router router;
	router.use(
		[](conflux::http::OwnedRequest const &, conflux::http::Router::Handler const &) -> conflux::http::Response {
			throw std::runtime_error{"middleware crash"};
		});
	router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};

	auto resp = http_get_on(srv.port(), "/ok");
	REQUIRE(resp.starts_with("HTTP/1.1 500 Internal Server Error"));
	REQUIRE(resp.find("middleware crash") != std::string::npos);
	srv.stop();
}

TEST_CASE(
	"POST with Transfer-Encoding: chunked body is decoded and echoed") {
	ScopedTestServer srv{mw_config(), echo_body_router()};
	std::string_view const raw =
		"POST /api/echo-body HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Type: text/plain\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: close\r\n"
		"\r\n"
		"7\r\n"
		"Hello, \r\n"
		"6\r\n"
		"world!\r\n"
		"0\r\n"
		"\r\n";
	auto resp = chunked_post(srv.port(), raw);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(body_of(resp) == "Hello, world!");
	srv.stop();
}

TEST_CASE(
	"POST with chunked body and chunk extension is decoded correctly") {
	ScopedTestServer srv{mw_config(), echo_body_router()};
	std::string_view const raw =
		"POST /api/echo-body HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Type: text/plain\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: close\r\n"
		"\r\n"
		"5;ext=ignored\r\n"
		"abcde\r\n"
		"0\r\n"
		"\r\n";
	auto resp = chunked_post(srv.port(), raw);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(body_of(resp) == "abcde");
	srv.stop();
}

TEST_CASE(
	"POST with chopped chunked body is decoded incrementally") {
	ScopedTestServer srv{mw_config(), echo_body_router()};
	LocalTcpClient client{srv.port()};
	client.set_recv_timeout(std::chrono::seconds{5});

	std::string_view const headers =
		"POST /api/echo-body HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Type: text/plain\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: close\r\n"
		"\r\n";
	auto sent = client.send(headers);
	REQUIRE(sent >= 0);
	sent = client.send("7\r\nHe");
	REQUIRE(sent >= 0);
	sent = client.send("llo, \r\n6\r\nwo");
	REQUIRE(sent >= 0);
	sent = client.send("rld!\r\n0\r\n");
	REQUIRE(sent >= 0);
	sent = client.send("X-Trailer: ok\r\n\r\n");
	REQUIRE(sent >= 0);

	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(body_of(resp) == "Hello, world!");
	srv.stop();
}

TEST_CASE(
	"POST with many small chunked frames stays under decoded body limit") {
	Config cfg = mw_config();
	cfg.max_body_size = 16U * 1024U;
	cfg.parser_limits.max_chunks = 20000;
	conflux::http::Router router;
	router.post("/upload", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(req.body);
	});
	ScopedTestServer srv{cfg, std::move(router)};

	std::string chunks;
	static constexpr int kChunkCount = 15000;
	chunks.reserve(static_cast<std::size_t>(kChunkCount) * 6 + 5);
	for (int i = 0; i < kChunkCount; ++i) {
		chunks += "1\r\nx\r\n";
	}
	chunks += "0\r\n\r\n";

	LocalTcpClient client{srv.port()};
	client.set_recv_timeout(std::chrono::seconds{5});
	std::string_view const headers =
		"POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\nTransfer-Encoding: "
		"chunked\r\nConnection: close\r\n\r\n";
	auto sent = client.send(headers);
	REQUIRE(sent == static_cast<ssize_t>(headers.size()));
	sent = client.send(chunks);
	REQUIRE(sent == static_cast<ssize_t>(chunks.size()));
	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(body_of(resp) == std::string(static_cast<std::size_t>(kChunkCount), 'x'));
	srv.stop();
}

TEST_CASE(
	"POST with many small chunked frames streams encoded upload framing") {
	Config cfg = mw_config();
	cfg.max_body_size = 8U * 1024U;
	cfg.parser_limits.max_chunks = 6000;
	conflux::http::Router router;
	router.post("/upload", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::format("{}", req.body.size()));
	});
	ScopedTestServer srv{cfg, std::move(router)};

	LocalTcpClient client{srv.port()};
	client.set_recv_timeout(std::chrono::seconds{5});
	std::string_view const headers =
		"POST /upload HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\nTransfer-Encoding: "
		"chunked\r\nConnection: close\r\n\r\n";
	auto sent = client.send(headers);
	REQUIRE(sent == static_cast<ssize_t>(headers.size()));

	static constexpr int kChunkCount = 256;
	for (int i = 0; i < kChunkCount; ++i) {
		auto chunk = std::format("1;upload-frame={}\r\nx\r\n", i);
		sent = client.send(chunk);
		REQUIRE(sent == static_cast<ssize_t>(chunk.size()));
	}
	sent = client.send("0\r\n\r\n");
	REQUIRE(sent == 5);

	auto resp = client.read_one_response();
	srv.stop();
	INFO(resp);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(body_of(resp) == std::format("{}", kChunkCount));
}

TEST_CASE(
	"Cookie header is parsed and individual cookies are accessible") {
	ScopedTestServer srv{mw_config(), cookie_and_group_router()};
	auto resp = http_get_on(srv.port(), "/api/echo-cookie", "Cookie: name=hello; other=world\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(body_of(resp) == "hello");
	srv.stop();
}

TEST_CASE(
	"Cookie header with single cookie is parsed correctly") {
	ScopedTestServer srv{mw_config(), cookie_and_group_router()};
	auto resp = http_get_on(srv.port(), "/api/echo-cookie", "Cookie: name=just-one\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(body_of(resp) == "just-one");
	srv.stop();
}

TEST_CASE(
	"conflux::http::OwnedRequest without Cookie header finds no cookies") {
	ScopedTestServer srv{mw_config(), cookie_and_group_router()};
	auto resp = http_get_on(srv.port(), "/api/echo-cookie");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
	srv.stop();
}

TEST_CASE(
	"conflux::http::Response set_cookie emits Set-Cookie headers") {
	ScopedTestServer srv{mw_config(), cookie_and_group_router()};
	auto resp = http_get_on(srv.port(), "/api/set-cookie");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Set-Cookie: session=abc123; Path=/; HttpOnly\r\n") != std::string::npos);
	REQUIRE(resp.find("Set-Cookie: theme=dark\r\n") != std::string::npos);
	srv.stop();
}

TEST_CASE(
	"group route responds at prefixed path") {
	ScopedTestServer srv{mw_config(), cookie_and_group_router()};
	auto resp = http_get_on(srv.port(), "/api/v2/status");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(body_of(resp) == R"({"v":"2","status":"ok"})");
	srv.stop();
}

TEST_CASE(
	"group middleware stamps header on grouped routes only") {
	ScopedTestServer srv{mw_config(), cookie_and_group_router()};
	auto v2 = http_get_on(srv.port(), "/api/v2/status");
	REQUIRE(v2.find("X-Api-Version: 2\r\n") != std::string::npos);

	auto v1 = http_get_on(srv.port(), "/api/v1/status");
	REQUIRE(v1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(v1.find("X-Api-Version:") == std::string::npos);
	srv.stop();
}

TEST_CASE(
	"group route with path parameter resolves param correctly") {
	ScopedTestServer srv{mw_config(), cookie_and_group_router()};
	auto resp = http_get_on(srv.port(), "/api/v2/item/42");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(body_of(resp) == R"({"id":"42"})");
	srv.stop();
}

TEST_CASE(
	"group route not found returns 404") {
	ScopedTestServer srv{mw_config(), cookie_and_group_router()};
	auto resp = http_get_on(srv.port(), "/api/v2/nonexistent");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
	srv.stop();
}
