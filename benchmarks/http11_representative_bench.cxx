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
using namespace std::string_view_literals;
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

[[nodiscard]] unsigned nproc() {
	return std::max(1u, std::thread::hardware_concurrency());
}

void pin_thread(
	unsigned cpu) {
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	(void)sched_setaffinity(0, sizeof(set), &set);
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
		std::filesystem::remove_all(path, ec);
	}

	TempDir(TempDir const &) = delete;
	TempDir &operator =(TempDir const &) = delete;
};

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
		connect_to(port);
	}

	~BenchClient() { close(); }
	BenchClient(BenchClient const &) = delete;
	BenchClient &operator =(BenchClient const &) = delete;

	BenchClient(
		BenchClient &&o) noexcept
		: fd{std::exchange(o.fd, -1)} {}

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

	void connect_to(
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
			fd = -1;
			throw std::runtime_error{"connect failed"};
		}
	}

	void reconnect(
		std::uint16_t port) {
		close();
		connect_to(port);
	}

	void send_all(
		std::string_view data) const {
		auto const *p = data.data();
		auto left = data.size();
		while (left > 0) {
			auto const n = ::send(fd, p, left, MSG_NOSIGNAL);
			if (n < 0 && would_block_errno(errno)) {
				throw TimeoutError{"send timeout"};
			}
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				throw std::runtime_error{"send failed"};
			}
			p += static_cast<std::size_t>(n);
			left -= static_cast<std::size_t>(n);
		}
	}

	[[nodiscard]] std::size_t recv_response(
		std::span<char> buf) const {
		std::size_t total = 0;
		std::size_t hdr_end_pos = std::string_view::npos;
		std::size_t body_len = 0;
		bool have_cl = false;
		for (;;) {
			if (total == buf.size()) {
				throw std::runtime_error{"receive buffer exhausted"};
			}
			auto const n = ::recv(fd, buf.data() + total, buf.size() - total, 0);
			if (n < 0 && would_block_errno(errno)) {
				throw TimeoutError{"recv timeout"};
			}
			if (n < 0 && errno == EINTR) {
				continue;
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
				if (auto cl = hdrs.find("Content-Length: "); cl != std::string_view::npos) {
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
};

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
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
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
	unsigned rings,
	unsigned entries = 256) {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = std::max(1u, rings);
	cfg.ring_entries = entries;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 0;
	cfg.tls_sniff_timeout_ms = 0;
	cfg.startup_banner = false;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 2;
	cfg.send_buffer_slabs = 4;
	cfg.send_buffer_bytes = 16384;
	return cfg;
}

std::string make_medium_json() {
	std::string body = R"({"items":[)";
	for (int i = 0; i < 32; ++i) {
		if (i != 0) {
			body += ',';
		}
		body += std::format(R"({{"id":{},"name":"item-{}","active":true}})", i, i);
	}
	body += R"(],"count":32})";
	return body;
}

std::string make_get(
	std::string_view target,
	std::string_view extra_headers = {}) {
	std::string req;
	req.reserve(128 + target.size() + extra_headers.size());
	req += "GET ";
	req += target;
	req += " HTTP/1.1\r\nHost: localhost\r\n";
	req += extra_headers;
	req += "\r\n";
	return req;
}

std::string make_post(
	std::string_view target,
	std::size_t body_size,
	std::string_view content_type = "text/plain"sv,
	std::string_view extra_headers = {}) {
	std::string body(body_size, 'X');
	return std::format(
		"POST {} HTTP/1.1\r\nHost: localhost\r\nContent-Type: {}\r\nContent-Length: {}\r\n{}\r\n{}",
		target,
		content_type,
		body_size,
		extra_headers,
		body);
}

std::string make_chunked_many(
	std::string_view target,
	std::size_t total_size,
	std::size_t chunk_size) {
	std::string req = std::format("POST {} HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n", target);
	std::string chunk(chunk_size, 'Y');
	for (std::size_t sent = 0; sent < total_size; sent += chunk_size) {
		auto const cs = std::min(chunk_size, total_size - sent);
		req += std::format("{:x}\r\n", cs);
		req.append(chunk.data(), cs);
		req += "\r\n";
	}
	req += "0\r\n\r\n";
	return req;
}

struct RequestSpec {
	std::string name;
	std::string bytes;
	int weight{};
	std::size_t recv_buffer_bytes{8192};
};

enum class ConnectionMode : std::uint8_t {
	keepalive,
	connect_close,
};

struct WorkloadDef {
	std::string name;
	std::string label;
	bool middleware_server{};
	ConnectionMode mode{ConnectionMode::keepalive};
	int default_connections{};
	std::vector<RequestSpec> requests;
};

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
	return {.p50 = pct(0.50), .p90 = pct(0.90), .p99 = pct(0.99), .p999 = pct(0.999), .max = latencies.back()};
}

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
		.involuntary_context_switches = static_cast<std::uint64_t>(ru.ru_nivcsw)};
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

