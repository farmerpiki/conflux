// External TLS validation tests.
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import conflux.tests.external_support;
TEST_CASE(
	"ext/curl: HTTPS GET /ping returns 200 with JSON body") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [code, body] = fx.curl_https("/ping");
	REQUIRE(code == 0);
	REQUIRE(body == R"({"ok":true})");
}
TEST_CASE(
	"ext/curl: HTTPS GET with path param echoes name") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [code, body] = fx.curl_https("/hello/conflux");
	REQUIRE(code == 0);
	REQUIRE(body == "hello conflux");
}
TEST_CASE(
	"ext/curl: HTTPS POST body is echoed") {
	conflux::tests::HttpsServerFixture fx{conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd_retry(format(
		"curl -sk --http1.1 --max-time 5 -X POST -d 'hello world' "
		"https://127.0.0.1:{}/echo",
		fx.port()));
	REQUIRE(code == 0);
	REQUIRE(body == "hello world");
}
TEST_CASE(
	"ext/curl: HTTPS unknown route returns 404") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [code, status] = fx.curl_https_status("/does-not-exist");
	REQUIRE(code == 0);
	REQUIRE(status == "404");
}
TEST_CASE(
	"ext/curl: HTTPS and HTTP on same port both work") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [tls_code, tls_body] = fx.curl_https("/ping");
	REQUIRE(tls_code == 0);
	REQUIRE(tls_body == R"({"ok":true})");
	auto [plain_code, plain_body] = fx.curl_http("/ping");
	REQUIRE(plain_code == 0);
	REQUIRE(plain_body == R"({"ok":true})");
}
TEST_CASE(
	"ext/curl: HTTPS static file mmap fallback sends full body") {
	char dir_template[] = "/tmp/conflux_tls_static_XXXXXX";
	char *const dir_ptr = ::mkdtemp(dir_template);
	REQUIRE(dir_ptr != nullptr);
	S const dir{dir_ptr};
	S const body(256UL * 1024, 'M');
	{
		std::ofstream out{dir + "/large.bin", std::ios::binary};
		out << body;
	}

	Config cfg = Config::test();
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	Router router;
	router.serve_static("/static", dir);

	conflux::tests::HttpsServerFixture const fx{cfg, move(router)};
	auto [code, got] = conflux::tests::run_cmd_retry(
		format("curl -sk --http1.1 --max-time 5 https://127.0.0.1:{}/static/large.bin", fx.port()));
	fs::remove_all(dir);

	REQUIRE(code == 0);
	REQUIRE(got == body);
}
TEST_CASE(
	"ext/curl: TLS 1.2 is accepted") {
	conflux::tests::HttpsServerFixture fx{conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd(format(
		"curl -skS --http1.1 --tlsv1.2 --tls-max 1.2 "
		"--connect-timeout 1 --max-time 5 "
		"https://127.0.0.1:{}/ping 2>&1",
		fx.port()));
	INFO("curl exit=" << code << " output=" << body);
	REQUIRE(code == 0);
	REQUIRE(body == R"({"ok":true})");
}

TEST_CASE(
	"ext/curl: TLS 1.3 is accepted") {
	conflux::tests::HttpsServerFixture fx{conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd(format(
		"curl -skS --http1.1 --tlsv1.3 --tls-max 1.3 "
		"--connect-timeout 1 --max-time 5 "
		"https://127.0.0.1:{}/ping 2>&1",
		fx.port()));
	INFO("curl exit=" << code << " output=" << body);
	REQUIRE(code == 0);
	REQUIRE(body == R"({"ok":true})");
}
TEST_CASE(
	"ext/openssl: s_client GET /ping returns 200 OK") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto resp = fx.sclient_get("/ping");
	REQUIRE(resp.find("HTTP/1.") != S::npos);
	REQUIRE(resp.find("200") != S::npos);
	REQUIRE(resp.find(R"({"ok":true})") != S::npos);
}
TEST_CASE(
	"ext/openssl: s_client negotiates TLS and server does not crash") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto resp = fx.sclient_get("/ping");
	REQUIRE(!resp.empty());
	auto [code, body] = fx.curl_https("/ping");
	REQUIRE(code == 0);
	REQUIRE(body == R"({"ok":true})");
}
TEST_CASE(
	"ext/openssl: s_client GET path param echoes correctly") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto resp = fx.sclient_get("/hello/tls");
	REQUIRE(resp.find("200") != S::npos);
	REQUIRE(resp.find("hello tls") != S::npos);
}
TEST_CASE(
	"ext/openssl: multiple sequential s_client connections all succeed") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	for (int i = 0; i < 5; ++i) {
		auto resp = fx.sclient_get("/ping");
		REQUIRE(resp.find(R"({"ok":true})") != S::npos);
	}
}
TEST_CASE(
	"ext/curl: TLS 1.1 is rejected") {
	conflux::tests::HttpsServerFixture const fx{conflux::tests::make_external_test_router()};
	auto [code, body] = conflux::tests::run_cmd(
		format("curl -sk --tls-max 1.1 --tlsv1.1 --max-time 5 https://127.0.0.1:{}/ping 2>&1", fx.port()));
	// curl exits non-zero on handshake failure; body may be empty or an error message.
	REQUIRE(code != 0);
	REQUIRE(body.find(R"({"ok":true})") == S::npos);
}
TEST_CASE(
	"ext/curl: SSE streams all events and closes") {
	Router r;
	r.sse("/events", [](HttpRequest const &, SP<SseChannel> const &ch) {
		auto _ = ch->send("data: alpha\n\n");
		auto _ = ch->send("data: beta\n\n");
		ch->close();
	});
	conflux::tests::HttpsServerFixture const fx{move(r)};
	auto [code, body] = conflux::tests::run_cmd_retry(
		format("curl -sk --http1.1 -N --max-time 5 https://127.0.0.1:{}/events", fx.port()));
	INFO(format("code: {}, body: {}", code, body));
	REQUIRE(code == 0);
	REQUIRE(body.find("data: alpha\n\n") != S::npos);
	REQUIRE(body.find("data: beta\n\n") != S::npos);
}
TEST_CASE(
	"ext/curl: SSE send_event delivers typed event") {
	Router r;
	r.sse("/typed", [](HttpRequest const &, SP<SseChannel> const &ch) {
		auto _ = ch->send_event("update", "payload42");
		ch->close();
	});
	conflux::tests::HttpsServerFixture const fx{move(r)};
	auto [code, body] = conflux::tests::run_cmd_retry(
		format("curl -sk --http1.1 -N --max-time 5 https://127.0.0.1:{}/typed", fx.port()));
	INFO(format("code: {}, body: {}", code, body));
	REQUIRE(code == 0);
	REQUIRE(body.find("event: update\n") != S::npos);
	REQUIRE(body.find("data: payload42\n") != S::npos);
}
