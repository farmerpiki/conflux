// Plain TU — helper API behavior does not need an E2E server.
#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http.response;
import conflux.net.http.realtime;
import conflux.net.http_server_helpers;

TEST_CASE(
	"http_server_helpers: header name validation follows HTTP token grammar",
	"[http_server_helpers]") {
	CHECK(is_valid_header_name("X-conflux::http::OwnedRequest-ID"));
	CHECK(is_valid_header_name("!#$%&'*+-.^_`|~09AZaz"));
	CHECK_FALSE(is_valid_header_name(""));
	CHECK_FALSE(is_valid_header_name("Bad Header"));
	CHECK_FALSE(is_valid_header_name("Bad:Header"));
	CHECK_FALSE(is_valid_header_name(std::string_view{"\xC3\xA9", 2}));
}

TEST_CASE(
	"http_server_helpers: format_response filters invalid/framing headers and cookies",
	"[http_server_helpers]") {
	conflux::http::Response resp = conflux::http::Response::text("hello");
	resp.status_text = std::string{"OK\r\nInjected: bad"};
	resp.headers["X-Good"] = "yes";
	resp.headers["Bad Header"] = "dropped";
	resp.headers["X-Bad-Value"] = std::string{"bad\x7F"};
	resp.headers["Content-Length"] = "999";
	resp.headers["Connection"] = "upgrade";
	resp.set_cookies.push_back("sid=1; Path=/");
	resp.set_cookies.push_back(std::string{"bad=1\x7F"});

	auto wire = format_response(resp, "h3=\":443\"", true);
	CHECK(wire.find("HTTP/1.1 200 \r\n") == 0);
	CHECK(wire.find("Content-Length: 5\r\n") != std::string::npos);
	CHECK(wire.find("X-Good: yes\r\n") != std::string::npos);
	CHECK(wire.find("Set-Cookie: sid=1; Path=/\r\n") != std::string::npos);
	CHECK(wire.find("Alt-Svc: h3=\":443\"\r\n") != std::string::npos);
	CHECK(wire.find("Connection: close\r\n\r\nhello") != std::string::npos);
	CHECK(wire.find("Bad Header") == std::string::npos);
	CHECK(wire.find("X-Bad-Value") == std::string::npos);
	CHECK(wire.find("Content-Length: 999") == std::string::npos);
	CHECK(wire.find("Connection: upgrade") == std::string::npos);
	CHECK(wire.find("bad=1") == std::string::npos);
	CHECK(wire.find("Injected") == std::string::npos);
}

