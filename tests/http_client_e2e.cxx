#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.net.config;
import conflux.net.http.client;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;
namespace chttp = conflux::http;
using conflux::http::HttpClient;
using conflux::http::HttpClientOptions;
using conflux::http::HttpErrorKind;
using conflux::http::HttpTimeouts;

namespace {

std::uint16_t g_client_port = 0;
std::uint16_t g_redirect_source_port = 0;
std::uint16_t g_redirect_target_port = 0;

void ensure_client_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router router;
		router.get("/api/ping", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"status":"ok"})");
		});
		router.get("/api/echo-header", [](conflux::http::OwnedRequest const &req) {
			auto v = req.headers["x-test-header"];
			if (v.empty()) {
				return conflux::http::Response::not_found("x-test-header");
			}
			return conflux::http::Response::text(std::string{v});
		});
		router.post("/api/echo-json", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(req.body);
		});
		router.get("/api/with-header", [](conflux::http::OwnedRequest const &) {
			auto r = conflux::http::Response::text("ok");
			r.headers["X-Custom"] = "hello";
			r.headers["X-Another"] = "world";
			return r;
		});
		router.get("/api/large", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text(std::string(128, 'x'));
		});
		router.put("/api/resource/{id}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(std::format(R"({{"method":"PUT","id":"{}"}})", req.params["id"]));
		});
		router.patch("/api/resource/{id}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(std::format(R"({{"method":"PATCH","id":"{}"}})", req.params["id"]));
		});
		router.del("/api/resource/{id}", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::json(std::format(R"({{"method":"DELETE","id":"{}"}})", req.params["id"]));
		});
		g_client_port = test_servers().start(cfg, std::move(router));
	});
}

void ensure_redirect_servers() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();

		conflux::http::Router target;
		target.get("/echo-headers", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(
				std::format(
					"auth={}\ncookie={}\nproxy-authorization={}\nhost={}",
					std::string{req.headers["authorization"]},
					std::string{req.headers["cookie"]},
					std::string{req.headers["proxy-authorization"]},
					std::string{req.headers["host"]}));
		});
		g_redirect_target_port = test_servers().start(cfg, std::move(target));

		conflux::http::Router source;
		source.get("/echo-headers", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(
				std::format(
					"auth={}\ncookie={}\nproxy-authorization={}\nhost={}",
					std::string{req.headers["authorization"]},
					std::string{req.headers["cookie"]},
					std::string{req.headers["proxy-authorization"]},
					std::string{req.headers["host"]}));
		});
		auto echo_redirect = [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(
				std::format(
					"method={}\nbody={}\ncontent-type={}",
					req.method,
					req.body,
					std::string{req.headers["content-type"]}));
		};
		source.get("/echo-redirect", echo_redirect);
		source.post("/echo-redirect", echo_redirect);
		source.get("/same", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::redirect("/echo-headers");
		});
		source.get("/cross", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::redirect(
				std::format("http://127.0.0.1:{}/echo-headers", g_redirect_target_port));
		});
		source.post("/see-other", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::redirect("/echo-redirect", 303);
		});
		source.post("/temporary", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::redirect("/echo-redirect", 307);
		});
		source.get("/loop", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::redirect("/loop");
		});
		g_redirect_source_port = test_servers().start(cfg, std::move(source));
	});
}

} // namespace

TEST_CASE(
	"http client: GET /api/ping returns parsed response") {
	ensure_client_server();
	auto response = HttpClient{}.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/ping", g_client_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body == R"({"status":"ok"})");
}

TEST_CASE(
	"http client: GET /api/ping returns parsed response (blocking_send)") {
	ensure_client_server();
	HttpClient client{};
	auto response =
		client.blocking_send(chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/ping", g_client_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body == R"({"status":"ok"})");
}

TEST_CASE(
	"http client: blocking_send_streaming streams response body chunks") {
	ensure_client_server();
	HttpClient client{};
	std::string body;
	auto response = client.blocking_send_streaming(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/ping", g_client_port)),
		[&](std::string_view chunk) {
			body.append(chunk);
			return true;
		});
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]} == "application/json");
	CHECK(body == R"({"status":"ok"})");
}

TEST_CASE(
	"http client: blocking_send_streaming rejects oversized content-length before emitting body chunks") {
	ensure_client_server();
	HttpClientOptions opts{};
	opts.max_body_bytes = 16;
	HttpClient client{opts};
	bool sink_called = false;
	auto response = client.blocking_send_streaming(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/large", g_client_port)),
		[&](std::string_view) {
			sink_called = true;
			return true;
		});
	CHECK_FALSE(response.has_value());
	CHECK(response.error().kind == HttpErrorKind::body_too_large);
	CHECK_FALSE(sink_called);
}

TEST_CASE(
	"http client: convenience client sends headers and parses response headers") {
	ensure_client_server();
	HttpClient client{};
	conflux::http::HttpFields headers{true};
	headers["X-Test-Header"] = "client-header";

	auto response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/echo-header", g_client_port)).headers(headers));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body == "client-header");

	auto with_headers = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/with-header", g_client_port)));
	REQUIRE(with_headers);
	CHECK(with_headers->head.headers["x-custom"] == "hello");
	CHECK(with_headers->head.headers["x-another"] == "world");
}

