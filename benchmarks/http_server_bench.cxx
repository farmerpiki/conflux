#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#if CONFLUX_BENCH_HAS_TLS
	#include <openssl/ssl.h>
#endif

import std;
import conflux.types;
import conflux.net.http;
import conflux.work;
import bench_common;

using namespace std::literals;
using conflux::http::Config;
using conflux::http::HttpServerMetrics;
using conflux::http::RequestContext;
using conflux::http::SseChannel;
using conflux::http::WsConn;
namespace {

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

// ── BenchClient ─────────────────────────────────────────────────────────────

struct BenchClient {
	int fd = -1;
	explicit BenchClient(
		std::uint16_t port) {
		fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) {
			throw std::runtime_error{"socket failed"};
		}
		static constexpr int one = 1;
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof one);
		set_recv_timeout(std::chrono::seconds{5});
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
			::close(fd);
			throw std::runtime_error{"connect failed"};
		}
	}
	~BenchClient() { close(); }
	BenchClient(BenchClient const &) = delete;
	BenchClient &operator =(BenchClient const &) = delete;
	BenchClient(
		BenchClient &&o) noexcept
		: fd(std::exchange(o.fd, -1)) {}
	BenchClient &operator =(
		BenchClient &&o) noexcept {
		if (this != &o) {
			close();
			fd = std::exchange(o.fd, -1);
		}
		return *this;
	}
	void close() noexcept {
		if (fd >= 0) {
			::close(fd);
			fd = -1;
		}
	}
	void send_all(
		std::string_view data) const {
		auto const *p = data.data();
		auto remaining = data.size();
		while (remaining > 0) {
			auto n = ::send(fd, p, remaining, MSG_NOSIGNAL);
			if (n <= 0) {
				throw std::runtime_error{"send failed"};
			}
			p += n;
			remaining -= static_cast<std::size_t>(n);
		}
	}
	::ssize_t recv_some(
		std::span<char> buf,
		std::size_t total) const {
		if (total >= buf.size()) {
			throw std::runtime_error{"response buffer exhausted"};
		}
		auto const n = ::recv(fd, buf.data() + total, buf.size() - total, 0);
		if (n < 0) {
			throw std::runtime_error{"recv timed out or failed"};
		}
		return n;
	}
	std::size_t recv_response(
		std::span<char> buf) const {
		std::size_t total = 0;
		std::size_t hdr_end_pos = std::string_view::npos;
		std::size_t body_len = 0;
		bool have_cl = false;
		for (;;) {
			auto n = recv_some(buf, total);
			if (n == 0) {
				throw std::runtime_error{"connection closed before response completed"};
			}
			total += static_cast<std::size_t>(n);
			if (hdr_end_pos == std::string_view::npos) {
				std::string_view sofar{buf.data(), total};
				hdr_end_pos = sofar.find("\r\n\r\n");
				if (hdr_end_pos == std::string_view::npos) {
					continue;
				}
				hdr_end_pos += 4;
				std::string_view hdrs{buf.data(), hdr_end_pos};
				auto cl = hdrs.find("Content-Length: ");
				if (cl != std::string_view::npos) {
					cl += 16;
					auto end = hdrs.find("\r\n", cl);
					std::from_chars(buf.data() + cl, buf.data() + end, body_len);
					have_cl = true;
				}
				// 304/204 → no body
				if (hdrs.starts_with("HTTP/1.1 304") || hdrs.starts_with("HTTP/1.1 204")) {
					return total;
				}
			}
			if (have_cl && total >= hdr_end_pos + body_len) {
				return total;
			}
			if (!have_cl && hdr_end_pos != std::string_view::npos) {
				return total;
			}
		}
	}
	std::size_t recv_response_no_body(
		std::span<char> buf) const {
		std::size_t total = 0;
		for (;;) {
			auto n = recv_some(buf, total);
			if (n == 0) {
				throw std::runtime_error{"connection closed before response headers completed"};
			}
			total += static_cast<std::size_t>(n);
			std::string_view sofar{buf.data(), total};
			if (sofar.find("\r\n\r\n") != std::string_view::npos) {
				return total;
			}
		}
	}
	std::size_t recv_n_responses(
		std::span<char> buf,
		int count) const {
		std::size_t total = 0;
		int got = 0;
		std::size_t search_from = 0;
		while (got < count) {
			auto n = recv_some(buf, total);
			if (n == 0) {
				break;
			}
			total += static_cast<std::size_t>(n);
			std::string_view sofar{buf.data(), total};
			while (got < count) {
				auto hdr_end = sofar.find("\r\n\r\n", search_from);
				if (hdr_end == std::string_view::npos) {
					break;
				}
				hdr_end += 4;
				auto cl = sofar.find("Content-Length: ", search_from);
				if (cl == std::string_view::npos || cl >= hdr_end) {
					++got;
					search_from = hdr_end;
					continue;
				}
				cl += 16;
				auto cl_end = sofar.find("\r\n", cl);
				std::size_t body_len = 0;
				std::from_chars(buf.data() + cl, buf.data() + cl_end, body_len);
				std::size_t resp_end = hdr_end + body_len;
				if (resp_end > total) {
					break;
				}
				++got;
				search_from = resp_end;
			}
		}
		if (got < count) {
			throw std::runtime_error{std::format("recv_n_responses expected {} responses, got {}", count, got)};
		}
		return total;
	}
	std::size_t recv_until_close(
		std::span<char> buf) const {
		std::size_t total = 0;
		for (;;) {
			auto n = recv_some(buf, total);
			if (n == 0) {
				break;
			}
			total += static_cast<std::size_t>(n);
			if (total >= buf.size()) {
				break;
			}
		}
		return total;
	}
	void send_partial(
		std::string_view data,
		std::size_t bytes) const {
		auto n = ::send(fd, data.data(), std::min(bytes, data.size()), MSG_NOSIGNAL);
		(void)n;
	}
	std::size_t recv_slow(
		std::span<char> buf,
		std::size_t chunk_size,
		std::chrono::microseconds delay) const {
		std::size_t total = 0;
		std::size_t hdr_end_pos = std::string_view::npos;
		std::size_t body_len = 0;
		bool have_cl = false;
		for (;;) {
			auto to_read = std::min(chunk_size, buf.size() - total);
			if (to_read == 0) {
				throw std::runtime_error{"response buffer exhausted"};
			}
			auto n = ::recv(fd, buf.data() + total, to_read, 0);
			if (n < 0) {
				throw std::runtime_error{"recv timed out or failed"};
			}
			if (n == 0) {
				throw std::runtime_error{"connection closed before slow response completed"};
			}
			total += static_cast<std::size_t>(n);
			if (hdr_end_pos == std::string_view::npos) {
				std::string_view sofar{buf.data(), total};
				hdr_end_pos = sofar.find("\r\n\r\n");
				if (hdr_end_pos != std::string_view::npos) {
					hdr_end_pos += 4;
					std::string_view hdrs{buf.data(), hdr_end_pos};
					auto cl = hdrs.find("Content-Length: ");
					if (cl != std::string_view::npos) {
						cl += 16;
						auto end = hdrs.find("\r\n", cl);
						std::from_chars(buf.data() + cl, buf.data() + end, body_len);
						have_cl = true;
					}
				}
			}
			if (have_cl && total >= hdr_end_pos + body_len) {
				return total;
			}
			if (!have_cl && hdr_end_pos != std::string_view::npos) {
				return total;
			}
			std::this_thread::sleep_for(delay);
		}
	}
	void shutdown_wr() const { ::shutdown(fd, SHUT_WR); }
	void reconnect(
		std::uint16_t port) {
		close();
		fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) {
			throw std::runtime_error{"socket failed"};
		}
		static constexpr int one = 1;
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof one);
		set_recv_timeout(std::chrono::seconds{5});
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
			::close(fd);
			fd = -1;
			throw std::runtime_error{"reconnect failed"};
		}
	}
	void set_recv_timeout(
		std::chrono::seconds t) const {
		timeval tv{.tv_sec = t.count(), .tv_usec = 0};
		::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
};
// ── Server helpers ──────────────────────────────────────────────────────────

