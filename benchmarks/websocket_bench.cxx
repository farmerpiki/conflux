#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import conflux.net.http.realtime;
import conflux.work;
import bench_common;

using namespace std::literals;
using conflux::http::Config;
using conflux::http::HttpServerMetrics;
using HttpRequest = conflux::http::OwnedRequest;
using HttpServer = conflux::http::HttpServer;
using Router = conflux::http::Router;
using conflux::http::WsConn;

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::uint64_t> g_allocated_bytes{0};
std::atomic<std::uint64_t> g_sink{0};

void note_allocation(
	std::size_t size) noexcept {
	if (g_count_allocations.load(std::memory_order_relaxed)) {
		g_allocations.fetch_add(1, std::memory_order_relaxed);
		g_allocated_bytes.fetch_add(size, std::memory_order_relaxed);
	}
}

void reset_allocation_counters() noexcept {
	g_allocations.store(0, std::memory_order_relaxed);
	g_allocated_bytes.store(0, std::memory_order_relaxed);
}

[[nodiscard]] void *allocate_counted(
	std::size_t size) {
	if (size == 0) {
		size = 1;
	}
	note_allocation(size);
	if (auto *p = std::malloc(size); p != nullptr) {
		return p;
	}
	throw std::bad_alloc{};
}

[[nodiscard]] void *allocate_counted_aligned(
	std::size_t size,
	std::size_t align) {
	if (size == 0) {
		size = 1;
	}
	if (align < alignof(void *)) {
		align = alignof(void *);
	}
	note_allocation(size);
	void *p{};
	if (posix_memalign(&p, align, size) == 0 && p != nullptr) {
		return p;
	}
	throw std::bad_alloc{};
}

void use_sink(
	std::uint64_t v) noexcept {
	g_sink.fetch_add(v + 0x9e3779b97f4a7c15ULL, std::memory_order_relaxed);
}

void use_sink(
	std::string_view s) noexcept {
	std::uint64_t v = s.size();
	if (!s.empty()) {
		v += static_cast<unsigned char>(s.front());
		v += static_cast<unsigned char>(s.back()) << 8U;
	}
	use_sink(v);
}

[[nodiscard]] std::span<std::byte const> as_bytes(
	std::string_view s) noexcept {
	return std::as_bytes(std::span{s.data(), s.size()});
}

[[nodiscard]] std::string make_payload(
	std::size_t size,
	char fill = 'x') {
	std::string s(size, fill);
	for (std::size_t i = 0; i < size; i += 251) {
		s[i] = static_cast<char>('a' + (i % 26));
	}
	return s;
}

[[nodiscard]] std::string make_masked_frame(
	std::uint8_t opcode,
	std::string_view payload,
	bool fin = true,
	std::array<std::uint8_t, 4> mask = {0x12, 0x34, 0x56, 0x78}) {
	std::string frame;
	std::size_t const len = payload.size();
	std::size_t const ext = len < 126 ? 0 : len <= 0xFFFF ? 2 : 8;
	frame.reserve(2 + ext + 4 + len);
	frame.push_back(static_cast<char>((fin ? 0x80U : 0U) | opcode));
	if (len < 126) {
		frame.push_back(static_cast<char>(0x80U | static_cast<std::uint8_t>(len)));
	} else if (len <= 0xFFFF) {
		frame.push_back(static_cast<char>(0x80U | 126U));
		frame.push_back(static_cast<char>((len >> 8U) & 0xffU));
		frame.push_back(static_cast<char>(len & 0xffU));
	} else {
		frame.push_back(static_cast<char>(0x80U | 127U));
		for (int s = 56; s >= 0; s -= 8) {
			frame.push_back(static_cast<char>((static_cast<std::uint64_t>(len) >> s) & 0xffU));
		}
	}
	frame.append(reinterpret_cast<char const *>(mask.data()), mask.size());
	for (std::size_t i = 0; i < payload.size(); ++i) {
		frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i & 3U]));
	}
	return frame;
}

[[nodiscard]] std::string make_close_frame(
	std::uint16_t code = 1000,
	std::string_view reason = {}) {
	std::string payload;
	payload.push_back(static_cast<char>(code >> 8U));
	payload.push_back(static_cast<char>(code & 0xffU));
	payload.append(reason);
	return make_masked_frame(0x8U, payload);
}

[[nodiscard]] std::string make_fragmented_text(
	std::string_view payload,
	std::size_t fragments) {
	fragments = std::max<std::size_t>(1, fragments);
	std::string wire;
	wire.reserve(payload.size() + fragments * 16U);
	for (std::size_t i = 0; i < fragments; ++i) {
		auto const begin = payload.size() * i / fragments;
		auto const end = payload.size() * (i + 1) / fragments;
		auto const op = i == 0 ? 0x1U : 0x0U;
		auto const fin = i + 1 == fragments;
		wire += make_masked_frame(static_cast<std::uint8_t>(op), payload.substr(begin, end - begin), fin);
	}
	return wire;
}

