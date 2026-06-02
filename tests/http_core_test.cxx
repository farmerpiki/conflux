#include <catch2/catch_test_macros.hpp>

import std;
import conflux.tests.assert_probe_support;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.request;
import conflux.net.http.server_types;

namespace chttp = conflux::http;

#ifndef ASSERT_PROBE_BIN
	#error "ASSERT_PROBE_BIN must be defined by CMake"
#endif

TEST_CASE(
	"http core: Url::parse normalizes scheme and preserves authority/path/query",
	"[http.core]") {
	auto url = chttp::Url::parse("HTTPS://example.com:8443/api/v1?q=one");
	REQUIRE(url.has_value());
	CHECK(url->scheme == "https");
	CHECK(url->host == "example.com");
	CHECK(url->port == 8443);
	CHECK(url->path == "/api/v1");
	CHECK(url->query == "q=one");
	CHECK(url->str() == "https://example.com:8443/api/v1?q=one");
}

TEST_CASE(
	"http core: Url::parse handles default ports, bare query, and IPv6 literals",
	"[http.core]") {
	{
		auto url = chttp::Url::parse("http://example.com?x=1");
		REQUIRE(url.has_value());
		CHECK(url->port == 80);
		CHECK(url->path == "/");
		CHECK(url->query == "x=1");
		CHECK(url->str() == "http://example.com/?x=1");
	}
	{
		auto url = chttp::Url::parse("https://[2001:db8::1]:444/path");
		REQUIRE(url.has_value());
		CHECK(url->host == "[2001:db8::1]");
		CHECK(url->port == 444);
		CHECK(url->path == "/path");
		CHECK(url->str() == "https://[2001:db8::1]:444/path");
	}
}

TEST_CASE(
	"http core: Url::parse reports distinct URL error kinds",
	"[http.core]") {
	auto empty = chttp::Url::parse("");
	REQUIRE_FALSE(empty.has_value());
	CHECK(empty.error().kind == chttp::UrlErrorKind::empty);

	auto missing_scheme = chttp::Url::parse("example.com/path");
	REQUIRE_FALSE(missing_scheme.has_value());
	CHECK(missing_scheme.error().kind == chttp::UrlErrorKind::missing_scheme);

	auto unsupported = chttp::Url::parse("ftp://example.com/");
	REQUIRE_FALSE(unsupported.has_value());
	CHECK(unsupported.error().kind == chttp::UrlErrorKind::unsupported_scheme);

	auto missing_host = chttp::Url::parse("http://");
	REQUIRE_FALSE(missing_host.has_value());
	CHECK(missing_host.error().kind == chttp::UrlErrorKind::missing_host);

	auto bad_port = chttp::Url::parse("http://example.com:0/");
	REQUIRE_FALSE(bad_port.has_value());
	CHECK(bad_port.error().kind == chttp::UrlErrorKind::invalid_port);

	std::string too_long(8193, 'x');
	too_long.replace(0, 7, "http://");
	auto long_url = chttp::Url::parse(too_long);
	REQUIRE_FALSE(long_url.has_value());
	CHECK(long_url.error().kind == chttp::UrlErrorKind::too_long);
}

TEST_CASE(
	"http core: Url::set_query_param percent-encodes and appends",
	"[http.core]") {
	auto url = chttp::Url::parse("http://example.com/search?old=1");
	REQUIRE(url.has_value());
	url->set_query_param("a b", "x/y?z");
	url->set_query_param("plus+key", "one two");
	CHECK(url->query == "old=1&a%20b=x%2Fy%3Fz&plus%2Bkey=one%20two");
	CHECK(url->str() == "http://example.com/search?old=1&a%20b=x%2Fy%3Fz&plus%2Bkey=one%20two");
}

