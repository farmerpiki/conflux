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
import conflux.work;

namespace chttp = conflux::http;
using chttp::DeferredResponse;
using chttp::Response;

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
	REQUIRE(
		::fcntl(fd, F_SETFL, static_cast<int>(static_cast<unsigned>(flags) | static_cast<unsigned>(O_NONBLOCK))) == 0);
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
	static_assert(std::same_as<chttp::Response, conflux::http::Response>);

	CHECK(conflux::http::Response::status_text_for(kHttpBadRequest) == "Bad Request");
	CHECK(conflux::http::Response::status_text_for(408) == "Request Timeout");
	CHECK(conflux::http::Response::status_text_for(417) == "Expectation Failed");
	CHECK(conflux::http::Response::status_text_for(599).empty());

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
	auto not_found = conflux::http::Response::not_found("/x?<bad>&\"'");
	CHECK(not_found.status == kHttpNotFound);
	CHECK(not_found.text_body().find("&lt;bad&gt;&amp;&quot;&#39;") != std::string_view::npos);

	auto bad = conflux::http::Response::bad_request("<bad>&\"'");
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.text_body().find("&lt;bad&gt;&amp;&quot;&#39;") != std::string_view::npos);

	auto internal = conflux::http::Response::internal_error("<boom>");
	CHECK(internal.status == kHttpInternalServerError);
	CHECK(internal.text_body().find("&lt;boom&gt;") != std::string_view::npos);
}

TEST_CASE(
	"http response: set_cookie and append_vary preserve HTTP header semantics",
	"[http.response]") {
	conflux::http::Response resp;
	resp.set_cookie("session", "abc", "Path=/; HttpOnly");
	resp.set_cookie("theme", "dark");
	resp.set_cookie(
		chttp::CookieBuilder{"remember", "yes"}
			.path("/")
			.http_only()
			.secure()
			.same_site(chttp::SameSite::Lax)
			.max_age(std::chrono::hours{1}));
	CHECK(resp.set_cookies.size() == 3);
	CHECK(resp.set_cookies[0] == "session=abc; Path=/; HttpOnly");
	CHECK(resp.set_cookies[1] == "theme=dark");
	CHECK(resp.set_cookies[2] == "remember=yes; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=3600");

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
	conflux::http::Response resp = conflux::http::Response::text("hello");
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
		auto resp = conflux::http::Response::html("<p>ok</p>", 201, "Created");
		CHECK(resp.status == 201);
		CHECK(resp.status_text == "Created");
		CHECK(resp.content_type == "text/html; charset=utf-8");
		CHECK(resp.text_body() == "<p>ok</p>");
	}
	{
		auto resp = conflux::http::Response::json("{\"ok\":true}", 202, "Accepted");
		CHECK(resp.status == 202);
		CHECK(resp.status_text == "Accepted");
		CHECK(resp.content_type == "application/json");
		CHECK(resp.text_body() == "{\"ok\":true}");
	}
	{
		auto resp = conflux::http::Response::redirect("/next", kHttpPermanentRedirect);
		CHECK(resp.status == kHttpPermanentRedirect);
		CHECK(resp.status_text == "Permanent Redirect");
		CHECK(resp.headers["location"] == "/next");
	}
	{
		auto resp = conflux::http::Response::method_not_allowed({"GET", "POST"});
		CHECK(resp.status == kHttpMethodNotAllowed);
		CHECK(resp.headers["allow"] == "GET, POST");
	}
	{
		auto resp = conflux::http::Response::unauthorized("Basic realm=\"test\"");
		CHECK(resp.status == kHttpUnauthorized);
		CHECK(resp.headers["www-authenticate"] == "Basic realm=\"test\"");
	}
	{
		auto resp = conflux::http::Response::forbidden("<no>");
		CHECK(resp.status == kHttpForbidden);
		CHECK(resp.status_text == "Forbidden");
		CHECK(resp.text_body().find("&lt;no&gt;") != std::string_view::npos);
	}
	{
		auto resp = conflux::http::Response::unprocessable_entity("bad field");
		CHECK(resp.status == kHttpUnprocessableEntity);
		CHECK(resp.status_text == "Unprocessable Entity");
		CHECK(resp.text_body().find("bad field") != std::string_view::npos);
	}
	{
		auto resp = conflux::http::Response::uri_too_long();
		CHECK(resp.status == kHttpUriTooLong);
		CHECK(resp.status_text == "URI Too Long");
	}
	{
		auto resp = conflux::http::Response::header_fields_too_large();
		CHECK(resp.status == kHttpRequestHeaderFieldsTooLarge);
		CHECK(resp.status_text == "Request Header Fields Too Large");
	}
	{
		auto resp = conflux::http::Response::bad_gateway("upstream failed");
		CHECK(resp.status == kHttpBadGateway);
		CHECK(resp.text_body() == "upstream failed");
	}
	{
		auto resp = conflux::http::Response::content_too_large();
		CHECK(resp.status == kHttpRequestEntityTooLarge);
		CHECK(resp.status_text == "Content Too Large");
	}
	{
		auto resp = conflux::http::Response::no_content();
		CHECK(resp.status == kHttpNoContent);
		CHECK(resp.status_text == "No Content");
	}
	{
		auto resp = conflux::http::Response::not_modified(R"("abc")");
		CHECK(resp.status == kHttpNotModified);
		CHECK(resp.status_text == "Not Modified");
		CHECK(resp.headers["etag"] == R"("abc")");
	}
	{
		auto ch = std::make_shared<conflux::http::SseChannel>();
		auto resp = conflux::http::Response::sse(ch);
		CHECK(resp.status == kHttpOk);
		CHECK(resp.content_type == "text/event-stream");
		CHECK(resp.is_sse());
		CHECK(resp.sse_channel_ptr() == ch);
	}
	{
		auto deferred = std::make_shared<conflux::http::DeferredResponse>(std::chrono::milliseconds{10000});
		auto resp = conflux::http::Response::deferred(deferred);
		CHECK(resp.is_deferred());
		CHECK(resp.deferred_response_ptr() == deferred);
	}
}