[[nodiscard]] std::string repeated_wire(
	std::string_view frame,
	std::size_t count) {
	std::string wire;
	wire.reserve(frame.size() * count);
	for (std::size_t i = 0; i < count; ++i) {
		wire.append(frame);
	}
	return wire;
}

struct LatencyStats {
	std::uint64_t p50{};
	std::uint64_t p90{};
	std::uint64_t p99{};
	std::uint64_t p999{};
	std::uint64_t max{};
};

[[nodiscard]] LatencyStats latency_stats(
	std::vector<std::uint64_t> samples) {
	if (samples.empty()) {
		return {};
	}
	std::ranges::sort(samples);
	auto pick = [&](double q) -> std::uint64_t {
		auto idx = static_cast<std::size_t>(std::ceil(q * static_cast<double>(samples.size())));
		if (idx == 0) {
			idx = 1;
		}
		return samples[std::min<std::size_t>(idx - 1, samples.size() - 1)];
	};
	return {
		.p50 = pick(0.50),
		.p90 = pick(0.90),
		.p99 = pick(0.99),
		.p999 = pick(0.999),
		.max = samples.back(),
	};
}

struct Row {
	std::string config;
	std::string variant;
	std::string kind;
	std::string mechanism;
	std::size_t iterations{};
	std::size_t operations{};
	std::size_t payload_size{};
	std::size_t connections{};
	std::size_t messages_per_connection{};
	std::uint64_t total_ns{};
	std::uint64_t bytes_sent{};
	std::uint64_t bytes_received{};
	std::uint64_t errors{};
	std::uint64_t allocations{};
	std::uint64_t allocated_bytes{};
	LatencyStats latency{};
	HttpServerMetrics metrics{};
};

void emit(
	Row const &r,
	bool json) {
	auto const denom = r.operations == 0 ? 1.0 : static_cast<double>(r.operations);
	auto const ns_per_op = static_cast<double>(r.total_ns) / denom;
	auto const ops_per_sec = r.total_ns == 0 ? 0.0 : denom * 1'000'000'000.0 / static_cast<double>(r.total_ns);
	auto const allocs_per_op = static_cast<double>(r.allocations) / denom;
	auto const alloc_bytes_per_op = static_cast<double>(r.allocated_bytes) / denom;
	if (json) {
		auto const &p = r.metrics.pressure;
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"kind\":\"{}\",\"mechanism\":\"{}\","
			"\"iterations\":{},\"operations\":{},\"payload_size\":{},\"connections\":{},"
			"\"messages_per_connection\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"ns_per_op\":{:.2f},\"ops_per_sec\":{:.2f},\"bytes_sent\":{},"
			"\"bytes_received\":{},\"errors\":{},\"allocations\":{},\"allocated_bytes\":{},"
			"\"allocations_per_op\":{:.6f},\"allocated_bytes_per_op\":{:.2f},"
			"\"p50_ns\":{},\"p90_ns\":{},\"p99_ns\":{},\"p999_ns\":{},\"max_ns\":{},"
			"\"sq_dropped\":{},\"cq_overflow\":{},\"websocket_closed_for_pressure\":{},"
			"\"response_backpressure_events\":{}}}",
			r.config,
			r.variant,
			r.kind,
			r.mechanism,
			r.iterations,
			r.operations,
			r.payload_size,
			r.connections,
			r.messages_per_connection,
			r.total_ns,
			ns_per_op,
			ns_per_op,
			ops_per_sec,
			r.bytes_sent,
			r.bytes_received,
			r.errors,
			r.allocations,
			r.allocated_bytes,
			allocs_per_op,
			alloc_bytes_per_op,
			r.latency.p50,
			r.latency.p90,
			r.latency.p99,
			r.latency.p999,
			r.latency.max,
			r.metrics.sq_dropped,
			r.metrics.cq_overflow,
			p.websocket_closed_for_pressure,
			p.response_backpressure_events);
		return;
	}
	std::println(
		"{:<34} {:>10} ops {:>10.2f} ns/op {:>10.0f} ops/s alloc/op={:.3f} err={}",
		r.variant,
		r.operations,
		ns_per_op,
		ops_per_sec,
		allocs_per_op,
		r.errors);
}

