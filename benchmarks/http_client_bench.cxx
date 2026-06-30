#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <new>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef __has_feature
	#define __has_feature(x) 0
#endif

import std;
import conflux.types;
import conflux.net.http.client;
import conflux.net.client_wire;
import bench_common;

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::uint64_t> g_allocated_bytes{0};
thread_local bool g_suppress_allocation_count{false};
std::atomic<std::size_t> g_sink{0};

void reset_allocation_counters() noexcept {
	g_allocations.store(0, std::memory_order_relaxed);
	g_allocated_bytes.store(0, std::memory_order_relaxed);
}

#if !defined(__SANITIZE_THREAD__) && !__has_feature(thread_sanitizer)
void note_allocation(
	std::size_t size) noexcept {
	if (g_count_allocations.load(std::memory_order_relaxed) && !g_suppress_allocation_count) {
		g_allocations.fetch_add(1, std::memory_order_relaxed);
		g_allocated_bytes.fetch_add(size, std::memory_order_relaxed);
	}
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

#endif

} // namespace

#if !defined(__SANITIZE_THREAD__) && !__has_feature(thread_sanitizer)
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
#endif

namespace {

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using conflux::http::ClientRequest;
using conflux::http::ClientResult;
using conflux::http::HttpClient;
using conflux::http::HttpClientOptions;
using conflux::http::HttpFields;

struct ScopedAllocationCounting {
	ScopedAllocationCounting() {
		reset_allocation_counters();
		g_count_allocations.store(true, std::memory_order_relaxed);
	}
	~ScopedAllocationCounting() { g_count_allocations.store(false, std::memory_order_relaxed); }
	ScopedAllocationCounting(ScopedAllocationCounting const &) = delete;
	ScopedAllocationCounting &operator =(ScopedAllocationCounting const &) = delete;
};

struct ServerAllocationSuppression {
	bool old = g_suppress_allocation_count;
	ServerAllocationSuppression() { g_suppress_allocation_count = true; }
	~ServerAllocationSuppression() { g_suppress_allocation_count = old; }
	ServerAllocationSuppression(ServerAllocationSuppression const &) = delete;
	ServerAllocationSuppression &operator =(ServerAllocationSuppression const &) = delete;
};

struct UniqueFd {
	int fd{-1};
	UniqueFd() = default;
	explicit UniqueFd(
		int f) noexcept
		: fd{f} {}
	~UniqueFd() { reset(); }
	UniqueFd(UniqueFd const &) = delete;
	UniqueFd &operator =(UniqueFd const &) = delete;
	UniqueFd(
		UniqueFd &&o) noexcept
		: fd{std::exchange(o.fd, -1)} {}
	UniqueFd &operator =(
		UniqueFd &&o) noexcept {
		if (this != &o) {
			reset();
			fd = std::exchange(o.fd, -1);
		}
		return *this;
	}
	void reset(
		int next = -1) noexcept {
		if (fd >= 0) {
			::close(fd);
		}
		fd = next;
	}
	[[nodiscard]] int get() const noexcept { return fd; }
	[[nodiscard]] explicit operator bool() const noexcept { return fd >= 0; }
};

[[nodiscard]] std::uint16_t bound_port(
	int fd) {
	sockaddr_in addr{};
	socklen_t len = sizeof(addr);
	if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) < 0) {
		throw std::runtime_error{std::format("getsockname failed: {}", std::strerror(errno))};
	}
	return ntohs(addr.sin_port);
}

