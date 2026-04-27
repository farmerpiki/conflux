// Plain TU — Catch2 macros with templates trigger GCC's TU-local entity
// exposure rule inside module units; keep this file header-only.
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <openssl/ssl.h>
#include <unistd.h>

import std;
import conflux.features;
#if CONFLUX_HAS_HTTP3
import conflux.net.http;
import conflux.net.http3;
import conflux.tests.support;
#endif

#if CONFLUX_HAS_HTTP3
namespace {

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
		auto const cmd = std::format(
			"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} "
			"-days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
			key_tmp,
			cert_tmp);
		REQUIRE(::system(cmd.c_str()) == 0);
		cert_path = cert_tmp;
		key_path = key_tmp;
	}

	~TempCert() {
		::unlink(cert_path.c_str());
		::unlink(key_path.c_str());
	}
};

[[nodiscard]] Config http3_test_config(
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
	CHECK(http3_alt_svc_value(443, 86400) == R"(h3=":443"; ma=86400)");
	CHECK(http3_alt_svc_value(8443, 60) == R"(h3=":8443"; ma=60)");
	CHECK(kH3Alpn == "h3");
}

TEST_CASE(
	"http3 listener binds and stops cleanly") {
	Router router;
	router.get("/", [](HttpRequestView const &) { return HttpResponse::text("hi"); });
	Http3Config cfg{};
	cfg.enabled = true;
	SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
	REQUIRE(ctx != nullptr);
	try {
		Http3Listener listener(&router, cfg, 0, ctx);
		listener.start();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		listener.stop();
	} catch (...) {
		SSL_CTX_free(ctx);
		throw;
	}
	SSL_CTX_free(ctx);
}

TEST_CASE(
	"http3 alt-svc uses the actual bound port") {
	TempCert const cert;
	auto cfg = http3_test_config(cert);

	Router router;
	router.get("/ping", [](HttpRequestView const &) { return HttpResponse::json(R"({"ok":true})"); });
	conflux::tests::ScopedTestServer const srv{cfg, std::move(router)};

	conflux::http::HttpClientOptions opts1{};
	opts1.verify_peer = false;
	conflux::http::HttpClient client1{std::move(opts1)};
	auto response = client1.send_blocking(
		conflux::http::HttpRequest::get(std::format("https://127.0.0.1:{}/ping", srv.port()))
			.server_name("localhost")
			.build());
	REQUIRE(response);
	CHECK(response->head.headers["alt-svc"] == http3_alt_svc_value(srv.port(), cfg.http3.alt_svc_max_age_sec));
}

TEST_CASE(
	"http3 is not advertised for VHostRouter servers") {
	TempCert const cert;
	auto cfg = http3_test_config(cert);

	Router def;
	def.get("/ping", [](HttpRequestView const &) { return HttpResponse::json(R"({"ok":true})"); });
	VHostRouter vhosts;
	vhosts.set_default(std::move(def));

	auto const port = conflux::tests::test_servers().start(cfg, std::move(vhosts));
	conflux::http::HttpClientOptions opts2{};
	opts2.verify_peer = false;
	conflux::http::HttpClient client2{std::move(opts2)};
	auto response2 = client2.send_blocking(
		conflux::http::HttpRequest::get(std::format("https://127.0.0.1:{}/ping", port))
			.server_name("localhost")
			.build());
	REQUIRE(response2);
	CHECK(response2->head.status == 200);
	CHECK_FALSE(response2->head.headers.contains("alt-svc"));
}
#endif