template<class Fn>
[[nodiscard]] Row run_micro(
	std::string_view config,
	std::string variant,
	std::string mechanism,
	std::size_t outer_iterations,
	std::size_t operations_per_iteration,
	std::size_t payload_size,
	std::size_t warmup,
	Fn &&fn) {
	for (std::size_t i = 0; i < warmup; ++i) {
		fn();
	}
	reset_allocation_counters();
	g_count_allocations.store(true, std::memory_order_release);
	auto const t0 = bench_now_ns();
	std::uint64_t errors = 0;
	for (std::size_t i = 0; i < outer_iterations; ++i) {
		if (!fn()) {
			++errors;
		}
	}
	auto const total = bench_now_ns() - t0;
	g_count_allocations.store(false, std::memory_order_release);
	return Row{
		.config = std::string{config},
		.variant = std::move(variant),
		.kind = "micro/user-space",
		.mechanism = std::move(mechanism),
		.iterations = outer_iterations,
		.operations = outer_iterations * std::max<std::size_t>(1, operations_per_iteration),
		.payload_size = payload_size,
		.total_ns = total,
		.errors = errors,
		.allocations = g_allocations.load(std::memory_order_relaxed),
		.allocated_bytes = g_allocated_bytes.load(std::memory_order_relaxed),
	};
}

struct ServerHandle {
	std::shared_ptr<conflux::http::HttpServer> server;
	std::thread thread;
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

[[nodiscard]] Config bench_config(
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

[[nodiscard]] ServerHandle start_server(
	Config cfg,
	conflux::http::Router router) {
	(void)::signal(SIGPIPE, SIG_IGN);
	cfg.startup_banner = false;
	auto srv = std::make_shared<conflux::http::HttpServer>(cfg, std::move(router));
	std::thread t{[srv] {
		try {
			auto _ = srv->run();
		} catch (std::exception const &e) { std::println(std::cerr, "websocket bench server: {}", e.what()); }
	}};
	auto const p = srv->port();
	wait_for_server(p);
	return {.server = srv, .thread = std::move(t), .port = p};
}

[[nodiscard]] HttpServerMetrics stop_server(
	ServerHandle &h) {
	if (!h.server) {
		return {};
	}
	h.server->shutdown();
	if (h.thread.joinable()) {
		h.thread.join();
	}
	return h.server->metrics();
}

struct WsFrameRead {
	std::uint8_t opcode{};
	std::string payload;
};

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
			throw std::runtime_error{"connect failed"};
		}
	}

	void set_recv_timeout(
		std::chrono::microseconds timeout) const {
		timeval tv{};
		tv.tv_sec = static_cast<time_t>(timeout.count() / 1'000'000);
		tv.tv_usec = static_cast<suseconds_t>(timeout.count() % 1'000'000);
		::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
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
				throw std::runtime_error{std::format("send failed: {}", std::strerror(errno))};
			}
			p += n;
			remaining -= static_cast<std::size_t>(n);
		}
	}

	void recv_exact(
		std::span<char> dst) const {
		std::size_t off = 0;
		while (off < dst.size()) {
			auto n = ::recv(fd, dst.data() + off, dst.size() - off, 0);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				throw std::runtime_error{std::format("recv failed: {}", n < 0 ? std::strerror(errno) : "eof")};
			}
			off += static_cast<std::size_t>(n);
		}
	}

	[[nodiscard]] std::string read_headers() const {
		std::string out;
		std::array<char, 1024> buf{};
		for (;;) {
			auto n = ::recv(fd, buf.data(), buf.size(), 0);
			if (n < 0 && errno == EINTR) {
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

	[[nodiscard]] std::string upgrade(
		std::string_view path) const {
		auto req = std::format(
			"GET {} HTTP/1.1\r\n"
			"Host: localhost\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Version: 13\r\n"
			"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
			path);
		send_all(req);
		auto headers = read_headers();
		if (headers.find("101 Switching Protocols") == std::string::npos) {
			throw std::runtime_error{std::format("websocket upgrade failed: {}", headers)};
		}
		return headers;
	}

	[[nodiscard]] WsFrameRead read_frame() const {
		std::array<char, 2> first{};
		recv_exact(first);
		auto const b0 = static_cast<std::uint8_t>(first[0]);
		auto const b1 = static_cast<std::uint8_t>(first[1]);
		std::uint64_t len = b1 & 0x7fU;
		if (len == 126) {
			std::array<char, 2> ext{};
			recv_exact(ext);
			len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(ext[0])) << 8U)
				| static_cast<std::uint64_t>(static_cast<std::uint8_t>(ext[1]));
		} else if (len == 127) {
			std::array<char, 8> ext{};
			recv_exact(ext);
			len = 0;
			for (auto c: ext) {
				len = (len << 8U) | static_cast<std::uint8_t>(c);
			}
		}
		std::string payload(static_cast<std::size_t>(len), '\0');
		if (!payload.empty()) {
			recv_exact(std::span{payload.data(), payload.size()});
		}
		return {.opcode = static_cast<std::uint8_t>(b0 & 0x0fU), .payload = std::move(payload)};
	}
};

