// Explicit HTTP offload example: keep ring-thread handlers small, decode once,
// then move CPU work to a caller-owned WorkPool.
// Build and run: build/debug-gcc-stdcxx/conflux_http_explicit_offload_example
// Try:
//   curl http://localhost:9111/api/status
//   curl -X POST http://localhost:9111/api/hash \
//        -H 'Content-Type: application/json' -d '{"input":"conflux","rounds":200}'
import conflux.net.app;
import conflux.types;
import conflux.json;
import conflux.net.http.app_json;
import conflux.net.http.native_json;
import conflux.net.http.server_types;
import conflux.work;
import std;

namespace http = conflux::http;
namespace json = conflux::json;
using JsonProvider = conflux::json::boundary::NativeJsonProvider;

struct StatusReply {
	S status;
	S placement;
};

template<>
struct JsonMembers<StatusReply> {
	static constexpr auto members() {
		return Tup{
			json_member("status", &StatusReply::status),
			json_member("placement", &StatusReply::placement),
		};
	}
	static constexpr SV type_name() { return "StatusReply"; }
};

struct HashRequest {
	S input;
	i64 rounds{1};
};

template<>
struct JsonMembers<HashRequest> {
	static constexpr auto members() {
		return Tup{
			json_member("input", &HashRequest::input),
			json_member("rounds", &HashRequest::rounds),
		};
	}
	static constexpr SV type_name() { return "HashRequest"; }
};

struct HashReply {
	S algorithm;
	i64 rounds{};
	u64 hash{};
};

template<>
struct JsonMembers<HashReply> {
	static constexpr auto members() {
		return Tup{
			json_member("algorithm", &HashReply::algorithm),
			json_member("rounds", &HashReply::rounds),
			json_member("hash", &HashReply::hash),
		};
	}
	static constexpr SV type_name() { return "HashReply"; }
};

struct ApiError {
	S error;
};

template<>
struct JsonMembers<ApiError> {
	static constexpr auto members() {
		return Tup{
			json_member("error", &ApiError::error),
		};
	}
	static constexpr SV type_name() { return "ApiError"; }
};

static u64 hash_rounds(
	SV input,
	i64 rounds) {
	u64 h = 1469598103934665603ull;
	for (i64 round = 0; round < rounds; ++round) {
		for (unsigned char const c: input) {
			h ^= c;
			h *= 1099511628211ull;
		}
		h ^= static_cast<u64>(round);
		h *= 1099511628211ull;
	}
	return h;
}

static http::Response json_error(
	SV message,
	int status,
	SV status_text) {
	return http::json::response_or_internal_error(
		ApiError{.error = S{message}},
		http::json::ResponseOptions{.status = status, .status_text = status_text});
}

int main() {
	auto app = http::App::default_server();
	auto api = http::json::routes<JsonProvider>(app);

	WorkPool pool{WorkPoolOptions{
		.threads = 2,
		.max_inject_queue = 128,
		.queue_mode = WorkPoolQueueMode::no_stealing,
		.worker_name_prefix = "cf-hash",
	}};

	api.get("/api/status", [] {
		return StatusReply{.status = "ok", .placement = "ring-thread"};
	});

	app.post("/api/hash", [&pool](http::Request const &req) -> http::Response {
		auto decoded = json::boundary::decode_native<HashRequest>(req.body);
		if (!decoded) {
			return http::json::decode_error_response();
		}
		if (decoded->input.empty()) {
			return json_error("input is required", 422, "Unprocessable Entity");
		}
		if (decoded->rounds < 1 || decoded->rounds > 1000) {
			return json_error("rounds must be between 1 and 1000", 422, "Unprocessable Entity");
		}

		HashRequest body = move(*decoded);
		return http::defer(pool, [body = move(body)] {
			return http::json::response_or_internal_error(HashReply{
				.algorithm = "fnv1a64",
				.rounds = body.rounds,
				.hash = hash_rounds(body.input, body.rounds),
			});
		});
	});

	auto const status = move(app).run({.port = 9111});
	pool.drain_and_stop();
	return status == RunStatus::stopped_normally ? 0 : 1;
}