TEST_CASE(
	"http client: request headers override default headers once") {
	ensure_client_server();
	HttpClientOptions opts{};
	opts.default_headers["X-Test-Header"] = "default";
	HttpClient client{opts};

	auto default_response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/echo-header", g_client_port)));
	REQUIRE(default_response);
	CHECK(default_response->head.status == 200);
	CHECK(default_response->body == "default");

	auto override_response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/echo-header", g_client_port))
			.header("x-test-header", "override"));
	REQUIRE(override_response);
	CHECK(override_response->head.status == 200);
	CHECK(override_response->body == "override");
}

TEST_CASE(
	"http client: convenience client POST sends body and content type") {
	ensure_client_server();
	HttpClient client{};

	auto response = client.blocking_send(
		chttp::ClientRequest::post(std::format("http://127.0.0.1:{}/api/echo-json", g_client_port))
			.content_type("application/json")
			.body(R"({"from":"client"})"));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body == R"({"from":"client"})");
}

TEST_CASE(
	"http client: PUT sends body and receives response") {
	ensure_client_server();
	HttpClient client{};

	auto response = client.blocking_send(
		chttp::ClientRequest::put(std::format("http://127.0.0.1:{}/api/resource/42", g_client_port))
			.content_type("application/json")
			.body(R"({"x":1})"));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("PUT") != std::string::npos);
	CHECK(response->body.find("42") != std::string::npos);
}

TEST_CASE(
	"http client: PATCH sends body and receives response") {
	ensure_client_server();
	HttpClient client{};

	auto response = client.blocking_send(
		chttp::ClientRequest::patch(std::format("http://127.0.0.1:{}/api/resource/7", g_client_port))
			.content_type("application/json")
			.body(R"({"delta":1})"));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("PATCH") != std::string::npos);
	CHECK(response->body.find("7") != std::string::npos);
}

