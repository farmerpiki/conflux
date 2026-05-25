import conflux.curated;

int main() {
	auto app = conflux::http::app();
	app.get("/health", [] { return conflux::http::text("ok"); });

	auto parsed = conflux::json::parse(R"({"ok":true})");
	if (!parsed) {
		return 1;
	}
	return 0;
}