TEST_CASE(
	"http_server_helpers: format_response emits WebSocket upgrade handshake",
	"[http_server_helpers]") {
	auto up = std::make_shared<conflux::http::WsUpgrade>();
	up->accept_key = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

	conflux::http::Response resp;
	resp.set_ws_upgrade(up);
	resp.headers["X-Ignored"] = "must not be serialized";
	resp.set_text_body("body ignored by upgrade");
	resp.set_ws_upgrade(up);

	auto wire = format_response(resp, "h3=\":443\"", true);
	CHECK(wire ==
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n");
}

TEST_CASE(
	"http_server_helpers: format_response suppresses bodies where HTTP forbids them",
	"[http_server_helpers]") {
	conflux::http::Response no_content = conflux::http::Response::text("body");
	no_content.status = 204;
	no_content.status_text = "No Content";
	CHECK(format_response(no_content).find("body") == std::string::npos);
	CHECK(format_response(no_content).find("Content-Length") == std::string::npos);

	conflux::http::Response not_modified;
	not_modified.status = 304;
	not_modified.status_text = "Not Modified";
	not_modified.content_length_hint = 123;
	CHECK(format_response(not_modified).find("Content-Length: 123\r\n") != std::string::npos);

	conflux::http::Response head = conflux::http::Response::text("body");
	head.head_only = true;
	auto wire = format_response(head);
	CHECK(wire.find("Content-Length: 4\r\n") != std::string::npos);
	CHECK(wire.ends_with("\r\n\r\n"));
}

TEST_CASE(
	"http_server_helpers: small formatting helpers are deterministic",
	"[http_server_helpers]") {
	CHECK(format_sse_headers(false).find("Connection: keep-alive\r\n") != std::string::npos);
	CHECK(format_sse_headers(true).find("Connection: close\r\n") != std::string::npos);
	CHECK(format_http_chunk("hello") == "5\r\nhello\r\n");
	CHECK(format_http_chunk("") == "0\r\n\r\n");
}

TEST_CASE(
	"http_server_helpers: header parameter extraction handles quoted and token values",
	"[http_server_helpers]") {
	std::string_view header = R"(form-data; name="upload"; filename="a b.txt")";
	CHECK(extract_param(header, "name") == "upload");
	CHECK(extract_param(header, "filename") == "a b.txt");
	CHECK(extract_param("attachment; filename=plain.txt; size=3", "filename") == "plain.txt");
	CHECK(extract_param("attachment; filename=unterminated", "filename") == "unterminated");
	CHECK(extract_param("attachment; filename", "filename").empty());
	CHECK(extract_param(header, "missing").empty());
	CHECK(extract_param("multipart/form-data; boundary=abc123", "boundary") == "abc123");
	CHECK(extract_param("form-data; filename=only-file.txt", "name").empty());
	CHECK(extract_param("form-data; x-name=wrong; name=right", "name") == "right");
	CHECK(extract_param("form-data; NAME=upper", "name") == "upper");
	CHECK(extract_param(R"(form-data; name="upload"; filename="a;b.txt")", "filename") == "a;b.txt");
	CHECK(extract_param(R"(multipart/form-data; boundary="abc;123"; charset=utf-8)", "boundary") == "abc;123");
}

TEST_CASE(
	"http types: header_items split item params with quoted semicolon support",
	"[http_server_helpers]") {
	auto items = conflux::http::header_items(R"(gzip;q=0.5; note="a;b", br; q=1)");
	auto it = items.begin();
	REQUIRE(it != items.end());
	auto gzip = *it;
	CHECK(gzip.name == "gzip");
	CHECK_FALSE(gzip.has_value);
	CHECK(conflux::http::parse_http_q(gzip.params) == 0.5F);

	++it;
	REQUIRE(it != items.end());
	auto br = *it;
	CHECK(br.name == "br");
	CHECK_FALSE(br.has_value);
	CHECK(conflux::http::parse_http_q(br.params) == 1.0F);

	++it;
	CHECK(it == items.end());

	auto directives = conflux::http::header_items("max-age=60, no-cache");
	auto directive_it = directives.begin();
	REQUIRE(directive_it != directives.end());
	auto max_age = *directive_it;
	CHECK(max_age.name == "max-age");
	CHECK(max_age.has_value);
	CHECK(max_age.value == "60");

	++directive_it;
	REQUIRE(directive_it != directives.end());
	auto no_cache = *directive_it;
	CHECK(no_cache.name == "no-cache");
	CHECK_FALSE(no_cache.has_value);
}

TEST_CASE(
	"http_server_helpers: cookies parse repeated and valueless entries",
	"[http_server_helpers]") {
	HttpFieldsView cookies;
	parse_cookies("a=1; b=two words ; flag; a=2", cookies);

	REQUIRE(cookies.size() == 4);
	CHECK(cookies.values("a").size() == 2);
	CHECK(cookies.values("a")[0] == "1");
	CHECK(cookies.values("a")[1] == "2");
	CHECK(cookies["b"] == "two words");
	CHECK(cookies["flag"].empty());
}

TEST_CASE(
	"http_server_helpers: cookie parser trims RFC optional whitespace",
	"[http_server_helpers]") {
	HttpFieldsView cookies;
	parse_cookies("\t sid = abc \t;\t csrf_token = tok \t; ;\tflag\t", cookies);

	REQUIRE(cookies.size() == 3);
	CHECK(cookies["sid"] == "abc");
	CHECK(cookies["csrf_token"] == "tok");
	CHECK(cookies["flag"].empty());
	CHECK(cookies["\tsid"].empty());
}

TEST_CASE(
	"http_server_helpers: connection tokens are comma split and case-insensitive",
	"[http_server_helpers]") {
	HttpFieldsView headers{true};
	headers.emplace_back("Connection", "keep-alive, Upgrade");
	headers.emplace_back("connection", "x-custom");

	CHECK(has_connection_token(headers, "upgrade"));
	CHECK(has_connection_token(headers, "KEEP-ALIVE"));
	CHECK(has_connection_token(headers, "x-custom"));
	CHECK_FALSE(has_connection_token(headers, "close"));
}

TEST_CASE(
	"http_server_helpers: expect and transfer-encoding validation reject ambiguous framing",
	"[http_server_helpers]") {
	HttpFieldsView headers{true};
	CHECK(parse_expect_header(headers) == ExpectState::none);

	headers.emplace_back("Expect", "100-continue");
	CHECK(parse_expect_header(headers) == ExpectState::continue_100);
	headers.emplace_back("Expect", "other-token");
	CHECK(parse_expect_header(headers) == ExpectState::unsupported);

	HttpFieldsView chunked{true};
	chunked.emplace_back("Transfer-Encoding", "chunked");
	CHECK(has_valid_chunked_transfer_encoding(chunked));

	HttpFieldsView double_chunked{true};
	double_chunked.emplace_back("Transfer-Encoding", "chunked, chunked");
	CHECK_FALSE(has_valid_chunked_transfer_encoding(double_chunked));

	HttpFieldsView identity{true};
	identity.emplace_back("Transfer-Encoding", "identity");
	CHECK_FALSE(has_valid_chunked_transfer_encoding(identity));

	HttpFieldsView empty_token{true};
	empty_token.emplace_back("Transfer-Encoding", "chunked,");
	CHECK_FALSE(has_valid_chunked_transfer_encoding(empty_token));
}

TEST_CASE(
	"http_server_helpers: urlencoded parsing borrows plain fields and owns decoded fields",
	"[http_server_helpers]") {
	HttpFieldsView fields;
	parse_urlencoded("plain=value&spaced=a+b&encoded=%7Bok%7D&flag", fields);

	REQUIRE(fields.size() == 4);
	CHECK(fields["plain"] == "value");
	CHECK(fields["spaced"] == "a b");
	CHECK(fields["encoded"] == "{ok}");
	CHECK(fields["flag"].empty());
}

TEST_CASE(
	"http_server_helpers: multipart parser captures text fields and uploaded files",
	"[http_server_helpers]") {
	static constexpr std::string_view body =
		"--AaB03x\r\n"
		"Content-Disposition: form-data; name=\"title\"\r\n"
		"\r\n"
		"Report\r\n"
		"--AaB03x\r\n"
		"Content-Disposition: form-data; name=\"upload\"; filename=\"a.txt\"\r\n"
		"Content-Type: text/plain\r\n"
		"\r\n"
		"file-body\r\n"
		"--AaB03x--\r\n";

	HttpFieldsView form;
	std::vector<conflux::http::UploadedFile> files;
	parse_multipart(body, "AaB03x", form, files);

	REQUIRE(form.size() == 1);
	CHECK(form["title"] == "Report");
	REQUIRE(files.size() == 1);
	CHECK(files[0].name == "upload");
	CHECK(files[0].filename == "a.txt");
	CHECK(files[0].content_type == "text/plain");
	CHECK(files[0].data == "file-body");
}

TEST_CASE(
	"http_server_helpers: complete and incremental chunked decoders agree",
	"[http_server_helpers]") {
	std::string body;
	auto consumed = decode_chunked("4;ext=1\r\nWiki\r\n5\r\npedia\r\n0\r\nTrailer: ok\r\n\r\nextra", 64, 8, body);
	REQUIRE(consumed > 0);
	CHECK(body == "Wikipedia");

	std::string raw = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n";
	std::size_t const start = raw.find("4\r\n");
	REQUIRE(start != std::string::npos);
	ChunkedDecodeState st;
	CHECK(decode_chunked_incremental(raw, start, 64, 8, st) == 0);
	CHECK(st.body == "Wiki");
	raw += "5\r\npedia\r\n0\r\n\r\n";
	auto inc_consumed = decode_chunked_incremental(raw, start, 64, 8, st);
	REQUIRE(inc_consumed > 0);
	CHECK(st.body == "Wikipedia");
	CHECK(static_cast<std::size_t>(inc_consumed) == raw.size() - start);
}

TEST_CASE(
	"http_server_helpers: chunked decoder reports incomplete, malformed, and too-large bodies",
	"[http_server_helpers]") {
	std::string body;
	CHECK(decode_chunked("4\r\nWi", 64, 8, body) == 0);
	CHECK(decode_chunked("x\r\nnope\r\n", 64, 8, body) == -1);
	CHECK(decode_chunked("5\r\nhello\r\n0\r\n\r\n", 4, 8, body) == -2);

	ChunkedDecodeState st;
	CHECK(decode_chunked_incremental("5\r\nhello\r\n0\r\n\r\n", 0, 4, 8, st) == -2);
}
