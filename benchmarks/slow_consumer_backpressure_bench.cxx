#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import conflux.work;
import bench_common;

using namespace std::literals;

namespace {

[[nodiscard]] bool retryable_recv_errno(
	int e) noexcept {
	return e == EINTR
		|| e == EAGAIN
#if EWOULDBLOCK != EAGAIN
		|| e == EWOULDBLOCK
#endif
		;
}

struct BenchClient {
	int fd = -1;

	BenchClient() = default;
	explicit BenchClient(
		std::uint16_t port) {
		connect_to(port);
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

	void connect_to(
		std::uint16_t port) {
		fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) {
			throw std::runtime_error{"socket failed"};
		}
		static constexpr int one = 1;
		static constexpr int small_rcv = 4096;
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof one);
		::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small_rcv, sizeof small_rcv);
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
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				throw std::runtime_error{"send failed"};
			}
			p += n;
			remaining -= static_cast<std::size_t>(n);
		}
	}

	[[nodiscard]] bool poll_readable(
		int timeout_ms) const noexcept {
		pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
		return ::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
	}

	[[nodiscard]] std::string read_headers(
		int timeout_ms = 2000) const {
		std::string out;
		std::array<char, 1024> buf{};
		auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};
		while (std::chrono::steady_clock::now() < deadline) {
			if (!poll_readable(25)) {
				continue;
			}
			auto n = ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT);
			if (n < 0 && retryable_recv_errno(errno)) {
				continue;
			}
			if (n <= 0) {
				break;
			}
			out.append(buf.data(), static_cast<std::size_t>(n));
			if (out.find("\r\n\r\n") != std::string::npos) {
				break;
			}
		}
		return out;
	}
};

