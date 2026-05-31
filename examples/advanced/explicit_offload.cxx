// Explicit HTTP offload example: keep ring-thread handlers small, decode once,
// then move CPU work to a caller-owned WorkPool.
// Build and run: build/release-clang-libcxx/conflux_http_explicit_offload_example
// Try:
//   curl http://localhost:9111/api/status
//   curl -X POST http://localhost:9111/api/hash
//        -H 'Content-Type: application/json' -d '{"input":"conflux","rounds":200}'
import conflux.extended;
import conflux.net.http.server_types;
import conflux.net.http.native_json;
import std;

namespace http = conflux::http;
using conflux::work::WorkPool;
using conflux::work::WorkPoolOptions;
using conflux::work::WorkPoolQueueMode;

struct StatusReply {
	std::string status;
	std::string placement;
};

template<>
struct conflux::json::JsonMembers<StatusReply> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("status", &StatusReply::status),
			conflux::json::json_member("placement", &StatusReply::placement),
		};
	}
	static constexpr std::string_view type_name() { return "StatusReply"; }
};

struct HashRequest {
	std::string input;
	std::int64_t rounds{1};
};

template<>
struct conflux::json::JsonMembers<HashRequest> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("input", &HashRequest::input),
			conflux::json::json_member("rounds", &HashRequest::rounds),
		};
	}
	static constexpr std::string_view type_name() { return "HashRequest"; }
};

struct HashReply {
	std::string algorithm;
	std::int64_t rounds{};
	std::uint64_t hash{};
};

template<>
struct conflux::json::JsonMembers<HashReply> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("algorithm", &HashReply::algorithm),
			conflux::json::json_member("rounds", &HashReply::rounds),
			conflux::json::json_member("hash", &HashReply::hash),
		};
	}
	static constexpr std::string_view type_name() { return "HashReply"; }
};

static std::uint64_t hash_rounds(
	std::string_view input,
	std::int64_t rounds) {
	std::uint64_t h = 1469598103934665603ull;
	for (std::int64_t round = 0; round < rounds; ++round) {
		for (char const c: input) {
			h ^= static_cast<unsigned char>(c);
			h *= 1099511628211ull;
		}
		h ^= static_cast<std::uint64_t>(round);
		h *= 1099511628211ull;
	}
	return h;
}

int main() {
	auto app = http::App::default_server();

	WorkPool pool{
		WorkPoolOptions{
						.threads = 2,
						.max_inject_queue = 128,
						.queue_mode = WorkPoolQueueMode::no_stealing,
						.worker_name_prefix = "cf-hash",
						}
    };

	app.get("/api/status", [] {
		return http::Json{
			StatusReply{.status = "ok", .placement = "ring-thread"}
        };
	});

	app.post("/api/hash", [&pool](http::Json<HashRequest> const &body) -> http::Result<http::Response> {
		if (body->input.empty()) {
			return std::unexpected{http::problem::bad_request("invalid_hash_request", "input is required")};
		}
		if (body->rounds < 1 || body->rounds > 1000) {
			return std::unexpected{
				http::problem::bad_request("invalid_hash_request", "rounds must be between 1 and 1000")};
		}

		HashRequest request = *body;
		return http::offload(pool, [request = std::move(request)] {
			return http::codec::json::response_or_internal_error(
				HashReply{
					.algorithm = "fnv1a64",
					.rounds = request.rounds,
					.hash = hash_rounds(request.input, request.rounds),
				});
		});
	});

	auto const status = std::move(app).run({.port = 9111});
	pool.drain_and_stop();
	return status == conflux::http::RunStatus::stopped_normally ? 0 : 1;
}
