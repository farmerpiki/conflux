// Plain TU for consistency with the HTTP E2E test sources.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.http.extended;
import conflux.net.app;
import conflux.net.config;
import conflux.net.http.realtime;
import conflux.net.http.static_files;
import conflux.net.router;
import conflux.tests.support;
import conflux.work;

namespace {
namespace chttp = conflux::http;

conflux::work::Task<void> short_async_test_delay() {
	auto task_source = conflux::work::root::make_task_source<int>();
	auto gate = std::move(std::get<0>(task_source));
	auto source = std::move(std::get<1>(task_source));
	std::thread([source = std::move(source)] mutable {
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
		auto _ = source.try_set_value(conflux::work::root::Success<int>{0});
	}).detach();
	auto _ = co_await std::move(gate);
}

conflux::work::Task<chttp::Response> async_body_text_echo(
	chttp::BodyText const &body) {
	co_await short_async_test_delay();
	co_return chttp::text(body.get());
}

conflux::work::Task<chttp::CreatedBody<std::string>> async_body_text_created(
	chttp::BodyText const &body) {
	co_await short_async_test_delay();
	co_return chttp::created(chttp::Json{std::string{body.get()}});
}

conflux::work::Task<chttp::Response> async_request_view_echo(
	chttp::RequestView req) {
	co_await short_async_test_delay();
	co_return chttp::text(std::format("{}:{}", req.header("x-check"), req.body));
}

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

struct TempTreeCleanup {
	std::filesystem::path root;

	~TempTreeCleanup() {
		std::error_code ec;
		auto _ = std::filesystem::remove_all(root, ec);
	}
};

[[nodiscard]] std::filesystem::path unique_upload_temp_root() {
	return std::filesystem::temp_directory_path()
		 / std::format("conflux-upload-save-{}", std::chrono::steady_clock::now().time_since_epoch().count());
}

[[nodiscard]] std::string read_text_file(
	std::filesystem::path const &path) {
	std::ifstream in{path};
	return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE(
	"http app: try_server constructs server without throwing",
	"[http][app]") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
}

TEST_CASE(
	"http app: facade forwards websocket and static route registration",
	"[http][app]") {
	auto app = chttp::App::default_server();
	auto &ws_app = app.ws("/ws", [](conflux::http::RequestView const &, conflux::http::WsConn &) {});
	CHECK(&ws_app == &app);
	auto &sse_app =
		app.sse("/events", [](conflux::http::RequestView const &, std::shared_ptr<conflux::http::SseChannel> const &) {
		});
	CHECK(&sse_app == &app);
	auto &static_app = app.serve_static("/assets", std::filesystem::temp_directory_path().string());
	CHECK(&static_app == &app);

	auto routes = app.routes();
	REQUIRE(routes.size() == 2);
	CHECK(routes[0].method == "GET");
	CHECK(routes[0].path == "/ws");
	CHECK(routes[0].handler_kind == "ws");
	CHECK(routes[1].method == "GET");
	CHECK(routes[1].path == "/events");
	CHECK(routes[1].handler_kind == "sse");
}

TEST_CASE(
	"http app: generic route registration and introspection stay on facade",
	"[http][app]") {
	auto app = chttp::App::default_server();
	auto &added = app.add("REPORT", "/reports/{id}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["id"]});
	});
	CHECK(&added == &app);

	auto &ctx_added = app.add_context(
		"POST",
		"/jobs/{id}",
		[](conflux::http::RequestView const &,
		   chttp::RequestContext const &) -> conflux::work::root::Task<conflux::http::Response> {
			auto [task, source] = conflux::work::root::make_task_source<conflux::http::Response>();
			(void)source.try_set_value(
				conflux::work::root::Success<conflux::http::Response>{conflux::http::Response::text("queued")});
			return std::move(task);
		});
	CHECK(&ctx_added == &app);
	CHECK(chttp::router(app).has_context_routes());

	auto infos = chttp::route_infos(app);
	REQUIRE(infos.size() == 2);
	CHECK(infos[0].method == "REPORT");
	CHECK(infos[0].path_pattern == "/reports/{id}");
	REQUIRE(infos[0].path_params.size() == 1);
	CHECK(infos[0].path_params[0] == "id");
	CHECK(infos[1].method == "POST");
	CHECK(infos[1].path_pattern == "/jobs/{id}");
	REQUIRE(infos[1].path_params.size() == 1);
	CHECK(infos[1].path_params[0] == "id");

