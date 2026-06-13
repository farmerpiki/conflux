#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.response_cache;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

std::atomic<int> g_resp_cache_count{0};
std::uint16_t g_resp_cache_port = 0;

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

void ensure_resp_cache_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::response_cache_middleware({
				.max_entries = 16,
				.default_ttl = std::chrono::seconds{60},
			}));
		router.get("/counted", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			return conflux::http::Response::text(std::format("visit {}", n));
		});
		router.post("/counted", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			return conflux::http::Response::text(std::format("post {}", n));
		});
		router.get("/no-store", [](conflux::http::OwnedRequest const &) {
			auto r = conflux::http::Response::text("uncacheable");
			r.headers["Cache-Control"] = "no-store";
			return r;
		});
		router.get("/vary", [](conflux::http::OwnedRequest const &req) {
			int n = ++g_resp_cache_count;
			auto r = conflux::http::Response::text(std::format("{} enc={}", n, req.headers["accept-encoding"]));
			r.headers["Vary"] = "Accept-Encoding";
			return r;
		});
		router.get("/vary-star", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = conflux::http::Response::text(std::format("star {}", n));
			r.headers["Vary"] = "*";
			return r;
		});
		router.get("/private", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = conflux::http::Response::text(std::format("priv {}", n));
			r.headers["Cache-Control"] = "private";
			return r;
		});
		router.get("/max-age-zero", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = conflux::http::Response::text(std::format("zero {}", n));
			r.headers["Cache-Control"] = "max-age=0";
			return r;
		});
		router.get("/no-cache", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = conflux::http::Response::text(std::format("nocache {}", n));
			r.headers["Cache-Control"] = "no-cache";
			return r;
		});
		router.get("/cache-control-substrings", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = conflux::http::Response::text(std::format("substrings {}", n));
			r.headers["Cache-Control"] = "no-storehouse, privateer, no-cacheable, s-maxage=0";
			return r;
		});
		router.get("/semicolon-private", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = conflux::http::Response::text(std::format("semicolon-private {}", n));
			r.headers["Cache-Control"] = "max-age=60; private";
			return r;
		});
		router.get("/semicolon-no-store", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = conflux::http::Response::text(std::format("semicolon-no-store {}", n));
			r.headers["Cache-Control"] = "max-age=60; no-store";
			return r;
		});
		router.get("/semicolon-no-cache", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = conflux::http::Response::text(std::format("semicolon-no-cache {}", n));
			r.headers["Cache-Control"] = "max-age=60; no-cache";
			return r;
		});
		router.get("/set-cookie-resp", [](conflux::http::OwnedRequest const &) {
			int n = ++g_resp_cache_count;
			auto r = conflux::http::Response::text(std::format("cookie {}", n));
			r.set_cookie("sid", "abc");
			return r;
		});
		router.get("/not-found-resp", [](conflux::http::OwnedRequest const &) {
			++g_resp_cache_count;
			return conflux::http::Response::not_found("/not-found-resp");
		});
		router.get("/query", [](conflux::http::OwnedRequest const &req) {
			int n = ++g_resp_cache_count;
			return conflux::http::Response::text(std::format("{} value={}", n, req.query["value"]));
		});
		g_resp_cache_port = start_mw_server(mw_config(), std::move(router));
	});
}

} // namespace

TEST_CASE(
	"response_cache: first GET hits handler, second GET is served from cache") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto resp1 = http_get_on(g_resp_cache_port, "/counted");
	auto resp2 = http_get_on(g_resp_cache_port, "/counted");
	int const after = g_resp_cache_count.load();
	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(after == before + 1);
	REQUIRE(extract_body(resp1) == extract_body(resp2));
}

TEST_CASE(
	"response_cache: POST bypasses cache and hits handler") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto resp = http_post_on(g_resp_cache_port, "/counted", "application/x-www-form-urlencoded", "");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 1);
}

TEST_CASE(
	"response_cache: no-store response is not cached") {
	ensure_resp_cache_server();
	auto resp1 = http_get_on(g_resp_cache_port, "/no-store");
	auto resp2 = http_get_on(g_resp_cache_port, "/no-store");
	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp1) == "uncacheable");
}

TEST_CASE(
	"response_cache: Vary header partitions cache by request header value") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto gzip1 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: gzip\r\n");
	auto id1 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: identity\r\n");
	auto gzip2 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: gzip\r\n");
	auto id2 = http_get_on(g_resp_cache_port, "/vary", "Accept-Encoding: identity\r\n");
	REQUIRE(gzip1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(id1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(gzip1) == extract_body(gzip2));
	REQUIRE(extract_body(id1) == extract_body(id2));
	REQUIRE(extract_body(gzip1) != extract_body(id1));
	REQUIRE(extract_body(gzip1).find("enc=gzip") != std::string::npos);
	REQUIRE(extract_body(id1).find("enc=identity") != std::string::npos);
}

TEST_CASE(
	"response_cache: Vary: * is never cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/vary-star");
	auto r2 = http_get_on(g_resp_cache_port, "/vary-star");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}

TEST_CASE(
	"response_cache: query std::string participates in cache key") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto a1 = http_get_on(g_resp_cache_port, "/query?value=a");
	auto b1 = http_get_on(g_resp_cache_port, "/query?value=b");
	auto a2 = http_get_on(g_resp_cache_port, "/query?value=a");
	REQUIRE(extract_body(a1) == std::format("{} value=a", before + 1));
	REQUIRE(extract_body(b1) == std::format("{} value=b", before + 2));
	REQUIRE(extract_body(a2) == extract_body(a1));
}