struct ServerHandle {
	std::shared_ptr<HttpServer> server;
	std::thread thr;
	std::uint16_t port{};
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

HttpServerMetrics stop_server(
	ServerHandle &h) {
	if (!h.server) {
		return {};
	}
	h.server->shutdown();
	if (h.thr.joinable()) {
		h.thr.join();
	}
	return h.server->metrics();
}

Config bench_config(
	unsigned rings = 1,
	unsigned entries = 256) {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = rings;
	cfg.ring_entries = entries;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.request_timeout_ms = 0;
	cfg.tls_sniff_timeout_ms = 0;
	cfg.startup_banner = false;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	cfg.send_buffer_slabs = 2;
	cfg.send_buffer_bytes = 4096;
	return cfg;
}

[[nodiscard]] long rss_kb() {
	std::ifstream f{"/proc/self/statm"};
	long pages = 0;
	long rss = 0;
	f >> pages >> rss;
	return rss * 4;
}

[[nodiscard]] int count_fds() {
	int count = 0;
	std::error_code ec;
	for (auto const &entry: std::filesystem::directory_iterator{"/proc/self/fd", ec}) {
		(void)entry;
		++count;
	}
	return count;
}

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

struct RowStats {
	std::string config;
	std::string variant;
	std::uint64_t total_ns{};
	std::size_t iterations{};
	std::size_t connections{};
	std::size_t duration_s{};
	std::uint64_t bytes_read{};
	std::uint64_t bytes_expected{};
	std::uint64_t attempts{};
	std::uint64_t accepted{};
	std::uint64_t dropped{};
	std::uint64_t errors{};
	std::uint64_t timeouts{};
	std::uint64_t queue_depth_high_water{};
	std::uint64_t queue_bytes_high_water{};
	int fd_start{-1};
	int fd_end{-1};
	long rss_start{-1};
	long rss_end{-1};
	LatencyStats latency{};
	HttpServerMetrics metrics{};
	SsePressureMetrics sse{};
	WorkPoolQueueStats work{};
};

void emit(
	RowStats const &s,
	bool json) {
	auto const ns_per_iter =
		s.iterations == 0 ? 0.0 : static_cast<double>(s.total_ns) / static_cast<double>(s.iterations);
	if (json) {
		auto const &p = s.metrics.pressure;
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"connections\":{},\"duration_s\":{},\"bytes_read\":{},\"bytes_expected\":{},"
			"\"attempts\":{},\"accepted\":{},\"dropped\":{},\"errors\":{},\"timeouts\":{},"
			"\"queue_depth_high_water\":{},\"queue_bytes_high_water\":{},"
			"\"p50_ns\":{},\"p90_ns\":{},\"p99_ns\":{},\"p999_ns\":{},\"max_ns\":{},"
			"\"fd_count_start\":{},\"fd_count_end\":{},\"rss_kb_start\":{},\"rss_kb_end\":{},"
			"\"sq_dropped\":{},\"cq_overflow\":{},\"accept_rejected\":{},"
			"\"connections_closed_for_pressure\":{},\"response_backpressure_events\":{},"
			"\"sse_dropped_newest\":{},\"sse_dropped_oldest\":{},\"sse_disconnected_for_pressure\":{},"
			"\"websocket_closed_for_pressure\":{},\"work_enqueue_attempts\":{},"
			"\"work_enqueue_full_rejections\":{},\"work_jobs_run\":{}}}",
			s.config,
			s.variant,
			s.iterations,
			s.total_ns,
			ns_per_iter,
			s.connections,
			s.duration_s,
			s.bytes_read,
			s.bytes_expected,
			s.attempts,
			s.accepted,
			s.dropped,
			s.errors,
			s.timeouts,
			s.queue_depth_high_water,
			s.queue_bytes_high_water,
			s.latency.p50,
			s.latency.p90,
			s.latency.p99,
			s.latency.p999,
			s.latency.max,
			s.fd_start,
			s.fd_end,
			s.rss_start,
			s.rss_end,
			s.metrics.sq_dropped,
			s.metrics.cq_overflow,
			p.accept_rejected,
			p.connections_closed_for_pressure,
			p.response_backpressure_events,
			p.sse_dropped_newest + s.sse.dropped_newest,
			p.sse_dropped_oldest + s.sse.dropped_oldest,
			p.sse_disconnected_for_pressure + s.sse.disconnected_for_pressure,
			p.websocket_closed_for_pressure,
			s.work.enqueue_attempts,
			s.work.enqueue_full_rejections,
			s.work.jobs_run);
		return;
	}
	std::println(
		"{:<34} {:>8} iters {:>10.2f} ns/iter  bytes={}/{} errors={} drops={} ws_pressure={}",
		s.variant,
		s.iterations,
		ns_per_iter,
		s.bytes_read,
		s.bytes_expected,
		s.errors,
		s.dropped,
		s.metrics.pressure.websocket_closed_for_pressure);
}

struct SlowWorkerResult {
	std::uint64_t bytes{};
	std::uint64_t errors{};
	std::uint64_t timeouts{};
	std::vector<std::uint64_t> latencies{};
};

SlowWorkerResult run_slow_reader_worker(
	std::uint16_t port,
	std::size_t clients,
	std::chrono::steady_clock::time_point deadline,
	std::size_t chunk_bytes,
	std::chrono::milliseconds delay) {
	SlowWorkerResult result;
	std::vector<BenchClient> conns;
	conns.reserve(clients);
	for (std::size_t i = 0; i < clients; ++i) {
		try {
			auto const t0 = bench_now_ns();
			auto &c = conns.emplace_back(port);
			c.send_all("GET /large HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"sv);
			result.latencies.push_back(bench_now_ns() - t0);
		} catch (...) { ++result.errors; }
	}
	std::vector<char> buf(chunk_bytes);
	while (std::chrono::steady_clock::now() < deadline) {
		for (auto &c: conns) {
			if (c.fd < 0) {
				continue;
			}
			if (!c.poll_readable(1)) {
				++result.timeouts;
				continue;
			}
			auto n = ::recv(c.fd, buf.data(), buf.size(), MSG_DONTWAIT);
			if (n < 0 && retryable_recv_errno(errno)) {
				continue;
			}
			if (n <= 0) {
				c.close();
				continue;
			}
			result.bytes += static_cast<std::uint64_t>(n);
			std::this_thread::sleep_for(delay);
		}
	}
	return result;
}

