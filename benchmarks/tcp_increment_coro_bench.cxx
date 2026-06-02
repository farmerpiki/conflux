// Benchmark: TCP round-trip (send N, expect N+1).
// fr/* variants use conflux::file_io::FileReader; str/* variants use
// conflux::socket_io::SocketTaskRing/conflux::socket_io::TcpStream. Phase 1: all four variants run against the same
// blocking single-connection server.
#include <arpa/inet.h>
#include <charconv>
#include <liburing.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.socket_io;
import conflux.socket_io.coro;
import conflux.socket_io.blocking;
import bench_common;

namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}
struct Config {
	std::size_t iterations = 200;
	std::size_t warmup = 40;
	std::size_t samples = 0;
	std::size_t batch = 0;
	std::size_t clients = 4;
	std::string_view config_name = "default";
	bool json_out = false;
};
template<class T, std::size_t N>
void consume_prefix(
	std::array<T, N> &buf,
	std::size_t &held,
	std::size_t drop) {
	if (drop >= held) {
		held = 0;
		return;
	}
	std::size_t const remain = held - drop;
	for (std::size_t i = 0; i < remain; ++i) {
		buf[i] = buf[i + drop];
	}
	held = remain;
}
namespace {

std::uint64_t parse_u64(
	char const *s) noexcept {
	std::string_view const sv{s};
	std::uint64_t v{};
	std::from_chars(sv.data(), sv.data() + sv.size(), v);
	return v;
}

} // namespace
Config parse_args(
	std::span<char *> args) {
	Config cfg;
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const a = args[i];
		if (a == "--iterations" && i + 1 < args.size()) {
			cfg.iterations = parse_u64(args[++i]);
		} else if (a == "--warmup" && i + 1 < args.size()) {
			cfg.warmup = parse_u64(args[++i]);
		} else if (a == "--samples" && i + 1 < args.size()) {
			cfg.samples = parse_u64(args[++i]);
		} else if (a == "--batch" && i + 1 < args.size()) {
			cfg.batch = parse_u64(args[++i]);
		} else if (a == "--clients" && i + 1 < args.size()) {
			cfg.clients = parse_u64(args[++i]);
		} else if (a == "--config" && i + 1 < args.size()) {
			cfg.config_name = args[++i];
		} else if (a == "--json") {
			cfg.json_out = true;
		} else if (a == "--help" || a == "-h") {
			std::println(
				"Usage: conflux_tcp_increment_coro_bench [--iterations N] [--warmup N] [--clients N] [--json]");
			std::exit(0);
		}
	}
	return cfg;
}
// ── server ────────────────────────────────────────────────────────────────────
void serve_one(
	int cfd,
	std::atomic_flag &stop) {
	int one = 1;
	::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	std::array<char, 64> buf{};
	std::size_t held = 0;
	while (!stop.test(std::memory_order_acquire)) {
		ssize_t const got = ::read(cfd, buf.data() + held, buf.size() - held);
		if (got <= 0) {
			break;
		}
		held += static_cast<std::size_t>(got);
		std::size_t scan = 0;
		while (scan < held) {
			auto view = std::span{buf}.subspan(scan, held - scan);
			auto it = std::ranges::find(view, '\n');
			if (it == view.end()) {
				break;
			}
			std::size_t const msg_end = scan + static_cast<std::size_t>(it - view.begin());
			std::uint64_t n = 0;
			if (std::from_chars(buf.data() + scan, buf.data() + msg_end, n).ec != std::errc{}) {
				::close(cfd);
				return;
			}
			++n;
			std::array<char, 24> out{};
			auto const conv = std::to_chars(out.data(), out.data() + out.size() - 1, n);
			if (conv.ec != std::errc{}) {
				::close(cfd);
				return;
			}
			*conv.ptr = '\n';
			std::size_t const out_len = static_cast<std::size_t>(conv.ptr - out.data()) + 1;
			std::size_t sent = 0;
			while (sent < out_len) {
				ssize_t const w = ::write(cfd, out.data() + sent, out_len - sent);
				if (w <= 0) {
					::close(cfd);
					return;
				}
				sent += static_cast<std::size_t>(w);
			}
			scan = msg_end + 1;
		}
		if (scan > 0) {
			consume_prefix(buf, held, scan);
		}
		if (held == buf.size()) {
			::close(cfd);
			return;
		}
	}
	::close(cfd);
}
void run_server(
	int listen_fd,
	std::atomic_flag &stop) {
	timeval tv{.tv_sec = 0, .tv_usec = 100000};
	::setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (!stop.test(std::memory_order_acquire)) {
		int const cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
		if (cfd < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
			break;
		}
		serve_one(cfd, stop);
	}
	::close(listen_fd);
}
int start_listener(
	std::uint16_t &port_out) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket"};
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"bind"};
	}
	socklen_t slen = sizeof(addr);
	if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &slen) < 0) {
		::close(fd);
		throw std::runtime_error{"getsockname"};
	}
	port_out = ::ntohs(addr.sin_port);
	if (::listen(fd, 16) < 0) {
		::close(fd);
		throw std::runtime_error{"listen"};
	}
	return fd;
}
int connect_to(
	std::uint16_t port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket"};
	}
	int one = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = ::htons(port);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"connect"};
	}
	return fd;
}
sockaddr_storage loopback_addr(
	std::uint16_t port) noexcept {
	sockaddr_storage ss{};
	auto *sin = reinterpret_cast<sockaddr_in *>(&ss);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	sin->sin_port = ::htons(port);
	return ss;
}
std::size_t encode_line(
	std::span<char> out,
	std::uint64_t n) {
	auto const r = std::to_chars(out.data(), out.data() + out.size() - 1, n);
	if (r.ec != std::errc{}) {
		throw std::runtime_error{"to_chars"};
	}
	*r.ptr = '\n';
	return static_cast<std::size_t>(r.ptr - out.data()) + 1;
}
std::uint64_t decode_line(
	std::string_view line) {
	std::uint64_t n = 0;
	if (std::from_chars(line.data(), line.data() + line.size(), n).ec != std::errc{}) {
		throw std::runtime_error{"from_chars"};
	}
	return n;
}
// ── fr/* (conflux::file_io::FileReader) variants ────────────────────────────────────────────────
struct FrLineReader {
	conflux::file_io::FileReader &files;
	conflux::uring::FileHandle const &handle;
	std::array<std::byte, 128> buf{};
	std::size_t held = 0;
	conflux::work::root::Task<std::string_view> read_line() {
		for (;;) {
			auto view = std::span{buf}.first(held);
			auto it = std::ranges::find(view, static_cast<std::byte>('\n'));
			if (it != view.end()) {
				auto const end = static_cast<std::size_t>(it - view.begin());
				co_return std::string_view{reinterpret_cast<char const *>(buf.data()), end};
			}
			auto got = co_await files.read_into(handle, 0, std::span{buf.data() + held, buf.size() - held});
			if (got == 0) {
				throw std::runtime_error{"eof"};
			}
			held += got;
		}
	}
	void consume_line(
		std::size_t line_len) {
		consume_prefix(buf, held, line_len + 1);
	}
};
std::uint64_t run_fr_callback(
	conflux::file_io::FileReader &files,
	conflux::uring::FileHandle const &sock,
	std::size_t iters,
	std::uint64_t start) {
	FrLineReader reader{.files = files, .handle = sock};
	std::array<char, 24> out{};
	std::uint64_t n = start;
	auto const t0 = std::chrono::steady_clock::now();
	for (std::size_t i = 0; i < iters; ++i) {
		std::size_t const len = encode_line(out, n);
		block_on(files, files.write_into(sock, 0, std::as_bytes(std::span{out.data(), len})));
		auto line = block_on(files, reader.read_line());
		std::uint64_t const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw std::runtime_error{std::format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
conflux::work::root::Task<std::uint64_t> fr_coro_loop(
	conflux::file_io::FileReader &files,
	conflux::uring::FileHandle const &sock,
	std::size_t iters,
	std::uint64_t start) {
	FrLineReader reader{.files = files, .handle = sock};
	std::array<char, 24> out{};
	std::uint64_t n = start;
	for (std::size_t i = 0; i < iters; ++i) {
		std::size_t const len = encode_line(out, n);
		co_await files.write_into(sock, 0, std::as_bytes(std::span{out.data(), len}));
		auto line = co_await reader.read_line();
		std::uint64_t const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw std::runtime_error{std::format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	co_return n;
}
std::uint64_t run_fr_coroutine(
	conflux::file_io::FileReader &files,
	conflux::uring::FileHandle const &sock,
	std::size_t iters,
	std::uint64_t start) {
	auto const t0 = std::chrono::steady_clock::now();
	auto _ = block_on(files, fr_coro_loop(files, sock, iters, start));
	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
// ── str/* (conflux::socket_io::SocketTaskRing) variants ───────────────────────────────────────────
struct StrLineReader {
	conflux::socket_io::TcpStream &stream;
	conflux::socket_io::SocketTaskRing &ring;
	std::array<std::uint8_t, 128> buf{};
	std::size_t held = 0;
	std::string_view read_line() {
		for (;;) {
			auto view = std::span{buf}.first(held);
			auto it = std::ranges::find(view, std::uint8_t('\n'));
			if (it != view.end()) {
				std::size_t const end = static_cast<std::size_t>(it - view.begin());
				return std::string_view{reinterpret_cast<char const *>(buf.data()), end};
			}
			std::size_t const got = sync_wait_socket_task(
				ring,
				stream.async_recv_borrowed(std::span<std::uint8_t>{buf.data() + held, buf.size() - held}));
			if (got == 0) {
				throw std::runtime_error{"eof"};
			}
			held += got;
		}
	}
	void consume_line(
		std::size_t line_len) {
		consume_prefix(buf, held, line_len + 1);
	}
};
std::uint64_t run_str_callback(
	conflux::socket_io::SocketTaskRing &ring,
	conflux::socket_io::TcpStream &stream,
	std::size_t iters,
	std::uint64_t start) {
	StrLineReader reader{.stream = stream, .ring = ring};
	std::array<char, 24> out{};
	std::uint64_t n = start;
	auto const t0 = std::chrono::steady_clock::now();
	for (std::size_t i = 0; i < iters; ++i) {
		std::size_t const len = encode_line(out, n);
		sync_wait_socket_task(
			ring,
			stream.async_write_all_borrowed(
				std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const *>(out.data()), len}));
		auto line = reader.read_line();
		std::uint64_t const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw std::runtime_error{std::format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
conflux::work::root::Task<std::uint64_t> str_coro_loop(
	conflux::socket_io::TcpStream &stream,
	std::size_t iters,
	std::uint64_t start) {
	std::array<std::uint8_t, 128> rbuf{};
	std::size_t held = 0;
	std::array<char, 24> out{};
	std::uint64_t n = start;
	for (std::size_t i = 0; i < iters; ++i) {
		std::size_t const len = encode_line(out, n);
		co_await stream.async_write_all_borrowed(
			std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const *>(out.data()), len});
		for (;;) {
			auto view = std::span{rbuf}.first(held);
			auto it = std::ranges::find(view, std::uint8_t('\n'));
			if (it != view.end()) {
				std::size_t const end = static_cast<std::size_t>(it - view.begin());
				std::string_view const line{reinterpret_cast<char const *>(rbuf.data()), end};
				std::uint64_t const got = decode_line(line);
				consume_prefix(rbuf, held, end + 1);
				if (got != n + 1) {
					throw std::runtime_error{std::format("expected {} got {}", n + 1, got)};
				}
				n = got;
				break;
			}
			std::size_t const r =
				co_await stream.async_recv_borrowed(std::span<std::uint8_t>{rbuf.data() + held, rbuf.size() - held});
			if (r == 0) {
				throw std::runtime_error{"eof"};
			}
			held += r;
		}
	}
	co_return n;
}
std::uint64_t run_str_coroutine(
	conflux::socket_io::SocketTaskRing &ring,
	conflux::socket_io::TcpStream &stream,
	std::size_t iters,
	std::uint64_t start) {
	auto const t0 = std::chrono::steady_clock::now();
	auto _ = sync_wait_socket_task(ring, str_coro_loop(stream, iters, start));
	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}
// ── async server ──────────────────────────────────────────────────────────────
conflux::work::root::Task<void> serve_one_async(
	conflux::socket_io::TcpStream stream) {
	std::array<std::uint8_t, 128> rbuf{};
	std::size_t held = 0;
	for (;;) {
		std::size_t const got =
			co_await stream.async_recv_borrowed(std::span<std::uint8_t>{rbuf.data() + held, rbuf.size() - held});
		if (got == 0) {
			co_return;
		}
		held += got;
		std::size_t scan = 0;
		while (scan < held) {
			auto view = std::span<std::uint8_t>{rbuf.data() + scan, held - scan};
			auto it = std::ranges::find(view, std::uint8_t('\n'));
			if (it == view.end()) {
				break;
			}
			std::size_t const msg_end = scan + static_cast<std::size_t>(it - view.begin());
			std::uint64_t n = 0;
			if (std::from_chars(
					reinterpret_cast<char const *>(rbuf.data() + scan),
					reinterpret_cast<char const *>(rbuf.data() + msg_end),
					n)
					.ec
				!= std::errc{}) {
				co_return;
			}
			++n;
			std::array<char, 24> out{};
			auto const conv = std::to_chars(out.data(), out.data() + out.size() - 1, n);
			if (conv.ec != std::errc{}) {
				co_return;
			}
			*conv.ptr = '\n';
			std::size_t const out_len = static_cast<std::size_t>(conv.ptr - out.data()) + 1;
			co_await stream.async_write_all_borrowed(
				std::span<std::uint8_t const>{reinterpret_cast<std::uint8_t const *>(out.data()), out_len});
			scan = msg_end + 1;
		}
		if (scan > 0) {
			consume_prefix(rbuf, held, scan);
		}
		if (held == rbuf.size()) {
			co_return;
		}
	}
}
void run_async_server(
	std::atomic<std::uint16_t> &port_out,
	std::atomic_flag &port_ready,
	std::atomic_flag &stop) {
	conflux::socket_io::TcpListener listener{
		conflux::socket_io::TcpListenerOptions{
											   .port = 0,
											   .bind = conflux::socket_io::TcpBindAddress::loopback_v4,
											   .reuse_addr = true}
    };
	port_out.store(listener.port(), std::memory_order_release);
	port_ready.test_and_set(std::memory_order_release);
	::io_uring raw{};
	if (::io_uring_queue_init(64, &raw, 0) < 0) {
		return;
	}
	conflux::uring::CompletionTable ct;
	conflux::socket_io::SocketTaskRing ring{
		conflux::socket_io::SocketRawRing{&raw},
		ct,
		[](std::uint32_t s, std::uint32_t g) noexcept -> std::uint64_t { return pack_ud(s, g); }};
	conflux::socket_io::async_tcp_accept_multishot(
		listener,
		ring,
		{},
		[](conflux::socket_io::TcpStream s) -> conflux::work::root::Task<void> {
			return serve_one_async(std::move(s));
		})
		.detach();
	auto drain = [&]() noexcept {
		std::array<::io_uring_cqe *, 32> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(&raw, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				std::uint64_t const ud = batch[i]->user_data;
				ct.dispatch(
					static_cast<std::uint32_t>(ud & 0xFFFFFFFFU),
					static_cast<std::uint32_t>(ud >> 32U),
					batch[i]->res,
					conflux::uring::CqeFlags{batch[i]->flags});
			}
			::io_uring_cq_advance(&raw, n);
		}
	};
	while (!stop.test(std::memory_order_acquire)) {
		::io_uring_cqe *cqe{};
		__kernel_timespec ts{.tv_sec = 0, .tv_nsec = 50000000};
		int const rc = ::io_uring_submit_and_wait_timeout(&raw, &cqe, 1, &ts, nullptr);
		if (rc == -EINTR) {
			continue;
		}
		if (rc < 0 && rc != -ETIME) {
			break;
		}
		drain();
	}
	{ auto _ = std::move(listener); }
	for (int i = 0; i < 5; ++i) {
		::io_uring_cqe *cqe{};
		__kernel_timespec ts{.tv_sec = 0, .tv_nsec = 10000000};
		::io_uring_submit_and_wait_timeout(&raw, &cqe, 1, &ts, nullptr);
		drain();
	}
	::io_uring_queue_exit(&raw);
}
// ── str/parallel_4 ────────────────────────────────────────────────────────────
conflux::work::root::Task<void> str_parallel_inner(
	conflux::socket_io::SocketTaskRing &ring,
	std::uint16_t port,
	std::size_t iters,
	std::uint64_t start) {
	auto ss = loopback_addr(port);
	auto streams = co_await conflux::work::join_all(
		async_tcp_connect(ring, AF_INET, ss, sizeof(sockaddr_in)),
		async_tcp_connect(ring, AF_INET, ss, sizeof(sockaddr_in)),
		async_tcp_connect(ring, AF_INET, ss, sizeof(sockaddr_in)),
		async_tcp_connect(ring, AF_INET, ss, sizeof(sockaddr_in)));
	co_await conflux::work::join_all(
		str_coro_loop(std::get<0>(streams), iters, start),
		str_coro_loop(std::get<1>(streams), iters, start),
		str_coro_loop(std::get<2>(streams), iters, start),
		str_coro_loop(std::get<3>(streams), iters, start));
}
std::uint64_t run_str_parallel(
	conflux::socket_io::SocketTaskRing &ring,
	std::uint16_t port,
	std::size_t iters,
	std::uint64_t start) {
	auto const t0 = std::chrono::steady_clock::now();
	sync_wait_socket_task(ring, str_parallel_inner(ring, port, iters, start));
	auto const t1 = std::chrono::steady_clock::now();
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

template<class Fn>
BenchStats measure_batched_loop(
	std::string_view config,
	std::string_view variant,
	std::size_t iterations,
	std::size_t warmup,
	std::size_t samples,
	std::size_t batch,
	Fn &&fn) {
	BenchSamplePlan const plan = bench_sample_plan(iterations, warmup, samples, batch);
	std::uint64_t total_ns = 0;
	for (std::size_t i = 0; i < plan.warmup_samples; ++i) {
		(void)fn(plan.batch);
	}
	for (std::size_t i = 0; i < plan.samples; ++i) {
		total_ns += fn(plan.batch);
	}
	BenchStats stats{
		.config = config,
		.variant = variant,
		.iterations = plan.iterations,
		.total_ns = total_ns,
		.ns_per_iter = static_cast<double>(total_ns) / static_cast<double>(plan.iterations),
	};
	bench_apply_sample_plan(stats, plan);
	return stats;
}

void print_tcp_stats(
	BenchStats const &stats,
	bool json) {
	if (json) {
		bench_print(stats, true, false);
	} else {
		std::println("  {:<18} {:>8.1f} ns/iter ({} ns total)", stats.variant, stats.ns_per_iter, stats.total_ns);
	}
}

} // namespace
int main(
	int argc,
	char **argv) {
	if (argc >= 2 && std::string_view{argv[1]} == "--bench-info") {
		std::print(
			"{}\n",
			R"({"name":"tcp_increment","parser":"standard","configs":[{"name":"default","extra":{},"target_ms":500,"max_iterations":200,"calibration_iterations":2,"args":["--iterations","0","--warmup","0","--config","default"]},{"name":"parallel_4","extra":{"clients":4},"target_ms":500,"max_iterations":200,"calibration_iterations":2,"args":["--iterations","0","--warmup","0","--clients","4","--config","parallel_4"]}]})");
		return 0;
	}
	auto cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	// 0=fr/callback 1=fr/coroutine 2=str/callback 3=str/coroutine
	// 4=str/async_callback 5=str/async_coroutine 6=str/parallel_4
	static constexpr std::array<std::string_view, 7> labels{
		"fr/callback",
		"fr/coroutine",
		"str/callback",
		"str/coroutine",
		"str/async_callback",
		"str/async_coroutine",
		"str/parallel_4"};
	auto const lbl = [&](int w) noexcept -> std::string_view { return labels[static_cast<std::size_t>(w)]; };
	for (int which = 0; which < 7; ++which) {
		bool const async_srv = which >= 4;
		std::uint16_t port = 0;
		std::atomic<std::uint16_t> async_port{0};
		std::atomic_flag port_ready{};
		std::atomic_flag server_stop{};
		int lfd = -1;
		if (!async_srv) {
			lfd = start_listener(port);
		}
		std::thread server;
		if (async_srv) {
			server = std::thread{
				[&async_port, &port_ready, &server_stop] { run_async_server(async_port, port_ready, server_stop); }};
			while (!port_ready.test(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			port = async_port.load(std::memory_order_acquire);
		} else {
			server = std::thread{[lfd, &server_stop] { run_server(lfd, server_stop); }};
		}
		::io_uring raw{};
		if (::io_uring_queue_init(64, &raw, 0) < 0) {
			server_stop.test_and_set(std::memory_order_release);
			server.join();
			std::println(std::cerr, "io_uring_queue_init failed");
			return 1;
		}
		conflux::uring::CompletionTable ct;
		try {
			if (which < 2) {
				conflux::file_io::FileReader files{&raw, &ct, pack_ud};
				int const csock = connect_to(port);
				conflux::uring::FileHandle sock = conflux::uring::FileHandle::from_fd(csock);
				auto const stats = measure_batched_loop(
					cfg.config_name,
					lbl(which),
					cfg.iterations,
					cfg.warmup,
					cfg.samples,
					cfg.batch,
					[&](std::size_t n) {
						return (which == 0) ? run_fr_callback(files, sock, n, cfg.warmup) :
											  run_fr_coroutine(files, sock, n, cfg.warmup);
					});
				if (!cfg.json_out && which == 0) {
					std::println("iterations: {}, warmup: {}", stats.iterations, cfg.warmup);
				}
				print_tcp_stats(stats, cfg.json_out);
				server_stop.test_and_set(std::memory_order_release);
				::shutdown(sock.raw_fd(), SHUT_RDWR);
				(void)sock.release_fd();
				::close(csock);
			} else if (which < 4) {
				conflux::socket_io::SocketTaskRing task_ring{
					conflux::socket_io::SocketRawRing{&raw},
					ct,
					[](std::uint32_t s, std::uint32_t g) noexcept -> std::uint64_t { return pack_ud(s, g); }};
				auto ss = loopback_addr(port);
				conflux::socket_io::TcpStream stream =
					sync_wait_socket_task(task_ring, async_tcp_connect(task_ring, AF_INET, ss, sizeof(sockaddr_in)));
				auto const stats = measure_batched_loop(
					cfg.config_name,
					lbl(which),
					cfg.iterations,
					cfg.warmup,
					cfg.samples,
					cfg.batch,
					[&](std::size_t n) {
						return (which == 2) ? run_str_callback(task_ring, stream, n, cfg.warmup) :
											  run_str_coroutine(task_ring, stream, n, cfg.warmup);
					});
				print_tcp_stats(stats, cfg.json_out);
				server_stop.test_and_set(std::memory_order_release);
				// stream dtor closes fd → unblocks server's ::read → server sees stop flag
			} else if (which < 6) {
				conflux::socket_io::SocketTaskRing task_ring{
					conflux::socket_io::SocketRawRing{&raw},
					ct,
					[](std::uint32_t s, std::uint32_t g) noexcept -> std::uint64_t { return pack_ud(s, g); }};
				auto ss = loopback_addr(port);
				conflux::socket_io::TcpStream stream =
					sync_wait_socket_task(task_ring, async_tcp_connect(task_ring, AF_INET, ss, sizeof(sockaddr_in)));
				auto const stats = measure_batched_loop(
					cfg.config_name,
					lbl(which),
					cfg.iterations,
					cfg.warmup,
					cfg.samples,
					cfg.batch,
					[&](std::size_t n) {
						return (which == 4) ? run_str_callback(task_ring, stream, n, cfg.warmup) :
											  run_str_coroutine(task_ring, stream, n, cfg.warmup);
					});
				print_tcp_stats(stats, cfg.json_out);
				server_stop.test_and_set(std::memory_order_release);
			} else {
				conflux::socket_io::SocketTaskRing task_ring{
					conflux::socket_io::SocketRawRing{&raw},
					ct,
					[](std::uint32_t s, std::uint32_t g) noexcept -> std::uint64_t { return pack_ud(s, g); }};
				auto stats = measure_batched_loop(
					cfg.config_name,
					lbl(which),
					cfg.iterations,
					cfg.warmup,
					cfg.samples,
					cfg.batch,
					[&](std::size_t n) { return run_str_parallel(task_ring, port, n, cfg.warmup); });
				stats.iterations *= cfg.clients;
				stats.ns_per_iter = static_cast<double>(stats.total_ns) / static_cast<double>(stats.iterations);
				stats.timer_overhead_pct = bench_timer_overhead_percent(
					BenchSamplePlan{
						.samples = stats.sample_count,
						.batch = stats.batch,
						.iterations = stats.iterations,
						.timer_sample_ns = stats.timer_sample_ns},
					stats.total_ns);
				if (cfg.json_out) {
					bench_print(stats, true, false);
				} else {
					std::println(
						"  {:<18} {:>8.1f} ns/iter ({} ns total)",
						lbl(which),
						stats.ns_per_iter,
						stats.total_ns);
				}
				server_stop.test_and_set(std::memory_order_release);
			}
		} catch (std::exception const &e) { std::println(std::cerr, "error: {}", e.what()); }
		::io_uring_queue_exit(&raw);
		server.join();
	}
}
