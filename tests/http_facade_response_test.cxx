#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.http;
import conflux.http.extended;
import conflux.json;

namespace http = conflux::http;
using namespace conflux::http;

static_assert(std::same_as<decltype(std::declval<http::BodyBytes const &>().get()), std::span<std::byte const>>);
static_assert(std::same_as<decltype(std::declval<http::BodyBytes const &>().text_view()), std::string_view>);

struct FacadeAnswer {
	std::string value;
};

template<>
struct conflux::json::JsonMembers<FacadeAnswer> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("value", &FacadeAnswer::value),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeAnswer"; }
};

TEST_CASE(
	"http facade: buffered stream helper writes buffered response bodies",
	"[http.facade]") {
	auto response = http::buffered_stream(
		[](http::StreamSink &out) {
			out.write("alpha");
			out.write(":");
			out.write("beta");
		},
		"text/plain; charset=utf-8");

	CHECK(response.status == kHttpOk);
	CHECK(response.content_type == "text/plain; charset=utf-8");
	CHECK(response.text_body() == "alpha:beta");

	auto explicit_buffered = http::buffered_stream([](http::StreamSink &out) { out.write("buffered"); }, "text/plain");
	CHECK(explicit_buffered.content_type == "text/plain");
	CHECK(explicit_buffered.text_body() == "buffered");
}

TEST_CASE(
	"http facade: first-contact response helpers expose status shortcuts and builder cookies",
	"[http.facade]") {
	auto bad = http::bad_request("bad input");
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.text_body().find("bad input") != std::string::npos);

	auto missing = http::not_found("/missing");
	CHECK(missing.status == kHttpNotFound);
	CHECK(missing.text_body().find("/missing") != std::string::npos);

	CHECK(http::unauthorized("Bearer").headers["WWW-Authenticate"] == "Bearer");
	CHECK(http::forbidden("nope").status == kHttpForbidden);
	CHECK(http::method_not_allowed({"GET", "POST"}).headers["Allow"] == "GET, POST");
	CHECK(http::unprocessable_entity("invalid").status == kHttpUnprocessableEntity);
	CHECK(http::internal_error("boom").status == kHttpInternalServerError);
	CHECK(http::not_modified(R"("etag")").headers["ETag"] == R"("etag")");
	CHECK(http::content_too_large().status == kHttpRequestEntityTooLarge);
	CHECK(http::bad_gateway("upstream").text_body() == "upstream");
	CHECK(http::gateway_timeout().status == kHttpGatewayTimeout);

	auto created = http::into_response(
		http::created(FacadeAnswer{.value = "ok"})
			.location("/answers/1")
			.cookie(http::cookie("session", "abc").path("/").http_only()));
	CHECK(created.status == kHttpCreated);
	CHECK(created.headers["Location"] == "/answers/1");
	REQUIRE_FALSE(created.set_cookies.empty());
	CHECK(created.set_cookies.front().find("session=abc") != std::string::npos);
}

TEST_CASE(
	"http facade: response helpers cover redirect and created JSON",
	"[http.facade]") {
	auto redirect = http::redirect("/next");
	CHECK(redirect.status == kHttpFound);
	CHECK(redirect.headers["Location"] == "/next");

	auto created = http::into_response(
		http::created(FacadeAnswer{.value = "made"}).location("/answers/1").content_type("application/vnd.test+json"));
	CHECK(created.status == kHttpCreated);
	CHECK(created.content_type == "application/vnd.test+json");
	CHECK(created.headers["Location"] == "/answers/1");
	CHECK(created.text_body() == R"({"value":"made"})");

	auto cookie = http::cookie("session", "abc").path("/").http_only().same_site(http::SameSite::Strict);
	conflux::http::Response response;
	response.set_cookie(std::move(cookie));
	REQUIRE(response.set_cookies.size() == 1);
	CHECK(response.set_cookies[0] == "session=abc; Path=/; HttpOnly; SameSite=Strict");
}

TEST_CASE(
	"http facade: response builders work on lvalues",
	"[http.facade]") {
	auto created = http::created(FacadeAnswer{.value = "made"});
	auto &same = created.header("X-Test", "yes").location("/answers/1").content_type("application/vnd.test+json");
	static_assert(std::same_as<decltype(same), http::CreatedBody<FacadeAnswer> &>);

	auto response = http::into_response(std::move(created));
	CHECK(response.status == kHttpCreated);
	CHECK(response.content_type == "application/vnd.test+json");
	CHECK(response.headers["X-Test"] == "yes");
	CHECK(response.headers["Location"] == "/answers/1");
	CHECK(response.text_body() == R"({"value":"made"})");
}

