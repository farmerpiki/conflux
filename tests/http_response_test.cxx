#include <cerrno>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.file_map;
import conflux.net.http.realtime;
import conflux.net.http.response;

namespace chttp = conflux::http;

namespace {

[[nodiscard]] std::uint64_t read_eventfd_value(
	int fd) {
	std::uint64_t value{};
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
	std::uint64_t value{};
	auto const n = ::read(fd, &value, sizeof(value));
	return n < 0 && errno == EAGAIN;
}

} // namespace

TEST_CASE(
	"http response: first-contact alias and status-code overloads are available",
	"[http.response]") {
	static_assert(std::same_as<chttp::Response, HttpResponse>);

	CHECK(HttpResponse::status_text_for(kHttpBadRequest) == "Bad Request");
	CHECK(HttpResponse::status_text_for(599).empty());

	auto text = chttp::Response::text("bad", kHttpBadRequest);
	CHECK(text.status == kHttpBadRequest);
	CHECK(text.status_text == "Bad Request");
	CHECK(text.text_body() == "bad");

	auto json = chttp::Response::json("{}", kHttpCreated);
	CHECK(json.status == kHttpCreated);
	CHECK(json.status_text == "Created");
	CHECK(json.content_type == "application/json");

	auto custom = chttp::Response::with_body("x", "application/problem+json", 599);
	CHECK(custom.status == 599);
	CHECK(custom.status_text.empty());
	CHECK(custom.content_type == "application/problem+json");
}