[[nodiscard]] conflux::http::Router make_router(
	std::size_t push_frames = 64,
	std::size_t push_payload = 4096) {
	conflux::http::Router router;
	router.ws("/ws_echo", [](HttpRequest const &, WsConn &ws) {
		for (;;) {
			auto frame = ws.recv();
			if (!frame || frame->opcode == WsConn::Opcode::Close) {
				break;
			}
			if (frame->opcode == WsConn::Opcode::Text) {
				(void)ws.send_text(frame->payload);
			} else if (frame->opcode == WsConn::Opcode::Binary) {
				(void)ws.send_binary(as_bytes(frame->payload));
			}
		}
	});
	router.ws("/ws_push", [push_frames, push_payload](HttpRequest const &, WsConn &ws) {
		std::string payload(push_payload, 'P');
		for (std::size_t i = 0; i < push_frames; ++i) {
			if (!ws.send_text(payload)) {
				break;
			}
		}
	});
	return router;
}

[[nodiscard]] Row run_live_upgrade(
	std::string_view config,
	std::size_t iterations,
	std::size_t warmup) {
	auto server = start_server(bench_config(), make_router());
	auto close_frame = make_close_frame();
	for (std::size_t i = 0; i < warmup; ++i) {
		BenchClient c{server.port};
		(void)c.upgrade("/ws_echo");
		c.send_all(close_frame);
		(void)c.read_frame();
	}
	Row row{
		.config = std::string{config},
		.variant = "live_upgrade_close",
		.kind = "live-kernel-sanity",
		.mechanism = "handshake-handoff-close",
		.iterations = iterations,
		.operations = iterations,
	};
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iterations; ++i) {
		try {
			BenchClient c{server.port};
			row.bytes_sent += c.upgrade("/ws_echo").size();
			c.send_all(close_frame);
			row.bytes_sent += close_frame.size();
			auto f = c.read_frame();
			row.bytes_received += f.payload.size();
		} catch (...) { ++row.errors; }
	}
	row.total_ns = bench_now_ns() - t0;
	row.metrics = stop_server(server);
	return row;
}

[[nodiscard]] Row run_live_echo(
	std::string_view config,
	std::string variant,
	std::size_t outer_iterations,
	std::size_t messages_per_connection,
	std::size_t payload_size,
	std::size_t warmup) {
	auto server = start_server(bench_config(), make_router());
	auto payload = make_payload(payload_size);
	auto frame = make_masked_frame(0x1U, payload);
	auto close_frame = make_close_frame();
	for (std::size_t i = 0; i < warmup; ++i) {
		BenchClient c{server.port};
		(void)c.upgrade("/ws_echo");
		c.send_all(frame);
		(void)c.read_frame();
		c.send_all(close_frame);
		(void)c.read_frame();
	}
	Row row{
		.config = std::string{config},
		.variant = std::move(variant),
		.kind = "live-kernel-sanity",
		.mechanism = "steady-state-echo-roundtrip",
		.iterations = outer_iterations,
		.operations = outer_iterations * messages_per_connection,
		.payload_size = payload_size,
		.messages_per_connection = messages_per_connection,
	};
	std::vector<std::uint64_t> latencies;
	latencies.reserve(row.operations);
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < outer_iterations; ++i) {
		try {
			BenchClient c{server.port};
			row.bytes_sent += c.upgrade("/ws_echo").size();
			for (std::size_t m = 0; m < messages_per_connection; ++m) {
				auto const l0 = bench_now_ns();
				c.send_all(frame);
				auto f = c.read_frame();
				latencies.push_back(bench_now_ns() - l0);
				row.bytes_sent += frame.size();
				row.bytes_received += f.payload.size();
				if (f.payload != payload) {
					++row.errors;
				}
			}
			c.send_all(close_frame);
			(void)c.read_frame();
		} catch (...) { ++row.errors; }
	}
	row.total_ns = bench_now_ns() - t0;
	row.latency = latency_stats(std::move(latencies));
	row.metrics = stop_server(server);
	return row;
}

[[nodiscard]] Row run_live_ping_pong(
	std::string_view config,
	std::size_t outer_iterations,
	std::size_t pings_per_connection,
	std::size_t payload_size,
	std::size_t warmup) {
	auto server = start_server(bench_config(), make_router());
	auto payload = make_payload(payload_size, 'p');
	auto frame = make_masked_frame(0x9U, payload);
	auto close_frame = make_close_frame();
	for (std::size_t i = 0; i < warmup; ++i) {
		BenchClient c{server.port};
		(void)c.upgrade("/ws_echo");
		c.send_all(frame);
		(void)c.read_frame();
		c.send_all(close_frame);
		(void)c.read_frame();
	}
	Row row{
		.config = std::string{config},
		.variant = "live_ping_pong_100x_32",
		.kind = "live-kernel-sanity",
		.mechanism = "control-frame-autopong",
		.iterations = outer_iterations,
		.operations = outer_iterations * pings_per_connection,
		.payload_size = payload_size,
		.messages_per_connection = pings_per_connection,
	};
	std::vector<std::uint64_t> latencies;
	latencies.reserve(row.operations);
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < outer_iterations; ++i) {
		try {
			BenchClient c{server.port};
			row.bytes_sent += c.upgrade("/ws_echo").size();
			for (std::size_t m = 0; m < pings_per_connection; ++m) {
				auto const l0 = bench_now_ns();
				c.send_all(frame);
				auto f = c.read_frame();
				latencies.push_back(bench_now_ns() - l0);
				row.bytes_sent += frame.size();
				row.bytes_received += f.payload.size();
				if (f.opcode != 0xAU || f.payload != payload) {
					++row.errors;
				}
			}
			c.send_all(close_frame);
			(void)c.read_frame();
		} catch (...) { ++row.errors; }
	}
	row.total_ns = bench_now_ns() - t0;
	row.latency = latency_stats(std::move(latencies));
	row.metrics = stop_server(server);
	return row;
}

