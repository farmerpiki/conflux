// Compile-only API snapshot for the public HTTP facade.
import std;
import conflux.http;

namespace http_snapshot {

namespace http = conflux::http;

struct SearchParams {
	std::string q;
	std::uint32_t page{};
};

} // namespace http_snapshot

template<>
struct conflux::json::JsonMembers<http_snapshot::SearchParams> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("q", &http_snapshot::SearchParams::q),
			conflux::json::json_member("page", &http_snapshot::SearchParams::page),
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
struct conflux::json::JsonMembers<http_snapshot::Payload> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("value", &http_snapshot::Payload::value),
		};
	}
	static constexpr std::string_view type_name() { return "Payload"; }
};

namespace http_snapshot {

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
	app.post("/payload", [](http::Json<Payload> const &body) -> http::Result<http::CreatedBody<Payload>> {
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
	app.use([](http::RequestView const &req, auto const &next) {
		auto response = next(req);
		response.headers.set("x-sync-middleware", "1");
		return response;
	});
	app.group("/scoped", [](auto &group) {
		group.use([](http::RequestView const &req, auto const &next) {
			auto response = next(req);
			response.headers.set("x-group-sync-middleware", "1");
			return response;
		});
		(void)group.get("/", [] { return http::text("ok"); });
	});
}

void run_spelling_compiles() {
	auto app = http::app();
	app.get("/", [] { return http::text("ok"); });
	(void)http::run(std::move(app), {});
	(void)http::exit_code(http::RunStatus::stopped_normally);
}

void config_preset_spelling_compiles() {
	[[maybe_unused]] auto public_cfg = http::Config::public_server();
	[[maybe_unused]] auto development_cfg = http::Config::development();
	[[maybe_unused]] auto low_latency_cfg = http::Config::low_latency();
	[[maybe_unused]] auto benchmark_cfg = http::Config::benchmark();
	[[maybe_unused]] auto unsafe_cfg = http::Config::unsafe_max_speed();
	[[maybe_unused]] auto test_cfg = http::Config::test();
}

void response_helpers_compile() {
	(void)http::html("<p>ok</p>");
	(void)http::html(std::string{"<p>ok</p>"});
	(void)http::owned_html(std::string{"<p>ok</p>"});
	(void)http::no_content();
	[[maybe_unused]] auto bad_request_response = http::bad_request("bad");
	[[maybe_unused]] auto not_found_response = http::not_found("/missing");
	[[maybe_unused]] auto unauthorized_response = http::unauthorized("Bearer");
	[[maybe_unused]] auto forbidden_response = http::forbidden("no");
	[[maybe_unused]] auto method_not_allowed_response = http::method_not_allowed({"GET", "POST"});
	[[maybe_unused]] auto unprocessable_entity_response = http::unprocessable_entity("invalid");
	[[maybe_unused]] auto internal_error_response = http::internal_error("boom");
	[[maybe_unused]] auto not_modified_response = http::not_modified(R"("abc")");
	[[maybe_unused]] auto content_too_large_response = http::content_too_large();
	[[maybe_unused]] auto bad_gateway_response = http::bad_gateway("upstream");
	[[maybe_unused]] auto gateway_timeout_response = http::gateway_timeout();
	(void)http::redirect("/next");
	(void)http::buffered_stream([](http::StreamSink &sink) { sink.write("chunk"); });
	(void)http::text(std::string{"ok"});
	(void)http::owned_text(std::string{"ok"});
	(void)http::json(Payload{.value = "ok"});
	(void)http::created(std::string{"ok"});
	(void)http::owned_created(std::string{"ok"});
	(void)http::created(Payload{.value = "ok"});
	[[maybe_unused]] auto created_with_cookie = http::created(Payload{.value = "ok"})
													.location("/payloads/1")
													.cookie(http::cookie("session", "abc").path("/").http_only());
}

} // namespace http_snapshot