struct RunTelemetry {
	int warmup_s{};
	int fd_start{-1};
	int fd_end{-1};
	long rss_start{};
	long rss_end{};
	ResourceSample sample{};
	UsageSnapshot usage_start{};
	UsageSnapshot usage_end{};
	HttpServerMetrics metrics_start{};
	HttpServerMetrics metrics_end{};
	std::uint64_t errors{};
	std::uint64_t timeouts{};
};

struct WorkerResult {
	std::vector<std::uint64_t> latencies;
	std::vector<std::uint64_t> request_counts;
	std::uint64_t errors{};
	std::uint64_t timeouts{};
};

[[nodiscard]] std::uint64_t next_rng(
	std::uint64_t &state) noexcept {
	state += 0x9e3779b97f4a7c15ull;
	auto z = state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

[[nodiscard]] std::size_t choose_request_index(
	WorkloadDef const &workload,
	int total_weight,
	std::uint64_t &rng) {
	auto pick = static_cast<int>(next_rng(rng) % static_cast<std::uint64_t>(total_weight));
	for (std::size_t i = 0; i < workload.requests.size(); ++i) {
		pick -= workload.requests[i].weight;
		if (pick < 0) {
			return i;
		}
	}
	return workload.requests.size() - 1;
}

WorkerResult run_worker(
	std::uint16_t port,
	WorkloadDef const &workload,
	int conn_count,
	std::atomic<bool> const &stop,
	std::atomic<bool> const &measure,
	std::uint64_t seed) {
	WorkerResult result;
	result.latencies.reserve(static_cast<std::size_t>(conn_count) * 4096);
	result.request_counts.assign(workload.requests.size(), 0);

	std::vector<BenchClient> clients;
	clients.reserve(static_cast<std::size_t>(conn_count));
	for (int i = 0; i < conn_count; ++i) {
		clients.emplace_back(port);
	}

	int total_weight = 0;
	for (auto const &r: workload.requests) {
		total_weight += r.weight;
	}
	std::vector<char> buf;
	std::uint64_t rng = seed;

	while (!stop.load(std::memory_order_relaxed)) {
		for (auto &c: clients) {
			if (stop.load(std::memory_order_relaxed)) {
				break;
			}
			auto const req_index = choose_request_index(workload, total_weight, rng);
			auto const &req = workload.requests[req_index];
			if (buf.size() < req.recv_buffer_bytes) {
				buf.assign(req.recv_buffer_bytes, 0);
			}
			try {
				auto const record = measure.load(std::memory_order_relaxed);
				auto const t0 = record ? bench_now_ns() : 0;
				if (workload.mode == ConnectionMode::connect_close) {
					c.reconnect(port);
				}
				c.send_all(req.bytes);
				(void)c.recv_response(std::span{buf});
				if (record) {
					result.latencies.push_back(bench_now_ns() - t0);
					++result.request_counts[req_index];
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

[[nodiscard]] std::string request_counts_json(
	WorkloadDef const &workload,
	std::span<std::uint64_t const> counts) {
	std::string out = "{";
	for (std::size_t i = 0; i < workload.requests.size(); ++i) {
		if (i != 0) {
			out += ',';
		}
		out += std::format("\"{}\":{}", workload.requests[i].name, counts[i]);
	}
	out += '}';
	return out;
}

void emit_result(
	std::string_view config,
	WorkloadDef const &workload,
	std::uint64_t total_requests,
	std::uint64_t total_ns,
	int connections,
	int duration_s,
	LatencyStats lat,
	std::span<std::uint64_t const> request_counts,
	RunTelemetry const &t,
	bool pinned,
	bool json) {
	auto const ns_per_iter =
		total_requests == 0 ? 0.0 : static_cast<double>(total_ns) / static_cast<double>(total_requests);
	auto const req_per_sec =
		total_ns == 0 ? 0.0 : static_cast<double>(total_requests) * 1e9 / static_cast<double>(total_ns);
	auto const user_ns = t.usage_end.user_ns - t.usage_start.user_ns;
	auto const sys_ns = t.usage_end.sys_ns - t.usage_start.sys_ns;
	auto const cpu_ns = user_ns + sys_ns;
	auto const cpu_util_pct = total_ns == 0 ? 0.0 : 100.0 * static_cast<double>(cpu_ns) / static_cast<double>(total_ns);
	auto const voluntary_cs = t.usage_end.voluntary_context_switches - t.usage_start.voluntary_context_switches;
	auto const involuntary_cs = t.usage_end.involuntary_context_switches - t.usage_start.involuntary_context_switches;
	auto const pressure_start = pressure_event_total(t.metrics_start);
	auto const pressure_end = pressure_event_total(t.metrics_end);
	auto const pressure_delta = pressure_end - pressure_start;
	auto const pressure_high_water_delta = t.sample.pressure_events_high_water - pressure_start;
	auto const sq_dropped = t.metrics_end.sq_dropped - t.metrics_start.sq_dropped;
	auto const cq_overflow = t.metrics_end.cq_overflow - t.metrics_start.cq_overflow;
	auto const sq_dropped_high_water_delta = t.sample.sq_dropped_high_water - t.metrics_start.sq_dropped;
	auto const cq_overflow_high_water_delta = t.sample.cq_overflow_high_water - t.metrics_start.cq_overflow;

	if (json) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"label\":\"{}\",\"iterations\":{},\"total_ns\":{},"
			"\"ns_per_iter\":{:.2f},\"connections\":{},\"warmup_s\":{},\"duration_s\":{},\"requests\":{},"
			"\"requests_per_sec\":{:.1f},\"p50_ns\":{},\"p90_ns\":{},\"p99_ns\":{},\"p999_ns\":{},\"max_ns\":{},"
			"\"errors\":{},\"timeouts\":{},\"pinned\":{},\"cpu_user_ns\":{},\"cpu_sys_ns\":{},\"cpu_total_ns\":{},"
			"\"cpu_util_pct\":{:.2f},\"voluntary_context_switches\":{},\"involuntary_context_switches\":{},"
			"\"fd_count_start\":{},\"fd_count_end\":{},\"rss_kb_start\":{},\"rss_kb_end\":{},\"rss_kb_high_water\":{},"
			"\"pressure_events\":{},\"pressure_events_high_water\":{},\"sq_dropped\":{},\"sq_dropped_high_water\":{},"
			"\"cq_overflow\":{},\"cq_overflow_high_water\":{},\"request_counts\":{}}}",
			config,
			workload.name,
			workload.label,
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
			pressure_delta,
			pressure_high_water_delta,
			sq_dropped,
			sq_dropped_high_water_delta,
			cq_overflow,
			cq_overflow_high_water_delta,
			request_counts_json(workload, request_counts));
		return;
	}

	std::println(
		"{:<28} {:>8} reqs {:>10.0f} req/s p50={:>7} p99={:>7} p999={:>7} max={:>7} ns err={} timeout={} cpu={:.1f}% "
		"[{}]",
		workload.name,
		total_requests,
		req_per_sec,
		lat.p50,
		lat.p99,
		lat.p999,
		lat.max,
		t.errors,
		t.timeouts,
		cpu_util_pct,
		workload.label);
}

std::vector<RequestSpec> make_request_specs() {
	std::string header_rich;
	header_rich += "User-Agent: conflux-bench/1\r\n";
	header_rich += "Accept: application/json\r\n";
	header_rich += "Accept-Language: en-US,en;q=0.8\r\n";
	header_rich += "Cookie: session=abcdef; theme=dark; flags=a,b,c\r\n";
	for (int i = 0; i < 12; ++i) {
		header_rich += std::format("X-Bench-{}: value-{}\r\n", i, i);
	}

	return {
		{.name = "ping", .bytes = make_get("/api/ping"), .weight = 1, .recv_buffer_bytes = 8192},
		{.name = "param", .bytes = make_get("/users/42?include=profile"), .weight = 1, .recv_buffer_bytes = 8192},
		{.name = "headers", .bytes = make_get("/api/ping", header_rich), .weight = 1, .recv_buffer_bytes = 8192},
		{.name = "json_medium", .bytes = make_get("/json/medium"), .weight = 1, .recv_buffer_bytes = 16384},
		{.name = "not_found", .bytes = make_get("/missing/route"), .weight = 1, .recv_buffer_bytes = 8192},
		{.name = "post_4k", .bytes = make_post("/api/echo-body", 4096), .weight = 1, .recv_buffer_bytes = 16384},
		{.name = "post_64k", .bytes = make_post("/api/echo-body", 65536), .weight = 1, .recv_buffer_bytes = 131072},
		{.name = "chunked_4k",
		 .bytes = make_chunked_many("/api/echo-body", 4096, 64),
		 .weight = 1,
		 .recv_buffer_bytes = 16384},
		{.name = "static_1k", .bytes = make_get("/1k.txt"), .weight = 1, .recv_buffer_bytes = 8192},
		{.name = "static_64k", .bytes = make_get("/64k.txt"), .weight = 1, .recv_buffer_bytes = 131072},
		{.name = "static_1m", .bytes = make_get("/1m.bin"), .weight = 1, .recv_buffer_bytes = 1200000},
	};
}

[[nodiscard]] RequestSpec const &by_name(
	std::span<RequestSpec const> specs,
	std::string_view name) {
	auto it = std::ranges::find_if(specs, [name](RequestSpec const &s) { return s.name == name; });
	if (it == specs.end()) {
		throw std::logic_error{"missing request spec"};
	}
	return *it;
}

[[nodiscard]] RequestSpec weighted(
	std::span<RequestSpec const> specs,
	std::string_view name,
	int weight) {
	auto out = by_name(specs, name);
	out.weight = weight;
	return out;
}

std::vector<WorkloadDef> make_workloads(
	std::span<RequestSpec const> specs,
	int connections_override) {
	auto connections = [&](int fallback) { return connections_override > 0 ? connections_override : fallback; };
	std::vector<WorkloadDef> out;
	out.push_back({
		.name = "api_read_plain",
		.label = "end-to-end-proof",
		.middleware_server = false,
		.mode = ConnectionMode::keepalive,
		.default_connections = connections(256),
		.requests = {
					 weighted(specs, "ping"sv, 40),
					 weighted(specs, "param"sv, 20),
					 weighted(specs, "headers"sv, 15),
					 weighted(specs, "json_medium"sv, 15),
					 weighted(specs, "not_found"sv, 5),
					 weighted(specs, "static_1k"sv, 5),
					 }
    });
	out.push_back({
		.name = "api_write_plain",
		.label = "end-to-end-proof",
		.middleware_server = false,
		.mode = ConnectionMode::keepalive,
		.default_connections = connections(256),
		.requests = {
					 weighted(specs, "ping"sv, 25),
					 weighted(specs, "post_4k"sv, 35),
					 weighted(specs, "chunked_4k"sv, 15),
					 weighted(specs, "post_64k"sv, 10),
					 weighted(specs, "param"sv, 10),
					 weighted(specs, "not_found"sv, 5),
					 }
    });
	out.push_back({
		.name = "api_mixed_middleware",
		.label = "end-to-end-proof",
		.middleware_server = true,
		.mode = ConnectionMode::keepalive,
		.default_connections = connections(256),
		.requests = {
					 weighted(specs, "ping"sv, 30),
					 weighted(specs, "param"sv, 15),
					 weighted(specs, "headers"sv, 15),
					 weighted(specs, "post_4k"sv, 20),
					 weighted(specs, "chunked_4k"sv, 10),
					 weighted(specs, "json_medium"sv, 5),
					 weighted(specs, "not_found"sv, 5),
					 }
    });
	out.push_back({
		.name = "large_response_plain",
		.label = "end-to-end-proof",
		.middleware_server = false,
		.mode = ConnectionMode::keepalive,
		.default_connections = connections(64),
		.requests = {
					 weighted(specs, "static_64k"sv, 45),
					 weighted(specs, "static_1m"sv, 20),
					 weighted(specs, "post_64k"sv, 20),
					 weighted(specs, "json_medium"sv, 15),
					 }
    });
	out.push_back({
		.name = "connection_churn_plain",
		.label = "lifecycle-tail-proof",
		.middleware_server = false,
		.mode = ConnectionMode::connect_close,
		.default_connections = connections(256),
		.requests = {
					 weighted(specs, "ping"sv, 65),
					 weighted(specs, "param"sv, 15),
					 weighted(specs, "post_4k"sv, 15),
					 weighted(specs, "not_found"sv, 5),
					 }
    });
	return out;
}

conflux::http::Router make_router(
	bool middleware,
	std::filesystem::path const &static_dir,
	std::string const &medium_json,
	std::string const &body_64k) {
	conflux::http::Router router;
	if (middleware) {
		conflux::http::SecurityOptions sopts{};
		sopts.hsts_only_on_tls = false;
		router.use(conflux::http::request_id_middleware());
		router.use(conflux::http::security_headers_middleware(sopts));
		router.use(conflux::http::cors_middleware({.allowed_origins = {"https://bench.example"}}));
		router.use(conflux::http::etag_middleware());
	}
	router.get("/api/ping", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::json(R"({"status":"ok"})");
	});
	router.get("/users/{id}", [](conflux::http::OwnedRequest const &req) {
		auto const id = req.params["id"];
		return conflux::http::Response::json(std::format(R"({{"id":"{}","name":"user-{}","active":true}})", id, id));
	});
	router.get("/json/medium", [&medium_json](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::json(medium_json);
	});
	router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(req.body);
	});
	router.get("/body/64k", [&body_64k](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text(body_64k);
	});
	router.serve_static("/", std::string{static_dir.string()});
	return router;
}

void write_static_files(
	std::filesystem::path const &dir) {
	auto write_file = [&](std::string_view name, std::size_t size, char fill) {
		auto path = dir / name;
		std::ofstream out{path, std::ios::binary};
		std::string data(size, fill);
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
	};
	write_file("1k.txt", 1024, 'S');
	write_file("64k.txt", 65536, 'M');
	write_file("1m.bin", 1048576, 'L');
}

[[nodiscard]] bool has_flag(
	std::span<char *> args,
	std::string_view flag) {
	return std::ranges::any_of(args.subspan(1), [flag](char const *arg) { return std::string_view{arg} == flag; });
}

[[nodiscard]] std::string_view option_value(
	std::span<char *> args,
	std::string_view flag,
	std::string_view fallback) {
	for (std::size_t i = 1; i < args.size(); ++i) {
		if (std::string_view{args[i]} == flag && i + 1 < args.size()) {
			return args[i + 1];
		}
	}
	return fallback;
}

[[nodiscard]] int option_int(
	std::span<char *> args,
	std::string_view flag,
	int fallback) {
	auto const value = option_value(args, flag, {});
	if (value.empty()) {
		return fallback;
	}
	int out = fallback;
	std::from_chars(value.data(), value.data() + value.size(), out);
	return out;
}

[[nodiscard]] std::vector<WorkloadDef> select_workloads(
	std::span<WorkloadDef const> workloads,
	std::string_view selected) {
	if (selected.empty() || selected == "all"sv) {
		return {workloads.begin(), workloads.end()};
	}
	auto it = std::ranges::find_if(workloads, [selected](WorkloadDef const &w) { return w.name == selected; });
	if (it == workloads.end()) {
		throw std::invalid_argument{std::format("unknown workload: {}", selected)};
	}
	return {*it};
}

void print_list(
	std::span<WorkloadDef const> workloads) {
	for (auto const &w: workloads) {
		std::println(
			"{:<28} connections={} mode={} server={}",
			w.name,
			w.default_connections,
			w.mode == ConnectionMode::keepalive ? "keepalive" : "connect-close",
			w.middleware_server ? "middleware" : "plain");
	}
}

void print_usage() {
	std::println(
		"Usage: conflux_http11_representative_bench [--workload NAME|all] [--duration N] [--warmup N] "
		"[--connections N] [--rings N] [--json] [--list]");
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"http11_representative","parser":"standard","configs":[{"name":"smoke","extra":{"label":"live-kernel-sanity","suite":"representative-http11"},"args":["--duration","1","--warmup","0","--workload","all","--config-name","smoke"],"reps":1},{"name":"api_mixed_middleware_30s","extra":{"label":"end-to-end-proof","workload":"api_mixed_middleware"},"args":["--duration","30","--warmup","5","--workload","api_mixed_middleware","--config-name","api_mixed_middleware_30s"],"reps":1}]})");

	try {
		auto const argv_span = std::span{argv, static_cast<std::size_t>(argc)};
		auto const args = bench_parse_args(argv_span);
		auto const json = args.json_out;
		auto const config_name = args.config_name.empty() ? "default"sv : std::string_view{args.config_name};
		auto const duration_s = std::max(1, option_int(argv_span, "--duration"sv, 1));
		int warmup_s = option_int(argv_span, "--warmup"sv, duration_s >= 30 ? 5 : 0);
		warmup_s = std::max(0, warmup_s);
		auto const connections_override = option_int(argv_span, "--connections"sv, 0);
		auto const rings_override = option_int(argv_span, "--rings"sv, 0);

		TempDir static_root{std::filesystem::temp_directory_path() / std::format("conflux_http11_repr_{}", ::getpid())};
		write_static_files(static_root.path);
		auto const medium_json = make_medium_json();
		auto const body_64k = std::string(65536, 'B');
		auto specs = make_request_specs();
		auto workloads = make_workloads(std::span<RequestSpec const>{specs}, connections_override);

		if (has_flag(argv_span, "--help"sv) || has_flag(argv_span, "-h"sv)) {
			print_usage();
			return 0;
		}
		if (has_flag(argv_span, "--list"sv)) {
			print_list(workloads);
			return 0;
		}

		auto const np = nproc();
		bool const can_pin = np >= 8;
		auto const half = std::max(1u, np / 2);
		auto const rings = rings_override > 0 ? static_cast<unsigned>(rings_override) : (can_pin ? half : np);

		auto plain = start_server(bench_config(rings), make_router(false, static_root.path, medium_json, body_64k));
		auto middleware = start_server(bench_config(rings), make_router(true, static_root.path, medium_json, body_64k));
		auto selected = select_workloads(workloads, option_value(argv_span, "--workload"sv, "all"sv));

		if (!json) {
			std::println(
				"http11_representative_bench: warmup={}s duration={}s rings={} pinned={} workloads={}\n",
				warmup_s,
				duration_s,
				rings,
				can_pin,
				selected.size());
		}

		for (auto const &workload: selected) {
			auto &server = workload.middleware_server ? middleware : plain;
			auto const connections = std::max(1, workload.default_connections);
			auto const worker_count = std::min(connections, can_pin ? static_cast<int>(half) : static_cast<int>(np));
			auto const conns_per_worker = connections / worker_count;
			auto const remainder = connections % worker_count;

			std::atomic<bool> stop{false};
			std::atomic<bool> measure{false};
			std::vector<std::thread> workers;
			std::vector<WorkerResult> results(static_cast<std::size_t>(worker_count));

			for (int t = 0; t < worker_count; ++t) {
				auto const my_conns = conns_per_worker + (t < remainder ? 1 : 0);
				workers.emplace_back([&, t, my_conns] {
					if (can_pin) {
						pin_thread(half + static_cast<unsigned>(t) % half);
					}
					results[static_cast<std::size_t>(t)] = run_worker(
						server.port,
						workload,
						my_conns,
						stop,
						measure,
						0x1234abcdull + static_cast<std::uint64_t>(t) * 0x10001ull);
				});
			}

			if (warmup_s > 0) {
				std::this_thread::sleep_for(std::chrono::seconds{warmup_s});
			}

			RunTelemetry telemetry{.warmup_s = warmup_s};
			telemetry.fd_start = count_fds();
			telemetry.rss_start = rss_kb();
			telemetry.metrics_start = server.server->metrics();
			telemetry.usage_start = read_usage();
			telemetry.sample.rss_kb_high_water = telemetry.rss_start;
			telemetry.sample.pressure_events_high_water = pressure_event_total(telemetry.metrics_start);
			telemetry.sample.sq_dropped_high_water = telemetry.metrics_start.sq_dropped;
			telemetry.sample.cq_overflow_high_water = telemetry.metrics_start.cq_overflow;

			std::atomic<bool> sample_stop{false};
			std::thread sampler{[&] {
				while (!sample_stop.load(std::memory_order_relaxed)) {
					auto const m = server.server->metrics();
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
			std::this_thread::sleep_for(std::chrono::seconds{duration_s});
			stop.store(true, std::memory_order_release);
			for (auto &w: workers) {
				w.join();
			}
			auto const run_end = bench_now_ns();

			telemetry.usage_end = read_usage();
			telemetry.metrics_end = server.server->metrics();
			telemetry.rss_end = rss_kb();
			telemetry.fd_end = count_fds();
			sample_stop.store(true, std::memory_order_relaxed);
			sampler.join();

			std::vector<std::uint64_t> all_latencies;
			std::vector<std::uint64_t> request_counts(workload.requests.size(), 0);
			for (auto &r: results) {
				all_latencies.insert(all_latencies.end(), r.latencies.begin(), r.latencies.end());
				for (std::size_t i = 0; i < request_counts.size(); ++i) {
					request_counts[i] += r.request_counts[i];
				}
				telemetry.errors += r.errors;
				telemetry.timeouts += r.timeouts;
			}
			auto const total_requests = static_cast<std::uint64_t>(all_latencies.size());
			auto lat = compute_percentiles(all_latencies);
			emit_result(
				config_name,
				workload,
				total_requests,
				run_end - run_start,
				connections,
				duration_s,
				lat,
				request_counts,
				telemetry,
				can_pin,
				json);
		}

		plain.server->shutdown();
		middleware.server->shutdown();
		plain.thr.join();
		middleware.thr.join();
		return 0;
	} catch (std::exception const &e) {
		std::println(std::cerr, "conflux_http11_representative_bench: {}", e.what());
		print_usage();
		return 1;
	}
}
