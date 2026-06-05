// Cost model example: explicit request ownership, JSON storage, response buffers,
// blocking file reads, and caller-owned worker placement.
// Build and run: build/release-clang-libcxx/conflux_cost_model_example
// Try:
//   curl http://localhost:9112/api/view -d '{"name":"Ada"}'
//   curl http://localhost:9112/api/owned -d '{"name":"Ada"}'
//   curl http://localhost:9112/api/report
//   curl http://localhost:9112/api/file
//   curl http://localhost:9112/api/offload
#include <ctime>
#include <fstream>
#include <unistd.h>

import conflux.extended;
import std;

namespace http = conflux::http;
namespace json = conflux::json;

struct TempFile {
	std::filesystem::path path;

	TempFile() {
		path = std::filesystem::temp_directory_path()
			 / std::format("conflux_cost_model_{}_{}.txt", static_cast<long long>(::getpid()), std::time(nullptr));
		std::ofstream out{path};
		out << "temporary file response\n";
	}

	~TempFile() {
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}
};

static std::string json_name_or_default(
	std::string_view body) {
	auto doc = json::parse_borrowed(body);
	if (!doc) {
		return "unknown";
	}
	auto root = doc->root().as_object();
	if (!root) {
		return "unknown";
	}
	auto name = root->member("name");
	if (!name) {
		return "unknown";
	}
	auto value = name->as_string();
	return value ? std::string{*value} : "unknown";
}

int main() {
	auto app = http::App::default_server();
	auto file = std::make_shared<TempFile>();
	conflux::work::WorkPool pool{
		conflux::work::WorkPoolOptions{
									   .threads = 2,
									   .max_inject_queue = 64,
									   .queue_mode = conflux::work::WorkPoolQueueMode::no_stealing,
									   .worker_name_prefix = "cf-cost",
									   }
    };

	app.post("/api/view", [](http::BodyBytes body) {
		auto name = json_name_or_default(body.text_view());
		return http::text(std::format("borrowed body bytes for {}\n", name));
	});

	app.post("/api/owned", [](http::OwnedBodyBytes body) {
		auto doc = json::parse_copy(std::move(body.value));
		auto name = std::string{"unknown"};
		if (doc) {
			auto root = doc->root().as_object();
			if (root) {
				auto member = root->member("name");
				if (member) {
					if (auto value = member->as_string()) {
						name = std::string{*value};
					}
				}
			}
		}
		return http::owned_text(std::format("owned request body for {}\n", name));
	});

	app.get("/api/report", [] {
		json::JsonArena arena{json::JsonArenaOptions{.initial_slab = 4096}};
		auto doc = arena.parse_into(R"({"copies":"arena-backed","allocations":"caller-owned slab"})");
		return http::buffered_stream(
			[&](http::StreamSink &sink) {
				sink.write("buffered response\n");
				sink.write(doc ? *doc->dump() : "{}");
				sink.write("\n");
			},
			"text/plain; charset=utf-8");
	});

	app.get("/api/file", [file] { return http::blocking_file_response(file->path, "text/plain; charset=utf-8"); });

	app.get("/api/offload", [&pool] {
		return http::offload(pool, [] {
			std::uint64_t total{};
			for (std::uint64_t i = 0; i != 20000; ++i) {
				total += i * 17;
			}
			return http::text(std::format("worker result {}\n", total));
		});
	});

	auto const status = std::move(app).run({.port = 9112});
	pool.drain_and_stop();
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
