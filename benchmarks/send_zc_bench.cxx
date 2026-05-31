#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef CONFLUX_BENCH_HAS_TLS
	#define CONFLUX_BENCH_HAS_TLS 0
#endif

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

struct BenchClient {
	int fd = -1;
	explicit BenchClient(
		std::uint16_t port)
		: BenchClient("127.0.0.1"sv, port) {}
	explicit BenchClient(
		std::string_view host,
		std::uint16_t port) {
		connect_to(host, port);
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
		std::string_view host,
		std::uint16_t port) {
		fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) {
			throw std::runtime_error{"socket failed"};
		}
		static constexpr int one = 1;
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof one);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		auto host_s = std::string{host};
		if (::inet_pton(AF_INET, host_s.c_str(), &addr.sin_addr) != 1) {
			::close(fd);
			fd = -1;
			throw std::runtime_error{std::format("invalid IPv4 host: {}", host)};
		}
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
	void reconnect(
		std::uint16_t port) {
		reconnect("127.0.0.1"sv, port);
	}
	void reconnect(
		std::string_view host,
		std::uint16_t port) {
		close();
		connect_to(host, port);
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
	std::size_t recv_response(
		std::span<char> buf) const {
		std::size_t total = 0;
		std::size_t hdr_end_pos = std::string_view::npos;
		std::size_t body_len = 0;
		bool have_cl = false;
		for (;;) {
			auto n = ::recv(fd, buf.data() + total, buf.size() - total, 0);
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
	std::string_view host,
	std::uint16_t port) {
	for (int i = 0; i < 200; ++i) {
		int const s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (s < 0) {
			continue;
		}
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		auto host_s = std::string{host};
		if (::inet_pton(AF_INET, host_s.c_str(), &addr.sin_addr) != 1) {
			::close(s);
			throw std::runtime_error{std::format("invalid IPv4 host: {}", host)};
		}
		bool const up = ::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
		::close(s);
		if (up) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	throw std::runtime_error{"server did not start in time"};
}
void wait_for_server(
	std::uint16_t port) {
	wait_for_server("127.0.0.1"sv, port);
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
Config bench_config_zc(
	std::string_view mode,
	std::size_t threshold) {
	Config cfg{};
	cfg.port = 0;
	cfg.rings = 1;
	cfg.ring_entries = 256;
	cfg.single_issuer = true;
	cfg.defer_taskrun = true;
	cfg.coop_taskrun = true;
	cfg.taskrun_flag = true;
	cfg.startup_banner = false;
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;
	cfg.send_zc = std::string{mode};
	cfg.send_zc_threshold = threshold;
	cfg.send_zc_report_usage = true;
	return cfg;
}
std::size_t parse_send_zc_threshold(
	std::span<char *> args) {
	std::size_t threshold = 16384;
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const arg = args[i];
		if ((arg == "--send-zc-threshold" || arg == "--zc-threshold") && i + 1 < args.size()) {
			threshold = bench_parse_sz(args[++i]);
		}
	}
	return threshold;
}
bool has_flag(
	std::span<char *> args,
	std::string_view flag) {
	for (std::size_t i = 1; i < args.size(); ++i) {
		if (std::string_view{args[i]} == flag) {
			return true;
		}
	}
	return false;
}
bool is_loopback_or_unspecified_ipv4(
	std::string_view host) {
	auto h = std::string{host};
	in_addr addr{};
	if (::inet_pton(AF_INET, h.c_str(), &addr) != 1) {
		return host == "localhost"sv;
	}
	auto const raw = ntohl(addr.s_addr);
	return (raw >> 24U) == 127U || raw == 0U;
}
std::size_t parse_sz_arg(
	std::span<char *> args,
	std::string_view name,
	std::size_t fallback) {
	for (std::size_t i = 1; i < args.size(); ++i) {
		if (std::string_view{args[i]} == name && i + 1 < args.size()) {
			return bench_parse_sz(args[++i]);
		}
	}
	return fallback;
}
std::string parse_string_arg(
	std::span<char *> args,
	std::string_view name,
	std::string fallback) {
	for (std::size_t i = 1; i < args.size(); ++i) {
		if (std::string_view{args[i]} == name && i + 1 < args.size()) {
			return std::string{args[++i]};
		}
	}
	return fallback;
}
using RunFn = std::function<void()>;
struct Variant {
	std::string name;
	RunFn setup;
	RunFn run;
	RunFn teardown;
	std::function<HttpServerMetrics()> metrics;
	std::size_t ops_per_iter = 1;
	std::size_t iters_override = 0;
};
struct SendZcBenchStats {
	std::string config;
	std::string variant;
	std::size_t iterations{};
	std::uint64_t total_ns{};
	double ns_per_iter{};
	HttpServerMetrics metrics{};
	std::size_t connections{};
	std::size_t duration_s{};
	double requests_per_sec{};
	std::uint64_t errors{};
	std::string transport{};
	std::string host{};
	std::uint16_t port{};
};
SendZcBenchStats run_variant(
	Variant const &v,
	std::size_t iterations,
	std::size_t warmup,
	std::string_view config_name) {
	if (v.iters_override) {
		iterations = v.iters_override;
		warmup = std::max(std::size_t{2}, v.iters_override / 5);
	}
	if (v.setup && warmup > 0) {
		v.setup();
		for (std::size_t i = 0; i < warmup; ++i) {
			v.run();
		}
		if (v.teardown) {
			v.teardown();
		}
	} else {
		for (std::size_t i = 0; i < warmup; ++i) {
			v.run();
		}
	}
	if (v.setup) {
		v.setup();
	}
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < iterations; ++i) {
		v.run();
	}
	auto const t1 = bench_now_ns();
	if (v.teardown) {
		v.teardown();
	}
	auto const total = t1 - t0;
	auto const ns_pi = static_cast<double>(total) / static_cast<double>(iterations);
	return SendZcBenchStats{
		.config = std::string{config_name},
		.variant = v.name,
		.iterations = iterations,
		.total_ns = total,
		.ns_per_iter = ns_pi,
		.metrics = v.metrics ? v.metrics() : HttpServerMetrics{},
	};
}
void print_variant(
	SendZcBenchStats const &s,
	bool json,
	std::size_t ops_per_iter) {
	auto const &zc = s.metrics.send_zc;
	if (json) {
		auto const total_ops = s.iterations * ops_per_iter;
		auto const ns_per_op = s.ns_per_iter / static_cast<double>(ops_per_iter);
		std::string extra;
		if (s.connections != 0) {
			extra = std::format(
				",\"connections\":{},\"duration_s\":{},\"requests_per_sec\":{:.1f},\"errors\":{}",
				s.connections,
				s.duration_s,
				s.requests_per_sec,
				s.errors);
		}
		if (!s.transport.empty()) {
			extra += std::format(",\"transport\":\"{}\",\"host\":\"{}\",\"port\":{}", s.transport, s.host, s.port);
		}
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"ops_per_iter\":{},\"total_ops\":{},\"ns_per_op\":{:.2f},"
			"\"zc_attempts\":{},\"zc_plain_attempts\":{},\"zc_mapped_attempts\":{},"
			"\"zc_bytes_requested\":{},\"zc_bytes_sent\":{},\"zc_notifications\":{},"
			"\"zc_copied_notifications\":{},\"zc_sends_without_notification\":{},"
			"\"zc_errors_enomem\":{},\"zc_errors_other\":{},\"zc_fallback_regular_send\":{},"
			"\"zc_tls_bypass\":{},\"zc_tls_bypass_bytes\":{},\"zc_adaptive_disable_count\":{},"
			"\"zc_notifications_pending\":{},\"zc_capable_rings\":{},\"zc_enabled_rings\":{}{}}}",
			s.config,
			s.variant,
			s.iterations,
			s.total_ns,
			s.ns_per_iter,
			ops_per_iter,
			total_ops,
			ns_per_op,
			zc.attempts,
			zc.plain_attempts,
			zc.mapped_attempts,
			zc.bytes_requested,
			zc.bytes_sent,
			zc.notifications,
			zc.copied_notifications,
			zc.sends_without_notification,
			zc.errors_enomem,
			zc.errors_other,
			zc.fallback_regular_send,
			zc.tls_bypass,
			zc.tls_bypass_bytes,
			zc.adaptive_disable_count,
			s.metrics.zc_notifications_pending,
			s.metrics.zc_capable_rings,
			s.metrics.zc_enabled_rings,
			extra);
	} else if (s.connections != 0) {
		std::println(
			"{:<40} {:>8} reqs  {:>10.0f} req/s  {:>10.2f} ns/req  zc={}/{} map={} errors={}",
			s.variant,
			s.iterations,
			s.requests_per_sec,
			s.ns_per_iter,
			zc.attempts,
			zc.copied_notifications,
			zc.mapped_attempts,
			s.errors);
	} else if (ops_per_iter > 1) {
		auto const ns_per_op = s.ns_per_iter / static_cast<double>(ops_per_iter);
		std::println(
			"{:<40} {:>8} iters  {:>10.2f} ns/iter  {:>10.2f} ns/op (x{})  zc={}/{} map={} tls_bypass={}",
			s.variant,
			s.iterations,
			s.ns_per_iter,
			ns_per_op,
			ops_per_iter,
			zc.attempts,
			zc.copied_notifications,
			zc.mapped_attempts,
			zc.tls_bypass);
	} else {
		std::println(
			"{:<40} {:>8} iters  {:>10.2f} ns/iter  zc={}/{} map={} tls_bypass={}",
			s.variant,
			s.iterations,
			s.ns_per_iter,
			zc.attempts,
			zc.copied_notifications,
			zc.mapped_attempts,
			zc.tls_bypass);
	}
}
struct ConcurrentWorkerResult {
	std::uint64_t requests{};
	std::uint64_t errors{};
};
ConcurrentWorkerResult run_concurrent_worker(
	std::string_view host,
	std::uint16_t port,
	std::string_view request,
	std::size_t response_bytes,
	int connection_count,
	std::atomic<bool> const &start,
	std::atomic<bool> const &stop,
	std::atomic<int> &ready) {
	ConcurrentWorkerResult result;
	std::vector<BenchClient> clients;
	clients.reserve(static_cast<std::size_t>(connection_count));
	for (int i = 0; i < connection_count; ++i) {
		clients.emplace_back(host, port);
	}
	ready.fetch_add(1, std::memory_order_release);
	while (!start.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	std::vector<char> recv_buf(std::max<std::size_t>(response_bytes + 4096, 8192));
	auto rb = std::span<char>{recv_buf};
	while (!stop.load(std::memory_order_relaxed)) {
		for (auto &client: clients) {
			if (stop.load(std::memory_order_relaxed)) {
				break;
			}
			try {
				client.send_all(request);
				(void)client.recv_response(rb);
				++result.requests;
			} catch (...) {
				++result.errors;
				try {
					client.reconnect(host, port);
				} catch (...) {}
			}
		}
	}
	return result;
}
SendZcBenchStats run_concurrent_variant(
	std::string_view config_name,
	std::string name,
	std::string_view mode,
	std::function<Router()> router_factory,
	std::string request,
	std::size_t response_bytes,
	std::size_t send_zc_threshold,
	std::size_t connections,
	std::size_t duration_s,
	std::string_view client_host = "127.0.0.1"sv,
	std::string transport = {}) {
	struct State {
		ServerHandle server;
		HttpServerMetrics metrics{};
	};
	State st{.server = start_server(bench_config_zc(mode, send_zc_threshold), router_factory())};
	auto const hw = std::max(1u, std::thread::hardware_concurrency());
	auto const thread_count = static_cast<int>(std::min<std::size_t>(connections, static_cast<std::size_t>(hw)));
	auto const base = static_cast<int>(connections / static_cast<std::size_t>(thread_count));
	auto const rem = static_cast<int>(connections % static_cast<std::size_t>(thread_count));
	std::atomic<bool> start{false};
	std::atomic<bool> stop{false};
	std::atomic<int> ready{0};
	std::vector<std::thread> workers;
	std::vector<ConcurrentWorkerResult> results(static_cast<std::size_t>(thread_count));
	for (int i = 0; i < thread_count; ++i) {
		auto const worker_connections = base + (i < rem ? 1 : 0);
		workers.emplace_back([&, i, worker_connections] {
			results[static_cast<std::size_t>(i)] = run_concurrent_worker(
				client_host,
				st.server.port,
				std::string_view{request},
				response_bytes,
				worker_connections,
				start,
				stop,
				ready);
		});
	}
	while (ready.load(std::memory_order_acquire) != thread_count) {
		std::this_thread::sleep_for(std::chrono::milliseconds{1});
	}
	auto const t0 = bench_now_ns();
	start.store(true, std::memory_order_release);
	std::this_thread::sleep_for(std::chrono::seconds{static_cast<int>(duration_s)});
	stop.store(true, std::memory_order_release);
	for (auto &worker: workers) {
		worker.join();
	}
	auto const t1 = bench_now_ns();
	st.metrics = stop_server(st.server);
	std::uint64_t requests = 0;
	std::uint64_t errors = 0;
	for (auto const &r: results) {
		requests += r.requests;
		errors += r.errors;
	}
	auto const total_ns = t1 - t0;
	auto const ns_per_iter = requests == 0 ? 0.0 : static_cast<double>(total_ns) / static_cast<double>(requests);
	auto const rps = static_cast<double>(requests) / (static_cast<double>(total_ns) / 1e9);
	return SendZcBenchStats{
		.config = std::string{config_name},
		.variant = std::move(name),
		.iterations = static_cast<std::size_t>(requests),
		.total_ns = total_ns,
		.ns_per_iter = ns_per_iter,
		.metrics = st.metrics,
		.connections = connections,
		.duration_s = duration_s,
		.requests_per_sec = rps,
		.errors = errors,
		.transport = std::move(transport),
		.host = std::string{client_host},
		.port = st.server.port,
	};
}

SendZcBenchStats run_remote_concurrent_variant(
	std::string_view config_name,
	std::string name,
	std::string_view host,
	std::uint16_t port,
	std::string request,
	std::size_t response_bytes,
	std::size_t connections,
	std::size_t duration_s) {
	auto const hw = std::max(1u, std::thread::hardware_concurrency());
	auto const thread_count = static_cast<int>(std::min<std::size_t>(connections, static_cast<std::size_t>(hw)));
	auto const base = static_cast<int>(connections / static_cast<std::size_t>(thread_count));
	auto const rem = static_cast<int>(connections % static_cast<std::size_t>(thread_count));
	std::atomic<bool> start{false};
	std::atomic<bool> stop{false};
	std::atomic<int> ready{0};
	std::vector<std::thread> workers;
	std::vector<ConcurrentWorkerResult> results(static_cast<std::size_t>(thread_count));
	for (int i = 0; i < thread_count; ++i) {
		auto const worker_connections = base + (i < rem ? 1 : 0);
		workers.emplace_back([&, i, worker_connections] {
			results[static_cast<std::size_t>(i)] = run_concurrent_worker(
				host,
				port,
				std::string_view{request},
				response_bytes,
				worker_connections,
				start,
				stop,
				ready);
		});
	}
	while (ready.load(std::memory_order_acquire) != thread_count) {
		std::this_thread::sleep_for(std::chrono::milliseconds{1});
	}
	auto const t0 = bench_now_ns();
	start.store(true, std::memory_order_release);
	std::this_thread::sleep_for(std::chrono::seconds{static_cast<int>(duration_s)});
	stop.store(true, std::memory_order_release);
	for (auto &worker: workers) {
		worker.join();
	}
	auto const t1 = bench_now_ns();
	std::uint64_t requests = 0;
	std::uint64_t errors = 0;
	for (auto const &r: results) {
		requests += r.requests;
		errors += r.errors;
	}
	auto const total_ns = t1 - t0;
	auto const ns_per_iter = requests == 0 ? 0.0 : static_cast<double>(total_ns) / static_cast<double>(requests);
	auto const rps = static_cast<double>(requests) / (static_cast<double>(total_ns) / 1e9);
	return SendZcBenchStats{
		.config = std::string{config_name},
		.variant = std::move(name),
		.iterations = static_cast<std::size_t>(requests),
		.total_ns = total_ns,
		.ns_per_iter = ns_per_iter,
		.connections = connections,
		.duration_s = duration_s,
		.requests_per_sec = rps,
		.errors = errors,
	};
}

#if CONFLUX_BENCH_HAS_TLS
struct SslCtxDeleter {
	void operator ()(
		SSL_CTX *p) const noexcept {
		if (p) {
			SSL_CTX_free(p);
		}
	}
};
struct SslDeleter {
	void operator ()(
		SSL *p) const noexcept {
		if (p) {
			SSL_shutdown(p);
			SSL_free(p);
		}
	}
};
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
	TempCertFiles(
		TempCertFiles &&o) noexcept
		: cert{std::move(o.cert)}
		, key{std::move(o.key)} {}
	TempCertFiles &operator =(TempCertFiles &&) = delete;
};
TempCertFiles make_self_signed_cert() {
	char cert_tmp[] = "/tmp/conflux_send_zc_cert_XXXXXX.pem";
	char key_tmp[] = "/tmp/conflux_send_zc_key_XXXXXX.pem";
	{
		int f = ::mkstemps(cert_tmp, 4);
		if (f < 0) {
			throw std::runtime_error{"mkstemps cert failed"};
		}
		::close(f);
	}
	{
		int f = ::mkstemps(key_tmp, 4);
		if (f < 0) {
			throw std::runtime_error{"mkstemps key failed"};
		}
		::close(f);
	}
	auto cmd = std::format(
		"openssl req -x509 -newkey rsa:2048 -keyout {} -out {} -days 1 -nodes -subj '/CN=localhost' 2>/dev/null",
		key_tmp,
		cert_tmp);
	if (::system(cmd.c_str()) != 0) {
		throw std::runtime_error{"openssl req failed — TLS send_zc bench requires openssl CLI"};
	}
	TempCertFiles files;
	files.cert = cert_tmp;
	files.key = key_tmp;
	return files;
}
void ssl_write_all(
	SSL *ssl,
	std::string_view data) {
	auto const *p = data.data();
	auto remaining = data.size();
	while (remaining > 0) {
		auto const n = SSL_write(ssl, p, static_cast<int>(std::min<std::size_t>(remaining, 16 * 1024)));
		if (n <= 0) {
			throw std::runtime_error{"SSL_write failed"};
		}
		p += n;
		remaining -= static_cast<std::size_t>(n);
	}
}
std::size_t ssl_recv_response(
	SSL *ssl,
	std::span<char> buf) {
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
}
#endif

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"send_zc","parser":"standard","configs":[{"name":"threshold_4k","extra":{"captures_send_zc_counters":true,"send_zc_threshold":4096},"target_ms":1000,"max_iterations":1000,"calibration_iterations":2,"args":["--send-zc-threshold","4096","--config-name","threshold_4k","--iterations","0","--warmup","0"],"reps":1},{"name":"threshold_16k","extra":{"captures_send_zc_counters":true,"send_zc_threshold":16384},"target_ms":1000,"max_iterations":1000,"calibration_iterations":2,"args":["--send-zc-threshold","16384","--config-name","threshold_16k","--iterations","0","--warmup","0"],"reps":1},{"name":"threshold_64k","extra":{"captures_send_zc_counters":true,"send_zc_threshold":65536},"target_ms":1000,"max_iterations":1000,"calibration_iterations":2,"args":["--send-zc-threshold","65536","--config-name","threshold_64k","--iterations","0","--warmup","0"],"reps":1},{"name":"threshold_4k_load","extra":{"captures_send_zc_counters":true,"send_zc_threshold":4096,"load":true,"connections":64,"duration_s":2},"args":["--concurrent","--connections","64","--duration","2","--send-zc-threshold","4096","--config-name","threshold_4k_load"],"reps":1},{"name":"threshold_16k_load","extra":{"captures_send_zc_counters":true,"send_zc_threshold":16384,"load":true,"connections":64,"duration_s":2},"args":["--concurrent","--connections","64","--duration","2","--send-zc-threshold","16384","--config-name","threshold_16k_load"],"reps":1},{"name":"threshold_64k_load","extra":{"captures_send_zc_counters":true,"send_zc_threshold":65536,"load":true,"connections":64,"duration_s":2},"args":["--concurrent","--connections","64","--duration","2","--send-zc-threshold","65536","--config-name","threshold_64k_load"],"reps":1}]})");

	auto const args = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	auto const iters = args.iterations;
	auto const warmup = args.warmup;
	auto const json_out = args.json_out;
	auto const raw_args = std::span{argv, static_cast<std::size_t>(argc)};
	bool const concurrent = has_flag(raw_args, "--concurrent"sv);
	bool const nic_concurrent = has_flag(raw_args, "--nic-concurrent"sv);
	bool const allow_loopback_remote = has_flag(raw_args, "--allow-loopback-remote"sv);
	bool const server_only = has_flag(raw_args, "--server-only"sv);
	bool const client_only = has_flag(raw_args, "--client-only"sv);
	std::size_t const concurrent_connections = std::max<std::size_t>(1, parse_sz_arg(raw_args, "--connections"sv, 64));
	std::size_t const concurrent_duration_s = std::max<std::size_t>(1, parse_sz_arg(raw_args, "--duration"sv, 2));
	std::size_t const send_zc_threshold = parse_send_zc_threshold(raw_args);
	std::uint16_t const remote_port = static_cast<std::uint16_t>(parse_sz_arg(raw_args, "--port"sv, 9095));
	std::string const remote_host = parse_string_arg(raw_args, "--host"sv, "127.0.0.1");
	std::string const remote_path = parse_string_arg(raw_args, "--path"sv, "/body/1M");
	std::string const remote_mode = parse_string_arg(raw_args, "--send-zc-mode"sv, "auto");
	std::size_t const remote_response_bytes = parse_sz_arg(raw_args, "--response-bytes"sv, 1048576);
	std::string const remote_variant =
		parse_string_arg(raw_args, "--variant"sv, std::format("remote/plain/1M/{}", remote_mode));
	std::string const inferred_config_name = std::format("threshold_{}", send_zc_threshold);
	std::string_view const config_name =
		args.config_name.empty() ? std::string_view{inferred_config_name} : std::string_view{args.config_name};
	struct BodySpec {
		std::string_view label;
		std::size_t size;
	};
	static constexpr std::array<BodySpec, 7> kBodies{
		{{"512B", 512}, {"1K", 1024}, {"4K", 4096}, {"16K", 16384}, {"64K", 65536}, {"256K", 262144}, {"1M", 1048576}}
    };

	std::map<std::string, std::string> body_map;
	for (auto const &[label, size]: kBodies) {
		body_map.emplace(std::string{label}, std::string(size, 'X'));
	}

	auto make_body_router = [&] {
		Router r;
		for (auto const &[label, body]: body_map) {
			auto const *body_ptr = &body;
			r.get(std::format("/body/{}", label), [body_ptr](Request const &) {
				return conflux::http::Response::text(*body_ptr);
			});
		}
		return r;
	};

	TempDir static_root{
		std::filesystem::temp_directory_path() / std::format("conflux_send_zc_bench_static_{}", ::getpid())};
	auto const &static_dir = static_root.path;
	for (auto const &[label, size]: kBodies) {
		auto path = static_dir / std::format("{}.bin", label);
		std::ofstream out{path, std::ios::binary};
		std::string data(size, 'Y');
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
	}
	auto make_static_router = [&] {
		Router r;
		r.serve_static("/", std::string{static_dir.string()});
		return r;
	};
	auto make_remote_router = [&] {
		Router r;
		for (auto const &[label, body]: body_map) {
			auto const *body_ptr = &body;
			r.get(std::format("/body/{}", label), [body_ptr](Request const &) {
				return conflux::http::Response::text(*body_ptr);
			});
		}
		r.serve_static("/", std::string{static_dir.string()});
		return r;
	};

	if (server_only && client_only) {
		throw std::runtime_error{"--server-only and --client-only are mutually exclusive"};
	}
	if (nic_concurrent && concurrent) {
		throw std::runtime_error{"--nic-concurrent and --concurrent are mutually exclusive"};
	}
	if (nic_concurrent && (server_only || client_only)) {
		throw std::runtime_error{"--nic-concurrent cannot be combined with --server-only or --client-only"};
	}
	if (nic_concurrent && !allow_loopback_remote && is_loopback_or_unspecified_ipv4(remote_host)) {
		throw std::runtime_error{
			"--nic-concurrent requires --host <non-loopback IPv4>; pass --allow-loopback-remote for smoke only"};
	}
	if (server_only) {
		auto cfg = bench_config_zc(std::string_view{remote_mode}, send_zc_threshold);
		cfg.port = remote_port;
		auto server = start_server(cfg, make_remote_router());
		if (!json_out) {
			std::println(
				"send_zc_bench server: port={}, mode={}, threshold={}, duration={}s",
				server.port,
				remote_mode,
				send_zc_threshold,
				concurrent_duration_s);
		}
		std::this_thread::sleep_for(std::chrono::seconds{static_cast<int>(concurrent_duration_s)});
		auto metrics = stop_server(server);
		SendZcBenchStats s{
			.config = std::string{config_name},
			.variant = remote_variant,
			.metrics = metrics,
			.duration_s = concurrent_duration_s,
		};
		print_variant(s, json_out, 1);
		std::filesystem::remove_all(static_dir);
		return 0;
	}
	if (client_only) {
		auto request = std::format("GET {} HTTP/1.1\r\nHost: {}\r\n\r\n", remote_path, remote_host);
		auto s = run_remote_concurrent_variant(
			config_name,
			remote_variant,
			remote_host,
			remote_port,
			std::move(request),
			remote_response_bytes,
			concurrent_connections,
			concurrent_duration_s);
		print_variant(s, json_out, 1);
		std::filesystem::remove_all(static_dir);
		return 0;
	}

	std::vector<char> recv_buf(1200000);
	auto rb = std::span<char>{recv_buf};
	std::vector<Variant> variants;

	auto make_http_variant = [&](std::string name,
								 std::string_view mode,
								 std::function<Router()> router_factory,
								 std::string request,
								 std::size_t iters_override = 0) {
		struct State {
			std::unique_ptr<ServerHandle> server;
			std::unique_ptr<BenchClient> client;
			HttpServerMetrics metrics{};
		};
		auto st = std::make_shared<State>();
		return Variant{
			.name = std::move(name),
			.setup =
				[st, mode = std::string{mode}, router_factory = std::move(router_factory), send_zc_threshold] {
					st->metrics = {};
					st->server = std::make_unique<ServerHandle>(
						start_server(bench_config_zc(std::string_view{mode}, send_zc_threshold), router_factory()));
					st->client = std::make_unique<BenchClient>(st->server->port);
				},
			.run =
				[st, request = std::move(request), rb] {
					st->client->send_all(request);
					(void)st->client->recv_response(rb);
				},
			.teardown =
				[st] {
					st->client.reset();
					if (st->server) {
						st->metrics = stop_server(*st->server);
						st->server.reset();
					}
				},
			.metrics = [st] { return st->metrics; },
			.ops_per_iter = 1,
			.iters_override = iters_override,
		};
	};

	if (nic_concurrent) {
		if (!json_out) {
			std::println(
				"send_zc_bench NIC-candidate: host={}, {} connections, {}s, threshold={} bytes\n",
				remote_host,
				concurrent_connections,
				concurrent_duration_s,
				send_zc_threshold);
		}
		for (auto const &[label, size]: kBodies) {
			if (size != 65536 && size != 1048576) {
				continue;
			}
			auto const label_s = std::string{label};
			auto const body_req =
				std::string{std::format("GET /body/{} HTTP/1.1\r\nHost: {}\r\n\r\n", label, remote_host)};
			auto const mapped_req =
				std::string{std::format("GET /{}.bin HTTP/1.1\r\nHost: {}\r\n\r\n", label, remote_host)};
			auto plain_off = run_concurrent_variant(
				config_name,
				std::format("nic/plain/{}/off", label_s),
				"off"sv,
				make_body_router,
				body_req,
				size,
				send_zc_threshold,
				concurrent_connections,
				concurrent_duration_s,
				remote_host,
				"non_loopback_nic_candidate");
			print_variant(plain_off, json_out, 1);
			auto plain_zc = run_concurrent_variant(
				config_name,
				std::format("nic/plain/{}/zc_auto", label_s),
				"auto"sv,
				make_body_router,
				body_req,
				size,
				send_zc_threshold,
				concurrent_connections,
				concurrent_duration_s,
				remote_host,
				"non_loopback_nic_candidate");
			print_variant(plain_zc, json_out, 1);
			auto mapped_off = run_concurrent_variant(
				config_name,
				std::format("nic/mapped/{}/off", label_s),
				"off"sv,
				make_static_router,
				mapped_req,
				size,
				send_zc_threshold,
				concurrent_connections,
				concurrent_duration_s,
				remote_host,
				"non_loopback_nic_candidate");
			print_variant(mapped_off, json_out, 1);
			auto mapped_zc = run_concurrent_variant(
				config_name,
				std::format("nic/mapped/{}/zc_auto", label_s),
				"auto"sv,
				make_static_router,
				mapped_req,
				size,
				send_zc_threshold,
				concurrent_connections,
				concurrent_duration_s,
				remote_host,
				"non_loopback_nic_candidate");
			print_variant(mapped_zc, json_out, 1);
		}
		std::filesystem::remove_all(static_dir);
		return 0;
	}

	if (concurrent) {
		if (!json_out) {
			std::println(
				"send_zc_bench concurrent: {} connections, {}s, threshold={} bytes\n",
				concurrent_connections,
				concurrent_duration_s,
				send_zc_threshold);
		}
		for (auto const &[label, size]: kBodies) {
			if (size != 65536 && size != 1048576) {
				continue;
			}
			auto const label_s = std::string{label};
			auto const body_req = std::string{std::format("GET /body/{} HTTP/1.1\r\nHost: localhost\r\n\r\n", label)};
			auto const mapped_req = std::string{std::format("GET /{}.bin HTTP/1.1\r\nHost: localhost\r\n\r\n", label)};
			auto plain_off = run_concurrent_variant(
				config_name,
				std::format("load/plain/{}/off", label_s),
				"off"sv,
				make_body_router,
				body_req,
				size,
				send_zc_threshold,
				concurrent_connections,
				concurrent_duration_s);
			print_variant(plain_off, json_out, 1);
			auto plain_zc = run_concurrent_variant(
				config_name,
				std::format("load/plain/{}/zc_auto", label_s),
				"auto"sv,
				make_body_router,
				body_req,
				size,
				send_zc_threshold,
				concurrent_connections,
				concurrent_duration_s);
			print_variant(plain_zc, json_out, 1);
			auto mapped_off = run_concurrent_variant(
				config_name,
				std::format("load/mapped/{}/off", label_s),
				"off"sv,
				make_static_router,
				mapped_req,
				size,
				send_zc_threshold,
				concurrent_connections,
				concurrent_duration_s);
			print_variant(mapped_off, json_out, 1);
			auto mapped_zc = run_concurrent_variant(
				config_name,
				std::format("load/mapped/{}/zc_auto", label_s),
				"auto"sv,
				make_static_router,
				mapped_req,
				size,
				send_zc_threshold,
				concurrent_connections,
				concurrent_duration_s);
			print_variant(mapped_zc, json_out, 1);
		}
		std::filesystem::remove_all(static_dir);
		return 0;
	}

	for (auto const &[label, size]: kBodies) {
		auto const req = std::string{std::format("GET /body/{} HTTP/1.1\r\nHost: localhost\r\n\r\n", label)};
		auto label_s = std::string{label};
		variants.push_back(make_http_variant(std::format("plain/{}/off", label_s), "off", make_body_router, req));
		variants.push_back(make_http_variant(std::format("plain/{}/zc_auto", label_s), "auto", make_body_router, req));
	}

	for (auto const &[label, size]: kBodies) {
		auto const req = std::string{std::format("GET /{}.bin HTTP/1.1\r\nHost: localhost\r\n\r\n", label)};
		auto label_s = std::string{label};
		variants.push_back(make_http_variant(std::format("mapped/{}/off", label_s), "off", make_static_router, req));
		variants.push_back(
			make_http_variant(std::format("mapped/{}/zc_auto", label_s), "auto", make_static_router, req));
	}

