#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.router;
import conflux.tests.support;
import conflux.work;

using namespace conflux::tests;
namespace chttp = conflux::http;

namespace {

std::uint16_t g_form_port = 0;

void ensure_form_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		auto cfg = mw_config();
		conflux::http::Router router;
		router.get("/api/ping", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"status":"ok"})");
		});
		router.post("/api/echo-form", [](conflux::http::OwnedRequest const &req) {
			auto v = req.form["field"];
			if (v.empty()) {
				return conflux::http::Response::not_found("field");
			}
			return conflux::http::Response::text(std::string{v});
		});
		router.post("/api/multipart-field", [](conflux::http::OwnedRequest const &req) {
			auto v = req.form["field"];
			if (v.empty()) {
				return conflux::http::Response::not_found("field");
			}
			return conflux::http::Response::text(std::string{v});
		});
		router.post("/api/multipart-file", [](conflux::http::RequestView const &req) {
			if (req.files.empty()) {
				return conflux::http::Response::not_found("file");
			}
			auto const &f = req.files[0];
			return conflux::http::Response::json(
				std::format(
					R"({{"name":"{}","filename":"{}","content_type":"{}","size":{}}})",
					f.name,
					f.filename,
					f.content_type,
					f.data.size()));
		});
		router.post("/api/multipart-counts", [](conflux::http::RequestView const &req) {
			return conflux::http::Response::json(
				std::format(R"({{"fields":{},"files":{}}})", req.form.values("field").size(), req.files.size()));
		});
		router.add_context(
			"POST",
			"/api/multipart-file-async",
			[](conflux::http::RequestView req,
			   chttp::RequestContext const &) -> conflux::work::root::Task<conflux::http::Response> {
				auto task_source = conflux::work::root::make_task_source<int>();
				auto gate = std::move(std::get<0>(task_source));
				auto source = std::move(std::get<1>(task_source));
				std::thread([source = std::move(source)] mutable {
					std::this_thread::sleep_for(std::chrono::milliseconds{10});
					auto _ = source.try_set_value(conflux::work::root::Success<int>{0});
				}).detach();
				auto _ = co_await std::move(gate);
				if (req.files.empty()) {
					co_return conflux::http::Response::not_found("file");
				}
				auto const &f = req.files[0];
				co_return conflux::http::Response::json(
					std::format(
						R"({{"name":"{}","filename":"{}","content_type":"{}","data":"{}"}})",
						f.name,
						f.filename,
						f.content_type,
						f.data));
			});
		g_form_port = test_servers().start(cfg, std::move(router));
	});
}

std::string form_post(
	std::string_view path,
	std::string_view content_type,
	std::string_view body) {
	ensure_form_server();
	return conflux::tests::http_post_on(g_form_port, path, content_type, body);
}

std::string make_multipart_text(
	std::string_view boundary,
	std::string_view name,
	std::string_view field_value) {
	return std::format(
		"--{}\r\n"
		"Content-Disposition: form-data; name=\"{}\"\r\n"
		"\r\n"
		"{}\r\n"
		"--{}--\r\n",
		boundary,
		name,
		field_value,
		boundary);
}

std::string make_multipart_file(
	std::string_view boundary,
	std::string_view name,
	std::string_view filename,
	std::string_view content_type,
	std::string_view data) {
	return std::format(
		"--{}\r\n"
		"Content-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\n"
		"Content-Type: {}\r\n"
		"\r\n"
		"{}\r\n"
		"--{}--\r\n",
		boundary,
		name,
		filename,
		content_type,
		data,
		boundary);
}

std::string make_multipart_text_and_file(
	std::string_view boundary,
	std::string_view field_name,
	std::string_view field_value,
	std::string_view file_name,
	std::string_view filename,
	std::string_view content_type,
	std::string_view data) {
	return std::format(
		"--{}\r\n"
		"Content-Disposition: form-data; name=\"{}\"\r\n"
		"\r\n"
		"{}\r\n"
		"--{}\r\n"
		"Content-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\n"
		"Content-Type: {}\r\n"
		"\r\n"
		"{}\r\n"
		"--{}--\r\n",
		boundary,
		field_name,
		field_value,
		boundary,
		file_name,
		filename,
		content_type,
		data,
		boundary);
}

std::string_view response_body(
	std::string_view response) {
	auto hdr_end = response.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string_view::npos);
	return response.substr(hdr_end + 4);
}

} // namespace

TEST_CASE(
	"urlencoded form field is parsed") {
	auto resp = form_post("/api/echo-form", "application/x-www-form-urlencoded", "field=hello");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "hello");
}

TEST_CASE(
	"urlencoded form field is percent-decoded") {
	auto resp = form_post("/api/echo-form", "application/x-www-form-urlencoded", "field=hello%20world");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "hello world");
}

TEST_CASE(
	"urlencoded form with multiple fields parses target field") {
	auto resp = form_post("/api/echo-form", "application/x-www-form-urlencoded", "other=x&field=target&more=y");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "target");
}

TEST_CASE(
	"non-urlencoded POST does not populate form") {
	auto resp = form_post("/api/echo-form", "text/plain", "field=hello");
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}

TEST_CASE(
	"percent-encoded UTF-8 in urlencoded form field is decoded correctly") {
	auto resp = form_post(
		"/api/echo-form",
		"application/x-www-form-urlencoded",
		"field=%E3%81%93%E3%82%93%E3%81%AB%E3%81%A1%E3%81%AF");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "こんにちは");
}

