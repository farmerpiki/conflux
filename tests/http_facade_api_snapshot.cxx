// Compile-only API snapshot for the public HTTP facade.
import std;
import conflux.http;
import conflux.net.http.server_types;
import conflux.net.router;

namespace http_snapshot {

namespace http = conflux::http;

struct SearchParams {
	std::string q;
	std::uint32_t page{};
};

} // namespace http_snapshot

template<>
struct JsonMembers<http_snapshot::SearchParams> {
	static constexpr auto members() {
		return std::tuple{
			json_member("q", &http_snapshot::SearchParams::q),
			json_member("page", &http_snapshot::SearchParams::page),
		};
	}
	static constexpr std::string_view type_name() { return "SearchParams"; }
};

namespace http_snapshot {

struct Payload {
	std::string value;
};

} // namespace http_snapshot

template<>
struct JsonMembers<http_snapshot::Payload> {
	static constexpr auto members() {
		return std::tuple{
			json_member("value", &http_snapshot::Payload::value),
		};
	}
	static constexpr std::string_view type_name() { return "Payload"; }
};

namespace http_snapshot {

static_assert(std::same_as<http::RequestView, RequestView>);
static_assert(std::same_as<http::Request, RequestView>);
static_assert(std::same_as<http::OwnedRequest, Request>);
static_assert(std::same_as<http::Response, Response>);
static_assert(std::same_as<http::RequestContext, RequestContext>);
static_assert(std::same_as<decltype(std::declval<http::BodyBytes const &>().get()), std::span<std::byte const>>);
static_assert(std::same_as<decltype(std::declval<http::BodyBytes const &>().text_view()), std::string_view>);
void route_forms_compile() {
	auto app = http::app();
	std::string state = "state";
	app.state(state);

	app.get("/", [] { return http::text("ok"); });
	app.get<"/items/{id:u64}">([](http::Path<"id", std::uint64_t> id, http::State<std::string> s) {
		return http::text(std::format("{}:{}", id.get(), s.get()));
	});
	app.get("/search", [](http::QueryParams<SearchParams> query) { return http::Json{Payload{.value = query->q}}; });
	app.post("/form", [](http::FormParams<SearchParams> form) {
		return http::text(std::format("{}:{}", form->q, form->page));
	});
	app.post("/doc", [](http::JsonDocument doc) {
		auto dumped = doc->dump();
		return http::text(dumped.value_or("{}"));
	});
	app.post("/payload", [](http::Json<Payload> const &body) -> std::expected<http::Created, http::Problem> {
		if (body->value.empty()) {
			return std::unexpected{http::problem::bad_request("empty_value", "value is required")};
		}
		return http::created(*body);
	});

	auto routes = app.routes();
	static_assert(std::same_as<decltype(routes), std::vector<http::AppRouteInfo>>);
	auto static_mounts = app.static_mounts();
	static_assert(std::same_as<decltype(static_mounts), std::vector<http::AppStaticMountInfo>>);
	(void)app.route_table();
	(void)app.openapi_spec();
	(void)app.validate().detailed_summary();
}

void middleware_forms_compile() {
	auto app = http::app();
	app.use(http::request_id());
	app.use(http::tracing({.propagate_in_response = false}));
	app.use(http::trace_context({.propagate_in_response = false}));
	app.use(http::security_headers({.hsts_max_age = 0}));
	app.use([](http::RequestView const &req, http::Next const &next) {
		auto response = next(req);
		response.headers.set("x-sync-middleware", "1");
		return response;
	});
	app.use(
		[](http::RequestView const &req,
		   http::RequestContext const &ctx,
		   http::AsyncNext const &next) -> http::Task<http::Response> {
			auto response = co_await next(req, ctx);
			response.headers.set("x-async-middleware", "1");
			co_return response;
		});
	app.group("/scoped", [](auto &group) {
		group.use(
			[](http::RequestView const &req,
			   http::RequestContext const &ctx,
			   http::AsyncNext const &next) -> http::Task<http::Response> {
				auto response = co_await next(req, ctx);
				response.headers.set("x-group-async-middleware", "1");
				co_return response;
			});
		(void)group.get("/", [](http::RequestView const &, http::RequestContext const &) -> http::Task<http::Response> {
			co_return http::text("ok");
		});
	});
}

void run_spelling_compiles() {
	auto app = http::app();
	app.get("/", [] { return http::text("ok"); });
	(void)http::run(std::move(app), {});
	(void)http::exit_code(http::RunStatus::stopped_normally);
}

void response_helpers_compile() {
	(void)http::html("<p>ok</p>");
	(void)http::owned_html(std::string{"<p>ok</p>"});
	(void)http::no_content();
	(void)http::redirect("/next");
	(void)http::buffered_stream([](http::StreamSink &sink) { sink.write("chunk"); });
	(void)http::owned_text(std::string{"ok"});
	(void)http::json(Payload{.value = "ok"});
	(void)http::owned_created(std::string{"ok"});
	(void)http::created(Payload{.value = "ok"});
}

} // namespace http_snapshot
