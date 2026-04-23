// Forms example: query params (GET) and urlencoded form bodies (POST).
// Build and run: build/debug-gcc-stdcxx/conflux_forms
// Then open:    http://localhost:9092/
// Or curl:
//   curl "http://localhost:9092/search?q=hello+world&lang=en"
//   curl -X POST http://localhost:9092/submit -d "name=Alice&age=30"
import conflux.net.http;
import std;

int main() {
	Config cfg{};
	cfg.port = 9092;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;

	Router router;

	// Home page with two forms: one GET (query params), one POST (urlencoded body).
	router.get("/", [](HttpRequestView const &) {
		return HttpResponse::html(
			"<html><body>"
			"<h1>conflux forms example</h1>"

			"<h2>Search — GET with query params</h2>"
			"<form method='get' action='/search'>"
			"  <input name='q' placeholder='query'> "
			"  <input name='lang' placeholder='lang (optional)'> "
			"  <button>Search</button>"
			"</form>"

			"<h2>Submit — POST with urlencoded body</h2>"
			"<form method='post' action='/submit'"
			"      enctype='application/x-www-form-urlencoded'>"
			"  <input name='name' placeholder='name'> "
			"  <input name='age'  placeholder='age'> "
			"  <button>Submit</button>"
			"</form>"
			"</body></html>");
	});

	// GET /search?q=...&lang=...
	router.get("/search", [](HttpRequestView const &req) {
		auto q = req.query["q"];
		auto lang = req.query["lang"];
		return HttpResponse::html(
			std::format(
				"<html><body>"
				"<h1>Search results</h1>"
				"<p>Query: <strong>{}</strong></p>"
				"<p>Language filter: <strong>{}</strong></p>"
				"<p><a href='/'>back</a></p>"
				"</body></html>",
				q.empty() ? "(none)" : q,
				lang.empty() ? "any" : lang));
	});

	// POST /submit  body: name=...&age=...
	router.post("/submit", [](HttpRequestView const &req) {
		auto name = req.form["name"];
		auto age = req.form["age"];
		return HttpResponse::html(
			std::format(
				"<html><body>"
				"<h1>Submitted</h1>"
				"<p>Name: <strong>{}</strong></p>"
				"<p>Age: <strong>{}</strong></p>"
				"<p><a href='/'>back</a></p>"
				"</body></html>",
				name.empty() ? "(none)" : name,
				age.empty() ? "(none)" : age));
	});

	HttpServer srv{cfg, std::move(router)};
	srv.run();
}