TEST_CASE(
	"http response: non-text body setters, accessors, and take helpers preserve variant boundaries",
	"[http.response]") {
	conflux::http::Response resp;
	CHECK(resp.is_text());
	CHECK(resp.text_body().empty());

	auto sse = std::make_shared<conflux::http::SseChannel>();
	resp.set_sse_channel(sse);
	CHECK(resp.is_sse());
	CHECK_FALSE(resp.is_text());
	CHECK(resp.sse_channel_ptr() == sse);
	CHECK(resp.content_length() == 0);
	CHECK(resp.take_sse_channel() == sse);
	CHECK_FALSE(resp.sse_channel_ptr());

	auto close_observed = std::make_shared<conflux::http::SseChannel>();
	int closed = 0;
	close_observed->on_close([&] { ++closed; });
	close_observed->close();
	close_observed->close();
	CHECK(closed == 1);
	close_observed->on_close([&] { ++closed; });
	CHECK(closed == 2);

	auto drop_newest = std::make_shared<conflux::http::SseChannel>(4, conflux::http::SseOverflowPolicy::DropNewest);
	CHECK(drop_newest->overflow_policy_vocabulary() == conflux::http::OverflowPolicy::drop_newest);
	CHECK(drop_newest->send("1234"));
	CHECK_FALSE(drop_newest->send("5"));
	CHECK(drop_newest->pressure_metrics().dropped_newest == 1);

	conflux::http::SseChannel view_channel;
	std::string frame = "data: before\n\n";
	CHECK(view_channel.send_view(frame));
	frame.assign("data: after\n\n");
	CHECK(view_channel.drain() == "data: before\n\n");

	auto drop_oldest = std::make_shared<conflux::http::SseChannel>(4, conflux::http::SseOverflowPolicy::DropOldest);
	CHECK(drop_oldest->overflow_policy_vocabulary() == conflux::http::OverflowPolicy::drop_oldest);
	CHECK(drop_oldest->send("1234"));
	CHECK(drop_oldest->send("5"));
	CHECK(drop_oldest->pressure_metrics().dropped_oldest == 1);

	auto disconnect = std::make_shared<conflux::http::SseChannel>(
		4,
		conflux::http::sse_overflow_policy(conflux::http::OverflowPolicy::close_connection));
	CHECK(disconnect->send("1234"));
	CHECK_FALSE(disconnect->send("5"));
	CHECK(disconnect->is_closed());
	CHECK(disconnect->pressure_metrics().disconnected_for_pressure == 1);

	auto ws = std::make_shared<conflux::http::WsUpgrade>();
	ws->accept_key = "accept";
	resp.set_ws_upgrade(ws);
	CHECK(resp.is_ws_upgrade());
	CHECK(resp.ws_upgrade_ptr() == ws);
	CHECK(resp.take_ws_upgrade() == ws);
	CHECK_FALSE(resp.ws_upgrade_ptr());

	int sockets[2]{};
	REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	{
		conflux::http::WsConn conn{sockets[0]};
		int closed = 0;
		conn.on_close([&] { ++closed; });
		CHECK(conn.send_text("hello"));
		auto bytes = std::string_view{"bytes"};
		CHECK(conn.send_binary(std::as_bytes(std::span{bytes.data(), bytes.size()})));
		CHECK(conn.send_ping("p"));
		conn.close();
		conn.close();
		CHECK(closed == 1);
		conn.on_close([&] { ++closed; });
		CHECK(closed == 2);
	}
	::close(sockets[1]);

	auto mapped = std::make_shared<conflux::file_map::MappedBody>();
	mapped->offset = 3;
	mapped->size = 42;
	resp.set_mapped_file(mapped);
	CHECK(resp.is_mapped_file());
	CHECK(resp.mapped_file_ptr() == mapped);
	CHECK(resp.content_length() == 42);
	CHECK(resp.take_mapped_file() == mapped);
	CHECK_FALSE(resp.mapped_file_ptr());

	auto streamed = std::make_shared<chttp::StreamedFile>();
	streamed->send_offset = 5;
	streamed->send_size = 77;
	streamed->total_size = 100;
	resp.set_streamed_file(streamed);
	CHECK(resp.is_streamed_file());
	CHECK(resp.streamed_file_ptr() == streamed);
	CHECK(resp.content_length() == 77);
	CHECK(resp.take_streamed_file() == streamed);
	CHECK_FALSE(resp.streamed_file_ptr());

	auto deferred = std::make_shared<conflux::http::DeferredResponse>(std::chrono::milliseconds{10000});
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
	auto streamed = std::make_shared<chttp::StreamedFile>();
	std::vector<chttp::StreamedFileResult> observed;
	streamed->on_complete([&](chttp::StreamedFileResult result) { observed.push_back(result); });

	streamed->notify_complete();
	streamed->notify_failed();
	streamed->on_complete([&](chttp::StreamedFileResult result) { observed.push_back(result); });

	REQUIRE(observed.size() == 2);
	CHECK(observed[0] == chttp::StreamedFileResult::completed);
	CHECK(observed[1] == chttp::StreamedFileResult::completed);
}

