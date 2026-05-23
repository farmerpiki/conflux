// static_strategy_matrix_bench — live static-file path matrix.
//
// This is a live-kernel-sanity benchmark. It intentionally exercises the
// HTTP server, filesystem, page cache, mmap fallback, and splice-capable
// streamed-file path so static-file numbers can be interpreted by strategy
// instead of as one opaque row in http_server_bench.

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import conflux.work;
import bench_common;

using namespace std::string_view_literals;
using namespace std::literals;

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
		std::filesystem::remove_all(path, ec);
	}

	TempDir(TempDir const &) = delete;
	TempDir &operator =(TempDir const &) = delete;
};

struct BenchClient {
	int fd = -1;

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

	void reconnect(
		std::uint16_t port) {
		close();
		connect_to(port);
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
		timeval tv{.tv_sec = 10, .tv_usec = 0};
		::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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

	void send_all(
		std::string_view data) const {
		auto const *p = data.data();
		auto left = data.size();
		while (left > 0) {
			auto const n = ::send(fd, p, left, MSG_NOSIGNAL);
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
		std::span<char> scratch) const {
		std::string headers;
		headers.reserve(4096);
		std::array<char, 4096> small{};
		std::size_t body_read = 0;
		std::size_t content_length = 0;
		bool have_header = false;
		bool have_cl = false;
		for (;;) {
			auto const n = ::recv(fd, small.data(), small.size(), 0);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				break;
			}
			auto chunk = std::string_view{small.data(), static_cast<std::size_t>(n)};
			if (!have_header) {
				headers.append(chunk);
				auto const pos = headers.find("\r\n\r\n");
				if (pos == std::string::npos) {
					continue;
				}
				have_header = true;
				auto const header_end = pos + 4;
				auto const cl = headers.find("Content-Length: ");
				if (cl != std::string::npos && cl < header_end) {
					auto const first = cl + 16;
					auto const last = headers.find("\r\n", first);
					std::from_chars(headers.data() + first, headers.data() + last, content_length);
					have_cl = true;
				}
				if (headers.starts_with("HTTP/1.1 304") || headers.starts_with("HTTP/1.1 204")) {
					return header_end;
				}
				body_read += headers.size() - header_end;
				if (have_cl && body_read >= content_length) {
					return header_end + body_read;
				}
				continue;
			}
			body_read += static_cast<std::size_t>(n);
			if (!scratch.empty()) {
				g_sink += static_cast<unsigned char>(chunk.front());
			}
			if (have_cl && body_read >= content_length) {
				return headers.find("\r\n\r\n") + 4 + body_read;
			}
		}
		return headers.size() + body_read;
	}

	static std::atomic<std::size_t> g_sink;
};

std::atomic<std::size_t> BenchClient::g_sink{};

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
		std::this_thread::sleep_for(10ms);
	}
	throw std::runtime_error{"server did not start in time"};
}

struct ServerHandle {
	std::shared_ptr<HttpServer> server;
	std::thread thr;
	std::uint16_t port{};

	~ServerHandle() { stop(); }
	ServerHandle() = default;
	ServerHandle(ServerHandle const &) = delete;
	ServerHandle &operator =(ServerHandle const &) = delete;
	ServerHandle(ServerHandle &&) noexcept = default;
	ServerHandle &operator =(ServerHandle &&) noexcept = default;

	void stop() {
		if (server) {
			server->shutdown();
			server.reset();
		}
		if (thr.joinable()) {
			thr.join();
		}
	}
};

[[nodiscard]] Config make_config(
	std::string_view strategy) {
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
	if (strategy == "splice"sv) {
		cfg.fixed_buffer_slabs = 16;
		cfg.fixed_buffer_bytes = 16U * 1024U;
		cfg.splice_pipe_pairs = 2;
	}
	if (strategy == "cached"sv) {
		cfg.static_file_cache.enabled = true;
		cfg.static_file_cache.small_file_max_bytes = 64U * 1024U;
		cfg.static_file_cache.max_total_bytes = 512U * 1024U;
	}
	return cfg;
}

[[nodiscard]] ServerHandle start_static_server(
	Config cfg,
	std::filesystem::path const &root) {
	(void)::signal(SIGPIPE, SIG_IGN);
	Router router;
	router.serve_static("/static", root.string());
	auto srv = std::make_shared<HttpServer>(cfg, std::move(router));
	std::thread t{[srv] {
		try {
			auto _ = srv->run();
		} catch (std::exception const &e) { std::println(std::cerr, "static strategy bench server: {}", e.what()); }
	}};
	auto const p = srv->port();
	wait_for_server(p);
	ServerHandle out;
	out.server = std::move(srv);
	out.thr = std::move(t);
	out.port = p;
	return out;
}

void write_file(
	std::filesystem::path const &path,
	std::size_t bytes,
	char seed) {
	std::ofstream out{path, std::ios::binary | std::ios::trunc};
	if (!out) {
		throw std::runtime_error{std::format("open failed: {}", path.string())};
	}
	std::array<char, 64 * 1024> block{};
	for (std::size_t i = 0; i < block.size(); ++i) {
		block[i] = static_cast<char>(seed + static_cast<char>(i % 23));
	}
	std::size_t left = bytes;
	while (left > 0) {
		auto const n = std::min(left, block.size());
		out.write(block.data(), static_cast<std::streamsize>(n));
		left -= n;
	}
}

