// HTTP/3 external tests. Spawns a real HTTPS server with h3 enabled,
// drives it with system curl --http3-only.
// Plain TU — see TRICKS.md.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.net.http;
import conflux.tests.external_support;
namespace {

Router make_router() {
	return conflux::tests::make_external_test_router();
}

} // namespace
TEST_CASE(
	"h3: curl --http3-only GET /ping returns 200 JSON") {
	conflux::tests::Http3ServerFixture const fx{make_router()};
	auto [code, out] = fx.curl_h3("/ping");
	REQUIRE(code == 0);
	REQUIRE(out == R"({"ok":true})");
}
TEST_CASE(
	"h3: curl --http3-only GET with path param") {
	conflux::tests::Http3ServerFixture const fx{make_router()};
	auto [code, out] = fx.curl_h3("/hello/quic");
	REQUIRE(code == 0);
	REQUIRE(out == "hello quic");
}
TEST_CASE(
	"h3: curl --http3-only POST echoes body") {
	conflux::tests::Http3ServerFixture const fx{make_router()};
	auto [code, out] = conflux::tests::run_cmd(
		std::format(
			"curl -sk --http3-only --max-time 5 --resolve localhost:{}:127.0.0.1 "
			"-d 'hello h3' https://localhost:{}/echo",
			fx.port(),
			fx.port()));
	REQUIRE(code == 0);
	REQUIRE(out == "hello h3");
}
TEST_CASE(
	"h3: unknown route returns 404") {
	conflux::tests::Http3ServerFixture const fx{make_router()};
	auto [code, out] = fx.curl_h3_status("/does-not-exist");
	REQUIRE(code == 0);
	REQUIRE(out == "404");
}
TEST_CASE(
	"h3: large body delivered fully") {
	std::size_t const kSize = 128 * 1024;
	std::string big(kSize, 'Q');
	Router r;
	r.get("/big", [&](Request const &) { return Response::text(big); });
	conflux::tests::Http3ServerFixture const fx{std::move(r)};
	auto [code, out] = fx.curl_h3("/big");
	REQUIRE(code == 0);
	REQUIRE(out.size() == kSize);
	REQUIRE(out == big);
}