	auto routes = app.routes();
	REQUIRE(routes.size() == 2);
	CHECK(routes[0].path == "/reports/{id}");
	CHECK(routes[0].handler_kind == "app");
	CHECK(routes[1].path == "/jobs/{id}");
	CHECK(routes[1].handler_kind == "context");
}

TEST_CASE(
	"async extracted body reference survives suspension") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/async-body-ref", async_body_text_echo);
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_post_on((*server)->port(), "/async-body-ref", "text/plain", "borrowed-body");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "borrowed-body");
}

TEST_CASE(
	"async extracted non-response task keeps extractor references alive") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/async-created-ref", async_body_text_created);
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_post_on((*server)->port(), "/async-created-ref", "text/plain", "created-body");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 201 Created"));
	REQUIRE(extract_body(resp) == "\"created-body\"");
}

TEST_CASE(
	"async request view handler survives suspension") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/async-request-view", async_request_view_echo);
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_request_on(
		(*server)->port(),
		"POST",
		"/async-request-view",
		"text/plain",
		"borrowed-body",
		"X-Check: alive\r\n"
		"Connection: close\r\n");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "alive:borrowed-body");
}

TEST_CASE(
	"upload body handlers see streaming body instead of buffered request body") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post(
		"/stream-upload",
		[](chttp::RequestView req, chttp::UploadBody body) -> conflux::work::Task<chttp::Response> {
			std::string payload;
			while (true) {
				auto read = co_await body.read();
				if (!read) {
					co_return chttp::upload_error_response(read.error());
				}
				if (!*read) {
					break;
				}
				payload += (*read)->text_view();
			}
			co_return chttp::text(
				std::format("{}:{}:{}:{}", req.body.empty(), req.form.size(), req.files.size(), payload));
		});
	REQUIRE(app.validate().ok());
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp =
		conflux::tests::http_post_on((*server)->port(), "/stream-upload", "application/x-www-form-urlencoded", "a=b");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "true:0:0:a=b");
}

TEST_CASE(
	"upload body handler starts before HTTP/1 content-length body completes") {
	auto handler_started = std::make_shared<std::atomic_bool>(false);
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/stream-upload", [handler_started](chttp::UploadBody body) -> conflux::work::Task<chttp::Response> {
		handler_started->store(true, std::memory_order_release);
		std::string payload;
		while (true) {
			auto read = co_await body.read();
			if (!read) {
				co_return chttp::upload_error_response(read.error());
			}
			if (!*read) {
				break;
			}
			payload += (*read)->text_view();
		}
		co_return chttp::text(payload);
	});
	REQUIRE(app.validate().ok());
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { auto _ = srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	conflux::tests::LocalTcpClient client{(*server)->port()};
	auto sent = client.send(
		"POST /stream-upload HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Length: 6\r\n"
		"Connection: close\r\n\r\n");
	REQUIRE(sent > 0);
	auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
	while (!handler_started->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds{5});
	}
	REQUIRE(handler_started->load(std::memory_order_acquire));
	sent = client.send("abcdef");
	REQUIRE(sent == 6);
	auto resp = client.read_until_close();
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	auto _ = report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "abcdef");
}

TEST_CASE(
	"upload body handler streams HTTP/1 chunked request body") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/stream-upload", [](chttp::UploadBody body) -> conflux::work::Task<chttp::Response> {
		std::string payload;
		while (true) {
			auto read = co_await body.read();
			if (!read) {
				co_return chttp::upload_error_response(read.error());
			}
			if (!*read) {
				break;
			}
			payload += (*read)->text_view();
		}
		co_return chttp::text(payload);
	});
	REQUIRE(app.validate().ok());
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { auto _ = srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	conflux::tests::LocalTcpClient client{(*server)->port()};
	auto sent = client.send(
		"POST /stream-upload HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Transfer-Encoding: chunked\r\n"
		"Connection: close\r\n\r\n"
		"2\r\nab\r\n");
	REQUIRE(sent > 0);
	sent = client.send("4\r\ncdef\r\n0\r\n\r\n");
	REQUIRE(sent > 0);
	auto resp = client.read_until_close();
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	auto _ = report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "abcdef");
}