TEST_CASE(
	"http core: fallible request builders report URL errors without throwing",
	"[http.core]") {
	auto bad = chttp::try_get("example.com/path");
	REQUIRE_FALSE(bad.has_value());
	CHECK(bad.error().kind == chttp::UrlErrorKind::missing_scheme);

	auto builder = chttp::try_post("https://example.com/submit");
	REQUIRE(builder.has_value());
	auto req = std::move(*builder).body_view("payload").build();
	CHECK(req.method() == "POST");
	CHECK(req.url().host == "example.com");
	CHECK(req.body() == "payload");

	auto dynamic = chttp::try_request("PROPFIND", "https://example.com/root");
	REQUIRE(dynamic.has_value());
	CHECK(dynamic->method() == "PROPFIND");
	CHECK(dynamic->url().path == "/root");

	auto mutable_builder = chttp::ClientRequest::get("https://example.com/");
	auto changed = mutable_builder.try_url("http://example.org/next");
	REQUIRE(changed.has_value());
	CHECK(std::move(mutable_builder).build().url().host == "example.org");
}

TEST_CASE(
	"http core: server request aliases expose first-contact namespace names",
	"[http.core]") {
	static_assert(std::same_as<chttp::RunStatus, conflux::http::RunStatus>);
	static_assert(std::same_as<chttp::HttpRejectReason, conflux::http::HttpRejectReason>);
	static_assert(std::same_as<chttp::HttpRejectionMetrics, conflux::http::HttpRejectionMetrics>);
	static_assert(std::same_as<chttp::HttpServerMetrics, conflux::http::HttpServerMetrics>);
	static_assert(std::same_as<chttp::RequestView, conflux::http::RequestView>);
	static_assert(std::same_as<chttp::Request, conflux::http::RequestView>);
	static_assert(std::same_as<chttp::OwnedRequest, conflux::http::OwnedRequest>);
	static_assert(std::same_as<chttp::UploadedFile, conflux::http::UploadedFile>);
	static_assert(std::same_as<chttp::CloneableFunction<void()>, conflux::http::CloneableFunction<void()>>);
}

TEST_CASE(
	"http core: controlled header classification is shared",
	"[http.core]") {
	CHECK(chttp::is_hop_by_hop_header("Connection"));
	CHECK(chttp::is_hop_by_hop_header("Trailer"));
	CHECK(chttp::is_hop_by_hop_header("Trailers"));
	CHECK(chttp::is_request_controlled_header("Host"));
	CHECK(chttp::is_request_controlled_header("Transfer-Encoding"));
	CHECK_FALSE(chttp::is_request_controlled_header("Content-Type"));
	CHECK(chttp::is_response_framing_header("Content-Length"));
	CHECK(chttp::is_response_framing_header("Trailer"));
	CHECK_FALSE(chttp::is_response_framing_header("ETag"));
}

TEST_CASE(
	"http core: rejection reason helpers expose stable codes and statuses",
	"[http.core]") {
	CHECK(chttp::reject_reason_code(chttp::HttpRejectReason::duplicate_content_length) == "duplicate_content_length");
	CHECK(chttp::reject_reason_code(chttp::HttpRejectReason::header_block_too_large) == "header_block_too_large");
	CHECK(
		chttp::reject_reason_diagnostic_code(chttp::HttpRejectReason::header_block_too_large)
		== "http.reject.header_block_too_large");
	CHECK(
		chttp::reject_reason_diagnostic_code(chttp::HttpRejectReason::content_length_with_transfer_encoding)
		== "http.reject.content_length_with_transfer_encoding");
	CHECK(chttp::reject_reason_status(chttp::HttpRejectReason::duplicate_content_length) == 400);
	CHECK(chttp::reject_reason_status(chttp::HttpRejectReason::header_block_too_large) == 431);
	CHECK(chttp::reject_reason_status(chttp::HttpRejectReason::body_too_large) == 413);
	CHECK(chttp::reject_reason_status(chttp::HttpRejectReason::body_timeout) == 408);
	CHECK(chttp::reject_reason_code(chttp::HttpRejectReason::body_timeout) == "body_timeout");
	CHECK(
		chttp::reject_reason_detail(chttp::HttpRejectReason::content_length_with_transfer_encoding)
			.find("Content-Length")
		!= std::string_view::npos);
}

