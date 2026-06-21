#include <cerrno>

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.http.client;
import conflux.net.async_client;

namespace chttp = conflux::http;

TEST_CASE(
	"http async client: public import exposes async_blocking_send",
	"[http][client][async_blocking][import]") {
	chttp::HttpClient client{};
	auto result = chttp::async_blocking_send(
		client,
		chttp::ClientRequest::get("http://127.0.0.1:9/"),
		chttp::AsyncClientRunOptions{.ring_entries = 0});
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error().kind == chttp::HttpErrorKind::protocol);
	CHECK(result.error().phase == chttp::HttpPhase::connect);
	CHECK(result.error().os_errno == EINVAL);
}
