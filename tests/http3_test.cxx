// Plain TU — Catch2 macros with templates trigger GCC's TU-local entity
// exposure rule inside module units; keep this file header-only.
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <openssl/ssl.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.features;
#if CONFLUX_HAS_HTTP3
import conflux.net.config;
import conflux.net.http.client;
import conflux.net.http_server;
import conflux.net.router;
import conflux.net.vhost;
import conflux.net.http3;
import conflux.net.tls;
import conflux.tests.support;
#endif

#if CONFLUX_HAS_HTTP3
namespace {

struct TempPathCleanup {
	std::string path;
	~TempPathCleanup() {
		if (!path.empty()) {
			::unlink(path.c_str());
		}
	}
};
struct TempCert {
	std::string cert_path;
	std::string key_path;
	TempCert() {
		char cert_tmp[] = "/tmp/conflux_http3_test_cert_XXXXXX.pem";
		char key_tmp[] = "/tmp/conflux_http3_test_key_XXXXXX.pem";
		int cert_fd = ::mkstemps(cert_tmp, 4);
		int key_fd = ::mkstemps(key_tmp, 4);
		REQUIRE(cert_fd >= 0);
		REQUIRE(key_fd >= 0);
		::close(cert_fd);
		::close(key_fd);
		TempPathCleanup cert_cleanup{cert_tmp};
		TempPathCleanup key_cleanup{key_tmp};
		auto const cmd = std::format(
			"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
			"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
			key_tmp,
			cert_tmp);
		REQUIRE(::system(cmd.c_str()) == 0);
		cert_path = cert_tmp;
		key_path = key_tmp;
		cert_cleanup.path.clear();
		key_cleanup.path.clear();
	}
	~TempCert() {
		::unlink(cert_path.c_str());
		::unlink(key_path.c_str());
	}
};
[[nodiscard]] conflux::http::Config http3_test_config(
	TempCert const &cert) {
	auto cfg = conflux::tests::mw_config();
	cfg.cert_file = cert.cert_path;
	cfg.key_file = cert.key_path;
	cfg.http3.enabled = true;
	return cfg;
}

} // namespace
TEST_CASE(
	"http3 alt-svc formatter") {
	CHECK(conflux::http::detail::http3_alt_svc_value(443, 86400) == R"(h3=":443"; ma=86400)");
	CHECK(conflux::http::detail::http3_alt_svc_value(8443, 60) == R"(h3=":8443"; ma=60)");
	CHECK(conflux::http::detail::kH3Alpn == "h3");
}
TEST_CASE(
	"http3 listener binds and stops cleanly") {
	conflux::http::Router router;
	router.get("/", [](conflux::http::RequestView const &) { return conflux::http::Response::text("hi"); });
	conflux::http::Http3Config cfg{};
	cfg.enabled = true;
	UniqueSslCtx const ctx{SSL_CTX_new(TLS_server_method())};
	REQUIRE(ctx);
	conflux::http::detail::Http3Listener listener(&router, cfg, 0, ctx.get());
	listener.start();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	listener.stop();
}
TEST_CASE(
	"http3 alt-svc uses the actual bound port") {
	TempCert const cert;
	auto cfg = http3_test_config(cert);

	conflux::http::Router router;
	router.get("/ping", [](conflux::http::RequestView const &) {
		return conflux::http::Response::json(R"({"ok":true})");
	});
	conflux::tests::ScopedTestServer const srv{cfg, std::move(router)};

	conflux::http::HttpClientOptions opts1{};
	opts1.verify_peer = false;
	conflux::http::HttpClient client1{std::move(opts1)};
	auto response = client1.blocking_send(
		conflux::http::ClientRequest::get(std::format("https://127.0.0.1:{}/ping", srv.port()))
			.server_name("localhost")
			.build());
	REQUIRE(response);
	CHECK(
		response->head.headers["alt-svc"]
		== conflux::http::detail::http3_alt_svc_value(srv.port(), cfg.http3.alt_svc_max_age_sec));
}
TEST_CASE(
	"http3 is not advertised for VHostRouter servers") {
	TempCert const cert;
	auto cfg = http3_test_config(cert);

	conflux::http::Router def;
	def.get("/ping", [](conflux::http::RequestView const &) {
		return conflux::http::Response::json(R"({"ok":true})");
	});
	conflux::http::VHostRouter vhosts;
	vhosts.set_default(std::move(def));

	auto const port = conflux::tests::test_servers().start(cfg, std::move(vhosts));
	conflux::http::HttpClientOptions opts2{};
	opts2.verify_peer = false;
	conflux::http::HttpClient client2{std::move(opts2)};
	auto response2 = client2.blocking_send(
		conflux::http::ClientRequest::get(std::format("https://127.0.0.1:{}/ping", port))
			.server_name("localhost")
			.build());
	REQUIRE(response2);
	CHECK(response2->head.status == 200);
	CHECK_FALSE(response2->head.headers.contains("alt-svc"));
}
#endif
