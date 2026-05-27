// Forms example: query params (GET) and urlencoded form bodies (POST).
// Build and run: build/debug-clang-libcxx/conflux_forms
// Then open:    http://localhost:9092/
// Or curl:
//   curl "http://localhost:9092/search?q=hello+world&lang=en"
//   curl -X POST http://localhost:9092/submit -d "name=Alice&age=30"
import conflux;
import std;
int main() {
	namespace http = conflux::http;
	auto app = http::app();

	// Home page with two forms: one GET (query params), one POST (urlencoded body).
	app.get("/", [](http::RequestView const &) {
		return http::html(
			"<html><body>"
			"<h1>conflux forms example</h1>"
			"<h2>Search — GET with query params</h2>"
			"<form method='get' action='/search'>"
			"  <input name='q' placeholder='query'> "
			"  <input name='lang' placeholder='lang (Opt)'> "
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
	app.get("/search", [](http::RequiredQuery<"q"> q, http::OptionalQuery<"lang"> lang) {
		return http::html(
			std::format(
				"<html><body>"
				"<h1>Search results</h1>"
				"<p>Query: <strong>{}</strong></p>"
				"<p>Language filter: <strong>{}</strong></p>"
				"<p><a href='/'>back</a></p>"
				"</body></html>",
				q.get(),
				lang.value_or("any")));
	});

	// POST /submit  body: name=...&age=...
	app.post("/submit", [](http::RequiredForm<"name"> name, http::RequiredForm<"age", std::uint32_t> age) {
		return http::html(
			std::format(
				"<html><body>"
				"<h1>Submitted</h1>"
				"<p>Name: <strong>{}</strong></p>"
				"<p>Age: <strong>{}</strong></p>"
				"<p><a href='/'>back</a></p>"
				"</body></html>",
				name.get().empty() ? "(none)" : name.get(),
				age.get()));
	});

	auto const status = std::move(app).run({.port = 9092});
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