TEST_CASE(
	"http client: DELETE returns response") {
	ensure_client_server();
	HttpClient client{};

	auto response = client.blocking_send(
		chttp::ClientRequest::del(std::format("http://127.0.0.1:{}/api/resource/99", g_client_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("DELETE") != std::string::npos);
	CHECK(response->body.find("99") != std::string::npos);
}

TEST_CASE(
	"http client: HEAD /api/ping returns 200 with no body") {
	ensure_client_server();
	HttpClient client{};

	auto response =
		client.blocking_send(chttp::ClientRequest::head(std::format("http://127.0.0.1:{}/api/ping", g_client_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(std::string{response->head.headers["content-type"]} == "application/json");
	CHECK(response->body.empty());
}

TEST_CASE(
	"http client: blocking_send works without pool") {
	ensure_client_server();
	HttpClient client{};
	auto response =
		client.blocking_send(chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/api/ping", g_client_port)));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body == R"({"status":"ok"})");
}

TEST_CASE(
	"http client: connection failure returns an error") {
	HttpTimeouts timeouts{};
	timeouts.connect = std::chrono::milliseconds{1000};
	HttpClientOptions opts{};
	opts.default_timeouts = timeouts;
	HttpClient client{opts};
	auto response = client.blocking_send(chttp::ClientRequest::get("http://127.0.0.1:9/"));
	REQUIRE_FALSE(response);
	CHECK(response.error().kind == HttpErrorKind::connect);
}

TEST_CASE(
	"http client: chunked response without trailers is decoded correctly") {
	std::uint16_t port = 0;
	int const lfd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	REQUIRE(lfd >= 0);
	int yes = 1;
	::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;
	REQUIRE(::bind(lfd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0);
	socklen_t salen = sizeof(sa);
	REQUIRE(::getsockname(lfd, reinterpret_cast<sockaddr *>(&sa), &salen) == 0);
	port = ntohs(sa.sin_port);
	REQUIRE(::listen(lfd, 1) == 0);

	auto srv = std::thread([lfd] {
		int const c = ::accept(lfd, nullptr, nullptr);
		if (c < 0) {
			return;
		}
		char buf[4096];
		while (::recv(c, buf, sizeof(buf), 0) > 0) {
			if (std::strstr(buf, "\r\n\r\n")) {
				break;
			}
		}
		std::string_view const resp =
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain\r\n"
			"Transfer-Encoding: chunked\r\n"
			"Connection: close\r\n"
			"\r\n"
			"5\r\nhello\r\n"
			"6\r\n world\r\n"
			"0\r\n"
			"\r\n";
		::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
		::close(c);
	});

	auto result = HttpClient{}.blocking_send(chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/", port)));

	srv.join();
	::close(lfd);

	REQUIRE(result.has_value());
	CHECK(result->head.status == 200);
	CHECK(result->body == "hello world");
}

TEST_CASE(
	"http client: follow_redirects follows same-origin relative redirects") {
	ensure_redirect_servers();
	HttpClient client{};
	auto response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/same", g_redirect_source_port))
			.header("Authorization", "Bearer secret")
			.header("Cookie", "session=abc")
			.header("Proxy-Authorization", "Basic proxy")
			.follow_redirects(2));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("auth=Bearer secret") != std::string::npos);
	CHECK(response->body.find("cookie=session=abc") != std::string::npos);
	CHECK(response->body.find("proxy-authorization=Basic proxy") == std::string::npos);
	CHECK(response->body.find(std::format("host=127.0.0.1:{}", g_redirect_source_port)) != std::string::npos);
}

TEST_CASE(
	"http client: follow_redirects strips sensitive headers across host changes") {
	ensure_redirect_servers();
	HttpClient client{};
	auto response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/cross", g_redirect_source_port))
			.header("Authorization", "Bearer secret")
			.header("Cookie", "session=abc")
			.header("Proxy-Authorization", "Basic proxy")
			.follow_redirects(2));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("auth=") != std::string::npos);
	CHECK(response->body.find("cookie=") != std::string::npos);
	CHECK(response->body.find("proxy-authorization=") != std::string::npos);
	CHECK(response->body.find("Bearer secret") == std::string::npos);
	CHECK(response->body.find("session=abc") == std::string::npos);
	CHECK(response->body.find("Basic proxy") == std::string::npos);
	CHECK(response->body.find(std::format("host=127.0.0.1:{}", g_redirect_target_port)) != std::string::npos);
}

TEST_CASE(
	"http client: follow_redirects preserves default sensitive headers on same origin") {
	ensure_redirect_servers();
	HttpClientOptions opts{};
	opts.default_headers["Authorization"] = "Bearer default-secret";
	opts.default_headers["Cookie"] = "session=default-cookie";
	opts.default_headers["Proxy-Authorization"] = "Basic default-proxy";
	HttpClient client{opts};
	auto response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/same", g_redirect_source_port)).follow_redirects(2));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("auth=Bearer default-secret") != std::string::npos);
	CHECK(response->body.find("cookie=session=default-cookie") != std::string::npos);
	CHECK(response->body.find("Basic default-proxy") == std::string::npos);
	CHECK(response->body.find(std::format("host=127.0.0.1:{}", g_redirect_source_port)) != std::string::npos);
}

TEST_CASE(
	"http client: follow_redirects strips default sensitive headers across host changes") {
	ensure_redirect_servers();
	HttpClientOptions opts{};
	opts.default_headers["Authorization"] = "Bearer default-secret";
	opts.default_headers["Cookie"] = "session=default-cookie";
	opts.default_headers["Proxy-Authorization"] = "Basic default-proxy";
	HttpClient client{opts};
	auto response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/cross", g_redirect_source_port))
			.follow_redirects(2));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("auth=") != std::string::npos);
	CHECK(response->body.find("cookie=") != std::string::npos);
	CHECK(response->body.find("proxy-authorization=") != std::string::npos);
	CHECK(response->body.find("Bearer default-secret") == std::string::npos);
	CHECK(response->body.find("session=default-cookie") == std::string::npos);
	CHECK(response->body.find("Basic default-proxy") == std::string::npos);
	CHECK(response->body.find(std::format("host=127.0.0.1:{}", g_redirect_target_port)) != std::string::npos);
}

TEST_CASE(
	"http client: follow_redirects converts 303 to GET without body") {
	ensure_redirect_servers();
	HttpClient client{};
	auto response = client.blocking_send(
		chttp::ClientRequest::post(std::format("http://127.0.0.1:{}/see-other", g_redirect_source_port))
			.content_type("text/plain")
			.body("payload")
			.follow_redirects(2));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("method=GET") != std::string::npos);
	CHECK(response->body.find("body=") != std::string::npos);
	CHECK(response->body.find("body=payload") == std::string::npos);
	CHECK(response->body.find("content-type=text/plain") == std::string::npos);
}

TEST_CASE(
	"http client: follow_redirects preserves method and body for 307") {
	ensure_redirect_servers();
	HttpClient client{};
	auto response = client.blocking_send(
		chttp::ClientRequest::post(std::format("http://127.0.0.1:{}/temporary", g_redirect_source_port))
			.content_type("text/plain")
			.body("payload")
			.follow_redirects(2));
	REQUIRE(response);
	CHECK(response->head.status == 200);
	CHECK(response->body.find("method=POST") != std::string::npos);
	CHECK(response->body.find("body=payload") != std::string::npos);
	CHECK(response->body.find("content-type=text/plain") != std::string::npos);
}

TEST_CASE(
	"http client: follow_redirects reports redirect limit exhaustion") {
	ensure_redirect_servers();
	HttpClient client{};
	auto response = client.blocking_send(
		chttp::ClientRequest::get(std::format("http://127.0.0.1:{}/loop", g_redirect_source_port)).follow_redirects(1));
	REQUIRE_FALSE(response);
	CHECK(response.error().kind == HttpErrorKind::redirect_limit);
}