RowStats run_large_response_slow(
	std::string_view config,
	std::string variant,
	std::size_t connections,
	int duration_s,
	std::size_t body_bytes,
	unsigned ring_entries) {
	std::string body(body_bytes, 'B');
	Router router;
	router.get("/large", [&body](Request const &) { return Response::text(body); });
	auto server = start_server(bench_config(1, ring_entries), std::move(router));

	RowStats row{.config = std::string{config}, .variant = std::move(variant), .iterations = connections};
	row.connections = connections;
	row.duration_s = static_cast<std::size_t>(duration_s);
	row.bytes_expected = body_bytes * connections;
	row.fd_start = count_fds();
	row.rss_start = rss_kb();
	auto const t0 = bench_now_ns();
	auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{duration_s};
	auto workers_n = std::min<std::size_t>(std::max<std::size_t>(1, std::thread::hardware_concurrency()), connections);
	std::vector<std::thread> workers;
	std::vector<SlowWorkerResult> results(workers_n);
	for (std::size_t i = 0; i < workers_n; ++i) {
		auto const base = connections / workers_n;
		auto const rem = connections % workers_n;
		auto const mine = base + (i < rem ? 1 : 0);
		workers.emplace_back([&, i, mine] {
			results[i] = run_slow_reader_worker(server.port, mine, deadline, 1024, std::chrono::milliseconds{1000});
		});
	}
	for (auto &w: workers) {
		w.join();
	}
	row.total_ns = bench_now_ns() - t0;
	std::vector<std::uint64_t> all_latencies;
	for (auto &r: results) {
		row.bytes_read += r.bytes;
		row.errors += r.errors;
		row.timeouts += r.timeouts;
		all_latencies.insert(all_latencies.end(), r.latencies.begin(), r.latencies.end());
	}
	row.latency = compute_percentiles(all_latencies);
	row.queue_bytes_high_water = row.bytes_expected > row.bytes_read ? row.bytes_expected - row.bytes_read : 0;
	row.queue_depth_high_water = connections;
	row.metrics = stop_server(server);
	row.fd_end = count_fds();
	row.rss_end = rss_kb();
	return row;
}

RowStats run_sse_policy(
	std::string_view config,
	std::string variant,
	SseOverflowPolicy policy,
	std::size_t events,
	std::size_t frame_bytes,
	std::size_t max_queue_bytes,
	int duration_s) {
	std::mutex mu;
	std::shared_ptr<SseChannel> channel;
	Router router;
	router.get("/sse", [&](Request const &) {
		auto ch = std::make_shared<SseChannel>(max_queue_bytes, policy);
		{
			std::scoped_lock const lk{mu};
			channel = ch;
		}
		return Response::sse(std::move(ch));
	});
	auto server = start_server(bench_config(), std::move(router));
	BenchClient client{server.port};
	client.send_all("GET /sse HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n"sv);
	(void)client.read_headers();

	for (int i = 0; i < 200; ++i) {
		{
			std::scoped_lock const lk{mu};
			if (channel) {
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{5});
	}

	std::shared_ptr<SseChannel> ch;
	{
		std::scoped_lock const lk{mu};
		ch = channel;
	}
	if (!ch) {
		throw std::runtime_error{"SSE channel was not created"};
	}

	std::string payload(frame_bytes, 'S');
	std::string frame = std::format("event: pressure\ndata: {}\n\n", payload);
	RowStats row{.config = std::string{config}, .variant = std::move(variant), .iterations = events};
	row.connections = 1;
	row.duration_s = static_cast<std::size_t>(duration_s);
	row.fd_start = count_fds();
	row.rss_start = rss_kb();
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < events; ++i) {
		++row.attempts;
		if (ch->send(frame)) {
			++row.accepted;
		} else {
			++row.dropped;
		}
	}
	std::this_thread::sleep_for(std::chrono::seconds{duration_s});
	row.total_ns = bench_now_ns() - t0;
	row.bytes_expected = events * frame.size();
	row.sse = ch->pressure_metrics();
	row.dropped = std::max<std::uint64_t>(row.dropped, ch->dropped_count());
	row.queue_bytes_high_water = max_queue_bytes;
	row.queue_depth_high_water = events;
	ch->close();
	row.metrics = stop_server(server);
	row.fd_end = count_fds();
	row.rss_end = rss_kb();
	return row;
}