[[nodiscard]] Row run_live_fragmented(
	std::string_view config,
	std::size_t outer_iterations,
	std::size_t payload_size,
	std::size_t fragments,
	std::size_t warmup) {
	auto server = start_server(bench_config(), make_router());
	auto payload = make_payload(payload_size, 'f');
	auto wire = make_fragmented_text(payload, fragments);
	auto close_frame = make_close_frame();
	for (std::size_t i = 0; i < warmup; ++i) {
		BenchClient c{server.port};
		(void)c.upgrade("/ws_echo");
		c.send_all(wire);
		(void)c.read_frame();
		c.send_all(close_frame);
		(void)c.read_frame();
	}
	Row row{
		.config = std::string{config},
		.variant = "live_fragmented_16x256",
		.kind = "live-kernel-sanity",
		.mechanism = "fragment-accumulator-echo",
		.iterations = outer_iterations,
		.operations = outer_iterations,
		.payload_size = payload_size,
		.messages_per_connection = 1,
	};
	std::vector<std::uint64_t> latencies;
	latencies.reserve(outer_iterations);
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < outer_iterations; ++i) {
		try {
			BenchClient c{server.port};
			row.bytes_sent += c.upgrade("/ws_echo").size();
			auto const l0 = bench_now_ns();
			c.send_all(wire);
			auto f = c.read_frame();
			latencies.push_back(bench_now_ns() - l0);
			row.bytes_sent += wire.size();
			row.bytes_received += f.payload.size();
			if (f.payload != payload) {
				++row.errors;
			}
			c.send_all(close_frame);
			(void)c.read_frame();
		} catch (...) { ++row.errors; }
	}
	row.total_ns = bench_now_ns() - t0;
	row.latency = latency_stats(std::move(latencies));
	row.metrics = stop_server(server);
	return row;
}

[[nodiscard]] Row run_live_concurrent_echo(
	std::string_view config,
	std::size_t connections,
	std::size_t messages_per_connection,
	std::size_t payload_size) {
	auto server = start_server(bench_config(std::max<unsigned>(1, std::thread::hardware_concurrency())), make_router());
	auto payload = make_payload(payload_size, 'c');
	auto frame = make_masked_frame(0x1U, payload);
	auto close_frame = make_close_frame();
	Row row{
		.config = std::string{config},
		.variant = "live_concurrent_16x100_32",
		.kind = "live-kernel-sanity",
		.mechanism = "threaded-handoff-and-echo-contention",
		.iterations = connections,
		.operations = connections * messages_per_connection,
		.payload_size = payload_size,
		.connections = connections,
		.messages_per_connection = messages_per_connection,
	};
	std::mutex mu;
	std::vector<std::uint64_t> latencies;
	latencies.reserve(row.operations);
	auto const t0 = bench_now_ns();
	std::vector<std::jthread> threads;
	threads.reserve(connections);
	for (std::size_t cidx = 0; cidx < connections; ++cidx) {
		threads.emplace_back([&, cidx] {
			(void)cidx;
			std::vector<std::uint64_t> local_latencies;
			local_latencies.reserve(messages_per_connection);
			std::uint64_t local_bytes_sent = 0;
			std::uint64_t local_bytes_received = 0;
			std::uint64_t local_errors = 0;
			try {
				BenchClient c{server.port};
				local_bytes_sent += c.upgrade("/ws_echo").size();
				for (std::size_t m = 0; m < messages_per_connection; ++m) {
					auto const l0 = bench_now_ns();
					c.send_all(frame);
					auto f = c.read_frame();
					local_latencies.push_back(bench_now_ns() - l0);
					local_bytes_sent += frame.size();
					local_bytes_received += f.payload.size();
					if (f.payload != payload) {
						++local_errors;
					}
				}
				c.send_all(close_frame);
				(void)c.read_frame();
			} catch (...) { ++local_errors; }
			std::scoped_lock const lk{mu};
			row.bytes_sent += local_bytes_sent;
			row.bytes_received += local_bytes_received;
			row.errors += local_errors;
			latencies.insert(latencies.end(), local_latencies.begin(), local_latencies.end());
		});
	}
	threads.clear();
	row.total_ns = bench_now_ns() - t0;
	row.latency = latency_stats(std::move(latencies));
	row.metrics = stop_server(server);
	return row;
}