TEST_CASE(
	"upload body handler early return cancels HTTP/1 upload and closes connection") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/stream-upload", [](chttp::UploadBody) -> conflux::work::Task<chttp::Response> {
		co_return chttp::text("done");
	});
	REQUIRE(app.validate().ok());
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { auto _ = srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	conflux::tests::LocalTcpClient client{(*server)->port()};
	auto sent = client.send(
		"POST /stream-upload HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Length: 100\r\n"
		"Connection: keep-alive\r\n\r\n");
	REQUIRE(sent > 0);
	auto resp = client.read_until_close();
	auto metrics = (*server)->metrics();
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	auto _ = report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Connection: close\r\n") != std::string::npos);
	REQUIRE(extract_body(resp) == "done");
	CHECK(metrics.uploads.canceled_by_handler == 1);
}

TEST_CASE(
	"upload body handler sends HTTP/1 100 Continue before streaming body") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/stream-upload", [](chttp::UploadBody body) -> conflux::work::Task<chttp::Response> {
		std::string payload;
		while (true) {
			auto read = co_await body.read();
			if (!read) {
				co_return chttp::upload_error_response(read.error());
			}
			if (!*read) {
				break;
			}
			payload += (*read)->text_view();
		}
		co_return chttp::text(payload);
	});
	REQUIRE(app.validate().ok());
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { auto _ = srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	conflux::tests::LocalTcpClient client{(*server)->port()};
	client.set_recv_timeout(std::chrono::seconds{5});
	std::string_view const body = "abcdef";
	auto sent = client.send(
		std::format(
			"POST /stream-upload HTTP/1.1\r\n"
			"Host: localhost\r\n"
			"Content-Length: {}\r\n"
			"Expect: 100-continue\r\n"
			"Connection: close\r\n\r\n",
			body.size()));
	REQUIRE(sent > 0);
	auto interim = client.read_headers();
	REQUIRE(interim.starts_with("HTTP/1.1 100 Continue"));
	sent = client.send(body);
	REQUIRE(sent == static_cast<ssize_t>(body.size()));
	auto resp = client.read_until_close();
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	auto _ = report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == body);
}

TEST_CASE(
	"upload body save_to creates parent directories and writes bounded chunks") {
	TempTreeCleanup cleanup{.root = unique_upload_temp_root()};
	auto path = cleanup.root / "nested" / "payload.txt";
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/save", [path](chttp::UploadBody body) -> conflux::work::Task<chttp::Response> {
		auto saved = co_await body.save_to(
			path,
			chttp::UploadSaveOptions{.overwrite = false, .create_parent_dirs = true, .buffer_size = 2});
		if (!saved) {
			co_return chttp::upload_error_response(saved.error());
		}
		co_return chttp::text(std::format("{}", saved->bytes_written));
	});
	REQUIRE(app.validate().ok());
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { auto _ = srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_post_on((*server)->port(), "/save", "application/octet-stream", "abcdef");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	auto _ = report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "6");
	REQUIRE(std::filesystem::exists(path));
	REQUIRE(read_text_file(path) == "abcdef");
}

TEST_CASE(
	"upload body save_to refuses overwrite without touching existing file") {
	TempTreeCleanup cleanup{.root = unique_upload_temp_root()};
	REQUIRE(std::filesystem::create_directories(cleanup.root));
	auto path = cleanup.root / "payload.txt";
	{
		std::ofstream out{path};
		out << "original";
	}
	REQUIRE(read_text_file(path) == "original");

	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/save", [path](chttp::UploadBody body) -> conflux::work::Task<chttp::Response> {
		auto saved = co_await body.save_to(path, chttp::UploadSaveOptions{.overwrite = false});
		if (!saved) {
			co_return chttp::upload_error_response(saved.error());
		}
		co_return chttp::text(std::format("{}", saved->bytes_written));
	});
	REQUIRE(app.validate().ok());
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { auto _ = srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_post_on((*server)->port(), "/save", "application/octet-stream", "new");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	auto _ = report;
	REQUIRE(resp.starts_with("HTTP/1.1 500 Internal Server Error"));
	REQUIRE(read_text_file(path) == "original");
}