std::string websocket_handshake_request() {
	return std::string{
		"GET /ws HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n"};
}

RowStats run_ws_workpool_full(
	std::string_view config,
	std::size_t handshakes,
	int duration_s) {
	auto release = std::make_shared<std::atomic<bool>>(false);
	auto pool = std::make_shared<WorkPool>(WorkPoolOptions{
		.threads = 1,
		.max_inject_queue = 1,
		.local_queue_capacity = 1,
		.queue_mode = WorkPoolQueueMode::no_stealing,
	});
	(void)pool->enqueue([release] {
		while (!release->load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(std::chrono::milliseconds{10});
		}
	});
	(void)pool->enqueue([release] {
		while (!release->load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(std::chrono::milliseconds{10});
		}
	});

	Router router;
	router.set_work_pool(pool);
	router.ws("/ws", [](Request const &, WsConn &ws) {
		std::string payload(4096, 'W');
		for (int i = 0; i < 256; ++i) {
			if (!ws.send_text(payload)) {
				break;
			}
		}
	});
	auto server = start_server(bench_config(), std::move(router));
	RowStats row{.config = std::string{config}, .variant = "ws_workpool_full", .iterations = handshakes};
	row.connections = handshakes;
	row.duration_s = static_cast<std::size_t>(duration_s);
	row.fd_start = count_fds();
	row.rss_start = rss_kb();
	auto const req = websocket_handshake_request();
	auto const t0 = bench_now_ns();
	std::vector<BenchClient> clients;
	clients.reserve(handshakes);
	for (std::size_t i = 0; i < handshakes; ++i) {
		try {
			auto &c = clients.emplace_back(server.port);
			c.send_all(req);
			std::string headers = c.read_headers(500);
			if (headers.find("101 Switching Protocols") == std::string::npos) {
				++row.errors;
			}
		} catch (...) { ++row.errors; }
	}
	std::this_thread::sleep_for(std::chrono::seconds{duration_s});
	row.total_ns = bench_now_ns() - t0;
	row.work = pool->queue_stats();
	row.metrics = stop_server(server);
	release->store(true, std::memory_order_release);
	pool->drain_and_stop();
	row.fd_end = count_fds();
	row.rss_end = rss_kb();
	return row;
}

std::vector<std::string_view> all_cases() {
	return {
		"large_response_slow_1kps"sv,
		"ring_pressure_slow_1kps"sv,
		"sse_drop_newest"sv,
		"sse_drop_oldest"sv,
		"sse_disconnect"sv,
		"ws_workpool_full"sv,
	};
}

