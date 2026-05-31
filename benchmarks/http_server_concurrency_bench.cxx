#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import bench_common;

using namespace std::literals;
using conflux::http::Config;
using conflux::http::HttpServerMetrics;
namespace {

struct TimeoutError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

[[nodiscard]] bool would_block_errno(
	int e) noexcept {
	return e == EAGAIN
#if EWOULDBLOCK != EAGAIN
		|| e == EWOULDBLOCK
#endif
		;
}

// ── CPU pinning ────────────────────────────────────────────────────────────

[[nodiscard]] unsigned nproc() {
	return std::thread::hardware_concurrency();
}
void pin_thread(
	unsigned cpu) {
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}
// ── BenchClient (blocking TCP) ─────────────────────────────────────────────

struct BenchClient {
	int fd = -1;
	static void configure_socket(
		int sock) {
		static constexpr int one = 1;
		::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		::setsockopt(sock, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof one);
		timeval tv{};
		tv.tv_sec = 5;
		::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	}
	explicit BenchClient(
		std::uint16_t port) {
		fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) {
			throw std::runtime_error{"socket failed"};
		}
		configure_socket(fd);
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
			if (n < 0 && would_block_errno(errno)) {
				throw TimeoutError{"send timeout"};
			}
			if (n <= 0) {
				throw std::runtime_error{"send failed"};
			}
			p += n;
			remaining -= static_cast<std::size_t>(n);
		}
	}
	[[nodiscard]] std::size_t recv_response(
		std::span<char> buf) const {
		std::size_t total = 0;
		std::size_t hdr_end_pos = std::string_view::npos;
		std::size_t body_len = 0;
		bool have_cl = false;
		for (;;) {
			auto n = ::recv(fd, buf.data() + total, buf.size() - total, 0);
			if (n < 0 && would_block_errno(errno)) {
				throw TimeoutError{"recv timeout"};
			}
			if (n <= 0) {
				break;
			}
			total += static_cast<std::size_t>(n);
			if (hdr_end_pos == std::string_view::npos) {
				std::string_view const sofar{buf.data(), total};
				hdr_end_pos = sofar.find("\r\n\r\n");
				if (hdr_end_pos == std::string_view::npos) {
					continue;
				}
				hdr_end_pos += 4;
				std::string_view const hdrs{buf.data(), hdr_end_pos};
				auto cl = hdrs.find("Content-Length: ");
				if (cl != std::string_view::npos) {
					cl += 16;
					auto const end = hdrs.find("\r\n", cl);
					std::from_chars(buf.data() + cl, buf.data() + end, body_len);
					have_cl = true;
				}
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
		return total;
	}
	void reconnect(
		std::uint16_t port) {
		close();
		fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) {
			throw std::runtime_error{"socket failed"};
		}
		configure_socket(fd);
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
};
// ── Server helpers ─────────────────────────────────────────────────────────

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
	std::shared_ptr<conflux::http::HttpServer> server;
	std::thread thr;
	std::uint16_t port{};
};
ServerHandle start_server(
	Config cfg,
	conflux::http::Router router) {
	(void)::signal(SIGPIPE, SIG_IGN);
	cfg.startup_banner = false;
	auto srv = std::make_shared<conflux::http::HttpServer>(cfg, std::move(router));
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
// ── Percentile computation ─────────────────────────────────────────────────

struct LatencyStats {
	std::uint64_t p50{};
	std::uint64_t p90{};
	std::uint64_t p99{};
	std::uint64_t p999{};
	std::uint64_t max{};
};
[[nodiscard]] LatencyStats compute_percentiles(
	std::vector<std::uint64_t> &latencies) {
	if (latencies.empty()) {
		return {};
	}
	std::ranges::sort(latencies);
	auto const n = latencies.size();
	auto pct = [&](double p) -> std::uint64_t {
		auto idx = static_cast<std::size_t>(static_cast<double>(n - 1) * p);
		return latencies[idx];
	};
	return {
		.p50 = pct(0.50),
		.p90 = pct(0.90),
		.p99 = pct(0.99),
		.p999 = pct(0.999),
		.max = latencies.back(),
	};
}
// ── Leak detection ─────────────────────────────────────────────────────────

[[nodiscard]] int count_fds() {
	int count = 0;
	auto const dir = std::filesystem::path{"/proc/self/fd"};
	for (auto const &entry: std::ranges::subrange{std::filesystem::directory_iterator{dir}, std::default_sentinel}) {
		if (entry.is_symlink() || entry.exists()) {
			++count;
		}
	}
	return count;
}
[[nodiscard]] long rss_kb() {
	std::ifstream f{"/proc/self/statm"};
	long pages = 0;
	long rss = 0;
	f >> pages >> rss;
	long const page_kb = ::sysconf(_SC_PAGESIZE) > 0 ? ::sysconf(_SC_PAGESIZE) / 1024 : 4;
	return rss * page_kb;
}
struct UsageSnapshot {
	std::uint64_t user_ns{};
	std::uint64_t sys_ns{};
	std::uint64_t voluntary_context_switches{};
	std::uint64_t involuntary_context_switches{};
};
[[nodiscard]] std::uint64_t timeval_to_ns(
	timeval tv) noexcept {
	return static_cast<std::uint64_t>(tv.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(tv.tv_usec) * 1'000ull;
}
[[nodiscard]] UsageSnapshot read_usage() {
	rusage ru{};
	(void)::getrusage(RUSAGE_SELF, &ru);
	return {
		.user_ns = timeval_to_ns(ru.ru_utime),
		.sys_ns = timeval_to_ns(ru.ru_stime),
		.voluntary_context_switches = static_cast<std::uint64_t>(ru.ru_nvcsw),
		.involuntary_context_switches = static_cast<std::uint64_t>(ru.ru_nivcsw),
	};
}
struct ResourceSample {
	long rss_kb_high_water{};
	std::uint64_t pressure_events_high_water{};
	std::uint64_t sq_dropped_high_water{};
	std::uint64_t cq_overflow_high_water{};
};
[[nodiscard]] std::uint64_t pressure_event_total(
	HttpServerMetrics const &m) noexcept {
	auto const &p = m.pressure;
	return p.accept_rejected
		 + p.connections_closed_for_pressure
		 + p.response_backpressure_events
		 + p.sse_dropped_newest
		 + p.sse_dropped_oldest
		 + p.sse_disconnected_for_pressure
		 + p.websocket_closed_for_pressure
		 + p.drain_started
		 + p.drain_deadline_hit
		 + p.drain_forced_close;
}
// ── conflux::http::OwnedRequest strings ────────────────────────────────────────────────────────

static auto const kGetPing = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
static auto const kGetBody64k = "GET /body/64k HTTP/1.1\r\nHost: localhost\r\n\r\n"sv;
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

// ── Variant definition ─────────────────────────────────────────────────────

enum class Mode {
	Keepalive,
	ConnectClose,
};
struct ConcVariant {
	std::string_view name;
	int connections{};
	std::chrono::seconds duration{5};
	Mode mode{Mode::Keepalive};
	std::string_view request{kGetPing};
	bool mixed{false};
	bool is_stress{false};
	bool is_idle{false};
};
// ── Client worker ──────────────────────────────────────────────────────────

struct WorkerResult {
	std::vector<std::uint64_t> latencies;
	std::uint64_t errors{};
	std::uint64_t timeouts{};
};
WorkerResult run_worker(
	std::uint16_t port,
	ConcVariant const &v,
	int conn_count,
	std::atomic<bool> const &stop,
	std::atomic<bool> const &measure,
	std::span<char> buf,
	std::string_view post_req) {
	WorkerResult result;
	result.latencies.reserve(static_cast<std::size_t>(conn_count) * 1000);

	std::vector<BenchClient> clients;
	clients.reserve(static_cast<std::size_t>(conn_count));
	for (int i = 0; i < conn_count; ++i) {
		clients.emplace_back(port);
	}

	if (v.is_idle) {
		while (!stop.load(std::memory_order_relaxed)) {
			std::this_thread::sleep_for(std::chrono::milliseconds{100});
		}
		return result;
	}

	std::atomic<std::uint64_t> mixed_seq{0};
	while (!stop.load(std::memory_order_relaxed)) {
		for (auto &c: clients) {
			try {
				std::string_view req = v.request;
				if (v.mixed) {
					auto seq = mixed_seq.fetch_add(1, std::memory_order_relaxed);
					req = (seq % 10 < 7) ? kGetPing : std::string_view{post_req};
				}

				auto const record = measure.load(std::memory_order_relaxed);
				auto const t0 = record ? bench_now_ns() : 0;

				if (v.mode == Mode::ConnectClose) {
					c.reconnect(port);
				}

				c.send_all(req);
				(void)c.recv_response(buf);

				if (record) {
					auto const t1 = bench_now_ns();
					result.latencies.push_back(t1 - t0);
				}
			} catch (TimeoutError const &) {
				if (measure.load(std::memory_order_relaxed)) {
					++result.timeouts;
				}
				try {
					c.reconnect(port);
				} catch (...) {}
			} catch (...) {
				if (measure.load(std::memory_order_relaxed)) {
					++result.errors;
				}
				try {
					c.reconnect(port);
				} catch (...) {}
			}
		}
	}
	return result;
}
// ── JSON output ────────────────────────────────────────────────────────────

struct RunTelemetry {
	int warmup_s{};
	std::string_view label{"live-kernel-sanity"};
	std::uint64_t errors{};
	std::uint64_t timeouts{};
	int fd_start{-1};
	int fd_end{-1};
	long rss_start{-1};
	long rss_end{-1};
	UsageSnapshot usage_start{};
	UsageSnapshot usage_end{};
	ResourceSample sample{};
	HttpServerMetrics metrics_start{};
	HttpServerMetrics metrics_end{};
};
[[nodiscard]] std::uint64_t saturated_sub(
	std::uint64_t a,
	std::uint64_t b) noexcept {
	return a >= b ? a - b : 0;
}
void emit_result(
	std::string_view config,
	std::string_view variant,
	std::size_t total_requests,
	std::uint64_t total_ns,
	int connections,
	int duration_s,
	LatencyStats const &lat,
	bool pinned,
	bool json,
	RunTelemetry const &t) {
	double const seconds = static_cast<double>(total_ns) / 1e9;
	double const req_per_sec = seconds > 0.0 ? static_cast<double>(total_requests) / seconds : 0.0;
	double const ns_per_iter =
		total_requests > 0 ? static_cast<double>(total_ns) / static_cast<double>(total_requests) : 0.0;
	auto const user_ns = saturated_sub(t.usage_end.user_ns, t.usage_start.user_ns);
	auto const sys_ns = saturated_sub(t.usage_end.sys_ns, t.usage_start.sys_ns);
	auto const cpu_ns = user_ns + sys_ns;
	auto const cpu_util_pct = total_ns > 0 ? static_cast<double>(cpu_ns) * 100.0 / static_cast<double>(total_ns) : 0.0;
	auto const voluntary_cs =
		saturated_sub(t.usage_end.voluntary_context_switches, t.usage_start.voluntary_context_switches);
	auto const involuntary_cs =
		saturated_sub(t.usage_end.involuntary_context_switches, t.usage_start.involuntary_context_switches);
	auto const sq_dropped = saturated_sub(t.metrics_end.sq_dropped, t.metrics_start.sq_dropped);
	auto const cq_overflow = saturated_sub(t.metrics_end.cq_overflow, t.metrics_start.cq_overflow);
	auto const pressure_start = pressure_event_total(t.metrics_start);
	auto const pressure_end = pressure_event_total(t.metrics_end);
	auto const pressure_delta = saturated_sub(pressure_end, pressure_start);
	auto const pressure_high_water_delta = saturated_sub(t.sample.pressure_events_high_water, pressure_start);
	auto const sq_dropped_high_water_delta = saturated_sub(t.sample.sq_dropped_high_water, t.metrics_start.sq_dropped);
	auto const cq_overflow_high_water_delta =
		saturated_sub(t.sample.cq_overflow_high_water, t.metrics_start.cq_overflow);

	if (json) {
		std::string line = std::format(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"label\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":"
			"{:.2f}"
			",\"connections\":{},\"warmup_s\":{},\"duration_s\":{},\"total_requests\":{},\"requests_per_sec\":{:.1f}"
			",\"p50_ns\":{},\"p90_ns\":{},\"p99_ns\":{},\"p999_ns\":{},\"max_ns\":{}"
			",\"errors\":{},\"timeouts\":{},\"pinned\":{}"
			",\"cpu_user_ns\":{},\"cpu_sys_ns\":{},\"cpu_total_ns\":{},\"cpu_util_pct\":{:.2f}"
			",\"voluntary_context_switches\":{},\"involuntary_context_switches\":{}"
			",\"fd_count_start\":{},\"fd_count_end\":{},\"rss_kb_start\":{},\"rss_kb_end\":{},\"rss_kb_high_water\":{}"
			",\"queue_depth_high_water\":{},\"queue_depth_source\":\"pressure-event-counter\""
			",\"pressure_events\":{},\"pressure_events_high_water\":{}"
			",\"sq_dropped\":{},\"sq_dropped_high_water\":{},\"cq_overflow\":{},\"cq_overflow_high_water\":{}"
			"}}",
			config,
			variant,
			t.label,
			total_requests,
			total_ns,
			ns_per_iter,
			connections,
			t.warmup_s,
			duration_s,
			total_requests,
			req_per_sec,
			lat.p50,
			lat.p90,
			lat.p99,
			lat.p999,
			lat.max,
			t.errors,
			t.timeouts,
			pinned ? "true" : "false",
			user_ns,
			sys_ns,
			cpu_ns,
			cpu_util_pct,
			voluntary_cs,
			involuntary_cs,
			t.fd_start,
			t.fd_end,
			t.rss_start,
			t.rss_end,
			t.sample.rss_kb_high_water,
			pressure_high_water_delta,
			pressure_delta,
			pressure_high_water_delta,
			sq_dropped,
			sq_dropped_high_water_delta,
			cq_overflow,
			cq_overflow_high_water_delta);
		std::println("{}", line);
	} else {
		std::println(
			"{:<36} {:>8} reqs  {:>10.0f} req/s  p50={:>7}  p99={:>7}  p999={:>7}  max={:>7} ns  err={} timeout={} "
			"cpu={:.1f}% [{}]",
			variant,
			total_requests,
			req_per_sec,
			lat.p50,
			lat.p99,
			lat.p999,
			lat.max,
			t.errors,
			t.timeouts,
			cpu_util_pct,
			t.label);
		std::println(
			"  warmup={}s duration={}s  cs voluntary/involuntary={}/{}  fd {}→{}  rss {}→{} KB hwm={} KB  pressure={} "
			"sq_drop={} cq_overflow={}",
			t.warmup_s,
			duration_s,
			voluntary_cs,
			involuntary_cs,
			t.fd_start,
			t.fd_end,
			t.rss_start,
			t.rss_end,
			t.sample.rss_kb_high_water,
			pressure_delta,
			sq_dropped,
			cq_overflow);
	}
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"http_server_concurrency","parser":"standard","configs":[{"name":"smoke","extra":{"label":"live-kernel-sanity"},"args":["--duration","1","--warmup","0"],"reps":1},{"name":"tail-proof","extra":{"label":"end-to-end-proof"},"args":["--duration","30","--warmup","5"],"reps":1}]})");

	auto const args = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	auto const json = args.json_out;
	auto const config_name = args.config_name.empty() ? "default"sv : std::string_view{args.config_name};

	int duration_s = 1;
	int warmup_s = -1;
	for (int i = 1; i < argc; ++i) {
		if (std::string_view{argv[i]} == "--duration" && i + 1 < argc) {
			std::from_chars(argv[i + 1], argv[i + 1] + std::strlen(argv[i + 1]), duration_s);
		}
		if (std::string_view{argv[i]} == "--warmup" && i + 1 < argc) {
			std::from_chars(argv[i + 1], argv[i + 1] + std::strlen(argv[i + 1]), warmup_s);
		}
	}
	if (warmup_s < 0) {
		warmup_s = duration_s >= 30 ? 5 : 0;
	}
	bool const serious_row = duration_s >= 30 && warmup_s >= 5;
	auto const row_label = serious_row ? "end-to-end-proof"sv : "live-kernel-sanity"sv;

	auto const np = nproc();
	bool const can_pin = np >= 8;
	auto const half = np / 2;

	// ── Servers ────────────────────────────────────────────────────────

	auto const body_64k = std::string(65536, 'C');
	auto const post_4k = make_post_request("/api/echo-body", 4096);

	auto make_router = [&] {
		conflux::http::Router r;
		r.get("/api/ping", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::json(R"({"status":"ok"})");
		});
		r.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(req.body);
		});
		r.get("/body/64k", [&body_64k](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text(body_64k);
		});
		return r;
	};

	auto r1 = start_server(bench_config(1), make_router());
	auto rn = start_server(bench_config(can_pin ? half : np), make_router());

	// ── Variants ───────────────────────────────────────────────────────

	std::vector<ConcVariant> variants{
		{.name = "parallel_keepalive_32"sv, .connections = 32, .duration = std::chrono::seconds{duration_s}},
		{.name = "parallel_keepalive_256"sv, .connections = 256, .duration = std::chrono::seconds{duration_s}},
		{.name = "parallel_keepalive_1k"sv, .connections = 1024, .duration = std::chrono::seconds{duration_s}},
		{.name = "parallel_connect_close_256"sv,
		 .connections = 256,
		 .duration = std::chrono::seconds{duration_s},
		 .mode = Mode::ConnectClose},
		{.name = "parallel_connect_close_1k"sv,
		 .connections = 1024,
		 .duration = std::chrono::seconds{duration_s},
		 .mode = Mode::ConnectClose},
		{.name = "parallel_post_4k_256"sv,
		 .connections = 256,
		 .duration = std::chrono::seconds{duration_s},
		 .request = std::string_view{post_4k}},
		{.name = "parallel_mixed_256"sv,
		 .connections = 256,
		 .duration = std::chrono::seconds{duration_s},
		 .mixed = true},
		{.name = "parallel_large_body_64"sv,
		 .connections = 64,
		 .duration = std::chrono::seconds{duration_s},
		 .request = kGetBody64k},
		{.name = "idle_keepalive_1k"sv,
		 .connections = 1024,
		 .duration = std::chrono::seconds{duration_s},
		 .is_idle = true},
	};

	if (duration_s >= 30) {
		variants.push_back(
			{.name = "stress_keepalive_256"sv,
			 .connections = 256,
			 .duration = std::chrono::seconds{duration_s},
			 .is_stress = true});
		variants.push_back(
			{.name = "stress_connect_close_256"sv,
			 .connections = 256,
			 .duration = std::chrono::seconds{duration_s},
			 .mode = Mode::ConnectClose,
			 .is_stress = true});
	}
	// ── Run ────────────────────────────────────────────────────────────

	struct ServerConfig {
		std::string_view suffix;
		std::uint16_t port;
		std::shared_ptr<conflux::http::HttpServer> server;
	};
	std::array<ServerConfig, 2> configs{
		{{.suffix = "_r1"sv, .port = r1.port, .server = r1.server},
		 {.suffix = "_rN"sv, .port = rn.port, .server = rn.server}}
    };

	if (!json) {
		std::println(
			"http_server_concurrency_bench: warmup={}s duration={}s label={} pinned={}\n",
			warmup_s,
			duration_s,
			row_label,
			can_pin);
	}

	for (auto const &cfg: configs) {
		for (auto const &v: variants) {
			auto const variant_name = std::format("{}{}", v.name, cfg.suffix);
			auto const dur = v.duration;
			auto const num_threads =
				std::min(static_cast<int>(v.connections), can_pin ? static_cast<int>(half) : static_cast<int>(np));
			auto const conns_per_thread = v.connections / num_threads;
			auto const remainder = v.connections % num_threads;

			std::atomic<bool> stop{false};
			std::atomic<bool> measure{false};
			std::vector<std::thread> workers;
			std::vector<WorkerResult> results(static_cast<std::size_t>(num_threads));

			for (int t = 0; t < num_threads; ++t) {
				int const my_conns = conns_per_thread + (t < remainder ? 1 : 0);
				workers.emplace_back([&, t, my_conns, port = cfg.port] {
					if (can_pin) {
						pin_thread(half + static_cast<unsigned>(t) % half);
					}
					std::vector<char> buf(v.request == kGetBody64k ? 131072 : 8192);
					results[static_cast<std::size_t>(t)] =
						run_worker(port, v, my_conns, stop, measure, std::span{buf}, post_4k);
				});
			}

			if (warmup_s > 0) {
				std::this_thread::sleep_for(std::chrono::seconds{warmup_s});
			}

			RunTelemetry telemetry{.warmup_s = warmup_s, .label = row_label};
			telemetry.fd_start = count_fds();
			telemetry.rss_start = rss_kb();
			telemetry.metrics_start = cfg.server->metrics();
			telemetry.usage_start = read_usage();
			telemetry.sample.rss_kb_high_water = telemetry.rss_start;
			telemetry.sample.pressure_events_high_water = pressure_event_total(telemetry.metrics_start);
			telemetry.sample.sq_dropped_high_water = telemetry.metrics_start.sq_dropped;
			telemetry.sample.cq_overflow_high_water = telemetry.metrics_start.cq_overflow;

			std::atomic<bool> sample_stop{false};
			std::thread sampler{[&] {
				while (!sample_stop.load(std::memory_order_relaxed)) {
					auto const m = cfg.server->metrics();
					telemetry.sample.rss_kb_high_water = std::max(telemetry.sample.rss_kb_high_water, rss_kb());
					telemetry.sample.pressure_events_high_water =
						std::max(telemetry.sample.pressure_events_high_water, pressure_event_total(m));
					telemetry.sample.sq_dropped_high_water =
						std::max(telemetry.sample.sq_dropped_high_water, m.sq_dropped);
					telemetry.sample.cq_overflow_high_water =
						std::max(telemetry.sample.cq_overflow_high_water, m.cq_overflow);
					std::this_thread::sleep_for(std::chrono::milliseconds{50});
				}
			}};

			auto const run_start = bench_now_ns();
			measure.store(true, std::memory_order_release);
			std::this_thread::sleep_for(dur);
			stop.store(true, std::memory_order_relaxed);

			for (auto &w: workers) {
				w.join();
			}

			auto const run_end = bench_now_ns();
			telemetry.usage_end = read_usage();
			telemetry.metrics_end = cfg.server->metrics();
			telemetry.rss_end = rss_kb();
			telemetry.fd_end = count_fds();
			sample_stop.store(true, std::memory_order_relaxed);
			sampler.join();

			auto const total_ns = run_end - run_start;

			std::vector<std::uint64_t> all_latencies;
			std::uint64_t total_errors = 0;
			std::uint64_t total_timeouts = 0;
			for (auto &r: results) {
				all_latencies.insert(all_latencies.end(), r.latencies.begin(), r.latencies.end());
				total_errors += r.errors;
				total_timeouts += r.timeouts;
			}
			telemetry.errors = total_errors;
			telemetry.timeouts = total_timeouts;

			auto const total_requests = all_latencies.size();
			auto lat = compute_percentiles(all_latencies);

			emit_result(
				config_name,
				variant_name,
				total_requests,
				total_ns,
				v.connections,
				static_cast<int>(dur.count()),
				lat,
				can_pin,
				json,
				telemetry);
		}
	}

	// ── Shutdown ───────────────────────────────────────────────────────

	r1.server->shutdown();
	rn.server->shutdown();
	r1.thr.join();
	rn.thr.join();
}
