// Static files + JSON API example.
//
// Build and run: build/debug-gcc-stdcxx/conflux_static
// Then open:     http://localhost:9095/
// Try:
//   curl http://localhost:9095/api/info
//   curl http://localhost:9095/assets/hello.txt
//   curl http://localhost:9095/assets/app.css
#include <filesystem>
#include <fstream>

import conflux.net.http.server;
import std;
static void write_file(
	std::filesystem::path const &path,
	std::string_view contents) {
	std::ofstream out(path, std::ios::binary);
	out << contents;
}
int main() {
	namespace http = conflux::http;
	auto app = http::App::default_server();
	app.config().fixed_buffer_slabs = 8;
	app.config().splice_pipe_pairs = 2;

	auto asset_dir = std::filesystem::temp_directory_path() / "conflux_static_example";
	std::filesystem::create_directories(asset_dir);
	write_file(asset_dir / "hello.txt", "hello from conflux static files\n");
	write_file(asset_dir / "app.css", "body{font-family:monospace;background:#f6f6f1;color:#222;}");
	write_file(
		asset_dir / "index.html",
		"<html><head><link rel='stylesheet' href='/assets/app.css'></head>"
		"<body><h1>conflux static example</h1>"
		"<p>Try <a href='/api/info'>/api/info</a> or <a href='/assets/hello.txt'>/assets/hello.txt</a>.</p>"
		"</body></html>");

	app.get("/", [](http::Request const &) { return http::Response::redirect("/assets/"); });

	app.get("/api/info", [asset_dir = asset_dir.string()](http::Request const &) {
		return http::Response::json(
			format(R"({{"status":"ok","assets":"{}","routes":["/","/api/info","/assets/{{*file}}"]}})", asset_dir));
	});

	app.serve_static("/assets", asset_dir.string(), {.directory_listing = true});

	auto const status = move(app).run({.port = 9095});
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