TEST_CASE(
	"upload body save_to max_bytes removes partial file") {
	TempTreeCleanup cleanup{.root = unique_upload_temp_root()};
	auto path = cleanup.root / "nested" / "payload.txt";

	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/save", [path](chttp::UploadBody body) -> conflux::work::Task<chttp::Response> {
		auto saved = co_await body.save_to(
			path,
			chttp::UploadSaveOptions{.create_parent_dirs = true, .max_bytes = 3, .buffer_size = 2});
		if (!saved) {
			co_return chttp::upload_error_response(saved.error());
		}
		co_return chttp::text(std::format("{}", saved->bytes_written));
	});
	REQUIRE(app.validate().ok());
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { auto _ = srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_post_on((*server)->port(), "/save", "application/octet-stream", "abcdef");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	auto _ = report;
	REQUIRE(resp.starts_with("HTTP/1.1 413 "));
	REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE(
	"upload body metrics track HTTP/1 streaming lifecycle") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.post("/stream-upload", [](chttp::UploadBody body) -> conflux::work::Task<chttp::Response> {
		std::uint64_t total{};
		while (true) {
			auto read = co_await body.read();
			if (!read) {
				co_return chttp::upload_error_response(read.error());
			}
			if (!*read) {
				break;
			}
			total += (*read)->bytes().size();
		}
		co_return chttp::text(std::format("{}", total));
	});
	REQUIRE(app.validate().ok());
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { auto _ = srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_post_on((*server)->port(), "/stream-upload", "application/octet-stream", "abcdef");
	auto metrics = (*server)->metrics();
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	auto _ = report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "6");
	CHECK(metrics.uploads.streams_started == 1);
	CHECK(metrics.uploads.bytes_received == 6);
	CHECK(metrics.uploads.bytes_consumed == 6);
}

TEST_CASE(
	"async context route timeout returns gateway timeout and cancels handler with deadline") {
	namespace root = conflux::work::root;

	std::atomic<int> observed_reason{-1};
	auto cfg = chttp::Config::public_server();
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 64;
	cfg.startup_banner = false;
	conflux::http::Router router;
	auto timeout = std::make_shared<std::chrono::milliseconds>(25);
	router.add_context_with_timeout(
		"POST",
		"/async-timeout",
		timeout,
		[&observed_reason](conflux::http::RequestView const &, chttp::RequestContext const &)
			-> conflux::work::Task<chttp::Response> {
			auto source_slot = std::make_shared<std::optional<root::TaskSource<chttp::Response>>>();
			auto [task, source] = root::make_cancellable_task_source<chttp::Response>(
				[&observed_reason, source_slot](root::CancelReason reason) noexcept {
					observed_reason.store(static_cast<int>(reason), std::memory_order_release);
					if (*source_slot) {
						auto source = std::move(**source_slot);
						source_slot->reset();
						(void)source.try_set_cancelled(reason);
					}
				});
			source_slot->emplace(std::move(source));
			return std::move(task);
		});
	auto port = conflux::tests::test_servers().start(cfg, std::move(router));
	auto resp = conflux::tests::http_post_on(port, "/async-timeout", "text/plain", "body");
	REQUIRE(resp.starts_with("HTTP/1.1 504 Gateway Timeout"));
	for (int i = 0; i != 50 && observed_reason.load(std::memory_order_acquire) == -1; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
	}
	CHECK(observed_reason.load(std::memory_order_acquire) == static_cast<int>(root::CancelReason::deadline));
}

TEST_CASE(
	"async middleware request view survives suspension") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.use(
		[](chttp::RequestView req,
		   chttp::RequestContext const &ctx,
		   auto const &next) -> conflux::work::Task<chttp::Response> {
			auto task_source = conflux::work::root::make_task_source<int>();
			auto gate = std::move(std::get<0>(task_source));
			auto source = std::move(std::get<1>(task_source));
			std::thread([source = std::move(source)] mutable {
				std::this_thread::sleep_for(std::chrono::milliseconds{10});
				(void)source.try_set_value(conflux::work::root::Success<int>{0});
			}).detach();
			(void)co_await std::move(gate);
			auto response = co_await next(req, ctx);
			response.headers.set("x-async-middleware-view", std::string{req.header("x-check")});
			co_return response;
		});
	app.post("/async-middleware-view", [](chttp::BodyText const &body) { return chttp::text(body.get()); });
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_request_on(
		(*server)->port(),
		"POST",
		"/async-middleware-view",
		"text/plain",
		"borrowed-body",
		"X-Check: alive\r\n"
		"Connection: close\r\n");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("x-async-middleware-view: alive") != std::string::npos);
	REQUIRE(extract_body(resp) == "borrowed-body");
}