TEST_CASE(
	"http core: typed server request field extractors parse common values",
	"[http.core]") {
	conflux::http::HttpFieldsView params;
	params.emplace_back("id", "42");
	conflux::http::HttpFieldsView headers{true};
	headers.emplace_back("X-Limit", "128");
	conflux::http::HttpFieldsView query;
	query.emplace_back("page", "3");
	query.emplace_back("enabled", "yes");
	query.emplace_back("bad", "12x");
	conflux::http::HttpFieldsView form;
	form.emplace_back("price", "19.5");
	conflux::http::HttpFieldsView cookies;
	cookies.emplace_back("sid", "abc-123");

	conflux::http::RequestView req{
		"GET",
		"/items/42",
		"HTTP/1.1",
		"127.0.0.1",
		false,
		params,
		headers,
		query,
		form,
		cookies,
		std::span<conflux::http::UploadedFile const>{},
		{}};

	auto id = req.param_as<std::uint32_t>("id");
	REQUIRE(id.has_value());
	CHECK(*id == 42);

	auto limit = req.header_as<std::uint32_t>("x-limit");
	REQUIRE(limit.has_value());
	CHECK(*limit == 128);

	auto enabled = req.query_as<bool>("enabled");
	REQUIRE(enabled.has_value());
	CHECK(*enabled);

	auto price = req.form_as<double>("price");
	REQUIRE(price.has_value());
	CHECK(*price == 19.5);

	auto sid = req.cookie_as<std::string_view>("sid");
	REQUIRE(sid.has_value());
	CHECK(*sid == "abc-123");

	auto missing = req.optional_query_as<std::int32_t>("missing");
	REQUIRE(missing.has_value());
	CHECK_FALSE(missing->has_value());

	auto bad = req.query_as<std::int32_t>("bad");
	REQUIRE_FALSE(bad.has_value());
	CHECK(bad.error().kind == chttp::HttpFieldErrorKind::invalid);
	CHECK(bad.error().source == chttp::HttpFieldSource::query);
	CHECK(bad.error().name == "bad");
}

TEST_CASE(
	"http core: typed owned request field extractors preserve owned lifetimes",
	"[http.core]") {
	conflux::http::OwnedRequest req;
	req.headers.set("Content-Length", "512");
	req.query.emplace_back("debug", "off");
	req.cookies.emplace_back("theme", "dark");

	auto len = req.header_as<std::uint64_t>("content-length");
	REQUIRE(len.has_value());
	CHECK(*len == 512);

	auto debug = req.query_as<bool>("debug");
	REQUIRE(debug.has_value());
	CHECK_FALSE(*debug);

	auto theme = req.cookie_as<std::string>("theme");
	REQUIRE(theme.has_value());
	CHECK(*theme == "dark");

	auto missing = req.param_as<std::uint32_t>("id");
	REQUIRE_FALSE(missing.has_value());
	CHECK(missing.error().kind == chttp::HttpFieldErrorKind::missing);
	CHECK(missing.error().source == chttp::HttpFieldSource::params);
}

TEST_CASE(
	"http core: ClientRequest builder encodes auth and form bodies",
	"[http.core]") {
	conflux::http::HttpFields form{true};
	form.emplace_back("a b", "c+d");
	form.emplace_back("slash", "/=");

	auto req =
		chttp::ClientRequest::post("https://example.com/submit").basic("testuser", "testpass").body_form(form).build();

	CHECK(req.method() == "POST");
	CHECK(req.headers()["authorization"] == "Basic dGVzdHVzZXI6dGVzdHBhc3M=");
	CHECK(req.headers()["content-type"] == "application/x-www-form-urlencoded");
	CHECK(req.body() == "a+b=c%2Bd&slash=%2F%3D");
}