TEST_CASE(
	"multipart/form-data text field is parsed into req.form") {
	auto body = make_multipart_text("boundary123", "field", "hello from multipart");
	auto ct = std::format("multipart/form-data; boundary=boundary123");
	auto resp = form_post("/api/multipart-field", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "hello from multipart");
}

TEST_CASE(
	"multipart/form-data value with special characters is preserved") {
	auto body = make_multipart_text("bnd42", "field", "a=1&b=2 <> \"quotes\"");
	auto ct = std::string{"multipart/form-data; boundary=bnd42"};
	auto resp = form_post("/api/multipart-field", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "a=1&b=2 <> \"quotes\"");
}

TEST_CASE(
	"multipart/form-data file part populates req.files") {
	auto body = make_multipart_file("fileBnd", "upload", "hello.txt", "text/plain", "file content here");
	auto ct = std::string{"multipart/form-data; boundary=fileBnd"};
	auto resp = form_post("/api/multipart-file", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto json = response_body(resp);
	REQUIRE(json.find("\"name\":\"upload\"") != std::string_view::npos);
	REQUIRE(json.find("\"filename\":\"hello.txt\"") != std::string_view::npos);
	REQUIRE(json.find("\"content_type\":\"text/plain\"") != std::string_view::npos);
	REQUIRE(json.find("\"size\":17") != std::string_view::npos);
}

TEST_CASE(
	"multipart/form-data quoted semicolon filename is preserved") {
	auto body = make_multipart_file("semiBnd", "upload", "hello;semi.txt", "text/plain", "file content here");
	auto ct = std::string{"multipart/form-data; boundary=semiBnd"};
	auto resp = form_post("/api/multipart-file", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto json = response_body(resp);
	REQUIRE(json.find("\"filename\":\"hello;semi.txt\"") != std::string_view::npos);
}

TEST_CASE(
	"multipart/form-data file part survives async suspension") {
	auto body = make_multipart_file("asyncBnd", "upload", "async.txt", "text/plain", "async file content");
	auto resp = form_post("/api/multipart-file-async", "multipart/form-data; boundary=asyncBnd", body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto json = response_body(resp);
	REQUIRE(json.find("\"name\":\"upload\"") != std::string_view::npos);
	REQUIRE(json.find("\"filename\":\"async.txt\"") != std::string_view::npos);
	REQUIRE(json.find("\"content_type\":\"text/plain\"") != std::string_view::npos);
	REQUIRE(json.find("\"data\":\"async file content\"") != std::string_view::npos);
}

TEST_CASE(
	"async request buffer cut preserves pipelined follow-up request") {
	ensure_form_server();
	LocalTcpClient client{g_form_port};
	client.set_recv_timeout(std::chrono::seconds{5});

	auto body = make_multipart_file("pipeBnd", "upload", "pipe.txt", "text/plain", "pipelined file");
	auto req = std::format(
		"POST /api/multipart-file-async HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Content-Type: multipart/form-data; boundary=pipeBnd\r\n"
		"Content-Length: {}\r\n"
		"\r\n"
		"{}"
		"GET /api/ping HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Connection: close\r\n"
		"\r\n",
		body.size(),
		body);
	auto _ = client.send(req);
	auto combined = client.read_until_close();
	auto first_pos = combined.find("HTTP/1.1 200 OK");
	REQUIRE(first_pos != std::string::npos);
	auto second_pos = combined.find("HTTP/1.1 200 OK", first_pos + 1);
	REQUIRE(second_pos != std::string::npos);
	auto first = combined.substr(first_pos, second_pos - first_pos);
	auto second = combined.substr(second_pos);
	REQUIRE(response_body(first).find("\"data\":\"pipelined file\"") != std::string_view::npos);
	REQUIRE(response_body(second) == R"({"status":"ok"})");
}

TEST_CASE(
	"multipart/form-data parses each part exactly once") {
	auto body =
		make_multipart_text_and_file("countBnd", "field", "value", "upload", "hello.txt", "text/plain", "file content");
	auto resp = form_post("/api/multipart-counts", "multipart/form-data; boundary=countBnd", body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == R"({"fields":1,"files":1})");
}

TEST_CASE(
	"multipart/form-data delimiter text inside file content is preserved") {
	std::string const data = "before --fileBnd after";
	auto body = make_multipart_file("fileBnd", "upload", "hello.txt", "text/plain", data);
	auto ct = std::string{"multipart/form-data; boundary=fileBnd"};
	auto resp = form_post("/api/multipart-file", ct, body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto json = response_body(resp);
	REQUIRE(json.find(std::format("\"size\":{}", data.size())) != std::string_view::npos);
}

TEST_CASE(
	"multipart/form-data part header without space after colon is parsed") {
	std::string const body =
		"--bNoSpace\r\n"
		"Content-Disposition:form-data; name=\"field\"\r\n"
		"\r\n"
		"hello\r\n"
		"--bNoSpace--\r\n";
	auto resp = form_post("/api/multipart-field", "multipart/form-data; boundary=bNoSpace", body);
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "hello");
}

TEST_CASE(
	"multipart/form-data without boundary returns 404 (field not parsed)") {
	auto body = make_multipart_text("bnd", "field", "ignored");
	auto resp = form_post("/api/multipart-field", "multipart/form-data", body);
	REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
}
