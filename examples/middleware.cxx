// Middleware-focused example.
// Shows request IDs, tracing, auth, and request handlers.
//
// Build and run: build/debug-clang-libcxx/conflux_middleware
// Try:
//   curl http://localhost:9094/
//   curl http://localhost:9094/public/ping
//   curl -u demo:demo http://localhost:9094/private/profile
//   curl -H 'Authorization: Bearer valid-token' http://localhost:9094/private/token
import conflux;
import std;

struct PingReply {
	std::string status;
	std::string request_id;
	std::string traceparent;
};

template<>
struct JsonMembers<PingReply> {
	static constexpr auto members() {
		return std::tuple{
			json_member("status", &PingReply::status),
			json_member("request_id", &PingReply::request_id),
			json_member("traceparent", &PingReply::traceparent),
		};
	}
};

struct ProfileReply {
	std::string user;
	std::string request_id;
	std::string remote_addr;
};

template<>
struct JsonMembers<ProfileReply> {
	static constexpr auto members() {
		return std::tuple{
			json_member("user", &ProfileReply::user),
			json_member("request_id", &ProfileReply::request_id),
			json_member("remote_addr", &ProfileReply::remote_addr),
		};
	}
};

struct TokenReply {
	std::string token;
	std::string request_id;
};

template<>
struct JsonMembers<TokenReply> {
	static constexpr auto members() {
		return std::tuple{
			json_member("token", &TokenReply::token),
			json_member("request_id", &TokenReply::request_id),
		};
	}
};

int main() {
	namespace http = conflux::http;
	auto app = http::app();

	app.use(request_id_middleware());
	app.use(tracing_middleware({.propagate_in_response = true}));
	app.use(
		[](http::Request const &req,
		   http::RequestContext const &ctx,
		   http::AsyncNext const &next) -> http::Task<http::Response> {
			auto response = co_await next(req, ctx);
			response.headers.set("x-middleware-model", "async-owned");
			co_return response;
		});

	app.get("/", [](http::Request const &) {
		return http::html(
			"<html><body>"
			"<h1>conflux middleware example</h1>"
			"<ul>"
			"<li><a href='/public/ping'>/public/ping</a></li>"
			"<li><a href='/private/profile'>/private/profile</a> (basic auth: demo/demo)</li>"
			"<li><a href='/private/token'>/private/token</a> (bearer token: valid-token)</li>"
			"</ul>"
			"</body></html>");
	});

	app.get("/public/ping", [](http::RequestId request_id, http::TraceContext trace) {
		return http::json(
			PingReply{
				.status = "ok",
				.request_id = std::string{request_id.get()},
				.traceparent = std::string{trace.traceparent}});
	});

	app.group("/private", [](auto &g) {
		g.use(basic_auth_middleware(
			[](std::string_view user, std::string_view pass) { return user == "demo" && pass == "demo"; }));

		(void)g.get("/profile", [](http::Request const &req) {
			return http::json(
				ProfileReply{
					.user = "demo",
					.request_id = std::string{req.header("x-request-id")},
					.remote_addr = req.remote_addr});
		});
	});

	app.group("/private", [](auto &g) {
		g.use(bearer_auth_middleware([](std::string_view token) { return token == "valid-token"; }));

		(void)g.get("/token", [](http::Request const &req) {
			return http::json(TokenReply{.token = "accepted", .request_id = std::string{req.header("x-request-id")}});
		});
	});

	auto const status = http::run(std::move(app), {.port = 9094});
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