void print_usage() {
	std::println(
		"Usage: conflux_slow_consumer_backpressure_bench [--case NAME|--all-cases] [--duration S] "
		"[--connections N] [--json] [--list]");
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"slow_consumer_backpressure","parser":"standard","configs":[{"name":"large_response_slow_1kps","extra":{"kind":"live-kernel-sanity","evidence_role":"backpressure proof harness","case":"large response with client reading 1 KiB/s"},"args":["--case","large_response_slow_1kps","--config-name","large_response_slow_1kps","--duration","1"]},{"name":"ring_pressure_slow_1kps","extra":{"kind":"live-kernel-sanity","evidence_role":"backpressure proof harness","case":"small ring + slow response readers"},"args":["--case","ring_pressure_slow_1kps","--config-name","ring_pressure_slow_1kps","--duration","1"]},{"name":"sse_drop_newest","extra":{"kind":"live-kernel-sanity","evidence_role":"backpressure proof harness","case":"SSE slow client drop_newest"},"args":["--case","sse_drop_newest","--config-name","sse_drop_newest","--duration","1"]},{"name":"sse_drop_oldest","extra":{"kind":"live-kernel-sanity","evidence_role":"backpressure proof harness","case":"SSE slow client drop_oldest"},"args":["--case","sse_drop_oldest","--config-name","sse_drop_oldest","--duration","1"]},{"name":"sse_disconnect","extra":{"kind":"live-kernel-sanity","evidence_role":"backpressure proof harness","case":"SSE slow client disconnect"},"args":["--case","sse_disconnect","--config-name","sse_disconnect","--duration","1"]},{"name":"ws_workpool_full","extra":{"kind":"live-kernel-sanity","evidence_role":"backpressure proof harness","case":"WebSocket handoff while work pool queue is full"},"args":["--case","ws_workpool_full","--config-name","ws_workpool_full","--duration","1"]}]})");

	auto const args = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	std::string selected = "large_response_slow_1kps";
	bool run_all = false;
	int duration_s = 1;
	std::size_t connections = 8;
	for (int i = 1; i < argc; ++i) {
		std::string_view const arg{argv[i]};
		if (arg == "--case" && i + 1 < argc) {
			selected = argv[++i];
		} else if (arg == "--all-cases") {
			run_all = true;
		} else if (arg == "--duration" && i + 1 < argc) {
			std::from_chars(argv[i + 1], argv[i + 1] + std::strlen(argv[i + 1]), duration_s);
			++i;
		} else if (arg == "--connections" && i + 1 < argc) {
			connections = bench_parse_sz(argv[++i]);
		} else if (arg == "--list") {
			for (auto name: all_cases()) {
				std::println("{}", name);
			}
			return 0;
		} else if (arg == "--help" || arg == "-h") {
			print_usage();
			return 0;
		}
	}
	if (duration_s < 1) {
		duration_s = 1;
	}
	connections = std::max<std::size_t>(1, connections);
	auto config = args.config_name.empty() ? std::string{selected} : args.config_name;

	try {
		auto run_one = [&](std::string_view name) {
			RowStats row;
			if (name == "large_response_slow_1kps"sv) {
				row = run_large_response_slow(
					config,
					std::string{name},
					connections,
					duration_s,
					16U * 1024U * 1024U,
					256);
			} else if (name == "ring_pressure_slow_1kps"sv) {
				row = run_large_response_slow(
					config,
					std::string{name},
					connections * 4U,
					duration_s,
					16U * 1024U * 1024U,
					16);
			} else if (name == "sse_drop_newest"sv) {
				row = run_sse_policy(
					config,
					std::string{name},
					SseOverflowPolicy::DropNewest,
					2048,
					512,
					8192,
					duration_s);
			} else if (name == "sse_drop_oldest"sv) {
				row = run_sse_policy(
					config,
					std::string{name},
					SseOverflowPolicy::DropOldest,
					2048,
					512,
					8192,
					duration_s);
			} else if (name == "sse_disconnect"sv) {
				row = run_sse_policy(
					config,
					std::string{name},
					SseOverflowPolicy::Disconnect,
					2048,
					512,
					8192,
					duration_s);
			} else if (name == "ws_workpool_full"sv) {
				row = run_ws_workpool_full(config, std::max<std::size_t>(connections, 16), duration_s);
			} else {
				throw std::runtime_error{std::format("unknown case: {}", name)};
			}
			emit(row, args.json_out);
		};
		if (run_all) {
			for (auto name: all_cases()) {
				run_one(name);
			}
		} else {
			run_one(selected);
		}
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_slow_consumer_backpressure_bench: {}", ex.what());
		return 1;
	}
	return 0;
}