void wait_for_server(
	std::uint16_t port) {
	for (int i = 0; i < 200; ++i) {
		int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		bool const up = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
		::close(fd);
		if (up) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	throw std::runtime_error{"server did not start in time"};
}
struct ServerHandle {
	std::shared_ptr<HttpServer> server;
	std::thread thr;
	std::uint16_t port{};
};
ServerHandle start_server(
	Config cfg,
	Router router) {
	(void)::signal(SIGPIPE, SIG_IGN);
	cfg.startup_banner = false;
	auto srv = std::make_shared<HttpServer>(cfg, std::move(router));
	std::thread t{[srv] {
		try {
			auto _ = srv->run();
		} catch (std::exception const &e) { std::println(std::cerr, "bench server: {}", e.what()); }
	}};
	auto p = srv->port();
	wait_for_server(p);
	return {.server = srv, .thr = std::move(t), .port = p};
}
Config bench_config(
	unsigned rings = 1,
	unsigned ring_entries = 256) {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = rings;
	cfg.ring_entries = ring_entries;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.startup_banner = false;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	return cfg;
}
// ── Pre-built request strings ───────────────────────────────────────────────

static auto const kGetRoot = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetJson = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetContext = "GET /context HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetParam = "GET /hello/world HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetGroup = "GET /api/v2/status HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGet404 = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetBody8k = "GET /body/8k HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetBody64k = "GET /body/64k HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetBody1m = "GET /body/1m HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetBigGzip = "GET /big HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n"sv;
static auto const kGetBigNoEnc = "GET /big HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetSecurity = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetCorsOrigin = "GET /api HTTP/1.1\r\nHost: localhost\r\nOrigin: https://bench.example\r\n\r\n"sv;
static auto const kGetCorsNone = "GET /api HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetAuthBearer =
	"GET /protected HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer valid-bench-token\r\n\r\n"sv;
static auto const kGetAuthMissing = "GET /protected HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetEtag = "GET /content HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetCacheHit = "GET /counted HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetFullStack =
	"GET /big HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: gzip\r\nOrigin: https://bench.example\r\n\r\n"sv;
static auto const kGetRootClose = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"sv;
std::string make_post_request(
	std::string_view path,
	std::size_t body_size) {
	std::string body(body_size, 'X');
	return std::format(
		"POST {} HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n\r\n{}",
		path,
		body_size,
		body);
}
std::string make_chunked_single(
	std::string_view path,
	std::size_t body_size) {
	std::string body(body_size, 'X');
	return std::format(
		"POST {} HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n{:x}\r\n{}\r\n0\r\n\r\n",
		path,
		body_size,
		body);
}
std::string make_chunked_many(
	std::string_view path,
	std::size_t total_size,
	std::size_t chunk_size) {
	std::string req = std::format("POST {} HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n", path);
	std::string chunk(chunk_size, 'X');
	for (std::size_t sent = 0; sent < total_size; sent += chunk_size) {
		auto cs = std::min(chunk_size, total_size - sent);
		req += std::format("{:x}\r\n", cs);
		req.append(chunk.data(), cs);
		req += "\r\n";
	}
	req += "0\r\n\r\n";
	return req;
}
// Error / malformed request strings
static auto const kBadRequestLine = "GET@HOME / HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kHttp10Close = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n"sv;
static auto const kConnectionClose = kGetRootClose;
static auto const kDuplicateCL =
	"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello"sv;
static auto const kConflictingCL =
	"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 10\r\n\r\nhello"sv;
static auto const kTEPlusCL =
	"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n5\r\nhello\r\n0\r\n\r\n"sv;
static auto const kObsFold = "GET / HTTP/1.1\r\nHost: localhost\r\nX-Test: value\r\n continued\r\n\r\n"sv;
static auto const kInvalidHeaderWS = "GET / HTTP/1.1\r\nHost: localhost\r\nX\t-Bad: value\r\n\r\n"sv;
static auto const kInvalidTE =
	"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip\r\n\r\ndata"sv;
std::string make_header_too_large() {
	std::string hdr = "GET / HTTP/1.1\r\nHost: localhost\r\nX-Big: ";
	hdr.append(65536, 'A');
	hdr += "\r\n\r\n";
	return hdr;
}
std::string make_body_too_large_headers() {
	return std::format(
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n\r\n",
		100 * 1024 * 1024);
}
// ── Variant definition ──────────────────────────────────────────────────────

using RunFn = std::function<void()>;
struct Variant {
	std::string_view name;
	RunFn setup;
	RunFn run;
	RunFn teardown;
	std::size_t ops_per_iter = 1;
	std::size_t iters_override = 0;
};

[[nodiscard]] std::string_view parse_only_variant(
	std::span<char *> args) {
	for (std::size_t i = 1; i < args.size(); ++i) {
		if (std::string_view{args[i]} == "--only"sv && i + 1 < args.size()) {
			return args[i + 1];
		}
	}
	return {};
}

// ── Benchmark runner ────────────────────────────────────────────────────────

BenchStats run_variant(
	Variant const &v,
	std::size_t iterations,
	std::size_t warmup,
	std::string_view config_name) {
	if (v.iters_override) {
		iterations = std::min(iterations, v.iters_override);
		warmup = std::min(warmup, std::max(std::size_t{2}, v.iters_override / 5));
	}
	if (v.setup) {
		v.setup();
	}

	std::uint64_t t0{};
	std::uint64_t t1{};
	try {
		for (std::size_t i = 0; i < warmup; ++i) {
			v.run();
		}

		t0 = bench_now_ns();
		for (std::size_t i = 0; i < iterations; ++i) {
			v.run();
		}
		t1 = bench_now_ns();
	} catch (...) {
		if (v.teardown) {
			v.teardown();
		}
		throw;
	}

	if (v.teardown) {
		v.teardown();
	}

	auto const total = t1 - t0;
	auto const ns_pi = static_cast<double>(total) / static_cast<double>(iterations);
	return BenchStats{
		.config = config_name,
		.variant = v.name,
		.iterations = iterations,
		.total_ns = total,
		.ns_per_iter = ns_pi,
	};
}
void print_variant(
	BenchStats const &s,
	bool json,
	std::size_t ops_per_iter) {
	if (json) {
		if (ops_per_iter > 1) {
			auto const total_ops = s.iterations * ops_per_iter;
			auto const ns_per_op = s.ns_per_iter / static_cast<double>(ops_per_iter);
			std::println(
				"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},\"ops_"
				"per_iter\":{},\"total_ops\":{},\"ns_per_op\":{:.2f}}}",
				s.config,
				s.variant,
				s.iterations,
				s.total_ns,
				s.ns_per_iter,
				ops_per_iter,
				total_ops,
				ns_per_op);
		} else {
			std::println(
				"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f}}}",
				s.config,
				s.variant,
				s.iterations,
				s.total_ns,
				s.ns_per_iter);
		}
	} else if (ops_per_iter > 1) {
		auto const ns_per_op = s.ns_per_iter / static_cast<double>(ops_per_iter);
		std::println(
			"{:<32} {:>8} iters  {:>10.2f} ns/iter  {:>10.2f} ns/op (x{})",
			s.variant,
			s.iterations,
			s.ns_per_iter,
			ns_per_op,
			ops_per_iter);
	} else {
		std::println("{:<32} {:>8} iters  {:>10.2f} ns/iter", s.variant, s.iterations, s.ns_per_iter);
	}
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"http_server","parser":"standard","configs":[{"name":"default","extra":{"tier":"full-suite-smoke"},"target_ms":1000,"max_iterations":100,"calibration_iterations":2,"args":["--iterations","0","--warmup","0"],"reps":1},{"name":"pipeline_100","extra":{"tier":"dispatch","case":"HTTP/1 pipelined GET"},"target_ms":500,"max_iterations":10000,"calibration_iterations":2,"args":["--only","pipeline_100","--config-name","pipeline_100","--iterations","0","--warmup","0"]},{"name":"context_exact","extra":{"tier":"dispatch","case":"HTTP/1 exact async context route"},"target_ms":500,"max_iterations":10000,"calibration_iterations":2,"args":["--only","context_exact","--config-name","context_exact","--iterations","0","--warmup","0"]},{"name":"post_echo_4k","extra":{"tier":"dispatch","case":"HTTP/1 Content-Length POST 4KiB"},"target_ms":500,"max_iterations":10000,"calibration_iterations":2,"args":["--only","post_echo_4k","--config-name","post_echo_4k","--iterations","0","--warmup","0"]},{"name":"post_chunked_4k_many","extra":{"tier":"dispatch","case":"HTTP/1 chunked POST 4KiB"},"target_ms":500,"max_iterations":10000,"calibration_iterations":2,"args":["--only","post_chunked_4k_many","--config-name","post_chunked_4k_many","--iterations","0","--warmup","0"]}]})");

	auto const argv_span = std::span{argv, static_cast<std::size_t>(argc)};
	auto const args = bench_parse_args(argv_span);
	auto const iters = args.iterations;
	auto const warmup = args.warmup;
	auto const json = args.json_out;
	auto const config_name = args.config_name.empty() ? "default"sv : std::string_view{args.config_name};
	auto const only_variant = parse_only_variant(argv_span);

	// ── Start servers ─────────────────────────────────────────────────────

	auto const body_8k = std::string(8192, 'B');
	auto const body_64k = std::string(65536, 'C');
	auto const body_1m = std::string(1048576, 'D');
	auto const post_64 = make_post_request("/api/echo-body", 64);
	auto const post_4k = make_post_request("/api/echo-body", 4096);
	auto const post_64k = make_post_request("/api/echo-body", 65536);

	// plain_r1
	Router plain_router;
	plain_router.get("/", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::html("<html><body><h1>Hello from conflux!</h1></body></html>");
	});
	plain_router.get("/api/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"status":"ok"})"); });
	plain_router.get_context(
		"/context",
		[](conflux::http::RequestView const &, RequestContext const &) -> conflux::work::root::Task<conflux::http::Response> {
			co_return conflux::http::Response::text("context");
		});
	plain_router.get("/hello/{name}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::html(
			std::format("<html><body><h1>Hello, {}!</h1></body></html>", req.params["name"]));
	});
	plain_router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) { return conflux::http::Response::text(req.body); });
	plain_router.group("/api/v2", [](Router::Group &g) {
		g.use([](conflux::http::OwnedRequest const &req, Router::Handler const &next) {
			auto resp = next(req);
			resp.headers["X-Api-Version"] = "2";
			return resp;
		});
		g.get("/status", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"v":"2","status":"ok"})"); });
	});
	plain_router.get("/body/8k", [&body_8k](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(body_8k); });
	plain_router.get("/body/64k", [&body_64k](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(body_64k); });
	plain_router.get("/body/1m", [&body_1m](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(body_1m); });
	plain_router.sse("/events", [](conflux::http::OwnedRequest const &, std::shared_ptr<SseChannel> const &ch) {
		(void)ch->send_event("msg", "event1");
		(void)ch->send_event("msg", "event2");
		(void)ch->send_event("msg", "event3");
		ch->close();
	});
	plain_router.ws("/ws", [](conflux::http::OwnedRequest const &, WsConn &ws) {
		for (;;) {
			auto frame = ws.recv();
			if (!frame || frame->opcode == WsConn::Opcode::Close) {
				break;
			}
			if (frame->opcode == WsConn::Opcode::Text) {
				(void)ws.send_text(frame->payload);
			}
		}
	});

	auto plain = start_server(bench_config(), std::move(plain_router));

	// compress
	Router compress_router;
	compress_router.use(compress_middleware());
	compress_router.get("/big", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::html(std::string(512, 'A')); });
	auto compress = start_server(bench_config(), std::move(compress_router));

	// security
	Router security_router;
	conflux::http::SecurityOptions sopts{};
	sopts.hsts_only_on_tls = false;
	security_router.use(conflux::http::security_headers_middleware(sopts));
	security_router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	auto security = start_server(bench_config(), std::move(security_router));

	// cors
	Router cors_router;
	cors_router.use(conflux::http::cors_middleware({.allowed_origins = {"https://bench.example"}}));
	cors_router.get("/api", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"ok":true})"); });
	auto cors = start_server(bench_config(), std::move(cors_router));

	// auth
	Router auth_router;
	auth_router.use(conflux::http::bearer_auth_middleware([](std::string_view token) { return token == "valid-bench-token"; }));
	auth_router.get("/protected", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("secret"); });
	auto auth = start_server(bench_config(), std::move(auth_router));

	// etag
	Router etag_router;
	etag_router.use(conflux::http::etag_middleware());
	etag_router.get("/content", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("hello world"); });
	auto etag = start_server(bench_config(), std::move(etag_router));

	// cache
	Router cache_router;
	cache_router.use(conflux::http::response_cache_middleware({.max_entries = 64, .default_ttl = std::chrono::seconds{60}}));
	cache_router.get("/counted", [](conflux::http::OwnedRequest const &) {
		static std::atomic<int> count{0};
		int n = ++count;
		return conflux::http::Response::text(std::format("visit {}", n));
	});
	auto cache = start_server(bench_config(), std::move(cache_router));

	// full_stack
	Router fs_router;
	fs_router.use(conflux::http::security_headers_middleware(sopts));
	fs_router.use(conflux::http::cors_middleware({.allowed_origins = {"https://bench.example"}}));
	fs_router.use(compress_middleware());
	fs_router.use(conflux::http::etag_middleware());
	fs_router.get("/big", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::html(std::string(512, 'A')); });
	auto full_stack = start_server(bench_config(), std::move(fs_router));

	// deferred
	auto defer_pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 2});
	Router defer_router;
	defer_router.get("/api/defer-ok", [&defer_pool](conflux::http::OwnedRequest const &) {
		return conflux::http::defer(defer_pool, [] { return conflux::http::Response::json(R"({"deferred":"ok"})"); });
	});
	auto deferred = start_server(bench_config(), std::move(defer_router));

	// plain_rN (multi-ring)
	Router rn_router;
	rn_router.get("/", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::html("<html><body><h1>Hello from conflux!</h1></body></html>");
	});
	rn_router.get("/api/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"status":"ok"})"); });
	rn_router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) { return conflux::http::Response::text(req.body); });
	rn_router.get("/body/64k", [&body_64k](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(body_64k); });
	auto plain_rn = start_server(bench_config(std::thread::hardware_concurrency()), std::move(rn_router));

	// static file serving
	TempDir static_root{std::filesystem::temp_directory_path() / std::format("conflux_bench_static_{}", ::getpid())};
	auto const &static_dir = static_root.path;
	{
		auto write_file = [&](std::string_view name, std::size_t size, char fill) {
			auto path = static_dir / name;
			std::ofstream out{path, std::ios::binary};
			std::string data(size, fill);
			out.write(data.data(), static_cast<std::streamsize>(data.size()));
		};
		write_file("1k.txt", 1024, 'S');
		write_file("64k.txt", 65536, 'M');
		write_file("1m.bin", 1048576, 'L');
	}
	Router static_router;
	auto static_cfg = bench_config();
	static_cfg.splice_pipe_pairs = 2;
	static_router.serve_static("/", std::string{static_dir.string()});
	auto static_srv = start_server(static_cfg, std::move(static_router));

	// stress configs
	Router sr32_router;
	sr32_router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::html("<html><body>ok</body></html>"); });
	sr32_router.get("/api/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"status":"ok"})"); });
	sr32_router.get("/body/1m", [&body_1m](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(body_1m); });
	sr32_router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) { return conflux::http::Response::text(req.body); });
	auto small_ring_32 = start_server(bench_config(1, 32), std::move(sr32_router));

	Router sr64_router;
	sr64_router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::html("<html><body>ok</body></html>"); });
	sr64_router.get("/api/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"status":"ok"})"); });
	sr64_router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) { return conflux::http::Response::text(req.body); });
	auto small_ring_64 = start_server(bench_config(1, 64), std::move(sr64_router));

	Router br64_router;
	br64_router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::html("<html><body>ok</body></html>"); });
	br64_router.get("/api/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"status":"ok"})"); });
	br64_router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) { return conflux::http::Response::text(req.body); });
	br64_router.get("/body/64k", [&body_64k](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(body_64k); });
	auto small_buf_ring_64 = start_server(bench_config(1, 16), std::move(br64_router));

	Router br128_router;
	br128_router.get("/body/64k", [&body_64k](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(body_64k); });
	auto small_buf_ring_128 = start_server(bench_config(1, 32), std::move(br128_router));