#if CONFLUX_BENCH_HAS_TLS
	auto tls_cert_files = make_self_signed_cert();
	std::unique_ptr<SSL_CTX, SslCtxDeleter> ssl_ctx{[] {
		SSL_CTX *c = SSL_CTX_new(TLS_client_method());
		if (c == nullptr) {
			throw std::runtime_error{"SSL_CTX_new failed"};
		}
		SSL_CTX_set_verify(c, SSL_VERIFY_NONE, nullptr);
		SSL_CTX_set_session_cache_mode(c, SSL_SESS_CACHE_OFF);
		return c;
	}()};

	auto make_tls_variant = [&](std::string name,
								std::string_view mode,
								std::function<Router()> router_factory,
								std::string request,
								std::size_t iters_override = 200) {
		struct State {
			std::unique_ptr<ServerHandle> server;
			std::unique_ptr<BenchClient> client;
			std::unique_ptr<SSL, SslDeleter> ssl;
			HttpServerMetrics metrics{};
		};
		auto st = std::make_shared<State>();
		return Variant{
			.name = std::move(name),
			.setup =
				[&, st, mode = std::string{mode}, router_factory = std::move(router_factory), send_zc_threshold] {
					st->metrics = {};
					auto cfg = bench_config_zc(std::string_view{mode}, send_zc_threshold);
					cfg.cert_file = tls_cert_files.cert;
					cfg.key_file = tls_cert_files.key;
					st->server = std::make_unique<ServerHandle>(start_server(cfg, router_factory()));
					st->client = std::make_unique<BenchClient>(st->server->port);
					SSL *ssl = SSL_new(ssl_ctx.get());
					if (ssl == nullptr) {
						throw std::runtime_error{"SSL_new failed"};
					}
					SSL_set_fd(ssl, st->client->fd);
					if (SSL_connect(ssl) != 1) {
						SSL_free(ssl);
						throw std::runtime_error{"SSL_connect failed"};
					}
					st->ssl.reset(ssl);
				},
			.run =
				[st, request = std::move(request), rb] {
					ssl_write_all(st->ssl.get(), request);
					(void)ssl_recv_response(st->ssl.get(), rb);
				},
			.teardown =
				[st] {
					st->ssl.reset();
					st->client.reset();
					if (st->server) {
						st->metrics = stop_server(*st->server);
						st->server.reset();
					}
				},
			.metrics = [st] { return st->metrics; },
			.ops_per_iter = 1,
			.iters_override = iters_override,
		};
	};

	for (auto const &[label, size]: kBodies) {
		if (size < 65536) {
			continue;
		}
		auto label_s = std::string{label};
		auto const body_req = std::string{std::format("GET /body/{} HTTP/1.1\r\nHost: localhost\r\n\r\n", label)};
		auto const mapped_req = std::string{std::format("GET /{}.bin HTTP/1.1\r\nHost: localhost\r\n\r\n", label)};
		std::size_t const tls_iters = size >= 1048576 ? 50 : 200;
		variants.push_back(
			make_tls_variant(std::format("tls/plain/{}/off", label_s), "off", make_body_router, body_req, tls_iters));
		variants.push_back(make_tls_variant(
			std::format("tls/plain/{}/zc_auto", label_s),
			"auto",
			make_body_router,
			body_req,
			tls_iters));
		variants.push_back(make_tls_variant(
			std::format("tls/mapped/{}/off", label_s),
			"off",
			make_static_router,
			mapped_req,
			tls_iters));
		variants.push_back(make_tls_variant(
			std::format("tls/mapped/{}/zc_auto", label_s),
			"auto",
			make_static_router,
			mapped_req,
			tls_iters));
	}
#endif

	if (!json_out) {
		std::println("send_zc_bench: {} iterations, {} warmup, threshold={} bytes\n", iters, warmup, send_zc_threshold);
	}

	for (auto const &v: variants) {
		auto s = run_variant(v, iters, warmup, config_name);
		print_variant(s, json_out, v.ops_per_iter);
	}

	std::filesystem::remove_all(static_dir);
}
