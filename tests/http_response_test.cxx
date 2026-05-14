#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.response;

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
	"http response: status factories set expected status, text, and headers",
	"[http.response]") {
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
		auto resp = HttpResponse::bad_gateway("upstream failed");
		CHECK(resp.status == kHttpBadGateway);
		CHECK(resp.text_body() == "upstream failed");
	}
}