void set_common_socket_options(
	int fd) noexcept {
	static constexpr int one = 1;
	(void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	(void)::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
	timeval tv{.tv_sec = 10, .tv_usec = 0};
	(void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void send_all(
	int fd,
	std::string_view data) {
	auto const *p = data.data();
	auto left = data.size();
	while (left > 0) {
		auto const n = ::send(fd, p, left, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			throw std::runtime_error{std::format("send failed: {}", std::strerror(errno))};
		}
		p += static_cast<std::size_t>(n);
		left -= static_cast<std::size_t>(n);
	}
}

[[nodiscard]] std::size_t parse_content_length(
	std::string_view header_block) {
	auto pos = header_block.find("Content-Length: "sv);
	if (pos == std::string_view::npos) {
		return 0;
	}
	pos += "Content-Length: "sv.size();
	auto const end = header_block.find("\r\n"sv, pos);
	std::size_t out{};
	auto const *last =
		end == std::string_view::npos ? header_block.data() + header_block.size() : header_block.data() + end;
	(void)std::from_chars(header_block.data() + pos, last, out);
	return out;
}

[[nodiscard]] std::string_view request_target(
	std::string_view request) noexcept {
	auto const first_space = request.find(' ');
	if (first_space == std::string_view::npos) {
		return "/"sv;
	}
	auto const second_space = request.find(' ', first_space + 1);
	if (second_space == std::string_view::npos || second_space <= first_space + 1) {
		return "/"sv;
	}
	return request.substr(first_space + 1, second_space - first_space - 1);
}

[[nodiscard]] std::string read_request(
	int fd) {
	std::string request;
	request.reserve(4096);
	std::array<char, 8192> buf{};
	std::size_t header_end = std::string::npos;
	std::size_t content_length = 0;
	for (;;) {
		auto const n = ::recv(fd, buf.data(), buf.size(), 0);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		request.append(buf.data(), static_cast<std::size_t>(n));
		if (header_end == std::string::npos) {
			header_end = request.find("\r\n\r\n"sv);
			if (header_end != std::string::npos) {
				header_end += 4;
				content_length = parse_content_length(std::string_view{request}.substr(0, header_end));
			}
		}
		if (header_end != std::string::npos && request.size() >= header_end + content_length) {
			break;
		}
	}
	return request;
}

void append_decimal(
	std::string &out,
	std::size_t value) {
	std::array<char, 32> buf{};
	auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
	if (ec == std::errc{}) {
		out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
	}
}

[[nodiscard]] std::string make_body(
	std::size_t bytes,
	char seed) {
	std::string body;
	body.resize(bytes);
	for (std::size_t i = 0; i < bytes; ++i) {
		body[i] = static_cast<char>(seed + static_cast<char>(i % 23));
	}
	return body;
}

void append_extra_response_headers(
	std::string &out,
	std::size_t header_count,
	std::size_t set_cookie_count) {
	for (std::size_t i = 0; i < header_count; ++i) {
		out += "X-Bench-Header-";
		append_decimal(out, i);
		out += ": value-";
		append_decimal(out, i);
		out += "\r\n";
	}
	for (std::size_t i = 0; i < set_cookie_count; ++i) {
		out += "Set-Cookie: bench";
		append_decimal(out, i);
		out += "=value";
		append_decimal(out, i);
		out += "; Path=/; HttpOnly\r\n";
	}
}

enum class LiveBodyMode : std::uint8_t {
	content_length,
	chunked,
	eof_delimited,
};

struct LiveScenario {
	std::string id;
	std::string variant;
	std::string description;
	LiveBodyMode mode{LiveBodyMode::content_length};
	std::size_t response_body_size{};
	std::size_t request_body_size{};
	std::size_t response_header_count{};
	std::size_t set_cookie_count{};
	std::size_t chunk_size{4096};
	std::size_t redirect_hops{};
	bool method_head{false};
	bool split_header{false};
	std::size_t default_iterations{100};
};

[[nodiscard]] std::string make_fixed_response(
	LiveScenario const &scenario) {
	auto const body = scenario.method_head ? std::string{} : make_body(scenario.response_body_size, 'a');
	std::string out = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n";
	append_extra_response_headers(out, scenario.response_header_count, scenario.set_cookie_count);
	out += "Content-Length: ";
	append_decimal(out, scenario.response_body_size);
	out += "\r\n\r\n";
	out += body;
	return out;
}

[[nodiscard]] std::string make_eof_response(
	LiveScenario const &scenario) {
	std::string out = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n";
	append_extra_response_headers(out, scenario.response_header_count, scenario.set_cookie_count);
	out += "\r\n";
	out += make_body(scenario.response_body_size, 'e');
	return out;
}

[[nodiscard]] std::string make_chunked_response(
	LiveScenario const &scenario) {
	std::string out = "HTTP/1.1 200 OK\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n";
	append_extra_response_headers(out, scenario.response_header_count, scenario.set_cookie_count);
	out += "\r\n";
	auto const body = make_body(scenario.response_body_size, 'c');
	std::size_t pos = 0;
	while (pos < body.size()) {
		auto const n = std::min(scenario.chunk_size, body.size() - pos);
		std::array<char, 32> hex{};
		auto const [ptr, ec] = std::to_chars(hex.data(), hex.data() + hex.size(), n, 16);
		if (ec != std::errc{}) {
			throw std::runtime_error{"chunk size conversion failed"};
		}
		out.append(hex.data(), static_cast<std::size_t>(ptr - hex.data()));
		out += "\r\n";
		out.append(body.data() + pos, n);
		out += "\r\n";
		pos += n;
	}
	out += "0\r\n\r\n";
	return out;
}

[[nodiscard]] std::string make_scenario_response(
	LiveScenario const &scenario) {
	switch (scenario.mode) {
	case LiveBodyMode::content_length: return make_fixed_response(scenario);
	case LiveBodyMode::chunked       : return make_chunked_response(scenario);
	case LiveBodyMode::eof_delimited : return make_eof_response(scenario);
	}
	std::unreachable();
}

[[nodiscard]] std::string make_redirect_response(
	std::size_t next_hop) {
	std::string out = "HTTP/1.1 302 Found\r\nConnection: close\r\nContent-Length: 0\r\nLocation: /redirect/";
	append_decimal(out, next_hop);
	out += "\r\n\r\n";
	return out;
}

class LoopbackHttpServer {
	UniqueFd listen_fd_;
	std::jthread thread_;
	LiveScenario scenario_;
	std::string final_response_;
	std::vector<std::string> redirects_;
	std::uint16_t port_{};
	std::atomic<std::size_t> served_{0};
	std::atomic<std::size_t> errors_{0};
	std::atomic<bool> ready_{false};
	std::size_t expected_connections_{};

	void serve_one(
		int fd) {
		UniqueFd conn{fd};
		set_common_socket_options(conn.get());
		auto const raw = read_request(conn.get());
		auto const target = request_target(raw);
		if (scenario_.redirect_hops > 0 && target.starts_with("/redirect/"sv)) {
			std::size_t hop{};
			auto const hop_sv = target.substr("/redirect/"sv.size());
			(void)std::from_chars(hop_sv.data(), hop_sv.data() + hop_sv.size(), hop);
			if (hop < scenario_.redirect_hops) {
				send_all(conn.get(), redirects_.at(hop));
				return;
			}
		}
		if (scenario_.split_header) {
			auto const marker = final_response_.find("\r\n\r\n"sv);
			auto const split =
				marker == std::string::npos ? std::min<std::size_t>(final_response_.size(), 32) : marker + 2;
			send_all(conn.get(), std::string_view{final_response_}.substr(0, split));
			send_all(conn.get(), std::string_view{final_response_}.substr(split));
			return;
		}
		send_all(conn.get(), final_response_);
	}

	void run() noexcept {
		ServerAllocationSuppression suppress;
		ready_.store(true, std::memory_order_release);
		while (served_.load(std::memory_order_relaxed) < expected_connections_) {
			sockaddr_in peer{};
			socklen_t len = sizeof(peer);
			int fd = ::accept4(listen_fd_.get(), reinterpret_cast<sockaddr *>(&peer), &len, SOCK_CLOEXEC);
			if (fd < 0) {
				if (errno == EINTR) {
					continue;
				}
				if (listen_fd_.get() < 0 || errno == EBADF || errno == EINVAL) {
					break;
				}
				errors_.fetch_add(1, std::memory_order_relaxed);
				continue;
			}
			try {
				serve_one(fd);
			} catch (...) { errors_.fetch_add(1, std::memory_order_relaxed); }
			served_.fetch_add(1, std::memory_order_relaxed);
		}
	}

public:
	LoopbackHttpServer(
		LiveScenario scenario,
		std::size_t expected_connections)
		: scenario_{std::move(scenario)}
		, final_response_{make_scenario_response(scenario_)}
		, expected_connections_{expected_connections} {
		if (scenario_.redirect_hops > 0) {
			redirects_.reserve(scenario_.redirect_hops);
			for (std::size_t i = 0; i < scenario_.redirect_hops; ++i) {
				redirects_.push_back(make_redirect_response(i + 1));
			}
		}
		listen_fd_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP));
		if (!listen_fd_) {
			throw std::runtime_error{std::format("socket failed: {}", std::strerror(errno))};
		}
		static constexpr int one = 1;
		(void)::setsockopt(listen_fd_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (::bind(listen_fd_.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
			throw std::runtime_error{std::format("bind failed: {}", std::strerror(errno))};
		}
		if (::listen(listen_fd_.get(), 1024) < 0) {
			throw std::runtime_error{std::format("listen failed: {}", std::strerror(errno))};
		}
		port_ = bound_port(listen_fd_.get());
		thread_ = std::jthread{[this] { run(); }};
		while (!ready_.load(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
	}

	~LoopbackHttpServer() {
		if (listen_fd_.get() >= 0) {
			(void)::shutdown(listen_fd_.get(), SHUT_RDWR);
		}
		listen_fd_.reset();
	}

	[[nodiscard]] std::uint16_t port() const noexcept { return port_; }
	[[nodiscard]] std::size_t served() const noexcept { return served_.load(std::memory_order_relaxed); }
	[[nodiscard]] std::size_t errors() const noexcept { return errors_.load(std::memory_order_relaxed); }
};

struct ClientStats {
	std::string config;
	std::string variant;
	std::string kind;
	std::size_t iterations{};
	std::uint64_t total_ns{};
	double ns_per_iter{};
	double ops_per_second{};
	double request_bytes_per_iter{};
	double response_bytes_per_iter{};
	double response_body_bytes_per_iter{};
	double allocations_per_iter{};
	double allocated_bytes_per_iter{};
	double dns_ns_per_iter{};
	double connect_ns_per_iter{};
	double ttfb_ns_per_iter{};
	double body_ns_per_iter{};
	std::size_t sample_count{};
	std::size_t batch{};
	std::uint64_t timer_sample_ns{};
	double timer_overhead_pct{};
};

[[nodiscard]] double per_iter(
	std::uint64_t value,
	std::size_t iterations) noexcept {
	return iterations == 0 ? 0.0 : static_cast<double>(value) / static_cast<double>(iterations);
}

void print_stats(
	ClientStats const &s,
	bool json_out) {
	if (json_out) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"kind\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{"
			":.2f},\"request_bytes_per_iter\":{:.2f},\"response_bytes_per_iter\":{:.2f},\"response_body_bytes_per_"
			"iter\":{:.2f},\"allocations_per_iter\":{:.4f},\"allocated_bytes_per_iter\":{:.2f},\"dns_ns_per_iter\":{:."
			"2f},\"connect_ns_per_iter\":{:.2f},\"ttfb_ns_per_iter\":{:.2f},\"body_ns_per_iter\":{:.2f},"
			"\"sample_count\":{},\"batch\":{},\"timer_sample_ns\":{},\"timer_overhead_pct\":{:.4f}}}",
			s.config,
			s.variant,
			s.kind,
			s.iterations,
			s.total_ns,
			s.ns_per_iter,
			s.request_bytes_per_iter,
			s.response_bytes_per_iter,
			s.response_body_bytes_per_iter,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter,
			s.dns_ns_per_iter,
			s.connect_ns_per_iter,
			s.ttfb_ns_per_iter,
			s.body_ns_per_iter,
			s.sample_count,
			s.batch,
			s.timer_sample_ns,
			s.timer_overhead_pct);
		return;
	}
	std::println(
		"{:<44} {:>8} iters {:>12.2f} ns/iter {:>9.2f} allocs/iter {:>10.1f} bytes/iter",
		s.variant,
		s.iterations,
		s.ns_per_iter,
		s.allocations_per_iter,
		s.allocated_bytes_per_iter);
}

struct BenchCase {
	std::string id;
	std::string variant;
	std::string kind;
	std::string description;
	std::size_t default_iterations{};
	std::function<
		ClientStats(std::string_view config, std::size_t warmup, std::size_t iterations, BenchArgs const &args)>
		run;
};

[[nodiscard]] conflux::http::HttpFields make_headers(
	std::size_t count,
	std::string_view prefix = "X-Bench-Header") {
	conflux::http::HttpFields fields{true};
	fields.reserve(count);
	for (std::size_t i = 0; i < count; ++i) {
		fields.emplace_back(std::format("{}-{}", prefix, i), std::format("value-{}", i));
	}
	return fields;
}

[[nodiscard]] ClientRequest make_get_request(
	std::string_view url,
	std::size_t header_count = 0) {
	auto builder = ClientRequest::get(url);
	for (auto const &[k, v]: make_headers(header_count)) {
		builder.header(k, v);
	}
	return std::move(builder).build();
}

[[nodiscard]] ClientRequest make_post_request(
	std::string_view url,
	std::size_t body_size,
	std::size_t header_count = 0) {
	auto body = make_body(body_size, 'p');
	auto builder = ClientRequest::post(url).body(std::move(body));
	for (auto const &[k, v]: make_headers(header_count)) {
		builder.header(k, v);
	}
	return std::move(builder).build();
}

[[nodiscard]] BenchCase make_wire_case(
	std::string id,
	std::string variant,
	std::string description,
	ClientRequest req,
	conflux::http::HttpFields defaults,
	std::size_t default_iterations) {
	auto const variant_name = variant;
	return BenchCase{
		.id = std::move(id),
		.variant = std::move(variant),
		.kind = "micro/client-wire-build",
		.description = std::move(description),
		.default_iterations = default_iterations,
		.run = [req = std::move(req), defaults = std::move(defaults), variant = variant_name](
				   std::string_view config,
				   std::size_t warmup,
				   std::size_t iterations,
				   BenchArgs const &args) mutable {
			for (std::size_t i = 0; i < warmup; ++i) {
				auto wire = conflux::http::client_wire::build_http1_request_wire(req, defaults);
				g_sink.fetch_add(wire->size(), std::memory_order_relaxed);
			}
			BenchSamplePlan const plan = bench_sample_plan(iterations, 0, args.samples, args.batch);
			std::uint64_t request_bytes{};
			std::uint64_t response_bytes{};
			std::uint64_t response_body_bytes{};
			std::uint64_t total_ns{};
			{
				ScopedAllocationCounting counting;
				for (std::size_t i = 0; i < plan.samples; ++i) {
					auto const start = bench_now_ns();
					for (std::size_t j = 0; j < plan.batch; ++j) {
						auto wire = conflux::http::client_wire::build_http1_request_wire(req, defaults);
						request_bytes += wire->size();
						g_sink.fetch_add(wire->size(), std::memory_order_relaxed);
					}
					total_ns += bench_now_ns() - start;
				}
			}
			return ClientStats{
				.config = std::string{config},
				.variant = variant,
				.kind = "micro/client-wire-build",
				.iterations = plan.iterations,
				.total_ns = total_ns,
				.ns_per_iter = per_iter(total_ns, plan.iterations),
				.ops_per_second =
					total_ns == 0 ? 0.0 : static_cast<double>(plan.iterations) * 1e9 / static_cast<double>(total_ns),
				.request_bytes_per_iter = per_iter(request_bytes, plan.iterations),
				.response_bytes_per_iter = per_iter(response_bytes, plan.iterations),
				.response_body_bytes_per_iter = per_iter(response_body_bytes, plan.iterations),
				.allocations_per_iter = per_iter(g_allocations.load(std::memory_order_relaxed), plan.iterations),
				.allocated_bytes_per_iter =
					per_iter(g_allocated_bytes.load(std::memory_order_relaxed), plan.iterations),
				.sample_count = plan.samples,
				.batch = plan.batch,
				.timer_sample_ns = plan.timer_sample_ns,
				.timer_overhead_pct = bench_timer_overhead_percent(plan, total_ns)};
		}};
}

[[nodiscard]] std::string make_response_head(
	std::size_t header_count,
	std::size_t set_cookie_count,
	bool chunked,
	std::size_t content_length) {
	std::string out = "HTTP/1.1 200 OK\r\nServer: conflux-bench\r\n";
	append_extra_response_headers(out, header_count, set_cookie_count);
	if (chunked) {
		out += "Transfer-Encoding: chunked\r\n";
	} else {
		out += "Content-Length: ";
		append_decimal(out, content_length);
		out += "\r\n";
	}
	out += "\r\n";
	return out;
}

[[nodiscard]] BenchCase make_head_case(
	std::string id,
	std::string variant,
	std::string description,
	std::string response_head,
	std::size_t default_iterations) {
	auto const variant_name = variant;
	return BenchCase{
		.id = std::move(id),
		.variant = std::move(variant),
		.kind = "micro/client-response-head-parse",
		.description = std::move(description),
		.default_iterations = default_iterations,
		.run = [head = std::move(response_head), variant = variant_name](
				   std::string_view config,
				   std::size_t warmup,
				   std::size_t iterations,
				   BenchArgs const &args) mutable {
			for (std::size_t i = 0; i < warmup; ++i) {
				auto parsed = conflux::http::client_wire::parse_http1_response_head(head, 16 * 1024 * 1024);
				if (!parsed) {
					throw std::runtime_error{parsed.error().message};
				}
				g_sink.fetch_add(parsed->headers.size() + parsed->set_cookies.size(), std::memory_order_relaxed);
			}
			BenchSamplePlan const plan = bench_sample_plan(iterations, 0, args.samples, args.batch);
			std::uint64_t response_bytes{};
			std::uint64_t total_ns{};
			{
				ScopedAllocationCounting counting;
				for (std::size_t i = 0; i < plan.samples; ++i) {
					auto const start = bench_now_ns();
					for (std::size_t j = 0; j < plan.batch; ++j) {
						auto parsed = conflux::http::client_wire::parse_http1_response_head(head, 16 * 1024 * 1024);
						if (!parsed) {
							throw std::runtime_error{parsed.error().message};
						}
						response_bytes += head.size();
						g_sink.fetch_add(
							parsed->headers.size() + parsed->set_cookies.size(),
							std::memory_order_relaxed);
					}
					total_ns += bench_now_ns() - start;
				}
			}
			return ClientStats{
				.config = std::string{config},
				.variant = variant,
				.kind = "micro/client-response-head-parse",
				.iterations = plan.iterations,
				.total_ns = total_ns,
				.ns_per_iter = per_iter(total_ns, plan.iterations),
				.ops_per_second =
					total_ns == 0 ? 0.0 : static_cast<double>(plan.iterations) * 1e9 / static_cast<double>(total_ns),
				.response_bytes_per_iter = per_iter(response_bytes, plan.iterations),
				.allocations_per_iter = per_iter(g_allocations.load(std::memory_order_relaxed), plan.iterations),
				.allocated_bytes_per_iter =
					per_iter(g_allocated_bytes.load(std::memory_order_relaxed), plan.iterations),
				.sample_count = plan.samples,
				.batch = plan.batch,
				.timer_sample_ns = plan.timer_sample_ns,
				.timer_overhead_pct = bench_timer_overhead_percent(plan, total_ns)};
		}};
}

[[nodiscard]] std::size_t connections_per_iteration(
	LiveScenario const &scenario) noexcept {
	return scenario.redirect_hops + 1;
}

[[nodiscard]] ClientRequest make_live_request(
	LiveScenario const &scenario,
	std::uint16_t port) {
	auto const start_path = scenario.redirect_hops == 0 ? "/bench" : "/redirect/0";
	auto const url = std::format("http://127.0.0.1:{}{}", port, start_path);
	if (scenario.method_head) {
		return ClientRequest::head(url).build();
	}
	if (scenario.request_body_size > 0) {
		return make_post_request(url, scenario.request_body_size, 4);
	}
	if (scenario.redirect_hops > 0) {
		return ClientRequest::get(url).follow_redirects(static_cast<int>(scenario.redirect_hops + 2)).build();
	}
	return make_get_request(url, 4);
}

[[nodiscard]] BenchCase make_live_case(
	LiveScenario scenario) {
	auto const default_iterations = scenario.default_iterations;
	return BenchCase{
		.id = scenario.id,
		.variant = scenario.variant,
		.kind = "loopback/http-client-blocking",
		.description = scenario.description,
		.default_iterations = default_iterations,
		.run = [scenario = std::move(scenario)](
				   std::string_view config,
				   std::size_t warmup,
				   std::size_t iterations,
				   BenchArgs const &args) mutable {
			BenchSamplePlan const plan = bench_sample_plan(iterations, 0, args.samples, args.batch);
			auto const total_calls = warmup + plan.iterations;
			LoopbackHttpServer server{scenario, total_calls * connections_per_iteration(scenario)};
			HttpClientOptions opts{};
			opts.max_body_bytes = std::max<std::size_t>(16 * 1024 * 1024, scenario.response_body_size + 4096);
			opts.max_buffered_bytes = std::max<std::size_t>(4 * 1024 * 1024, scenario.response_body_size + 4096);
			opts.default_timeouts.resolve = 5s;
			opts.default_timeouts.connect = 5s;
			opts.default_timeouts.write = 5s;
			opts.default_timeouts.first_byte = 5s;
			opts.default_timeouts.between_bytes = 5s;
			HttpClient client{std::move(opts)};
			auto const req = make_live_request(scenario, server.port());

			auto run_once = [&]() -> ClientResult { return client.blocking_send(req); };
			for (std::size_t i = 0; i < warmup; ++i) {
				auto result = run_once();
				if (!result) {
					throw std::runtime_error{std::format("warmup request failed: {}", result.error().message)};
				}
				g_sink.fetch_add(
					result->body.size() + static_cast<std::size_t>(result->head.status),
					std::memory_order_relaxed);
			}

			std::uint64_t request_bytes{};
			std::uint64_t response_bytes{};
			std::uint64_t response_body_bytes{};
			std::uint64_t dns_ns{};
			std::uint64_t connect_ns{};
			std::uint64_t ttfb_ns{};
			std::uint64_t body_ns{};
			std::uint64_t total_ns{};
			{
				ScopedAllocationCounting counting;
				for (std::size_t i = 0; i < plan.samples; ++i) {
					auto const start = bench_now_ns();
					for (std::size_t j = 0; j < plan.batch; ++j) {
						auto result = run_once();
						if (!result) {
							throw std::runtime_error{std::format("request failed: {}", result.error().message)};
						}
						request_bytes += result->telemetry.bytes_sent;
						response_bytes += result->telemetry.bytes_received;
						response_body_bytes += result->body.size();
						dns_ns += static_cast<std::uint64_t>(result->telemetry.dns.count());
						connect_ns += static_cast<std::uint64_t>(result->telemetry.connect.count());
						ttfb_ns += static_cast<std::uint64_t>(result->telemetry.ttfb.count());
						body_ns += static_cast<std::uint64_t>(result->telemetry.body.count());
						g_sink.fetch_add(
							result->body.size() + static_cast<std::size_t>(result->head.status),
							std::memory_order_relaxed);
					}
					total_ns += bench_now_ns() - start;
				}
			}
			if (server.errors() != 0) {
				throw std::runtime_error{std::format("loopback server observed {} errors", server.errors())};
			}
			return ClientStats{
				.config = std::string{config},
				.variant = scenario.variant,
				.kind = "loopback/http-client-blocking",
				.iterations = plan.iterations,
				.total_ns = total_ns,
				.ns_per_iter = per_iter(total_ns, plan.iterations),
				.ops_per_second =
					total_ns == 0 ? 0.0 : static_cast<double>(plan.iterations) * 1e9 / static_cast<double>(total_ns),
				.request_bytes_per_iter = per_iter(request_bytes, plan.iterations),
				.response_bytes_per_iter = per_iter(response_bytes, plan.iterations),
				.response_body_bytes_per_iter = per_iter(response_body_bytes, plan.iterations),
				.allocations_per_iter = per_iter(g_allocations.load(std::memory_order_relaxed), plan.iterations),
				.allocated_bytes_per_iter =
					per_iter(g_allocated_bytes.load(std::memory_order_relaxed), plan.iterations),
				.dns_ns_per_iter = per_iter(dns_ns, plan.iterations),
				.connect_ns_per_iter = per_iter(connect_ns, plan.iterations),
				.ttfb_ns_per_iter = per_iter(ttfb_ns, plan.iterations),
				.body_ns_per_iter = per_iter(body_ns, plan.iterations),
				.sample_count = plan.samples,
				.batch = plan.batch,
				.timer_sample_ns = plan.timer_sample_ns,
				.timer_overhead_pct = bench_timer_overhead_percent(plan, total_ns)};
		}};
}

[[nodiscard]] std::vector<std::string_view> all_case_ids() {
	return {
		"wire_get_minimal"sv,
		"wire_get_16_headers"sv,
		"wire_get_defaults_override"sv,
		"wire_post_64k"sv,
		"head_simple_cl"sv,
		"head_64_headers"sv,
		"head_32_set_cookie"sv,
		"head_chunked"sv,
		"live_get_empty"sv,
		"live_get_64b"sv,
		"live_get_4k"sv,
		"live_get_64k"sv,
		"live_get_1m"sv,
		"live_head_cl_64k"sv,
		"live_chunked_64k_4k"sv,
		"live_chunked_16k_tiny"sv,
		"live_eof_64k"sv,
		"live_resp_64_headers"sv,
		"live_resp_32_set_cookie"sv,
		"live_post_4k"sv,
		"live_post_64k"sv,
		"live_redirect_1"sv,
		"live_redirect_3"sv,
		"live_split_header"sv,
	};
}

[[nodiscard]] BenchCase make_case(
	std::string_view id) {
	if (id == "wire_get_minimal"sv) {
		return make_wire_case(
			std::string{id},
			"micro/client_wire/build/get_minimal",
			"Build minimal GET request wire bytes",
			make_get_request("http://127.0.0.1:8080/bench"),
			conflux::http::HttpFields{true},
			300000);
	}
	if (id == "wire_get_16_headers"sv) {
		return make_wire_case(
			std::string{id},
			"micro/client_wire/build/get_16_headers",
			"Build GET request with 16 caller headers",
			make_get_request("http://127.0.0.1:8080/bench?x=1", 16),
			conflux::http::HttpFields{true},
			150000);
	}
	if (id == "wire_get_defaults_override"sv) {
		auto defaults = make_headers(16, "X-Default");
		auto req = ClientRequest::get("http://127.0.0.1:8080/bench")
					   .header("X-Default-7", "override")
					   .header("User-Agent", "conflux-client-bench")
					   .build();
		return make_wire_case(
			std::string{id},
			"micro/client_wire/build/default_headers_override",
			"Build GET request with default-header merge and override",
			std::move(req),
			std::move(defaults),
			100000);
	}
	if (id == "wire_post_64k"sv) {
		return make_wire_case(
			std::string{id},
			"micro/client_wire/build/post_64k",
			"Build POST request headers for a 64 KiB request body",
			make_post_request("http://127.0.0.1:8080/upload", 64 * 1024, 8),
			conflux::http::HttpFields{true},
			100000);
	}
	if (id == "head_simple_cl"sv) {
		return make_head_case(
			std::string{id},
			"micro/client_head/parse/simple_content_length",
			"Parse simple 200 response head with Content-Length",
			make_response_head(2, 0, false, 64),
			250000);
	}
	if (id == "head_64_headers"sv) {
		return make_head_case(
			std::string{id},
			"micro/client_head/parse/64_headers",
			"Parse response head with 64 ordinary headers",
			make_response_head(64, 0, false, 128),
			80000);
	}
	if (id == "head_32_set_cookie"sv) {
		return make_head_case(
			std::string{id},
			"micro/client_head/parse/32_set_cookie",
			"Parse response head with 32 Set-Cookie fields",
			make_response_head(4, 32, false, 128),
			80000);
	}
	if (id == "head_chunked"sv) {
		return make_head_case(
			std::string{id},
			"micro/client_head/parse/chunked",
			"Parse response head with Transfer-Encoding: chunked",
			make_response_head(4, 0, true, 0),
			200000);
	}
	if (id == "live_get_empty"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/get_empty_cl",
				.description = "GET empty Content-Length response over loopback",
				.response_body_size = 0,
				.default_iterations = 300});
	}
	if (id == "live_get_64b"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/get_64b_cl",
				.description = "GET 64 B Content-Length response over loopback",
				.response_body_size = 64,
				.default_iterations = 300});
	}
	if (id == "live_get_4k"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/get_4k_cl",
				.description = "GET 4 KiB Content-Length response over loopback",
				.response_body_size = 4 * 1024,
				.default_iterations = 250});
	}
	if (id == "live_get_64k"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/get_64k_cl",
				.description = "GET 64 KiB Content-Length response over loopback",
				.response_body_size = 64 * 1024,
				.default_iterations = 120});
	}
	if (id == "live_get_1m"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/get_1m_cl",
				.description = "GET 1 MiB Content-Length response over loopback",
				.response_body_size = 1024 * 1024,
				.default_iterations = 24});
	}
	if (id == "live_head_cl_64k"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/head_64k_cl",
				.description = "HEAD response with Content-Length but no body",
				.response_body_size = 64 * 1024,
				.method_head = true,
				.default_iterations = 250});
	}
	if (id == "live_chunked_64k_4k"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/chunked_64k_4k_chunks",
				.description = "GET 64 KiB chunked response in 4 KiB chunks",
				.mode = LiveBodyMode::chunked,
				.response_body_size = 64 * 1024,
				.chunk_size = 4 * 1024,
				.default_iterations = 100});
	}
	if (id == "live_chunked_16k_tiny"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/chunked_16k_16b_chunks",
				.description = "GET 16 KiB chunked response in many 16 B chunks",
				.mode = LiveBodyMode::chunked,
				.response_body_size = 16 * 1024,
				.chunk_size = 16,
				.default_iterations = 80});
	}
	if (id == "live_eof_64k"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/eof_64k",
				.description = "GET 64 KiB EOF-delimited response",
				.mode = LiveBodyMode::eof_delimited,
				.response_body_size = 64 * 1024,
				.default_iterations = 100});
	}
	if (id == "live_resp_64_headers"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/response_64_headers",
				.description = "GET response with 64 ordinary headers",
				.response_body_size = 64,
				.response_header_count = 64,
				.default_iterations = 150});
	}
	if (id == "live_resp_32_set_cookie"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/response_32_set_cookie",
				.description = "GET response with 32 Set-Cookie fields",
				.response_body_size = 64,
				.set_cookie_count = 32,
				.default_iterations = 150});
	}
	if (id == "live_post_4k"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/post_4k_req_64b_resp",
				.description = "POST 4 KiB request body, receive 64 B response",
				.response_body_size = 64,
				.request_body_size = 4 * 1024,
				.default_iterations = 180});
	}
	if (id == "live_post_64k"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/post_64k_req_64b_resp",
				.description = "POST 64 KiB request body, receive 64 B response",
				.response_body_size = 64,
				.request_body_size = 64 * 1024,
				.default_iterations = 100});
	}
	if (id == "live_redirect_1"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/redirect_1_hop",
				.description = "Follow one same-origin redirect",
				.response_body_size = 64,
				.redirect_hops = 1,
				.default_iterations = 120});
	}
	if (id == "live_redirect_3"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/redirect_3_hops",
				.description = "Follow three same-origin redirects",
				.response_body_size = 64,
				.redirect_hops = 3,
				.default_iterations = 60});
	}
	if (id == "live_split_header"sv) {
		return make_live_case(
			LiveScenario{
				.id = std::string{id},
				.variant = "loopback/client_blocking/split_response_header",
				.description = "GET response whose CRLFCRLF boundary is split across sends",
				.response_body_size = 4 * 1024,
				.split_header = true,
				.default_iterations = 120});
	}
	throw std::invalid_argument{std::format("unknown HTTP client benchmark case: {}", id)};
}