[[nodiscard]] Row run_live_slow_receiver(
	std::string_view config,
	std::size_t frames,
	std::size_t payload_size,
	std::chrono::microseconds delay) {
	auto server = start_server(bench_config(), make_router(frames, payload_size));
	Row row{
		.config = std::string{config},
		.variant = "live_slow_receiver_64x4k",
		.kind = "live-kernel-sanity",
		.mechanism = "blocking-ws-send-under-slow-reader",
		.iterations = frames,
		.operations = frames,
		.payload_size = payload_size,
		.connections = 1,
		.messages_per_connection = frames,
	};
	auto const t0 = bench_now_ns();
	try {
		BenchClient c{server.port};
		row.bytes_sent += c.upgrade("/ws_push").size();
		for (std::size_t i = 0; i < frames; ++i) {
			auto f = c.read_frame();
			row.bytes_received += f.payload.size();
			if (f.payload.size() != payload_size) {
				++row.errors;
			}
			std::this_thread::sleep_for(delay);
		}
	} catch (...) { ++row.errors; }
	row.total_ns = bench_now_ns() - t0;
	row.metrics = stop_server(server);
	return row;
}

void run_handshake_case(
	std::string_view config,
	std::size_t iterations,
	std::size_t warmup,
	bool json) {
	static constexpr std::string_view kKey = "dGhlIHNhbXBsZSBub25jZQ==";
	emit(
		run_micro(
			config,
			"handshake_accept_key",
			"sha1-base64-accept-key",
			iterations,
			1,
			kKey.size(),
			warmup,
			[] {
				auto out = conflux::http::detail::ws_accept_key(kKey);
				use_sink(out);
				return out.size() == 28;
			}),
		json);
	emit(
		run_micro(
			config,
			"handshake_validate_key",
			"client-key-base64-validation",
			iterations,
			1,
			kKey.size(),
			warmup,
			[] {
				bool const ok = conflux::http::detail::is_valid_client_key(kKey);
				use_sink(ok ? 1 : 0);
				return ok;
			}),
		json);
}

void run_build_case(
	std::string_view config,
	std::size_t iterations,
	std::size_t warmup,
	bool json) {
	for (auto size:
		 {std::size_t{0}, std::size_t{8}, std::size_t{125}, std::size_t{126}, std::size_t{4096}, std::size_t{65536}}) {
		auto payload = make_payload(size, 'b');
		auto variant = std::format("build_frame_{}", size);
		emit(
			run_micro(
				config,
				std::move(variant),
				"server-frame-construction-allocation",
				iterations,
				1,
				size,
				warmup,
				[&] {
					auto frame = conflux::http::detail::ws_build_frame(0x1U, as_bytes(payload));
					use_sink(frame);
					return frame.size() >= payload.size() + 2;
				}),
			json);
	}
}

void run_header_case(
	std::string_view config,
	std::size_t iterations,
	std::size_t warmup,
	bool json) {
	struct HeaderCase {
		std::string_view name;
		std::string wire;
		bool ok;
	};
	std::vector<HeaderCase> cases;
	cases.push_back({"parse_header_8", make_masked_frame(0x1U, make_payload(8)), true});
	cases.push_back({"parse_header_126", make_masked_frame(0x1U, make_payload(126)), true});
	cases.push_back({"parse_header_65536", make_masked_frame(0x2U, make_payload(65536)), true});
	cases.push_back({
		"parse_header_protocol_error_unmasked",
		std::string{"\x81\x05hello", 7},
		false
    });
	for (auto const &c: cases) {
		emit(
			run_micro(
				config,
				std::string{c.name},
				"frame-header-parse-and-validation",
				iterations,
				1,
				c.wire.size(),
				warmup,
				[&] {
					conflux::http::detail::FrameHeader hdr{};
					auto const st = conflux::http::detail::parse_frame_header(as_bytes(c.wire), hdr);
					use_sink(hdr.payload_len + hdr.header_size + static_cast<unsigned>(hdr.opcode));
					return c.ok ? st == conflux::http::detail::FrameParseStatus::Ok :
								  st == conflux::http::detail::FrameParseStatus::ProtocolError;
				}),
			json);
	}
}

