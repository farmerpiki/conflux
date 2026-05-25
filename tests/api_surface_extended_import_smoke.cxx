import std;
import conflux.extended;

int main() {
	conflux::http::Config cfg = conflux::http::Config::test();
	cfg.port = 0;

	auto req = conflux::http::ClientRequest::get("http://127.0.0.1/");
	(void)req;

	auto app = conflux::http::app();
	app.get("/health", [] { return conflux::http::text("ok"); }).name("health.check");
	auto openapi = conflux::http::openapi_handler(app, "API", "1.0.0");
	(void)openapi;

	WorkPool pool{WorkPoolOptions{.threads = 1}};
	auto offloaded = conflux::http::offload(pool, [] { return conflux::http::text("ok"); });
	(void)offloaded;

	auto [task, src] = conflux::work::root::make_task_source<int>();
	(void)src.try_set_value(conflux::work::root::Success<int>{42});
	auto out = conflux::work::root::blocking_join(std::move(task));
	if (!out.is_success()) {
		return 1;
	}
	return 0;
}