[[nodiscard]] std::string get_request(
	std::string_view path,
	std::string_view extra = {}) {
	return std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}\r\n", path, extra);
}

struct StaticCase {
	std::string_view name;
	std::string_view strategy;
	std::uint16_t port{};
	std::string request;
	std::size_t response_body_bytes{};
	std::size_t iterations{};
	std::size_t churn_files{};
};

struct RowStats {
	std::string_view name;
	std::string_view strategy;
	std::size_t iterations{};
	std::size_t bytes{};
	std::uint64_t total_ns{};
	double ns_per_iter{};
	double rps{};
	double mib_per_s{};
	std::uint64_t p50_ns{};
	std::uint64_t p99_ns{};
	std::uint64_t p999_ns{};
	std::size_t churn_files{};
};

[[nodiscard]] std::uint64_t percentile(
	std::vector<std::uint64_t> const &samples,
	double p) {
	if (samples.empty()) {
		return 0;
	}
	auto idx = static_cast<std::size_t>(std::ceil(p * static_cast<double>(samples.size())));
	idx = std::min(samples.size() - 1, idx == 0 ? std::size_t{0} : idx - 1);
	return samples[idx];
}

[[nodiscard]] RowStats run_case(
	StaticCase const &c,
	std::span<char> scratch,
	std::size_t warmup) {
	std::size_t churn_idx = 0;
	auto one_request = [&] {
		BenchClient client{c.port};
		if (c.churn_files == 0) {
			client.send_all(c.request);
		} else {
			auto req = get_request(std::format("/static/churn/{:03}.bin", churn_idx++ % c.churn_files));
			client.send_all(req);
		}
		return client.recv_response(scratch);
	};
	for (std::size_t i = 0; i < warmup; ++i) {
		(void)one_request();
	}
	std::vector<std::uint64_t> samples;
	samples.reserve(c.iterations);
	std::size_t bytes = 0;
	auto const t0 = bench_now_ns();
	for (std::size_t i = 0; i < c.iterations; ++i) {
		auto const s0 = bench_now_ns();
		auto const received = one_request();
		if (received < c.response_body_bytes) {
			throw std::runtime_error{
				std::format("{} received {} bytes, expected at least {}", c.name, received, c.response_body_bytes)};
		}
		bytes += received;
		auto const s1 = bench_now_ns();
		samples.push_back(s1 - s0);
	}
	auto const t1 = bench_now_ns();
	std::sort(samples.begin(), samples.end());
	auto const total_ns = t1 - t0;
	auto const sec = static_cast<double>(total_ns) / 1e9;
	return RowStats{
		.name = c.name,
		.strategy = c.strategy,
		.iterations = c.iterations,
		.bytes = bytes,
		.total_ns = total_ns,
		.ns_per_iter = static_cast<double>(total_ns) / static_cast<double>(c.iterations),
		.rps = sec > 0.0 ? static_cast<double>(c.iterations) / sec : 0.0,
		.mib_per_s = sec > 0.0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / sec : 0.0,
		.p50_ns = percentile(samples, 0.50),
		.p99_ns = percentile(samples, 0.99),
		.p999_ns = percentile(samples, 0.999),
		.churn_files = c.churn_files};
}

void print_row(
	RowStats const &r,
	bool json,
	bool first) {
	if (json) {
		std::println(
			"{{\"config\":\"static_strategy_matrix\",\"variant\":\"{}\",\"strategy\":\"{}\",\"iterations\":{},"
			"\"total_ns\":{},\"ns_per_iter\":{:.2f},\"rps\":{:.2f},\"mib_per_s\":{:.2f},\"p50_ns\":{},"
			"\"p99_ns\":{},\"p999_ns\":{},\"bytes\":{},\"churn_files\":{}}}",
			r.name,
			r.strategy,
			r.iterations,
			r.total_ns,
			r.ns_per_iter,
			r.rps,
			r.mib_per_s,
			r.p50_ns,
			r.p99_ns,
			r.p999_ns,
			r.bytes,
			r.churn_files);
		(void)first;
		return;
	}
	if (first) {
		std::println(
			"{:<28} {:<12} {:>8} {:>12} {:>12} {:>10} {:>10}",
			"variant",
			"strategy",
			"iters",
			"ns/iter",
			"MiB/s",
			"p99 us",
			"p999 us");
	}
	std::println(
		"{:<28} {:<12} {:>8} {:>12.2f} {:>12.2f} {:>10.2f} {:>10.2f}",
		r.name,
		r.strategy,
		r.iterations,
		r.ns_per_iter,
		r.mib_per_s,
		static_cast<double>(r.p99_ns) / 1000.0,
		static_cast<double>(r.p999_ns) / 1000.0);
}