TEST_CASE(
	"http core: ClientRequest builder exposes policy knobs and clear_body reset",
	"[http.core]") {
	chttp::HttpTimeouts timeouts{};
	timeouts.connect = std::chrono::milliseconds{1234};
	timeouts.first_byte = std::chrono::milliseconds{5678};

	auto req = chttp::ClientRequest::method("PROPFIND", "http://example.com/root")
				   .query("name", "a b")
				   .bearer("token-123")
				   .accept_json()
				   .if_none_match("\"abc\"")
				   .body("discard me")
				   .clear_body()
				   .body_json_raw("{\"ok\":true}")
				   .timeouts(timeouts)
				   .follow_redirects(3)
				   .verify_peer(false)
				   .server_name("sni.example")
				   .build();

	CHECK(req.method() == "PROPFIND");
	CHECK(req.url().str() == "http://example.com/root?name=a%20b");
	CHECK(req.headers()["authorization"] == "Bearer token-123");
	CHECK(req.headers()["accept"] == "application/json");
	CHECK(req.headers()["if-none-match"] == "\"abc\"");
	CHECK(req.headers()["content-type"] == "application/json");
	CHECK(req.body() == "{\"ok\":true}");
	CHECK(req.timeouts().connect == std::chrono::milliseconds{1234});
	CHECK(req.timeouts().first_byte == std::chrono::milliseconds{5678});
	CHECK(req.max_redirects() == 3);
	CHECK(req.follows_redirects());
	CHECK_FALSE(req.verify_peer());
	CHECK(req.server_name() == "sni.example");
}

TEST_CASE(
	"http core: ClientRequest builder formats conditional dates in HTTP-date form",
	"[http.core]") {
	auto epoch = std::chrono::system_clock::time_point{};
	auto req =
		chttp::ClientRequest::get("http://example.com/").if_modified_since(epoch).if_unmodified_since(epoch).build();

	CHECK(req.headers()["if-modified-since"] == "Thu, 01 Jan 1970 00:00:00 GMT");
	CHECK(req.headers()["if-unmodified-since"] == "Thu, 01 Jan 1970 00:00:00 GMT");
}

TEST_CASE(
	"http core: ClientRequest builder debug-asserts on double body setters",
	"[http.core][death]") {
#ifdef NDEBUG
	SKIP("assert inactive in release build");
#else
	REQUIRE(conflux::tests::run_assert_probe(ASSERT_PROBE_BIN, "body_after_body") == 42);
	REQUIRE(conflux::tests::run_assert_probe(ASSERT_PROBE_BIN, "json_after_body") == 42);
	REQUIRE(conflux::tests::run_assert_probe(ASSERT_PROBE_BIN, "form_after_body") == 42);
#endif
}

TEST_CASE(
	"http core: request views own uploaded files when converted",
	"[http.core]") {
	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/upload";
	req.version = "HTTP/1.1";
	req.remote_addr = "127.0.0.1";
	req.files.push_back(conflux::http::UploadedFile::borrowed("file", "a.txt", "text/plain", "payload"));
	req.body = "body";

	conflux::http::RequestView view{req};
	auto owned = view.to_owned();
	REQUIRE(owned.files.size() == 1);
	CHECK(owned.files[0].owns_metadata);
	CHECK(owned.files[0].owns_data);
	CHECK(owned.files[0].name == "file");
	CHECK(owned.files[0].filename == "a.txt");
	CHECK(owned.files[0].content_type == "text/plain");
	CHECK(owned.files[0].data == "payload");

	req.files[0] = conflux::http::UploadedFile::borrowed("mutated", "b.txt", "text/html", "changed");
	req.body = "changed body";
	CHECK(owned.files[0].name == "file");
	CHECK(owned.files[0].data == "payload");
	CHECK(owned.body == "body");
}