TEST_CASE(
	"http response: conflux::http::DeferredResponse complete wakes eventfd once and stores first response",
	"[http.response]") {
	conflux::http::DeferredResponse deferred{std::chrono::milliseconds{10000}};
	make_eventfd_nonblocking(deferred.eventfd_fd());

	deferred.complete(conflux::http::Response::text("first"));
	deferred.complete(conflux::http::Response::text("second"));

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
	"http response: conflux::http::DeferredResponse deadline expiry wakes eventfd and materializes gateway timeout",
	"[http.response]") {
	conflux::http::DeferredResponse deferred{std::chrono::milliseconds{10000}};
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

TEST_CASE(
	"http response: conflux::http::DeferredResponse cancellation methods propagate reasons",
	"[http.response]") {
	namespace root = conflux::work::root;

	auto [deadline_ctl, deadline_src] = root::make_task_control_source<conflux::http::Response>();
	conflux::http::DeferredResponse deadline{std::chrono::milliseconds{10000}};
	deadline.attach_cancel(deadline_ctl);
	deadline.cancel_deadline();
	CHECK(deadline_ctl.cancel_requested());
	REQUIRE(deadline_ctl.cancellation_reason().has_value());
	CHECK(*deadline_ctl.cancellation_reason() == root::CancelReason::deadline);
	(void)deadline_src.try_set_cancelled(root::CancelReason::deadline);

	auto [disconnect_ctl, disconnect_src] = root::make_task_control_source<conflux::http::Response>();
	conflux::http::DeferredResponse disconnect{std::chrono::milliseconds{10000}};
	disconnect.attach_cancel(disconnect_ctl);
	disconnect.cancel_disconnect();
	CHECK(disconnect_ctl.cancel_requested());
	REQUIRE(disconnect_ctl.cancellation_reason().has_value());
	CHECK(*disconnect_ctl.cancellation_reason() == root::CancelReason::requested);
	(void)disconnect_src.try_set_cancelled(root::CancelReason::requested);

	auto [shutdown_ctl, shutdown_src] = root::make_task_control_source<conflux::http::Response>();
	conflux::http::DeferredResponse shutdown{std::chrono::milliseconds{10000}};
	shutdown.attach_cancel(shutdown_ctl);
	shutdown.cancel_shutdown();
	CHECK(shutdown_ctl.cancel_requested());
	REQUIRE(shutdown_ctl.cancellation_reason().has_value());
	CHECK(*shutdown_ctl.cancellation_reason() == root::CancelReason::shutdown);
	(void)shutdown_src.try_set_cancelled(root::CancelReason::shutdown);
}

TEST_CASE(
	"http response: conflux::http::DeferredResponse deadline expiry cancels attached task with deadline reason",
	"[http.response]") {
	namespace root = conflux::work::root;

	auto [ctl, src] = root::make_task_control_source<conflux::http::Response>();
	conflux::http::DeferredResponse deferred{std::chrono::milliseconds{10000}};
	deferred.attach_cancel(ctl);

	auto const now = std::chrono::steady_clock::now();
	deferred.set_deadline(now - std::chrono::milliseconds{1});
	REQUIRE(deferred.expire_if_past_deadline(now));
	CHECK(ctl.cancel_requested());
	REQUIRE(ctl.cancellation_reason().has_value());
	CHECK(*ctl.cancellation_reason() == root::CancelReason::deadline);
	(void)src.try_set_cancelled(root::CancelReason::deadline);
}

TEST_CASE(
	"http response: conflux::http::DeferredResponse deadline expiry invokes attached task cancel hook",
	"[http.response]") {
	namespace root = conflux::work::root;

	std::atomic<int> observed{-1};
	auto [task, src] =
		root::make_cancellable_task_source<conflux::http::Response>([&observed](root::CancelReason reason) noexcept {
			observed.store(static_cast<int>(reason), std::memory_order_release);
		});
	conflux::http::DeferredResponse deferred{std::chrono::milliseconds{10000}};
	deferred.attach_cancel(task.control());

	auto const now = std::chrono::steady_clock::now();
	deferred.set_deadline(now - std::chrono::milliseconds{1});
	REQUIRE(deferred.expire_if_past_deadline(now));
	CHECK(observed.load(std::memory_order_acquire) == static_cast<int>(root::CancelReason::deadline));
	(void)src.try_set_cancelled(root::CancelReason::deadline);
}

TEST_CASE(
	"http response: conflux::http::DeferredResponse cancellation forwards through cancellable wrapper",
	"[http.response]") {
	namespace root = conflux::work::root;

	std::atomic<int> observed{-1};
	auto [child, child_src] =
		root::make_cancellable_task_source<conflux::http::Response>([&observed](root::CancelReason reason) noexcept {
			observed.store(static_cast<int>(reason), std::memory_order_release);
		});
	auto wrapper = root::make_cancellable_task(
		[child = std::move(child)](root::Cancellation cancel) mutable -> root::Task<conflux::http::Response> {
			(void)cancel;
			return std::move(child);
		});
	conflux::http::DeferredResponse deferred{std::chrono::milliseconds{10000}};
	deferred.attach_cancel(wrapper.control());

	auto const now = std::chrono::steady_clock::now();
	deferred.set_deadline(now - std::chrono::milliseconds{1});
	REQUIRE(deferred.expire_if_past_deadline(now));
	CHECK(observed.load(std::memory_order_acquire) == static_cast<int>(root::CancelReason::deadline));
	(void)child_src.try_set_cancelled(root::CancelReason::deadline);
}