[[nodiscard]] std::size_t variant_iterations(
	std::string_view name,
	std::size_t requested) {
	if (name.find("128m"sv) != std::string_view::npos) {
		return std::min<std::size_t>(requested, 8);
	}
	if (name.find("1m"sv) != std::string_view::npos || name.find("churn"sv) != std::string_view::npos) {
		return std::min<std::size_t>(requested, 64);
	}
	return requested;
}

[[nodiscard]] std::size_t variant_warmup(
	std::string_view name,
	std::size_t requested) {
	if (name.find("128m"sv) != std::string_view::npos) {
		return std::min<std::size_t>(requested, 1);
	}
	if (name.find("1m"sv) != std::string_view::npos || name.find("churn"sv) != std::string_view::npos) {
		return std::min<std::size_t>(requested, 4);
	}
	return requested;
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"static_strategy_matrix","category":"live-kernel-sanity","description":"Static file HTTP strategy matrix for mmap fallback, splice-capable streaming, cache hits, range requests, and cache churn","metrics":["ns_per_iter","rps","mib_per_s","p50_ns","p99_ns","p999_ns","bytes","churn_files"],"notes":"Does not drop kernel page cache and does not prove TLS read+write; use storage_read_bench and TLS static consumers for those gates."})");

	auto args = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	if (args.iterations == 200000) {
		args.iterations = 200;
	}
	if (args.warmup == 40000) {
		args.warmup = 10;
	}

	TempDir root{std::filesystem::temp_directory_path() / std::format("conflux_static_strategy_{}", ::getpid())};
	std::filesystem::create_directories(root.path / "churn");
	write_file(root.path / "1k.txt", 1024, 'a');
	write_file(root.path / "64k.bin", 64U * 1024U, 'b');
	write_file(root.path / "1m.bin", 1024U * 1024U, 'c');
	write_file(root.path / "128m.bin", 128U * 1024U * 1024U, 'd');
	static constexpr std::size_t kChurnFiles = 32;
	for (std::size_t i = 0; i < kChurnFiles; ++i) {
		write_file(root.path / "churn" / std::format("{:03}.bin", i), 64U * 1024U, static_cast<char>('A' + (i % 26)));
	}

	auto mmap_srv = start_static_server(make_config("mmap"sv), root.path);
	auto splice_srv = start_static_server(make_config("splice"sv), root.path);
	auto cache_srv = start_static_server(make_config("cached"sv), root.path);

	std::vector<char> scratch(256U * 1024U);
	std::vector<StaticCase> cases;
	auto add_case = [&](std::string_view name,
						std::string_view strategy,
						std::uint16_t port,
						std::string request,
						std::size_t body_bytes,
						std::size_t churn_files = 0) {
		cases.push_back(
			StaticCase{
				.name = name,
				.strategy = strategy,
				.port = port,
				.request = std::move(request),
				.response_body_bytes = body_bytes,
				.iterations = variant_iterations(name, args.iterations),
				.churn_files = churn_files});
	};

	add_case("mmap_1k_hot"sv, "mmap"sv, mmap_srv.port, get_request("/static/1k.txt"sv), 1024);
	add_case("mmap_64k_hot"sv, "mmap"sv, mmap_srv.port, get_request("/static/64k.bin"sv), 64U * 1024U);
	add_case("mmap_1m_hot"sv, "mmap"sv, mmap_srv.port, get_request("/static/1m.bin"sv), 1024U * 1024U);
	add_case("mmap_128m_hot"sv, "mmap"sv, mmap_srv.port, get_request("/static/128m.bin"sv), 128U * 1024U * 1024U);
	add_case(
		"mmap_range_1k"sv,
		"mmap_range"sv,
		mmap_srv.port,
		get_request("/static/64k.bin"sv, "Range: bytes=0-1023\r\n"sv),
		1024);
	add_case("splice_64k_hot"sv, "splice"sv, splice_srv.port, get_request("/static/64k.bin"sv), 64U * 1024U);
	add_case("splice_1m_hot"sv, "splice"sv, splice_srv.port, get_request("/static/1m.bin"sv), 1024U * 1024U);
	add_case("splice_128m_hot"sv, "splice"sv, splice_srv.port, get_request("/static/128m.bin"sv), 128U * 1024U * 1024U);
	add_case("cache_1k_hit"sv, "small_file_cache"sv, cache_srv.port, get_request("/static/1k.txt"sv), 1024);
	add_case("cache_64k_hit"sv, "small_file_cache"sv, cache_srv.port, get_request("/static/64k.bin"sv), 64U * 1024U);
	add_case("cache_churn_64k"sv, "cache_churn"sv, cache_srv.port, {}, 64U * 1024U, kChurnFiles);

	// Warm page cache and static cache explicitly. This benchmark has a cache-churn
	// row, not a privileged cold-cache row.
	for (auto const &c: cases) {
		BenchClient warm{c.port};
		if (c.churn_files == 0) {
			warm.send_all(c.request);
			(void)warm.recv_response(scratch);
		}
	}

	bool first = true;
	for (auto const &c: cases) {
		auto row = run_case(c, scratch, variant_warmup(c.name, args.warmup));
		print_row(row, args.json_out, first);
		first = false;
	}

	mmap_srv.stop();
	splice_srv.stop();
	cache_srv.stop();
	return 0;
}