void run_recv_case(
	std::string_view config,
	std::size_t iterations,
	std::size_t warmup,
	bool json) {
	for (auto size:
		 {std::size_t{0}, std::size_t{8}, std::size_t{125}, std::size_t{126}, std::size_t{4096}, std::size_t{65536}}) {
		auto payload = make_payload(size, 'r');
		auto frame = make_masked_frame(0x1U, payload);
		auto variant = std::format("recv_single_{}", size);
		emit(
			run_micro(
				config,
				std::move(variant),
				"actual-WsConn-recv-parse-copy-unmask-utf8",
				iterations,
				1,
				size,
				warmup,
				[&] {
					WsConn ws{-1, std::string{frame}};
					auto f = ws.recv();
					if (f) {
						use_sink(f->payload);
					}
					return f && f->payload == payload;
				}),
			json);
	}
	{
		auto payload = make_payload(32, 's');
		auto frame = make_masked_frame(0x1U, payload);
		auto wire = repeated_wire(frame, 100);
		emit(
			run_micro(
				config,
				"recv_stream_100x32",
				"actual-WsConn-buffer-consume-compaction",
				iterations,
				100,
				payload.size(),
				warmup,
				[&] {
					WsConn ws{-1, std::string{wire}};
					for (int i = 0; i < 100; ++i) {
						auto f = ws.recv();
						if (!f || f->payload != payload) {
							return false;
						}
						use_sink(f->payload);
					}
					return true;
				}),
			json);
	}
	{
		auto payload = make_payload(4096, 'f');
		auto wire = make_fragmented_text(payload, 16);
		emit(
			run_micro(
				config,
				"recv_fragmented_16x256",
				"actual-WsConn-fragment-accumulator",
				iterations,
				1,
				payload.size(),
				warmup,
				[&] {
					WsConn ws{-1, std::string{wire}};
					auto f = ws.recv();
					if (f) {
						use_sink(f->payload);
					}
					return f && f->payload == payload;
				}),
			json);
	}
	{
		auto close = make_close_frame(1000, std::string(64, 'c'));
		emit(
			run_micro(
				config,
				"recv_close_payload_64",
				"actual-WsConn-control-close-payload-validation",
				iterations,
				1,
				66,
				warmup,
				[&] {
					WsConn ws{-1, std::string{close}};
					auto f = ws.recv();
					use_sink(f ? f->payload.size() : 0);
					return !f;
				}),
			json);
	}
}

[[nodiscard]] std::size_t default_iterations(
	std::string_view c) noexcept {
	if (c.starts_with("live"sv)) {
		return 20;
	}
	if (c == "recv"sv) {
		return 2000;
	}
	if (c == "build"sv) {
		return 20000;
	}
	if (c == "header"sv || c == "handshake"sv) {
		return 50000;
	}
	return 1000;
}

[[nodiscard]] bool explicit_iterations_arg(
	int argc,
	char **argv) {
	for (int i = 1; i < argc; ++i) {
		if (std::string_view{argv[i]} == "--iterations"sv) {
			return true;
		}
	}
	return false;
}

void print_usage() {
	std::println(
		"Usage: conflux_websocket_bench [--case handshake|build|header|recv|live|live_concurrent|slow_receiver|all] "
		"[--iterations N] [--warmup N] [--json] [--list]");
}

[[nodiscard]] std::vector<std::string_view> all_cases() {
	return {
		"handshake"sv,
		"build"sv,
		"header"sv,
		"recv"sv,
		"live"sv,
		"live_concurrent"sv,
		"slow_receiver"sv,
	};
}

} // namespace