TEST_CASE(
	"http response: factories escape detail strings in generated HTML bodies",
	"[http.response]") {
	auto not_found = HttpResponse::not_found("/x?<bad>&\"'");
	CHECK(not_found.status == kHttpNotFound);
	CHECK(not_found.text_body().find("&lt;bad&gt;&amp;&quot;&#39;") != std::string_view::npos);

	auto bad = HttpResponse::bad_request("<bad>&\"'");
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.text_body().find("&lt;bad&gt;&amp;&quot;&#39;") != std::string_view::npos);

	auto internal = HttpResponse::internal_error("<boom>");
	CHECK(internal.status == kHttpInternalServerError);
	CHECK(internal.text_body().find("&lt;boom&gt;") != std::string_view::npos);
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
		CHECK(resp.text_body().find("&lt;no&gt;") != std::string_view::npos);
	}
	{
		auto resp = HttpResponse::unprocessable_entity("bad field");
		CHECK(resp.status == kHttpUnprocessableEntity);
		CHECK(resp.status_text == "Unprocessable Entity");
		CHECK(resp.text_body().find("bad field") != std::string_view::npos);
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
		auto ch = std::make_shared<SseChannel>();
		auto resp = HttpResponse::sse(ch);
		CHECK(resp.status == kHttpOk);
		CHECK(resp.content_type == "text/event-stream");
		CHECK(resp.is_sse());
		CHECK(resp.sse_channel_ptr() == ch);
	}
	{
		auto deferred = std::make_shared<DeferredResponse>(std::chrono::milliseconds{10000});
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

	auto sse = std::make_shared<SseChannel>();
	resp.set_sse_channel(sse);
	CHECK(resp.is_sse());
	CHECK_FALSE(resp.is_text());
	CHECK(resp.sse_channel_ptr() == sse);
	CHECK(resp.content_length() == 0);
	CHECK(resp.take_sse_channel() == sse);
	CHECK_FALSE(resp.sse_channel_ptr());

	auto close_observed = std::make_shared<SseChannel>();
	int closed = 0;
	close_observed->on_close([&] { ++closed; });
	close_observed->close();
	close_observed->close();
	CHECK(closed == 1);
	close_observed->on_close([&] { ++closed; });
	CHECK(closed == 2);

	auto drop_newest = std::make_shared<SseChannel>(4, SseOverflowPolicy::DropNewest);
	CHECK(drop_newest->overflow_policy_vocabulary() == OverflowPolicy::drop_newest);
	CHECK(drop_newest->send("1234"));
	CHECK_FALSE(drop_newest->send("5"));
	CHECK(drop_newest->pressure_metrics().dropped_newest == 1);

	auto drop_oldest = std::make_shared<SseChannel>(4, SseOverflowPolicy::DropOldest);
	CHECK(drop_oldest->overflow_policy_vocabulary() == OverflowPolicy::drop_oldest);
	CHECK(drop_oldest->send("1234"));
	CHECK(drop_oldest->send("5"));
	CHECK(drop_oldest->pressure_metrics().dropped_oldest == 1);

	auto disconnect = std::make_shared<SseChannel>(4, sse_overflow_policy(OverflowPolicy::close_connection));
	CHECK(disconnect->send("1234"));
	CHECK_FALSE(disconnect->send("5"));
	CHECK(disconnect->is_closed());
	CHECK(disconnect->pressure_metrics().disconnected_for_pressure == 1);

	auto ws = std::make_shared<WsUpgrade>();
	ws->accept_key = "accept";
	resp.set_ws_upgrade(ws);
	CHECK(resp.is_ws_upgrade());
	CHECK(resp.ws_upgrade_ptr() == ws);
	CHECK(resp.take_ws_upgrade() == ws);
	CHECK_FALSE(resp.ws_upgrade_ptr());

	int sockets[2]{};
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	{
		WsConn conn{sockets[0]};
		int closed = 0;
		conn.on_close([&] { ++closed; });
		conn.close();
		conn.close();
		CHECK(closed == 1);
		conn.on_close([&] { ++closed; });
		CHECK(closed == 2);
	}
	::close(sockets[1]);

	auto mapped = std::make_shared<MappedBody>();
	mapped->offset = 3;
	mapped->size = 42;
	resp.set_mapped_file(mapped);
	CHECK(resp.is_mapped_file());
	CHECK(resp.mapped_file_ptr() == mapped);
	CHECK(resp.content_length() == 42);
	CHECK(resp.take_mapped_file() == mapped);
	CHECK_FALSE(resp.mapped_file_ptr());

	auto streamed = std::make_shared<StreamedFile>();
	streamed->send_offset = 5;
	streamed->send_size = 77;
	streamed->total_size = 100;
	resp.set_streamed_file(streamed);
	CHECK(resp.is_streamed_file());
	CHECK(resp.streamed_file_ptr() == streamed);
	CHECK(resp.content_length() == 77);
	CHECK(resp.take_streamed_file() == streamed);
	CHECK_FALSE(resp.streamed_file_ptr());

	auto deferred = std::make_shared<DeferredResponse>(std::chrono::milliseconds{10000});
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
	"http response: streamed file completion callbacks run once",
	"[http][response]") {
	auto streamed = std::make_shared<StreamedFile>();
	std::vector<StreamedFileResult> observed;
	streamed->on_complete([&](StreamedFileResult result) { observed.push_back(result); });

	streamed->notify_complete();
	streamed->notify_failed();
	streamed->on_complete([&](StreamedFileResult result) { observed.push_back(result); });

	REQUIRE(observed.size() == 2);
	CHECK(observed[0] == StreamedFileResult::completed);
	CHECK(observed[1] == StreamedFileResult::completed);
}

TEST_CASE(
	"http response: DeferredResponse complete wakes eventfd once and stores first response",
	"[http.response]") {
	DeferredResponse deferred{std::chrono::milliseconds{10000}};
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
	DeferredResponse deferred{std::chrono::milliseconds{10000}};
	make_eventfd_nonblocking(deferred.eventfd_fd());

	auto const now = std::chrono::steady_clock::now();
	deferred.set_deadline(now + std::chrono::seconds{1});
	CHECK_FALSE(deferred.expire_if_past_deadline(now));
	CHECK(eventfd_would_block(deferred.eventfd_fd()));

	deferred.set_deadline(now - std::chrono::milliseconds{1});
	CHECK(deferred.expire_if_past_deadline(now));
	CHECK(read_eventfd_value(deferred.eventfd_fd()) == 1);
	CHECK(eventfd_would_block(deferred.eventfd_fd()));

	auto ready = deferred.take_ready();
	REQUIRE(ready.has_value());
	CHECK(ready->status == kHttpGatewayTimeout);
	CHECK(ready->status_text == "Gateway Timeout");
}
