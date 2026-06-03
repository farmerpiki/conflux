#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.router;
import conflux.tests.support;

using conflux::http::Config;
using conflux::tests::http_get_on;
using conflux::tests::http_post_on;
using conflux::tests::http_request_on;
using conflux::tests::LocalTcpClient;
using conflux::tests::mw_config;
using conflux::tests::ScopedTestServer;

namespace {

std::string extract_body(
	std::string_view response) {
	auto const hdr_end = response.find("\r\n\r\n");
	if (hdr_end == std::string_view::npos) {
		return {};
	}
	return std::string{response.substr(hdr_end + 4)};
}

std::size_t extract_content_length(
	std::string_view response) {
	auto pos = response.find("Content-Length: ");
	if (pos == std::string_view::npos) {
		return 0;
	}
	pos += 16;
	std::size_t value = 0;
	auto const end = response.find("\r\n", pos);
	auto const parse_end = end == std::string_view::npos ? response.size() : end;
	std::from_chars(response.data() + pos, response.data() + parse_end, value);
	return value;
}

conflux::http::Router request_semantics_router() {
	conflux::http::Router router;
	router.get("/", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::html("<html><body><h1>Hello from conflux!</h1></body></html>");
	});
	router.get("/api/ping", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::json(R"({"status":"ok"})");
	});
	router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(req.body);
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
	router.options("/api/resource", [](conflux::http::OwnedRequest const &) {
		auto r = conflux::http::Response::text("");
		r.status = 204;
		r.status_text = "No Content";
		r.headers["Allow"] = "GET, POST, PUT, PATCH, DELETE, OPTIONS";
		return r;
	});
	return router;
}

std::string read_head_response(
	std::uint16_t port,
	std::string_view path) {
	LocalTcpClient client{port};
	auto request = std::format("HEAD {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
	auto const sent = client.send(request);
	REQUIRE(sent >= 0);
	REQUIRE(static_cast<std::size_t>(sent) == request.size());
	return client.read_until_close();
}

} // namespace

// ---------------------------------------------------------------------------
// Middleware
// ---------------------------------------------------------------------------

TEST_CASE(
	"middleware chain: response header injection, auth guard, request enrichment") {
	Config cfg = mw_config();
	conflux::http::Router router;

	// Two logging middlewares — verify execution order (A then B, outermost first).
	router.use([](conflux::http::OwnedRequest const &req, conflux::http::Router::Handler const &next) {
		auto resp = next(req);
		resp.headers["X-MW-Order"] = std::string{resp.headers["X-MW-Order"]} + "A";
		return resp;
	});
	router.use([](conflux::http::OwnedRequest const &req, conflux::http::Router::Handler const &next) {
		auto resp = next(req);
		resp.headers["X-MW-Order"] = std::string{resp.headers["X-MW-Order"]} + "B";
		return resp;
	});

	// Auth guard: requires X-Api-Key: secret.
	router.use([](conflux::http::OwnedRequest const &req, conflux::http::Router::Handler const &next) {
		if (req.path == "/protected" && req.headers["x-api-key"] != "secret") {
			return conflux::http::Response::html("Forbidden", 403, "Forbidden");
		}
		return next(req);
	});

	// Middleware that enriches the request before passing downstream.
	router.use([](conflux::http::OwnedRequest const &req, conflux::http::Router::Handler const &next) {
		conflux::http::OwnedRequest enriched = req;
		enriched.headers["x-injected"] = "injected-value";
		return next(enriched);
	});

	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
	router.get("/protected", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text("secret");
	});
	router.get("/injected", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.headers["x-injected"]});
	});

	ScopedTestServer srv{cfg, std::move(router)};
	auto const port = srv.port();

	SECTION("middleware A runs outermost, B innermost (both stamp header)") {
		auto resp = http_get_on(port, "/ping");
		// A wraps B: B appends "B", then A appends "A" → "BA"
		REQUIRE(resp.find("X-MW-Order: BA\r\n") != std::string::npos);
	}

	SECTION("auth middleware blocks /protected without key") {
		auto resp = http_get_on(port, "/protected");
		REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
	}

	SECTION("auth middleware passes /protected with correct key") {
		auto resp = http_get_on(port, "/protected", "X-Api-Key: secret\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(extract_body(resp) == "secret");
	}

	SECTION("middleware can enrich request before passing to handler") {
		auto resp = http_get_on(port, "/injected");
		REQUIRE(extract_body(resp) == "injected-value");
	}

	srv.stop();
}

// ---------------------------------------------------------------------------
// conflux::http::OwnedRequest body size limit
// ---------------------------------------------------------------------------