void *operator new(
	std::size_t size) {
	return allocate_counted(size);
}
void *operator new[](
	std::size_t size) {
	return allocate_counted(size);
}
void *operator new(
	std::size_t size,
	std::align_val_t align) {
	return allocate_counted_aligned(size, static_cast<std::size_t>(align));
}
void *operator new[](
	std::size_t size,
	std::align_val_t align) {
	return allocate_counted_aligned(size, static_cast<std::size_t>(align));
}
void operator delete(
	void *p) noexcept {
	std::free(p);
}
void operator delete[](
	void *p) noexcept {
	std::free(p);
}
void operator delete(
	void *p,
	std::size_t) noexcept {
	std::free(p);
}
void operator delete[](
	void *p,
	std::size_t) noexcept {
	std::free(p);
}
void operator delete(
	void *p,
	std::align_val_t) noexcept {
	std::free(p);
}
void operator delete[](
	void *p,
	std::align_val_t) noexcept {
	std::free(p);
}
void operator delete(
	void *p,
	std::size_t,
	std::align_val_t) noexcept {
	std::free(p);
}
void operator delete[](
	void *p,
	std::size_t,
	std::align_val_t) noexcept {
	std::free(p);
}

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"websocket","parser":"standard","configs":[{"name":"ws_micro_handshake","extra":{"kind":"micro/user-space","evidence_role":"WebSocket handshake key hashing and validation"},"target_ms":500,"max_iterations":2000000,"calibration_iterations":16,"args":["--case","handshake","--config-name","ws_micro_handshake","--iterations","0","--warmup","0"]},{"name":"ws_micro_build_frame","extra":{"kind":"micro/user-space","evidence_role":"server frame construction allocation/copy cost"},"target_ms":500,"max_iterations":500000,"calibration_iterations":8,"args":["--case","build","--config-name","ws_micro_build_frame","--iterations","0","--warmup","0"]},{"name":"ws_micro_parse_header","extra":{"kind":"micro/user-space","evidence_role":"frame header parser and protocol rejection cost"},"target_ms":500,"max_iterations":2000000,"calibration_iterations":16,"args":["--case","header","--config-name","ws_micro_parse_header","--iterations","0","--warmup","0"]},{"name":"ws_micro_recv_path","extra":{"kind":"micro/user-space","evidence_role":"actual WsConn recv parse/unmask/consume/fragment/control cost"},"target_ms":500,"max_iterations":100000,"calibration_iterations":4,"args":["--case","recv","--config-name","ws_micro_recv_path","--iterations","0","--warmup","0"]},{"name":"ws_live_echo","extra":{"kind":"live-kernel-sanity","evidence_role":"plain WebSocket upgrade, echo, ping/pong, fragmentation loopback smoke"},"target_ms":1000,"max_iterations":200,"calibration_iterations":2,"args":["--case","live","--config-name","ws_live_echo","--iterations","0","--warmup","0"],"reps":1},{"name":"ws_live_concurrent","extra":{"kind":"live-kernel-sanity","evidence_role":"parallel WebSocket handoff and echo contention"},"args":["--case","live_concurrent","--config-name","ws_live_concurrent"],"reps":1},{"name":"ws_live_slow_receiver","extra":{"kind":"live-kernel-sanity","evidence_role":"blocking WebSocket send behavior with a slow receiver"},"args":["--case","slow_receiver","--config-name","ws_live_slow_receiver"],"reps":1}]})");

	auto const argv_span = std::span{argv, static_cast<std::size_t>(argc)};
	auto const args = bench_parse_args(argv_span);
	std::string selected = "handshake";
	bool run_all = false;
	bool list = false;
	for (int i = 1; i < argc; ++i) {
		std::string_view const arg{argv[i]};
		if (arg == "--case"sv && i + 1 < argc) {
			selected = argv[++i];
		} else if (arg == "--all-cases"sv) {
			run_all = true;
		} else if (arg == "--list"sv) {
			list = true;
		} else if (arg == "--help"sv || arg == "-h"sv) {
			print_usage();
			return 0;
		}
	}
	if (list) {
		for (auto name: all_cases()) {
			std::println("{}", name);
		}
		return 0;
	}

	auto run_case = [&](std::string_view name) {
		auto const explicit_iters = explicit_iterations_arg(argc, argv);
		auto iterations = explicit_iters && args.iterations != 0 ? args.iterations : default_iterations(name);
		auto const warmup = args.warmup;
		auto const config = args.config_name.empty() ? std::string{name} : args.config_name;
		if (name == "handshake"sv) {
			run_handshake_case(config, iterations, warmup, args.json_out);
		} else if (name == "build"sv) {
			run_build_case(config, iterations, warmup, args.json_out);
		} else if (name == "header"sv) {
			run_header_case(config, iterations, warmup, args.json_out);
		} else if (name == "recv"sv) {
			run_recv_case(config, iterations, warmup, args.json_out);
		} else if (name == "live"sv) {
			iterations = explicit_iters && args.iterations != 0 ? args.iterations : 20;
			emit(run_live_upgrade(config, iterations, warmup), args.json_out);
			emit(
				run_live_echo(config, "live_echo_100x32", std::max<std::size_t>(1, iterations / 2), 100, 32, warmup),
				args.json_out);
			emit(run_live_echo(config, "live_echo_20x4k", iterations, 20, 4096, warmup), args.json_out);
			emit(run_live_ping_pong(config, std::max<std::size_t>(1, iterations / 2), 100, 32, warmup), args.json_out);
			emit(run_live_fragmented(config, iterations, 4096, 16, warmup), args.json_out);
		} else if (name == "live_concurrent"sv) {
			emit(run_live_concurrent_echo(config, 16, 100, 32), args.json_out);
		} else if (name == "slow_receiver"sv) {
			emit(run_live_slow_receiver(config, 64, 4096, 1ms), args.json_out);
		} else {
			throw std::invalid_argument{std::format("unknown WebSocket benchmark case: {}", name)};
		}
	};

	try {
		if (run_all || selected == "all"sv) {
			for (auto name: all_cases()) {
				run_case(name);
			}
		} else {
			run_case(selected);
		}
	} catch (std::exception const &e) {
		std::println(std::cerr, "conflux_websocket_bench: {}", e.what());
		return 1;
	}
	return 0;
}