TEST_CASE(
	"sync middleware protects ordinary routes when async middleware is installed") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.use([](chttp::RequestView const &req, auto const &next) {
		if (req.header("authorization") != "Bearer ok") {
			return chttp::Response::unauthorized("Bearer");
		}
		return next(req);
	});
	app.use(
		[](chttp::RequestView const &req,
		   chttp::RequestContext const &ctx,
		   auto const &next) -> conflux::work::Task<chttp::Response> { co_return co_await next(req, ctx); });
	app.get("/sync-secret", [] { return chttp::text("secret"); });
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto denied = conflux::tests::http_get_on((*server)->port(), "/sync-secret");
	auto allowed = conflux::tests::http_request_on(
		(*server)->port(),
		"GET",
		"/sync-secret",
		"text/plain",
		"",
		"Authorization: Bearer ok\r\n"
		"Connection: close\r\n");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(denied.starts_with("HTTP/1.1 401 Unauthorized"));
	REQUIRE(allowed.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(allowed) == "secret");
}

TEST_CASE(
	"sync middleware protects async fixed typed routes") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.use([](chttp::RequestView const &, auto const &) { return chttp::Response::unauthorized("Bearer"); });
	app.get<"/async-fixed-secret">([]() -> conflux::work::Task<chttp::Response> { co_return chttp::text("secret"); });
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_get_on((*server)->port(), "/async-fixed-secret");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 401 Unauthorized"));
}

TEST_CASE(
	"async group middleware protects sync group routes") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.group("/admin", [](auto &group) {
		group.use(
			[](chttp::RequestView const &, chttp::RequestContext const &, auto const &)
				-> conflux::work::Task<chttp::Response> { co_return chttp::Response::unauthorized("Bearer"); });
		group.get("/stats", [] { return chttp::text("secret"); });
	});
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_get_on((*server)->port(), "/admin/stats");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 401 Unauthorized"));
}

TEST_CASE(
	"async group middleware protects sync extracted group routes") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.group("/admin", [](auto &group) {
		group.use(
			[](chttp::RequestView const &, chttp::RequestContext const &, auto const &)
				-> conflux::work::Task<chttp::Response> { co_return chttp::Response::unauthorized("Bearer"); });
		group.post("/echo", [](chttp::BodyText const &body) { return chttp::text(body.get()); });
	});
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_post_on((*server)->port(), "/admin/echo", "text/plain", "secret");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 401 Unauthorized"));
}

TEST_CASE(
	"sync group middleware protects async context group routes") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.group("/admin", [](auto &group) {
		group.use([](chttp::RequestView const &, auto const &) { return chttp::Response::unauthorized("Bearer"); });
		group.get(
			"/context",
			[](chttp::RequestView const &, chttp::RequestContext const &) -> conflux::work::Task<chttp::Response> {
				co_return chttp::text("secret");
			});
	});
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_get_on((*server)->port(), "/admin/context");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 401 Unauthorized"));
}

TEST_CASE(
	"sync group middleware protects async extracted group routes") {
	auto app = chttp::App::default_server();
	app.config().rings = 1;
	app.config().ring_entries = 64;
	app.config().startup_banner = false;
	app.group("/admin", [](auto &group) {
		group.use([](chttp::RequestView const &, auto const &) { return chttp::Response::unauthorized("Bearer"); });
		group.post("/echo", [](chttp::BodyText const &body) -> conflux::work::Task<chttp::Response> {
			co_return chttp::text(body.get());
		});
	});
	auto server = std::move(app).try_server({.port = 0});
	REQUIRE(server.has_value());
	std::thread thread{[srv = server->get()] { (void)srv->run(); }};
	conflux::tests::wait_for_server((*server)->port());
	auto resp = conflux::tests::http_post_on((*server)->port(), "/admin/echo", "text/plain", "secret");
	auto report = (*server)->drain();
	if (thread.joinable()) {
		thread.join();
	}
	(void)report;
	REQUIRE(resp.starts_with("HTTP/1.1 401 Unauthorized"));
}
