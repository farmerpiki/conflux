#include <cerrno>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.file_map;
import conflux.net.http.realtime;
import conflux.net.http.response;


namespace {

[[nodiscard]] u64 read_eventfd_value(
	int fd) {
	u64 value{};
	auto const n = ::read(fd, &value, sizeof(value));
	REQUIRE(n == static_cast<ssize_t>(sizeof(value)));
	return value;
}

void make_eventfd_nonblocking(
	int fd) {
	auto const flags = ::fcntl(fd, F_GETFL, 0);
	REQUIRE(flags >= 0);
	REQUIRE(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

[[nodiscard]] bool eventfd_would_block(
	int fd) {
	u64 value{};
	auto const n = ::read(fd, &value, sizeof(value));
	return n < 0 && errno == EAGAIN;
}

} // namespace

TEST_CASE(
	"http response: factories escape detail strings in generated HTML bodies",
	"[http.response]") {
	auto not_found = HttpResponse::not_found("/x?<bad>&\"'");
	CHECK(not_found.status == kHttpNotFound);
	CHECK(not_found.text_body().find("&lt;bad&gt;&amp;&quot;&#39;") != SV::npos);

	auto bad = HttpResponse::bad_request("<bad>&\"'");
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.text_body().find("&lt;bad&gt;&amp;&quot;&#39;") != SV::npos);

	auto internal = HttpResponse::internal_error("<boom>");
	CHECK(internal.status == kHttpInternalServerError);
	CHECK(internal.text_body().find("&lt;boom&gt;") != SV::npos);
}

TEST_CASE(
	"http response: set_cookie and append_vary preserve HTTP header semantics",
	"[http.response]") {
	HttpResponse resp;
	resp.set_cookie("session", "abc", "Path=/; HttpOnly");
	resp.set_cookie("theme", "dark");
	CHECK(resp.set_cookies.size() == 2);
	CHECK(resp.set_cookies[0] == "session=abc; Path=/; HttpOnly");
	CHECK(resp.set_cookies[1] == "theme=dark");

	resp.append_vary("Origin");
	resp.append_vary("origin");
	resp.append_vary("Accept-Encoding");
	CHECK(resp.headers["vary"] == "Origin, Accept-Encoding");

	resp.headers["vary"] = "*";
	resp.append_vary("Origin");
	CHECK(resp.headers["vary"] == "*");
}

TEST_CASE(
	"http response: content_length follows body kind and explicit HEAD hint",
	"[http.response]") {
	HttpResponse resp = HttpResponse::text("hello");
	CHECK(resp.content_length() == 5);
	CHECK(resp.text_body() == "hello");

	resp.content_length_hint = 123;
	CHECK(resp.content_length() == 123);

	resp.set_text_body("changed");
	resp.content_length_hint = 0;
	CHECK(resp.content_length() == 7);
	CHECK(resp.take_text_body() == "changed");
	CHECK(resp.text_body().empty());
}

TEST_CASE(
	"http response: status factories set expected status, text, headers, and body kinds",
	"[http.response]") {
	{
		auto resp = HttpResponse::html("<p>ok</p>", 201, "Created");
		CHECK(resp.status == 201);
		CHECK(resp.status_text == "Created");
		CHECK(resp.content_type == "text/html; charset=utf-8");
		CHECK(resp.text_body() == "<p>ok</p>");
	}
	{
		auto resp = HttpResponse::json("{\"ok\":true}", 202, "Accepted");
		CHECK(resp.status == 202);
		CHECK(resp.status_text == "Accepted");
		CHECK(resp.content_type == "application/json");
		CHECK(resp.text_body() == "{\"ok\":true}");
	}
	{
		auto resp = HttpResponse::redirect("/next", kHttpPermanentRedirect);
		CHECK(resp.status == kHttpPermanentRedirect);
		CHECK(resp.status_text == "Permanent Redirect");
		CHECK(resp.headers["location"] == "/next");
	}
	{
		auto resp = HttpResponse::method_not_allowed({"GET", "POST"});
		CHECK(resp.status == kHttpMethodNotAllowed);
		CHECK(resp.headers["allow"] == "GET, POST");
	}
	{
		auto resp = HttpResponse::unauthorized("Basic realm=\"test\"");
		CHECK(resp.status == kHttpUnauthorized);
		CHECK(resp.headers["www-authenticate"] == "Basic realm=\"test\"");
	}
	{
		auto resp = HttpResponse::forbidden("<no>");
		CHECK(resp.status == kHttpForbidden);
		CHECK(resp.status_text == "Forbidden");
		CHECK(resp.text_body().find("&lt;no&gt;") != SV::npos);
	}
	{
		auto resp = HttpResponse::unprocessable_entity("bad field");
		CHECK(resp.status == kHttpUnprocessableEntity);
		CHECK(resp.status_text == "Unprocessable Entity");
		CHECK(resp.text_body().find("bad field") != SV::npos);
	}
	{
		auto resp = HttpResponse::uri_too_long();
		CHECK(resp.status == kHttpUriTooLong);
		CHECK(resp.status_text == "URI Too Long");
	}
	{
		auto resp = HttpResponse::header_fields_too_large();
		CHECK(resp.status == kHttpRequestHeaderFieldsTooLarge);
		CHECK(resp.status_text == "Request Header Fields Too Large");
	}
	{
		auto resp = HttpResponse::bad_gateway("upstream failed");
		CHECK(resp.status == kHttpBadGateway);
		CHECK(resp.text_body() == "upstream failed");
	}
	{
		auto resp = HttpResponse::content_too_large();
		CHECK(resp.status == kHttpRequestEntityTooLarge);
		CHECK(resp.status_text == "Content Too Large");
	}
	{
		auto resp = HttpResponse::no_content();
		CHECK(resp.status == kHttpNoContent);
		CHECK(resp.status_text == "No Content");
	}
	{
		auto ch = make_shared<SseChannel>();
		auto resp = HttpResponse::sse(ch);
		CHECK(resp.status == kHttpOk);
		CHECK(resp.content_type == "text/event-stream");
		CHECK(resp.is_sse());
		CHECK(resp.sse_channel_ptr() == ch);
	}
	{
		auto deferred = make_shared<DeferredResponse>(chrono::milliseconds{10000});
		auto resp = HttpResponse::deferred(deferred);
		CHECK(resp.is_deferred());
		CHECK(resp.deferred_response_ptr() == deferred);
	}
}


TEST_CASE(
	"http response: non-text body setters, accessors, and take helpers preserve variant boundaries",
	"[http.response]") {
	HttpResponse resp;
	CHECK(resp.is_text());
	CHECK(resp.text_body().empty());

	auto sse = make_shared<SseChannel>();
	resp.set_sse_channel(sse);
	CHECK(resp.is_sse());
	CHECK_FALSE(resp.is_text());
	CHECK(resp.sse_channel_ptr() == sse);
	CHECK(resp.content_length() == 0);
	CHECK(resp.take_sse_channel() == sse);
	CHECK_FALSE(resp.sse_channel_ptr());

	auto ws = make_shared<WsUpgrade>();
	ws->accept_key = "accept";
	resp.set_ws_upgrade(ws);
	CHECK(resp.is_ws_upgrade());
	CHECK(resp.ws_upgrade_ptr() == ws);
	CHECK(resp.take_ws_upgrade() == ws);
	CHECK_FALSE(resp.ws_upgrade_ptr());

	auto mapped = make_shared<MappedBody>();
	mapped->offset = 3;
	mapped->size = 42;
	resp.set_mapped_file(mapped);
	CHECK(resp.is_mapped_file());
	CHECK(resp.mapped_file_ptr() == mapped);
	CHECK(resp.content_length() == 42);
	CHECK(resp.take_mapped_file() == mapped);
	CHECK_FALSE(resp.mapped_file_ptr());

	auto streamed = make_shared<StreamedFile>();
	streamed->send_offset = 5;
	streamed->send_size = 77;
	streamed->total_size = 100;
	resp.set_streamed_file(streamed);
	CHECK(resp.is_streamed_file());
	CHECK(resp.streamed_file_ptr() == streamed);
	CHECK(resp.content_length() == 77);
	CHECK(resp.take_streamed_file() == streamed);
	CHECK_FALSE(resp.streamed_file_ptr());

	auto deferred = make_shared<DeferredResponse>(chrono::milliseconds{10000});
	resp.set_deferred_response(deferred);
	CHECK(resp.is_deferred());
	CHECK(resp.deferred_response_ptr() == deferred);
	CHECK(resp.take_deferred_response() == deferred);
	CHECK_FALSE(resp.deferred_response_ptr());

	resp.text_body_mut() = "reset to text";
	CHECK(resp.is_text());
	CHECK(resp.text_body() == "reset to text");
}

TEST_CASE(
	"http response: DeferredResponse complete wakes eventfd once and stores first response",
	"[http.response]") {
	DeferredResponse deferred{chrono::milliseconds{10000}};
	make_eventfd_nonblocking(deferred.eventfd_fd());

	deferred.complete(HttpResponse::text("first"));
	deferred.complete(HttpResponse::text("second"));

	CHECK(deferred.is_ready());
	CHECK(read_eventfd_value(deferred.eventfd_fd()) == 1);
	CHECK(eventfd_would_block(deferred.eventfd_fd()));

	auto ready = deferred.take_ready();
	REQUIRE(ready.has_value());
	CHECK(ready->text_body() == "first");
	CHECK_FALSE(deferred.is_ready());
	CHECK_FALSE(deferred.take_ready().has_value());
}

TEST_CASE(
	"http response: DeferredResponse deadline expiry wakes eventfd and materializes gateway timeout",
	"[http.response]") {
	DeferredResponse deferred{chrono::milliseconds{10000}};
	make_eventfd_nonblocking(deferred.eventfd_fd());

	auto const now = chrono::steady_clock::now();
	deferred.set_deadline(now + chrono::seconds{1});
	CHECK_FALSE(deferred.expire_if_past_deadline(now));
	CHECK(eventfd_would_block(deferred.eventfd_fd()));

	deferred.set_deadline(now - chrono::milliseconds{1});
	CHECK(deferred.expire_if_past_deadline(now));
	CHECK(read_eventfd_value(deferred.eventfd_fd()) == 1);
	CHECK(eventfd_would_block(deferred.eventfd_fd()));

	auto ready = deferred.take_ready();
	REQUIRE(ready.has_value());
	CHECK(ready->status == kHttpGatewayTimeout);
	CHECK(ready->status_text == "Gateway Timeout");
}