#if CONFLUX_BENCH_HAS_TLS
	struct TempCertFiles {
		std::string cert;
		std::string key;
		~TempCertFiles() {
			if (!cert.empty()) {
				::unlink(cert.c_str());
			}
			if (!key.empty()) {
				::unlink(key.c_str());
			}
		}
		TempCertFiles() = default;
		TempCertFiles(TempCertFiles const &) = delete;
		TempCertFiles &operator =(TempCertFiles const &) = delete;
	};
	// tls — self-signed cert
	TempCertFiles tls_cert_files;
	{
		char cert_tmp[] = "/tmp/conflux_bench_cert_XXXXXX.pem";
		char key_tmp[] = "/tmp/conflux_bench_key_XXXXXX.pem";
		{
			int f = ::mkstemps(cert_tmp, 4);
			::close(f);
		}
		{
			int f = ::mkstemps(key_tmp, 4);
			::close(f);
		}
		auto cmd = std::format(
			"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} -days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
			key_tmp,
			cert_tmp);
		if (::system(cmd.c_str()) != 0) {
			throw std::runtime_error{"openssl req failed — TLS bench requires openssl CLI"};
		}
		tls_cert_files.cert = cert_tmp;
		tls_cert_files.key = key_tmp;
	}
	Router tls_router;
	tls_router.get("/", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::html("<html><body><h1>Hello from conflux!</h1></body></html>");
	});
	tls_router.get("/api/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::json(R"({"status":"ok"})"); });
	tls_router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) { return conflux::http::Response::text(req.body); });
	tls_router.get("/body/64k", [&body_64k](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(body_64k); });
	auto tls_cfg = bench_config();
	tls_cfg.cert_file = tls_cert_files.cert;
	tls_cfg.key_file = tls_cert_files.key;
	auto tls_srv = start_server(tls_cfg, std::move(tls_router));