[[nodiscard]] bool has_flag(
	std::span<char *> args,
	std::string_view flag) noexcept {
	return std::ranges::any_of(args.subspan(1), [flag](char const *arg) { return std::string_view{arg} == flag; });
}

[[nodiscard]] std::optional<std::string_view> arg_value(
	std::span<char *> args,
	std::string_view flag) {
	for (std::size_t i = 1; i + 1 < args.size(); ++i) {
		if (std::string_view{args[i]} == flag) {
			return std::string_view{args[i + 1]};
		}
	}
	return std::nullopt;
}

[[nodiscard]] std::vector<BenchCase> selected_cases(
	std::span<char *> args) {
	if (has_flag(args, "--all-cases"sv)) {
		std::vector<BenchCase> out;
		for (auto id: all_case_ids()) {
			out.push_back(make_case(id));
		}
		return out;
	}
	if (auto id = arg_value(args, "--case"sv); id.has_value()) {
		return {make_case(*id)};
	}
	std::vector<BenchCase> out;
	auto filters = bench_parse_args(args).filters;
	for (auto id: all_case_ids()) {
		auto c = make_case(id);
		if (bench_matches_filter(std::span<std::string const>{filters}, c.variant)
			|| bench_matches_filter(std::span<std::string const>{filters}, c.id)) {
			out.push_back(std::move(c));
		}
	}
	return out;
}

