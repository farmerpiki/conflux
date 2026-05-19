#include <filesystem>
#include <fstream>

import conflux.http;
import std;

namespace {

void write_file(
	std::filesystem::path const &path,
	std::string_view contents) {
	std::ofstream out(path, std::ios::binary);
	out << contents;
}

} // namespace

int main() {
	namespace http = conflux::http;

	auto app = http::app();
	auto asset_dir = std::filesystem::temp_directory_path() / "conflux_quickstart_static";
	std::filesystem::create_directories(asset_dir);
	write_file(asset_dir / "index.html", "<h1>conflux static files</h1>");

	app.get("/", [] { return http::redirect("/assets/"); });
	app.serve_static("/assets", asset_dir.string(), {.directory_listing = true});

	return http::run(std::move(app), {.port = 9095}) == http::RunStatus::stopped_normally ? 0 : 1;
}