TEST_CASE(
	"POST with body within default limit is accepted") {
	ScopedTestServer srv{mw_config(), request_semantics_router()};
	auto resp = http_post_on(srv.port(), "/api/echo-body", "text/plain", std::string(100, 'x'));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	srv.stop();
}

TEST_CASE(
	"POST claiming body larger than configured limit returns 413") {
	Config cfg = mw_config();
	cfg.max_body_size = 64;

	conflux::http::Router router;
	router.post("/upload", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(req.body);
	});

	ScopedTestServer srv{cfg, std::move(router)};
	auto const port = srv.port();

	auto post = [&](std::size_t body_size) {
		std::string body(body_size, 'A');
		return http_post_on(port, "/upload", "text/plain", body);
	};

	SECTION("body at limit is accepted") {
		auto resp = post(64);
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	}

	SECTION("body one std::byte over limit returns 413") {
		auto resp = post(65);
		REQUIRE(resp.starts_with("HTTP/1.1 413 Content Too Large"));
	}

	srv.stop();
}

TEST_CASE(
	"POST with near-limit request line and max body is accepted") {
	Config cfg = mw_config();
	cfg.max_body_size = 4;
	cfg.parser_limits.max_request_line_size = 80;
	cfg.parser_limits.max_header_block_size = 64;

	std::string path = "/";
	path.append(50, 'a');
	conflux::http::Router router;
	router.post(path, [](conflux::http::OwnedRequest const &req) { return conflux::http::Response::text(req.body); });
	ScopedTestServer srv{cfg, std::move(router)};

	LocalTcpClient client{srv.port()};
	std::string body = "ABCD";
	auto request = std::format(
		"POST {} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
		path,
		body.size(),
		body);
	auto const sent = client.send(request);
	REQUIRE(sent >= 0);
	REQUIRE(static_cast<std::size_t>(sent) == request.size());
	auto resp = client.read_one_response();

	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == body);
	srv.stop();
}

// ---------------------------------------------------------------------------
// HEAD method
// ---------------------------------------------------------------------------

TEST_CASE(
	"HEAD / returns same headers as GET but no body") {
	ScopedTestServer srv{mw_config(), request_semantics_router()};
	auto const port = srv.port();

	auto resp = read_head_response(port, "/");
	auto get_resp = http_get_on(port, "/");

	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Type: text/html") != std::string::npos);
	REQUIRE(extract_content_length(resp) == extract_content_length(get_resp));
	auto const hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.size() == hdr_end + 4);
	srv.stop();
}

TEST_CASE(
	"HEAD /api/ping matches GET route and returns no body") {
	ScopedTestServer srv{mw_config(), request_semantics_router()};
	auto resp = read_head_response(srv.port(), "/api/ping");

	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Type: application/json") != std::string::npos);
	auto const hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.size() == hdr_end + 4);
	srv.stop();
}

// ---------------------------------------------------------------------------
// PUT / PATCH / DELETE / OPTIONS
// ---------------------------------------------------------------------------

TEST_CASE(
	"PUT /api/resource/{id} is routed correctly") {
	ScopedTestServer srv{mw_config(), request_semantics_router()};
	auto resp = http_request_on(srv.port(), "PUT", "/api/resource/42");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == R"({"method":"PUT","id":"42"})");
	srv.stop();
}

TEST_CASE(
	"PATCH /api/resource/{id} is routed correctly") {
	ScopedTestServer srv{mw_config(), request_semantics_router()};
	auto resp = http_request_on(srv.port(), "PATCH", "/api/resource/7");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == R"({"method":"PATCH","id":"7"})");
	srv.stop();
}

TEST_CASE(
	"DELETE /api/resource/{id} is routed correctly") {
	ScopedTestServer srv{mw_config(), request_semantics_router()};
	auto resp = http_request_on(srv.port(), "DELETE", "/api/resource/99");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == R"({"method":"DELETE","id":"99"})");
	srv.stop();
}

TEST_CASE(
	"OPTIONS /api/resource returns Allow header") {
	ScopedTestServer srv{mw_config(), request_semantics_router()};
	auto resp = http_request_on(srv.port(), "OPTIONS", "/api/resource");
	REQUIRE(resp.starts_with("HTTP/1.1 204 No Content"));
	REQUIRE(resp.find("Allow: GET, POST, PUT, PATCH, DELETE, OPTIONS\r\n") != std::string::npos);
	srv.stop();
}

TEST_CASE(
	"unknown method on unregistered path returns 404") {
	ScopedTestServer srv{mw_config(), request_semantics_router()};
	auto resp = http_request_on(srv.port(), "DELETE", "/no-such-path");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
	srv.stop();
}
