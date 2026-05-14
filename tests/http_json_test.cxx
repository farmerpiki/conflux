#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.json.boundary;
import conflux.net.http.response_json;
import conflux.net.http.native_json;

namespace jb = conflux::json::boundary;
namespace hj = conflux::http::json;

namespace {

struct StreamingPayload {
	int value{};
};

struct StreamingOnlyProvider {
	static expected<void, jb::Error> write_json(
		StreamingPayload const &payload,
		jb::DumpOptions const &,
		auto &&sink) {
		sink(SV{R"({"value":)"});
		auto encoded = std::to_string(payload.value);
		sink(SV{encoded});
		sink(SV{"}"});
		return {};
	}
};

struct FailingPayload {};

struct FailingProvider {
	static expected<void, jb::Error> write_json(
		FailingPayload const &,
		jb::DumpOptions const &,
		auto &&) {
		return unexpected(jb::Error{
			.stage = jb::ErrorStage::dump,
			.code = jb::ErrorCode::provider_failure,
			.message = "forced failure",
		});
	}
};

} // namespace

TEST_CASE(
	"http json: route response helper serializes native provider values",
	"[http.json]") {
	auto resp = hj::try_response(static_cast<i64>(42), {.status = kHttpCreated, .status_text = "Created"});
	REQUIRE(resp.has_value());
	CHECK(resp->status == kHttpCreated);
	CHECK(resp->status_text == "Created");
	CHECK(resp->content_type == "application/json");
	CHECK(resp->text_body() == "42");
}

TEST_CASE(
	"http json: response writer accepts direct chunk providers",
	"[http.json]") {
	auto resp = hj::try_response_with<StreamingOnlyProvider>(StreamingPayload{.value = 7});
	REQUIRE(resp.has_value());
	CHECK(resp->status == kHttpOk);
	CHECK(resp->content_type == "application/json");
	CHECK(resp->text_body() == R"({"value":7})");
}

TEST_CASE(
	"http json: explicit error path preserves provider-neutral dump errors",
	"[http.json]") {
	auto failed = hj::try_response_with<FailingProvider>(FailingPayload{});
	REQUIRE_FALSE(failed.has_value());
	CHECK(failed.error().stage == jb::ErrorStage::dump);
	CHECK(failed.error().code == jb::ErrorCode::provider_failure);
	CHECK(failed.error().message == "forced failure");

	auto fallback = hj::response_or_internal_error_with<FailingProvider>(FailingPayload{});
	CHECK(fallback.status == kHttpInternalServerError);
	CHECK(fallback.content_type == "application/json");
	CHECK(fallback.text_body() == R"({"error":"json serialization failed"})");
}

TEST_CASE(
	"http json: native convenience can encode and decode request bodies",
	"[http.json]") {
	auto req = HttpRequest::post("https://example.test/api");
	hj::set_body(req, static_cast<i64>(99));
	auto built = move(req).build();
	CHECK(built.headers().value_or("Content-Type") == jb::kContentType);
	CHECK(built.body() == "99");

	auto decoded = hj::decode_body<i64>(built, {.copy_input = false});
	REQUIRE(decoded.has_value());
	CHECK(*decoded == 99);
}
