#include <filesystem>
#include <fstream>
#include <unistd.h>

import conflux;
import std;

namespace {

void write_file(
	std::filesystem::path const &path,
	std::string_view contents) {
	std::ofstream out(path, std::ios::binary);
	out << contents;
}

struct TempDir {
	std::filesystem::path path;
	explicit TempDir(
		std::filesystem::path p)
		: path{std::move(p)} {
		std::filesystem::create_directories(path);
	}
	~TempDir() {
		std::error_code ec;
		(void)std::filesystem::remove_all(path, ec);
	}
};

} // namespace

int main() {
	namespace http = conflux::http;

	auto app = http::app();
	TempDir assets{std::filesystem::temp_directory_path() / std::format("conflux_quickstart_static_{}", ::getpid())};
	auto const &asset_dir = assets.path;
	write_file(asset_dir / "index.html", "<h1>conflux static files</h1>");

	app.get("/", [] { return http::redirect("/assets/"); });
	app.serve_static("/assets", asset_dir.string(), {.directory_listing = true});

	return static_cast<int>(std::move(app).run({.port = 9095}));
}