#endif

	// ── Shared state for variants ───────────────────────────────────────

	std::array<char, 8192> small_buf{};
	auto sb = std::span<char>{small_buf};
	std::vector<char> large_buf(1200000);
	auto lb = std::span<char>{large_buf};

	// Pre-warm cache
	{
		BenchClient c{cache.port};
		c.send_all(kGetCacheHit);
		(void)c.recv_response(sb);
	}

	// Get ETag for etag_hit variant
	std::string etag_value;
	{
		BenchClient c{etag.port};
		c.send_all(kGetEtag);
		auto n = c.recv_response(sb);
		std::string_view resp{small_buf.data(), n};
		auto pos = resp.find("ETag: ");
		if (pos != std::string_view::npos) {
			pos += 6;
			auto end = resp.find("\r\n", pos);
			etag_value = std::string{resp.substr(pos, end - pos)};
		}
	}
	auto const kGetEtagHit =
		std::format("GET /content HTTP/1.1\r\nHost: localhost\r\nIf-None-Match: {}\r\n\r\n", etag_value);

	// ── Define variants ─────────────────────────────────────────────────

	std::unique_ptr<BenchClient> client;
	std::atomic<std::uint64_t> cache_miss_seq{0};

	std::vector<Variant> variants;

	// Core transport — plain_r1
	auto plain_setup = [&] { client = std::make_unique<BenchClient>(plain.port); };
	auto plain_teardown = [&] { client.reset(); };

	variants.push_back(
		{.name = "get_html"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetRoot);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "get_json"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetJson);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "context_exact"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetContext);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "get_param"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetParam);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "post_echo_64"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(post_64);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "post_echo_4k"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(post_4k);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "post_echo_64k"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(post_64k);
				 (void)client->recv_response(lb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "get_group"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetGroup);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "get_404"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGet404);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "keepalive_10"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 for (int i = 0; i < 10; ++i) {
					 client->send_all(kGetJson);
					 (void)client->recv_response(sb);
				 }
			 },
		 .teardown = plain_teardown,
		 .ops_per_iter = 10});
	variants.push_back(
		{.name = "connect_close"sv,
		 .setup = {},
		 .run =
			 [&] {
				 BenchClient c{plain.port};
				 c.send_all(kGetRoot);
				 (void)c.recv_response(sb);
			 },
		 .teardown = {}});

	// Large bodies
	variants.push_back(
		{.name = "get_body_8k"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetBody8k);
				 (void)client->recv_response(lb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "get_body_64k"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetBody64k);
				 (void)client->recv_response(lb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "get_body_1m"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetBody1m);
				 (void)client->recv_response(lb);
			 },
		 .teardown = plain_teardown});

	// Pipelining
	variants.push_back(
		{.name = "pipeline_10"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 for (int i = 0; i < 10; ++i) {
					 client->send_all(kGetJson);
				 }
				 (void)client->recv_n_responses(lb, 10);
			 },
		 .teardown = plain_teardown,
		 .ops_per_iter = 10});
	variants.push_back(
		{.name = "pipeline_100"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 for (int i = 0; i < 100; ++i) {
					 client->send_all(kGetJson);
				 }
				 (void)client->recv_n_responses(lb, 100);
			 },
		 .teardown = plain_teardown,
		 .ops_per_iter = 100});

	// Middleware — compress
	variants.push_back(
		{.name = "mw_compress_gzip"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(compress.port); },
		 .run =
			 [&] {
				 client->send_all(kGetBigGzip);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "mw_compress_passthrough"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(compress.port); },
		 .run =
			 [&] {
				 client->send_all(kGetBigNoEnc);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});

	// security
	variants.push_back(
		{.name = "mw_security"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(security.port); },
		 .run =
			 [&] {
				 client->send_all(kGetSecurity);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});

	// cors
	variants.push_back(
		{.name = "mw_cors_origin"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(cors.port); },
		 .run =
			 [&] {
				 client->send_all(kGetCorsOrigin);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "mw_cors_passthrough"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(cors.port); },
		 .run =
			 [&] {
				 client->send_all(kGetCorsNone);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});

	// auth
	variants.push_back(
		{.name = "mw_auth_bearer"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(auth.port); },
		 .run =
			 [&] {
				 client->send_all(kGetAuthBearer);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "mw_auth_reject"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(auth.port); },
		 .run =
			 [&] {
				 client->send_all(kGetAuthMissing);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});

	// etag
	variants.push_back(
		{.name = "mw_etag_miss"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(etag.port); },
		 .run =
			 [&] {
				 client->send_all(kGetEtag);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "mw_etag_hit"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(etag.port); },
		 .run =
			 [&] {
				 client->send_all(kGetEtagHit);
				 (void)client->recv_response_no_body(sb);
			 },
		 .teardown = plain_teardown});

	// cache
	variants.push_back(
		{.name = "mw_cache_hit"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(cache.port); },
		 .run =
			 [&] {
				 client->send_all(kGetCacheHit);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "mw_cache_miss"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(cache.port); },
		 .run =
			 [&] {
				 auto req = std::format(
					 "GET /counted?v={} HTTP/1.1\r\nHost: localhost\r\n\r\n",
					 cache_miss_seq.fetch_add(1, std::memory_order_relaxed));
				 client->send_all(req);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});

	// full_stack
	variants.push_back(
		{.name = "mw_full_stack"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(full_stack.port); },
		 .run =
			 [&] {
				 client->send_all(kGetFullStack);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});

	// ── Chunked TE ─────────────────────────────────────────────────────

	auto const chunked_64 = make_chunked_single("/api/echo-body", 64);
	auto const chunked_4k_many = make_chunked_many("/api/echo-body", 4096, 64);
	auto const chunked_split_hdr =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"s;
	auto const chunked_split_c1 = "20\r\n"s + std::string(32, 'X') + "\r\n";
	auto const chunked_split_c2 = "20\r\n"s + std::string(32, 'Y') + "\r\n";
	auto const chunked_split_end = "0\r\n\r\n"s;

	variants.push_back(
		{.name = "post_chunked_64"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(chunked_64);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "post_chunked_4k_many"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(chunked_4k_many);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "post_chunked_split"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(chunked_split_hdr);
				 client->send_all(chunked_split_c1);
				 client->send_all(chunked_split_c2);
				 client->send_all(chunked_split_end);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});

	// ── Error / malformed ──────────────────────────────────────────────

	auto const header_too_large = make_header_too_large();
	auto const body_too_large_hdrs = make_body_too_large_headers();

	auto error_setup = [&] {
		client = std::make_unique<BenchClient>(plain.port);
		client->set_recv_timeout(std::chrono::seconds{2});
	};
	auto error_run_reconnect = [&](std::string_view req) {
		return [&, req] {
			client->send_all(req);
			(void)client->recv_response(sb);
			client->reconnect(plain.port);
			client->set_recv_timeout(std::chrono::seconds{2});
		};
	};

	variants.push_back(
		{.name = "bad_request_line"sv,
		 .setup = error_setup,
		 .run = error_run_reconnect(kBadRequestLine),
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "header_too_large"sv,
		 .setup = error_setup,
		 .run = error_run_reconnect(header_too_large),
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "body_too_large"sv,
		 .setup = error_setup,
		 .run =
			 [&] {
				 client->send_all(body_too_large_hdrs);
				 (void)client->recv_response(sb);
				 client->reconnect(plain.port);
				 client->set_recv_timeout(std::chrono::seconds{2});
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "connection_close"sv,
		 .setup = error_setup,
		 .run = error_run_reconnect(kConnectionClose),
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "http_10_close"sv,
		 .setup = error_setup,
		 .run = error_run_reconnect(kHttp10Close),
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "client_early_close"sv,
		 .setup = {},
		 .run =
			 [&] {
				 BenchClient c{plain.port};
				 c.send_partial(kGetRoot, 10);
			 },
		 .teardown = {}});
	variants.push_back(
		{.name = "duplicate_content_length"sv,
		 .setup = error_setup,
		 .run = error_run_reconnect(kDuplicateCL),
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "conflicting_content_length"sv,
		 .setup = error_setup,
		 .run = error_run_reconnect(kConflictingCL),
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "te_plus_content_length"sv,
		 .setup = error_setup,
		 .run = error_run_reconnect(kTEPlusCL),
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "obs_fold_header"sv,
		 .setup = error_setup,
		 .run = error_run_reconnect(kObsFold),
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "invalid_header_whitespace"sv,
		 .setup = error_setup,
		 .run = error_run_reconnect(kInvalidHeaderWS),
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "invalid_te_value"sv,
		 .setup = error_setup,
		 .run = error_run_reconnect(kInvalidTE),
		 .teardown = plain_teardown});

	// ── Malformed pipelined ────────────────────────────────────────────

	auto const pipe_good_bad_good =
		std::string{kGetJson} + "GET@X / HTTP/1.1\r\nHost: localhost\r\n\r\n" + std::string{kGetJson};
	auto const pipe_bad_after_good = std::string{kGetJson} + "GET@X / HTTP/1.1\r\nHost: localhost\r\n\r\n";
	auto const pipe_oversized_header = std::string{kGetJson} + header_too_large;

	variants.push_back(
		{.name = "pipeline_good_bad_good"sv,
		 .setup = error_setup,
		 .run =
			 [&] {
				 client->send_all(pipe_good_bad_good);
				 (void)client->recv_until_close(lb);
				 client->reconnect(plain.port);
				 client->set_recv_timeout(std::chrono::seconds{2});
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "pipeline_bad_after_good"sv,
		 .setup = error_setup,
		 .run =
			 [&] {
				 client->send_all(pipe_bad_after_good);
				 (void)client->recv_until_close(lb);
				 client->reconnect(plain.port);
				 client->set_recv_timeout(std::chrono::seconds{2});
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "pipeline_oversized_header"sv,
		 .setup = error_setup,
		 .run =
			 [&] {
				 client->send_all(pipe_oversized_header);
				 (void)client->recv_until_close(lb);
				 client->reconnect(plain.port);
				 client->set_recv_timeout(std::chrono::seconds{2});
			 },
		 .teardown = plain_teardown});

	// ── Half-close ─────────────────────────────────────────────────────

	variants.push_back(
		{.name = "client_shutdown_wr_after_req"sv,
		 .setup = error_setup,
		 .run =
			 [&] {
				 client->send_all(kGetJson);
				 client->shutdown_wr();
				 (void)client->recv_response(sb);
				 client->reconnect(plain.port);
				 client->set_recv_timeout(std::chrono::seconds{2});
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "client_shutdown_wr_mid_body"sv,
		 .setup = {},
		 .run =
			 [&] {
				 BenchClient c{plain.port};
				 c.set_recv_timeout(std::chrono::seconds{2});
				 c.send_partial(post_4k, 60);
				 c.shutdown_wr();
				 (void)c.recv_response(sb);
			 },
		 .teardown = {}});

	// ── Backpressure / slow reader ─────────────────────────────────────

	variants.push_back(
		{.name = "slow_reader_64k"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetBody64k);
				 (void)client->recv_slow(lb, 1024, std::chrono::microseconds{1000});
			 },
		 .teardown = plain_teardown,
		 .iters_override = 50});
	variants.push_back(
		{.name = "slow_reader_1m"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetBody1m);
				 (void)client->recv_slow(lb, 1024, std::chrono::microseconds{1000});
			 },
		 .teardown = plain_teardown,
		 .iters_override = 10});
	variants.push_back(
		{.name = "client_stalls_after_headers"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetBody64k);
				 auto hdr_n = client->recv_response_no_body(sb);
				 std::this_thread::sleep_for(std::chrono::milliseconds{50});
				 std::string_view hdrs{small_buf.data(), hdr_n};
				 auto hdr_end = hdrs.find("\r\n\r\n");
				 std::size_t body_len = 0;
				 if (auto cl = hdrs.find("Content-Length: "); cl != std::string_view::npos) {
					 cl += 16;
					 auto end = hdrs.find("\r\n", cl);
					 std::from_chars(small_buf.data() + cl, small_buf.data() + end, body_len);
				 }
				 auto already = hdr_n - (hdr_end + 4);
				 std::size_t remaining = body_len > already ? body_len - already : 0;
				 while (remaining > 0) {
					 auto n = ::recv(client->fd, lb.data(), std::min(remaining, lb.size()), 0);
					 if (n <= 0) {
						 break;
					 }
					 remaining -= static_cast<std::size_t>(n);
				 }
			 },
		 .teardown = plain_teardown,
		 .iters_override = 50});
	variants.push_back(
		{.name = "client_close_during_send"sv,
		 .setup = {},
		 .run =
			 [&] {
				 BenchClient c{plain.port};
				 c.send_all(kGetBody1m);
				 std::array<char, 4096> tmp{};
				 (void)::recv(c.fd, tmp.data(), tmp.size(), 0);
			 },
		 .teardown = {}});

	// ── SSE ────────────────────────────────────────────────────────────

	static auto const kGetSSE = "GET /events HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n"sv;

	variants.push_back(
		{.name = "sse_3_events"sv,
		 .setup = plain_setup,
		 .run =
			 [&] {
				 client->send_all(kGetSSE);
				 (void)client->recv_until_close(lb);
				 client->reconnect(plain.port);
			 },
		 .teardown = plain_teardown});

	// sse cancel — connect, start SSE, close after first recv
	variants.push_back(
		{.name = "sse_client_disconnect"sv,
		 .setup = {},
		 .run =
			 [&] {
				 BenchClient c{plain.port};
				 c.send_all(kGetSSE);
				 std::array<char, 512> tmp{};
				 (void)::recv(c.fd, tmp.data(), tmp.size(), 0);
			 },
		 .teardown = {}});

	// ── WebSocket ──────────────────────────────────────────────────────

	auto ws_upgrade_req = [](std::uint16_t port) -> std::pair<BenchClient, std::string> {
		BenchClient c{port};
		c.send_all(
			"GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"sv);
		std::array<char, 4096> buf{};
		std::size_t total = 0;
		for (;;) {
			auto n = ::recv(c.fd, buf.data() + total, buf.size() - total, 0);
			if (n <= 0) {
				break;
			}
			total += static_cast<std::size_t>(n);
			std::string_view sofar{buf.data(), total};
			if (sofar.find("\r\n\r\n") != std::string_view::npos) {
				break;
			}
		}
		std::string resp{buf.data(), total};
		return {std::move(c), std::move(resp)};
	};

	auto ws_build_masked_text = [](std::string_view payload) -> std::string {
		std::string frame;
		frame += static_cast<char>(0x81);
		if (payload.size() < 126) {
			frame += static_cast<char>(0x80 | static_cast<std::uint8_t>(payload.size()));
		} else {
			frame += static_cast<char>(0x80 | 126);
			frame += static_cast<char>((payload.size() >> 8) & 0xff);
			frame += static_cast<char>(payload.size() & 0xff);
		}
		std::array<std::uint8_t, 4> mask{0x12, 0x34, 0x56, 0x78};
		frame.append(reinterpret_cast<char const *>(mask.data()), 4);
		for (std::size_t i = 0; i < payload.size(); ++i) {
			frame += static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i & 3]);
		}
		return frame;
	};

	auto const ws_text_frame = ws_build_masked_text("hello bench"sv);
	auto const ws_close_frame = "\x88\x82\x00\x00\x00\x00\x03\xe8"s;

	variants.push_back(
		{.name = "ws_upgrade"sv,
		 .setup = {},
		 .run =
			 [&] {
				 auto [c, resp] = ws_upgrade_req(plain.port);
				 (void)resp;
				 c.send_all(ws_close_frame);
				 std::array<char, 256> rbuf{};
				 (void)::recv(c.fd, rbuf.data(), rbuf.size(), 0);
			 },
		 .teardown = {}});
	variants.push_back(
		{.name = "ws_echo_100"sv,
		 .setup = {},
		 .run =
			 [&] {
				 auto [c, resp] = ws_upgrade_req(plain.port);
				 std::array<char, 4096> rbuf{};
				 for (int i = 0; i < 100; ++i) {
					 c.send_all(ws_text_frame);
					 (void)::recv(c.fd, rbuf.data(), rbuf.size(), 0);
				 }
				 c.send_all(ws_close_frame);
				 (void)::recv(c.fd, rbuf.data(), rbuf.size(), 0);
			 },
		 .teardown = {},
		 .ops_per_iter = 100});
	variants.push_back(
		{.name = "ws_client_disconnect"sv,
		 .setup = {},
		 .run =
			 [&] {
				 auto [c, resp] = ws_upgrade_req(plain.port);
				 c.send_all(ws_text_frame);
				 std::array<char, 256> rbuf{};
				 (void)::recv(c.fd, rbuf.data(), rbuf.size(), 0);
			 },
		 .teardown = {}});

	// ── Static file ────────────────────────────────────────────────────

	static auto const kGetStatic1k = "GET /1k.txt HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
	static auto const kGetStatic64k = "GET /64k.txt HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
	static auto const kGetStatic1m = "GET /1m.bin HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
	static auto const kGetStaticRange = "GET /64k.txt HTTP/1.1\r\nHost: localhost\r\nRange: bytes=0-1023\r\n\r\n"sv;

	auto static_setup = [&] { client = std::make_unique<BenchClient>(static_srv.port); };

	// pre-warm and get ETag for 1k.txt
	std::string static_etag;
	{
		BenchClient c{static_srv.port};
		c.send_all(kGetStatic1k);
		auto n = c.recv_response(sb);
		std::string_view resp{small_buf.data(), n};
		auto pos = resp.find("ETag: ");
		if (pos != std::string_view::npos) {
			pos += 6;
			auto end = resp.find("\r\n", pos);
			static_etag = std::string{resp.substr(pos, end - pos)};
		}
	}
	auto const kGetStaticEtagHit =
		std::format("GET /1k.txt HTTP/1.1\r\nHost: localhost\r\nIf-None-Match: {}\r\n\r\n", static_etag);

	variants.push_back(
		{.name = "static_1k"sv,
		 .setup = static_setup,
		 .run =
			 [&] {
				 client->send_all(kGetStatic1k);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "static_64k"sv,
		 .setup = static_setup,
		 .run =
			 [&] {
				 client->send_all(kGetStatic64k);
				 (void)client->recv_response(lb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "static_1m"sv,
		 .setup = static_setup,
		 .run =
			 [&] {
				 client->send_all(kGetStatic1m);
				 (void)client->recv_response(lb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "static_etag_hit"sv,
		 .setup = static_setup,
		 .run =
			 [&] {
				 client->send_all(kGetStaticEtagHit);
				 (void)client->recv_response_no_body(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "static_range"sv,
		 .setup = static_setup,
		 .run =
			 [&] {
				 client->send_all(kGetStaticRange);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});

	// ── Deferred (WorkPool roundtrip) ──────────────────────────────────

	static auto const kGetDefer = "GET /api/defer-ok HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;

	variants.push_back(
		{.name = "deferred_ok"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(deferred.port); },
		 .run =
			 [&] {
				 client->send_all(kGetDefer);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "deferred_timeout_cancel"sv,
		 .setup = {},
		 .run =
			 [&] {
				 BenchClient c{deferred.port};
				 c.send_all(kGetDefer);
			 },
		 .teardown = {}});
	variants.push_back(
		{.name = "deferred_many_inflight"sv,
		 .setup = {},
		 .run =
			 [&] {
				 std::vector<BenchClient> clients;
				 clients.reserve(8);
				 for (int i = 0; i < 8; ++i) {
					 clients.emplace_back(deferred.port);
					 clients.back().send_all(kGetDefer);
				 }
				 std::array<char, 4096> rbuf{};
				 for (auto &c: clients) {
					 (void)c.recv_response(std::span{rbuf});
				 }
			 },
		 .teardown = {}});

	// ── Multi-ring (plain_rN) ──────────────────────────────────────────

	static auto const kGetJsonRN = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
	auto const post_4k_rn = make_post_request("/api/echo-body", 4096);
	static auto const kGetBody64kRN = "GET /body/64k HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;

	auto rn_setup = [&] { client = std::make_unique<BenchClient>(plain_rn.port); };
	variants.push_back(
		{.name = "rN_get_json"sv,
		 .setup = rn_setup,
		 .run =
			 [&] {
				 client->send_all(kGetJsonRN);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "rN_post_echo_4k"sv,
		 .setup = rn_setup,
		 .run =
			 [&] {
				 client->send_all(post_4k_rn);
				 (void)client->recv_response(sb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "rN_connect_close"sv,
		 .setup = {},
		 .run =
			 [&] {
				 BenchClient c{plain_rn.port};
				 c.send_all(kGetJsonRN);
				 (void)c.recv_response(sb);
			 },
		 .teardown = {}});
	variants.push_back(
		{.name = "rN_get_body_64k"sv,
		 .setup = rn_setup,
		 .run =
			 [&] {
				 client->send_all(kGetBody64kRN);
				 (void)client->recv_response(lb);
			 },
		 .teardown = plain_teardown});

#if CONFLUX_BENCH_HAS_TLS
	// ── TLS ────────────────────────────────────────────────────────────
	struct SslCtxDeleter {
		void operator ()(
			SSL_CTX *p) const noexcept {
			if (p) {
				SSL_CTX_free(p);
			}
		}
	};
	std::unique_ptr<SSL_CTX, SslCtxDeleter> ctx{[] {
		SSL_CTX *c = SSL_CTX_new(TLS_client_method());
		SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);
		SSL_CTX_set_session_cache_mode(c, SSL_SESS_CACHE_OFF);
		return c;
	}()};

	auto tls_send_recv = [](SSL *ssl, std::string_view req, std::span<char> buf) -> std::size_t {
		SSL_write(ssl, req.data(), static_cast<int>(req.size()));
		std::size_t total = 0;
		std::size_t hdr_end_pos = std::string_view::npos;
		std::size_t body_len = 0;
		bool have_cl = false;
		for (;;) {
			auto n = SSL_read(ssl, buf.data() + total, static_cast<int>(buf.size() - total));
			if (n <= 0) {
				break;
			}
			total += static_cast<std::size_t>(n);
			if (hdr_end_pos == std::string_view::npos) {
				std::string_view sofar{buf.data(), total};
				hdr_end_pos = sofar.find("\r\n\r\n");
				if (hdr_end_pos == std::string_view::npos) {
					continue;
				}
				hdr_end_pos += 4;
				std::string_view hdrs{buf.data(), hdr_end_pos};
				auto cl = hdrs.find("Content-Length: ");
				if (cl != std::string_view::npos) {
					cl += 16;
					auto end = hdrs.find("\r\n", cl);
					std::from_chars(buf.data() + cl, buf.data() + end, body_len);
					have_cl = true;
				}
			}
			if (have_cl && total >= hdr_end_pos + body_len) {
				return total;
			}
			if (!have_cl && hdr_end_pos != std::string_view::npos) {
				return total;
			}
		}
		return total;
	};

	auto tls_connect = [&](std::uint16_t port) -> std::pair<BenchClient, SSL *> {
		auto c = std::make_unique<BenchClient>(port);
		SSL *ssl = SSL_new(ctx.get());
		SSL_set_fd(ssl, c->fd);
		if (SSL_connect(ssl) != 1) {
			SSL_free(ssl);
			throw std::runtime_error{"SSL_connect failed"};
		}
		return {std::move(*c), ssl};
	};
	struct SslDeleter {
		void operator ()(
			SSL *p) const noexcept {
			SSL_shutdown(p);
			SSL_free(p);
		}
	};
	std::unique_ptr<SSL, SslDeleter> tls_ssl;
	std::unique_ptr<BenchClient> tls_client;

	auto tls_setup = [&] {
		auto [c, ssl] = tls_connect(tls_srv.port);
		tls_client = std::make_unique<BenchClient>(std::move(c));
		tls_ssl.reset(ssl);
	};
	auto tls_teardown = [&] {
		tls_ssl.reset();
		tls_client.reset();
	};

	static auto const kGetJsonTLS = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
	auto const post_4k_tls = make_post_request("/api/echo-body", 4096);
	static auto const kGetBody64kTLS = "GET /body/64k HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;

	variants.push_back(
		{.name = "tls_get_json"sv,
		 .setup = tls_setup,
		 .run = [&] { (void)tls_send_recv(tls_ssl.get(), kGetJsonTLS, sb); },
		 .teardown = tls_teardown});
	variants.push_back(
		{.name = "tls_post_echo_4k"sv,
		 .setup = tls_setup,
		 .run = [&] { (void)tls_send_recv(tls_ssl.get(), post_4k_tls, sb); },
		 .teardown = tls_teardown});
	variants.push_back(
		{.name = "tls_get_body_64k"sv,
		 .setup = tls_setup,
		 .run = [&] { (void)tls_send_recv(tls_ssl.get(), kGetBody64kTLS, lb); },
		 .teardown = tls_teardown});
	variants.push_back(
		{.name = "tls_handshake"sv,
		 .setup = {},
		 .run =
			 [&] {
				 auto [c, ssl] = tls_connect(tls_srv.port);
				 (void)tls_send_recv(ssl, kGetJsonTLS, sb);
				 SSL_shutdown(ssl);
				 SSL_free(ssl);
			 },
		 .teardown = {}});
#endif

	// ── Stress / SQ ring pressure ──────────────────────────────────────

	variants.push_back(
		{.name = "sr32_pipeline_100"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(small_ring_32.port); },
		 .run =
			 [&] {
				 for (int i = 0; i < 100; ++i) {
					 client->send_all(kGetJson);
				 }
				 (void)client->recv_n_responses(lb, 100);
			 },
		 .teardown = plain_teardown,
		 .ops_per_iter = 100});
	variants.push_back(
		{.name = "sr32_get_body_1m"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(small_ring_32.port); },
		 .run =
			 [&] {
				 client->send_all(kGetBody1m);
				 (void)client->recv_response(lb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "sr64_connect_close"sv,
		 .setup = {},
		 .run =
			 [&] {
				 BenchClient c{small_ring_64.port};
				 c.send_all(kGetJson);
				 (void)c.recv_response(sb);
			 },
		 .teardown = {}});

	// ── Stress / buffer ring pressure ──────────────────────────────────

	auto const post_64k_br = make_post_request("/api/echo-body", 65536);

	variants.push_back(
		{.name = "br64_post_echo_64k"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(small_buf_ring_64.port); },
		 .run =
			 [&] {
				 client->send_all(post_64k_br);
				 (void)client->recv_response(lb);
			 },
		 .teardown = plain_teardown});
	variants.push_back(
		{.name = "br64_pipeline_100"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(small_buf_ring_64.port); },
		 .run =
			 [&] {
				 for (int i = 0; i < 100; ++i) {
					 client->send_all(kGetJson);
				 }
				 (void)client->recv_n_responses(lb, 100);
			 },
		 .teardown = plain_teardown,
		 .ops_per_iter = 100});
	variants.push_back(
		{.name = "br128_get_body_64k"sv,
		 .setup = [&] { client = std::make_unique<BenchClient>(small_buf_ring_128.port); },
		 .run =
			 [&] {
				 client->send_all(kGetBody64k);
				 (void)client->recv_response(lb);
			 },
		 .teardown = plain_teardown});

	// ── Run all variants ────────────────────────────────────────────────

	if (!json) {
		std::println("http_server_bench: {} iterations, {} warmup\n", iters, warmup);
	}

	bool variant_failed = false;
	for (auto const &v: variants) {
		if (!only_variant.empty() && v.name != only_variant) {
			continue;
		}
		try {
			auto const stats = run_variant(v, iters, warmup, config_name);
			print_variant(stats, json, v.ops_per_iter);
		} catch (std::exception const &e) {
			variant_failed = true;
			if (json) {
				std::println(std::cerr, "error in {}: {}", v.name, e.what());
			} else {
				std::println("  {:<32} ERROR: {}", v.name, e.what());
			}
		}
	}

	// ── Shutdown ────────────────────────────────────────────────────────

	plain.server->shutdown();
	compress.server->shutdown();
	security.server->shutdown();
	cors.server->shutdown();
	auth.server->shutdown();
	etag.server->shutdown();
	cache.server->shutdown();
	full_stack.server->shutdown();
	deferred.server->shutdown();
	plain_rn.server->shutdown();
	static_srv.server->shutdown();
	small_ring_32.server->shutdown();
	small_ring_64.server->shutdown();
	small_buf_ring_64.server->shutdown();
	small_buf_ring_128.server->shutdown();
#if CONFLUX_BENCH_HAS_TLS
	tls_srv.server->shutdown();
#endif

	plain.thr.join();
	compress.thr.join();
	security.thr.join();
	cors.thr.join();
	auth.thr.join();
	etag.thr.join();
	cache.thr.join();
	full_stack.thr.join();
	deferred.thr.join();
	plain_rn.thr.join();
	static_srv.thr.join();
	small_ring_32.thr.join();
	small_ring_64.thr.join();
	small_buf_ring_64.thr.join();
	small_buf_ring_128.thr.join();
#if CONFLUX_BENCH_HAS_TLS
	tls_srv.thr.join();
#endif

	defer_pool->drain_and_stop();
	defer_pool->wait();
	std::filesystem::remove_all(static_dir);
	return variant_failed ? 1 : 0;
}