void print_list() {
	for (auto id: all_case_ids()) {
		auto c = make_case(id);
		std::println("{:<28} {:<54} {}", c.id, c.variant, c.description);
	}
}

void print_usage() {
	std::println(
		"Usage: conflux_http_client_bench [--case NAME|--all-cases] [--filter SUBSTR] [--iterations N] "
		"[--warmup N] [--json] [--list]");
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"http_client","parser":"standard","configs":[{"name":"wire_get_minimal","extra":{"kind":"micro/client-wire-build","case":"minimal GET request serialization"},"target_ms":500,"max_iterations":2000000,"calibration_iterations":16,"args":["--case","wire_get_minimal","--config-name","wire_get_minimal","--iterations","0","--warmup","0"]},{"name":"wire_get_16_headers","extra":{"kind":"micro/client-wire-build","case":"GET serialization with 16 headers"},"target_ms":500,"max_iterations":1000000,"calibration_iterations":8,"args":["--case","wire_get_16_headers","--config-name","wire_get_16_headers","--iterations","0","--warmup","0"]},{"name":"wire_get_defaults_override","extra":{"kind":"micro/client-wire-build","case":"default header merge and override"},"target_ms":500,"max_iterations":1000000,"calibration_iterations":8,"args":["--case","wire_get_defaults_override","--config-name","wire_get_defaults_override","--iterations","0","--warmup","0"]},{"name":"wire_post_64k","extra":{"kind":"micro/client-wire-build","case":"POST request serialization metadata for 64 KiB body"},"target_ms":500,"max_iterations":1000000,"calibration_iterations":8,"args":["--case","wire_post_64k","--config-name","wire_post_64k","--iterations","0","--warmup","0"]},{"name":"head_simple_cl","extra":{"kind":"micro/client-response-head-parse","case":"simple Content-Length response head"},"target_ms":500,"max_iterations":2000000,"calibration_iterations":16,"args":["--case","head_simple_cl","--config-name","head_simple_cl","--iterations","0","--warmup","0"]},{"name":"head_64_headers","extra":{"kind":"micro/client-response-head-parse","case":"64 response headers"},"target_ms":500,"max_iterations":1000000,"calibration_iterations":8,"args":["--case","head_64_headers","--config-name","head_64_headers","--iterations","0","--warmup","0"]},{"name":"head_32_set_cookie","extra":{"kind":"micro/client-response-head-parse","case":"32 Set-Cookie response headers"},"target_ms":500,"max_iterations":1000000,"calibration_iterations":8,"args":["--case","head_32_set_cookie","--config-name","head_32_set_cookie","--iterations","0","--warmup","0"]},{"name":"head_chunked","extra":{"kind":"micro/client-response-head-parse","case":"chunked response head"},"target_ms":500,"max_iterations":2000000,"calibration_iterations":16,"args":["--case","head_chunked","--config-name","head_chunked","--iterations","0","--warmup","0"]},{"name":"live_get_empty","extra":{"kind":"loopback/http-client-blocking","case":"GET empty Content-Length body"},"target_ms":700,"max_iterations":3000,"calibration_iterations":2,"args":["--case","live_get_empty","--config-name","live_get_empty","--iterations","0","--warmup","0"],"reps":1},{"name":"live_get_64b","extra":{"kind":"loopback/http-client-blocking","case":"GET 64 B Content-Length body"},"target_ms":700,"max_iterations":3000,"calibration_iterations":2,"args":["--case","live_get_64b","--config-name","live_get_64b","--iterations","0","--warmup","0"],"reps":1},{"name":"live_get_4k","extra":{"kind":"loopback/http-client-blocking","case":"GET 4 KiB Content-Length body"},"target_ms":700,"max_iterations":2500,"calibration_iterations":2,"args":["--case","live_get_4k","--config-name","live_get_4k","--iterations","0","--warmup","0"],"reps":1},{"name":"live_get_64k","extra":{"kind":"loopback/http-client-blocking","case":"GET 64 KiB Content-Length body"},"target_ms":700,"max_iterations":1200,"calibration_iterations":2,"args":["--case","live_get_64k","--config-name","live_get_64k","--iterations","0","--warmup","0"],"reps":1},{"name":"live_get_1m","extra":{"kind":"loopback/http-client-blocking","case":"GET 1 MiB Content-Length body"},"target_ms":700,"max_iterations":160,"calibration_iterations":1,"args":["--case","live_get_1m","--config-name","live_get_1m","--iterations","0","--warmup","0"],"reps":1},{"name":"live_head_cl_64k","extra":{"kind":"loopback/http-client-blocking","case":"HEAD no-body response"},"target_ms":700,"max_iterations":3000,"calibration_iterations":2,"args":["--case","live_head_cl_64k","--config-name","live_head_cl_64k","--iterations","0","--warmup","0"],"reps":1},{"name":"live_chunked_64k_4k","extra":{"kind":"loopback/http-client-blocking","case":"64 KiB chunked body in 4 KiB chunks"},"target_ms":700,"max_iterations":1000,"calibration_iterations":2,"args":["--case","live_chunked_64k_4k","--config-name","live_chunked_64k_4k","--iterations","0","--warmup","0"],"reps":1},{"name":"live_chunked_16k_tiny","extra":{"kind":"loopback/http-client-blocking","case":"16 KiB chunked body in 16 B chunks"},"target_ms":700,"max_iterations":1000,"calibration_iterations":2,"args":["--case","live_chunked_16k_tiny","--config-name","live_chunked_16k_tiny","--iterations","0","--warmup","0"],"reps":1},{"name":"live_eof_64k","extra":{"kind":"loopback/http-client-blocking","case":"64 KiB EOF-delimited body"},"target_ms":700,"max_iterations":1000,"calibration_iterations":2,"args":["--case","live_eof_64k","--config-name","live_eof_64k","--iterations","0","--warmup","0"],"reps":1},{"name":"live_resp_64_headers","extra":{"kind":"loopback/http-client-blocking","case":"64 response headers"},"target_ms":700,"max_iterations":2000,"calibration_iterations":2,"args":["--case","live_resp_64_headers","--config-name","live_resp_64_headers","--iterations","0","--warmup","0"],"reps":1},{"name":"live_resp_32_set_cookie","extra":{"kind":"loopback/http-client-blocking","case":"32 Set-Cookie response headers"},"target_ms":700,"max_iterations":2000,"calibration_iterations":2,"args":["--case","live_resp_32_set_cookie","--config-name","live_resp_32_set_cookie","--iterations","0","--warmup","0"],"reps":1},{"name":"live_post_4k","extra":{"kind":"loopback/http-client-blocking","case":"POST 4 KiB request body"},"target_ms":700,"max_iterations":2000,"calibration_iterations":2,"args":["--case","live_post_4k","--config-name","live_post_4k","--iterations","0","--warmup","0"],"reps":1},{"name":"live_post_64k","extra":{"kind":"loopback/http-client-blocking","case":"POST 64 KiB request body"},"target_ms":700,"max_iterations":1000,"calibration_iterations":2,"args":["--case","live_post_64k","--config-name","live_post_64k","--iterations","0","--warmup","0"],"reps":1},{"name":"live_redirect_1","extra":{"kind":"loopback/http-client-blocking","case":"one same-origin redirect"},"target_ms":700,"max_iterations":1000,"calibration_iterations":2,"args":["--case","live_redirect_1","--config-name","live_redirect_1","--iterations","0","--warmup","0"],"reps":1},{"name":"live_redirect_3","extra":{"kind":"loopback/http-client-blocking","case":"three same-origin redirects"},"target_ms":700,"max_iterations":500,"calibration_iterations":2,"args":["--case","live_redirect_3","--config-name","live_redirect_3","--iterations","0","--warmup","0"],"reps":1},{"name":"live_split_header","extra":{"kind":"loopback/http-client-blocking","case":"split response header delimiter"},"target_ms":700,"max_iterations":1000,"calibration_iterations":2,"args":["--case","live_split_header","--config-name","live_split_header","--iterations","0","--warmup","0"],"reps":1}]})");
	try {
		(void)::signal(SIGPIPE, SIG_IGN);
		auto const args = std::span{argv, static_cast<std::size_t>(argc)};
		if (has_flag(args, "--help"sv) || has_flag(args, "-h"sv)) {
			print_usage();
			return 0;
		}
		if (has_flag(args, "--list"sv)) {
			print_list();
			return 0;
		}
		auto const parsed = bench_parse_args(args);
		auto const cases = selected_cases(args);
		if (cases.empty()) {
			throw std::invalid_argument{"no HTTP client benchmark cases selected"};
		}
		for (auto const &c: cases) {
			auto const iterations = parsed.iterations == 0 ? c.default_iterations : parsed.iterations;
			auto stats = c.run(
				parsed.config_name.empty() ? c.id : std::string_view{parsed.config_name},
				parsed.warmup,
				iterations,
				parsed);
			print_stats(stats, parsed.json_out);
		}
		std::println(std::cerr, "sink={}", g_sink.load(std::memory_order_relaxed));
		return 0;
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_http_client_bench: {}", ex.what());
		print_usage();
		return 1;
	}
}