TEST_CASE(
	"http facade: owned response helpers move caller bodies",
	"[http.facade]") {
	auto text_body = std::string{"owned text"};
	auto text = http::text(std::move(text_body));
	CHECK(text.text_body() == "owned text");

	auto html_body = std::string{"<p>owned</p>"};
	auto html = http::html(std::move(html_body));
	CHECK(html.content_type == "text/html; charset=utf-8");
	CHECK(html.text_body() == "<p>owned</p>");

	auto created_body = std::string{"created"};
	auto created = http::created(std::move(created_body), std::string{"text/custom"});
	CHECK(created.status == kHttpCreated);
	CHECK(created.content_type == "text/custom");
	CHECK(created.text_body() == "created");

	CHECK(http::owned_text(std::string{"alias"}).text_body() == "alias");
	CHECK(http::owned_html(std::string{"<b>alias</b>"}).text_body() == "<b>alias</b>");
	CHECK(http::owned_created(std::string{"alias"}).status == kHttpCreated);
}

TEST_CASE(
	"http facade: file helper reads small files",
	"[http.facade]") {
	struct Cleanup {
		std::filesystem::path path;
		~Cleanup() {
			std::error_code ec;
			auto _ = std::filesystem::remove(path, ec);
		}
	};
	Cleanup file{
		.path =
			std::filesystem::temp_directory_path() / std::format("conflux_http_facade_file_helper_{}.txt", ::getpid())};
	auto const &path = file.path;
	{
		std::ofstream out{path, std::ios::binary};
		out << "file-body";
	}

	auto response = http::blocking_file_response(path, "text/plain");
	CHECK(response.status == kHttpOk);
	CHECK(response.content_type == "text/plain");
	CHECK(response.text_body() == "file-body");
}

TEST_CASE(
	"http facade: problem helpers carry code/detail metadata",
	"[http.facade]") {
	auto problem = http::problem::bad_request("invalid_todo", "title is required");
	CHECK(problem.code == "invalid_todo");
	CHECK(problem.detail == "title is required");
	CHECK(problem.response.status == kHttpBadRequest);
	CHECK(problem.response.content_type == "application/problem+json");
	CHECK(
		problem.response.text_body()
		== R"({"type":"about:blank","title":"Bad Request","status":400,"detail":"title is required","code":"invalid_todo"})");

	auto rich = http::problem::bad_request("invalid_user", "invalid user")
					.type_uri("https://example.test/problems/invalid-user")
					.instance_uri("/users")
					.extension("trace_id", "abc")
					.field("email", "is required");
	rich.rebuild();
	CHECK(
		rich.response.text_body()
		== R"({"type":"https://example.test/problems/invalid-user","title":"Bad Request","status":400,"detail":"invalid user","instance":"/users","code":"invalid_user","trace_id":"abc","fields":{"email":"is required"}})");
	auto converted =
		http::into_response(http::problem::bad_request("invalid_user", "invalid user").field("name", "is required"));
	CHECK(
		converted.text_body()
		== R"({"type":"about:blank","title":"Bad Request","status":400,"detail":"invalid user","code":"invalid_user","fields":{"name":"is required"}})");

	CHECK(http::problem::not_found("missing", "not found").response.status == kHttpNotFound);
	CHECK(http::problem::unauthorized("login_required", "sign in").response.status == kHttpUnauthorized);
	CHECK(http::problem::forbidden("forbidden", "no access").response.status == kHttpForbidden);
	CHECK(http::problem::unprocessable_entity("invalid_entity", "invalid").response.status == kHttpUnprocessableEntity);
	CHECK(http::problem::content_too_large().response.status == kHttpRequestEntityTooLarge);
	CHECK(http::problem::uri_too_long().response.status == kHttpUriTooLong);
	CHECK(http::problem::header_fields_too_large().response.status == kHttpRequestHeaderFieldsTooLarge);
	CHECK(http::problem::gateway_timeout().response.status == kHttpGatewayTimeout);
	CHECK(http::problem::internal_error("internal", "failed").response.status == kHttpInternalServerError);
}
