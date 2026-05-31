// HTTP policy stack example: CORS, forwarding, IP filtering, redirects,
// request IDs, tracing, security headers, cache policy, response cache, ETag,
// structured logs, CSRF protection, and protected OpenAPI.
//
// Build and run: build/release-clang-libcxx/conflux_http_policy_stack_example
// Try:
//   curl -i http://localhost:9100/
//   curl -i http://localhost:9100/old-dashboard
//   curl -i -H 'Origin: https://app.example' http://localhost:9100/v2/users
//   curl -i -H 'Authorization: Bearer docs-token' http://localhost:9100/openapi.json
//   curl -i http://localhost:9100/form
#include <unistd.h>

import conflux.net.http.server;
import conflux.http.extended;
import conflux.types;
import std;

namespace {

struct TempFile {
	std::string path;
	explicit TempFile(
		std::filesystem::path p)
		: path{std::move(p).string()} {}
	~TempFile() {
		if (!path.empty()) {
			::unlink(path.c_str());
		}
	}
};

} // namespace

int main() {
	namespace http = conflux::http;

	auto app = http::App::default_server();
	TempFile access_log{
		std::filesystem::temp_directory_path() / std::format("conflux_policy_stack_access_{}.jsonl", ::getpid())};
	auto const &log_path = access_log.path;

	// Register broad request boundary policy first: first-registered middleware is
	// the outer wrapper, so CORS preflight can short-circuit before auth/CSRF.
	app.use(cors_middleware({
		.allowed_origins = {"https://app.example"},
		.allowed_methods = {"GET", "POST", "OPTIONS"},
		.allowed_headers = {"Content-Type", "Authorization", "X-CSRF-Token"},
		.expose_headers = {"X-Request-Id", "Traceparent", "ETag"},
		.allow_credentials = true,
	}));
	app.use(forwarded_middleware({
		.trusted_proxies = {"127.0.0.0/8", "::1/128"},
		.strict_mode = true,
	}));
	app.use(ip_filter_middleware({
		.mode = IpFilterMode::blocklist,
		.cidrs = {"203.0.113.0/24"},
	}));

	app.use(request_id_middleware());
	app.use(tracing_middleware({.propagate_in_response = true}));
	app.use(security_headers_middleware({
		.hsts_max_age = 0,
		.csp = "default-src 'self'; frame-ancestors 'none'",
		.hsts_only_on_tls = false,
	}));
	app.use(redirect_middleware({
		.rules =
			{
					{.from = "/old-dashboard", .to = "/dashboard", .status = 308},
					{.from = "/v1/", .to = "/v2/", .status = 307, .prefix_match = true},
					},
	}));
	app.use(trailing_slash_middleware({.mode = TrailingSlashMode::remove, .redirect_status = 308}));
	app.use(cookie_signing_middleware({.secrets = http::single_secret_rotation("0123456789abcdef")}));
	app.use(csrf_middleware({.cookie_attrs = "Path=/; SameSite=Strict"}));
	app.use(etag_middleware({.weak = true}));
	app.use(response_cache_middleware({
		.max_entries = 64,
		.max_bytes = 512 * 1024,
		.default_ttl = std::chrono::seconds{15},
	}));
	app.use(cache_control_middleware({
		.rules =
			{
					{.mime_prefix = "application/json", .directive = "no-store"},
					{.mime_prefix = "text/html", .directive = "no-cache"},
					},
		.default_directive = "max-age=60, public",
	}));
	app.use(structured_log_middleware({.log_file = log_path, .app_name = "policy-example"}));

	app.get("/", [](Request const &) {
		return Response::html(
			"<html><body><h1>policy stack</h1>"
			"<ul>"
			"<li><a href='/dashboard'>/dashboard</a></li>"
			"<li><a href='/v2/users'>/v2/users</a></li>"
			"<li><a href='/form'>/form</a></li>"
			"<li><a href='/login'>/login</a> then <a href='/me'>/me</a></li>"
			"<li>/openapi.json requires Authorization: Bearer docs-token</li>"
			"</ul></body></html>");
	});

	app.get("/dashboard", [](Request const &req) {
		return Response::text(
			format("dashboard request_id={} trace={}\n", req.headers["x-request-id"], req.headers["traceparent"]));
	});

	app.get("/v2/users", [](Request const &req) {
		return Response::json(
			std::format(
				R"({{"users":["ada","linus"],"remote":"{}","request_id":"{}"}})",
				req.remote_addr,
				req.headers["x-request-id"]));
	});

	app.get("/form", [](Request const &req) {
		return Response::html(
			std::format(
				"<html><body><h1>CSRF form</h1>"
				"<form method='post' action='/submit'>"
				"<input type='hidden' name='csrf_token' value='{}'>"
				"<input name='value' value='example'>"
				"<button>Submit</button>"
				"</form></body></html>",
				req.cookies["csrf_token"]));
	});

	app.get("/login", [](Request const &) {
		auto resp = Response::text("signed session cookie set; try /me\n");
		resp.set_cookie("session", sign_cookie("demo-user", "0123456789abcdef"), "Path=/; HttpOnly; SameSite=Lax");
		return resp;
	});

	app.get("/me", [](Request const &req) {
		auto user = req.cookies["session"];
		return user.empty() ? Response::unauthorized("Session") :
							  Response::text(std::format("session user={}\n", user));
	});

	app.post("/submit", [](Request const &req) {
		return Response::text(std::format("accepted value={}\n", req.form["value"]));
	});

	std::vector<conflux::http::Router::Middleware> openapi_auth;
	openapi_auth.push_back(bearer_auth_middleware([](std::string_view token) { return token == "docs-token"; }));
	auto &openapi_router = http::router(app);
	openapi_router.get(
		"/openapi.json",
		openapi_handler_protected(openapi_router, "conflux policy stack example", "0.1.0", std::move(openapi_auth)));

	std::println("policy stack listening on http://localhost:9100/");
	std::println("structured logs: {}", log_path);
	auto const status = std::move(app).run({.port = 9100});
	return status == conflux::http::RunStatus::stopped_normally ? 0 : 1;
}