TEST_CASE(
	"response_cache: Cache-Control: private response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/private");
	auto r2 = http_get_on(g_resp_cache_port, "/private");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}

TEST_CASE(
	"response_cache: Cache-Control: max-age=0 response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/max-age-zero");
	auto r2 = http_get_on(g_resp_cache_port, "/max-age-zero");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}

TEST_CASE(
	"response_cache: Cache-Control: no-cache response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/no-cache");
	auto r2 = http_get_on(g_resp_cache_port, "/no-cache");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}

TEST_CASE(
	"response_cache: Cache-Control directive checks do not match substrings") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/cache-control-substrings");
	auto r2 = http_get_on(g_resp_cache_port, "/cache-control-substrings");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 1);
	REQUIRE(extract_body(r1) == extract_body(r2));
}

TEST_CASE(
	"response_cache: semicolon-tailed Cache-Control directives are not cached") {
	ensure_resp_cache_server();
	auto check_uncached = [](std::string_view path) {
		int const before = g_resp_cache_count.load();
		auto r1 = http_get_on(g_resp_cache_port, path);
		auto r2 = http_get_on(g_resp_cache_port, path);
		REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(g_resp_cache_count.load() == before + 2);
		REQUIRE(extract_body(r1) != extract_body(r2));
	};

	check_uncached("/semicolon-private");
	check_uncached("/semicolon-no-store");
	check_uncached("/semicolon-no-cache");
}

TEST_CASE(
	"response_cache: Set-Cookie response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/set-cookie-resp");
	auto r2 = http_get_on(g_resp_cache_port, "/set-cookie-resp");
	REQUIRE(r1.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(r2.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
	REQUIRE(extract_body(r1) != extract_body(r2));
}

TEST_CASE(
	"response_cache: non-200 response is not cached") {
	ensure_resp_cache_server();
	int const before = g_resp_cache_count.load();
	auto r1 = http_get_on(g_resp_cache_port, "/not-found-resp");
	auto r2 = http_get_on(g_resp_cache_port, "/not-found-resp");
	REQUIRE(r1.starts_with("HTTP/1.1 404"));
	REQUIRE(r2.starts_with("HTTP/1.1 404"));
	REQUIRE(g_resp_cache_count.load() == before + 2);
}

TEST_CASE(
	"response_cache: LRU eviction when max_entries exceeded") {
	std::atomic<int> hits{0};
	conflux::http::Router router;
	router.use(conflux::http::response_cache_middleware({.max_entries = 2, .default_ttl = std::chrono::seconds{60}}));
	router.get("/a", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		return conflux::http::Response::text("a");
	});
	router.get("/b", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		return conflux::http::Response::text("b");
	});
	router.get("/c", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		return conflux::http::Response::text("c");
	});

	ScopedTestServer srv{mw_config(), std::move(router)};

	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	int after_fill = hits.load();
	REQUIRE(after_fill == 2);

	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 2);

	http_get_on(srv.port(), "/c");
	REQUIRE(hits.load() == 3);

	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 3);

	http_get_on(srv.port(), "/a");
	REQUIRE(hits.load() == 4);

	srv.stop();
}

TEST_CASE(
	"response_cache: response larger than max_bytes is not cached") {
	std::atomic<int> hits{0};
	conflux::http::Router router;
	router.use(
		conflux::http::response_cache_middleware(
			{.max_entries = 10, .max_bytes = 4, .default_ttl = std::chrono::seconds{60}}));
	router.get("/big", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		return conflux::http::Response::text("hello");
	});
	router.get("/small", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		return conflux::http::Response::text("hi");
	});

	ScopedTestServer srv{mw_config(), std::move(router)};

	http_get_on(srv.port(), "/big");
	http_get_on(srv.port(), "/big");
	REQUIRE(hits.load() == 2);

	http_get_on(srv.port(), "/small");
	http_get_on(srv.port(), "/small");
	REQUIRE(hits.load() == 3);

	srv.stop();
}

TEST_CASE(
	"response_cache: expired entry properly frees std::byte budget for new entries") {
	std::atomic<int> hits{0};
	conflux::http::Router router;
	router.use(
		conflux::http::response_cache_middleware(
			{.max_entries = 10, .max_bytes = 16, .default_ttl = std::chrono::seconds{60}}));
	router.get("/a", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		conflux::http::Response r = conflux::http::Response::text("aaaaaaaa");
		r.headers["Cache-Control"] = "max-age=1";
		return r;
	});
	router.get("/b", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		conflux::http::Response r = conflux::http::Response::text("bbbbbbbb");
		r.headers["Cache-Control"] = "max-age=1";
		return r;
	});
	router.get("/c", [&hits](conflux::http::OwnedRequest const &) {
		++hits;
		conflux::http::Response r = conflux::http::Response::text("cccccccc");
		r.headers["Cache-Control"] = "max-age=60";
		return r;
	});

	ScopedTestServer srv{mw_config(), std::move(router)};

	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 2);

	std::this_thread::sleep_for(std::chrono::milliseconds{1200});

	http_get_on(srv.port(), "/a");
	http_get_on(srv.port(), "/b");
	REQUIRE(hits.load() == 4);

	http_get_on(srv.port(), "/c");
	http_get_on(srv.port(), "/c");
	REQUIRE(hits.load() == 5);

	srv.stop();
}
