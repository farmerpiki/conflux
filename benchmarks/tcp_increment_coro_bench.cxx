// Benchmark: TCP round-trip loop (send N, expect N+1). Server uses blocking
// syscalls + std::from_chars/std::to_chars. Client uses async io_uring I/O
// via FileReader in two styles: block_on per op (callback) vs a single
// Task<void> that co_awaits each op (coroutine).
//
// TLS variant not included here — async TLS needs memory BIOs shuttled
// through the ring (the HTTP server does this internally); doing it
// cleanly for a bench is substantial.
#include <arpa/inet.h>
#include <charconv>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.work;
import conflux.file_io;

using namespace std;

namespace {

constexpr uint64_t pack_ud(
	uint32_t slot,
	uint32_t gen) noexcept {
	return (static_cast<uint64_t>(gen) << 32U) | slot;
}

struct Config {
	size_t iterations = 100'000;
	size_t warmup = 5'000;
	bool csv = false;
};

template<class T, size_t N>
void consume_prefix(
	array<T, N> &buf,
	size_t &held,
	size_t drop) {
	if (drop >= held) {
		held = 0;
		return;
	}
	size_t const remain = held - drop;
	for (size_t i = 0; i < remain; ++i) {
		buf[i] = buf[i + drop];
	}
	held = remain;
}

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (size_t i = 1; i < args.size(); ++i) {
		string_view a = args[i];
		if (a == "--iterations" && i + 1 < args.size()) {
			cfg.iterations = stoull(args[++i]);
		} else if (a == "--warmup" && i + 1 < args.size()) {
			cfg.warmup = stoull(args[++i]);
		} else if (a == "--csv") {
			cfg.csv = true;
		} else if (a == "--help" || a == "-h") {
			println("Usage: conflux_tcp_increment_coro_bench [--iterations N] [--warmup N] [--csv]");
			exit(0);
		}
	}
	return cfg;
}

// Each frame is variable-length ASCII digits terminated by '\n'.
// Server reads until '\n', parses, increments, writes "N+1\n".
void run_server(
	int listen_fd,
	atomic_flag &stop) {
	int cfd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
	if (cfd < 0) {
		return;
	}
	int one = 1;
	::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	array<char, 64> buf{};
	size_t held = 0;
	while (!stop.test(memory_order_acquire)) {
		ssize_t got = ::read(cfd, buf.data() + held, buf.size() - held);
		if (got <= 0) {
			break;
		}
		held += static_cast<size_t>(got);
		size_t scan = 0;
		while (scan < held) {
			auto it =
				find(buf.begin() + static_cast<ptrdiff_t>(scan), buf.begin() + static_cast<ptrdiff_t>(held), '\n');
			if (it == buf.begin() + static_cast<ptrdiff_t>(held)) {
				break;
			}
			size_t const msg_end = static_cast<size_t>(it - buf.begin());
			uint64_t n = 0;
			auto const parsed = from_chars(buf.data() + scan, buf.data() + msg_end, n);
			if (parsed.ec != errc{}) {
				::close(cfd);
				return;
			}
			++n;
			array<char, 24> out{};
			auto const conv = to_chars(out.data(), out.data() + out.size() - 1, n);
			if (conv.ec != errc{}) {
				::close(cfd);
				return;
			}
			*conv.ptr = '\n';
			size_t const out_len = static_cast<size_t>(conv.ptr - out.data()) + 1;
			size_t sent = 0;
			while (sent < out_len) {
				ssize_t const w = ::write(cfd, out.data() + sent, out_len - sent);
				if (w <= 0) {
					::close(cfd);
					return;
				}
				sent += static_cast<size_t>(w);
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

int start_listener(
	uint16_t &port_out) {
	int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw runtime_error{"socket"};
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw runtime_error{"bind"};
	}
	socklen_t slen = sizeof(addr);
	if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &slen) < 0) {
		::close(fd);
		throw runtime_error{"getsockname"};
	}
	port_out = ::ntohs(addr.sin_port);
	if (::listen(fd, 16) < 0) {
		::close(fd);
		throw runtime_error{"listen"};
	}
	return fd;
}

int connect_to(
	uint16_t port) {
	int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw runtime_error{"socket"};
	}
	int one = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = ::htons(port);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw runtime_error{"connect"};
	}
	return fd;
}

size_t encode_line(
	span<char> out,
	uint64_t n) {
	auto const r = to_chars(out.data(), out.data() + out.size() - 1, n);
	if (r.ec != errc{}) {
		throw runtime_error{"to_chars"};
	}
	*r.ptr = '\n';
	return static_cast<size_t>(r.ptr - out.data()) + 1;
}

uint64_t decode_line(
	string_view line) {
	uint64_t n = 0;
	auto const r = from_chars(line.data(), line.data() + line.size(), n);
	if (r.ec != errc{}) {
		throw runtime_error{"from_chars"};
	}
	return n;
}

struct AsyncLineReader {
	FileReader &files;
	FileHandle const &handle;
	array<byte, 128> buf{};
	size_t held = 0;

	Flow<string_view> read_line() { return read_line_impl(); }

private:
	Flow<string_view> read_line_impl() {
		FlowSource<string_view> src;
		auto flow = src.flow();
		step(src);
		return flow;
	}

	void step(
		FlowSource<string_view> dst) {
		auto it = find(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(held), static_cast<byte>('\n'));
		if (it != buf.begin() + static_cast<ptrdiff_t>(held)) {
			auto const end = static_cast<size_t>(it - buf.begin());
			dst.resolve(string_view{reinterpret_cast<char const *>(buf.data()), end});
			return;
		}
		auto read_flow = files.read_into(handle, 0, span{buf.data() + held, buf.size() - held});
		auto chained = move(read_flow)
					 | then([this, dst](size_t got) mutable {
						   if (got == 0) {
							   dst.reject(make_exception_ptr(runtime_error{"eof"}));
							   return;
						   }
						   held += got;
						   step(dst);
					   })
					 | on_error([dst](exception_ptr e) { dst.reject(e); });
		(void)chained;
	}

public:
	void consume_line(
		size_t line_len) {
		size_t const drop = line_len + 1; // includes '\n'
		consume_prefix(buf, held, drop);
	}
};

uint64_t run_callback(
	FileReader &files,
	FileHandle const &sock,
	size_t iters,
	uint64_t start) {
	AsyncLineReader reader{.files = files, .handle = sock};
	array<char, 24> out{};
	uint64_t n = start;
	auto const t0 = chrono::steady_clock::now();
	for (size_t i = 0; i < iters; ++i) {
		size_t const len = encode_line(out, n);
		block_on(files, files.write_into(sock, 0, as_bytes(span{out.data(), len})));
		auto line = block_on(files, reader.read_line());
		uint64_t const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw runtime_error{format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	auto const t1 = chrono::steady_clock::now();
	return static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

Task<uint64_t> coro_loop(
	FileReader &files,
	FileHandle const &sock,
	size_t iters,
	uint64_t start) {
	AsyncLineReader reader{.files = files, .handle = sock};
	array<char, 24> out{};
	uint64_t n = start;
	for (size_t i = 0; i < iters; ++i) {
		size_t const len = encode_line(out, n);
		co_await files.write_into(sock, 0, as_bytes(span{out.data(), len}));
		auto line = co_await reader.read_line();
		uint64_t const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw runtime_error{format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	co_return n;
}

uint64_t run_coroutine(
	FileReader &files,
	FileHandle const &sock,
	size_t iters,
	uint64_t start) {
	auto const t0 = chrono::steady_clock::now();
	(void)block_on(files, coro_loop(files, sock, iters, start));
	auto const t1 = chrono::steady_clock::now();
	return static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

} // namespace

int main(
	int argc,
	char **argv) {
	auto cfg = parse_args(span{argv, static_cast<size_t>(argc)});

	for (int which = 0; which < 2; ++which) {
		uint16_t port = 0;
		int const lfd = start_listener(port);
		atomic_flag server_stop{};
		thread server{[lfd, &server_stop] { run_server(lfd, server_stop); }};

		int const csock = connect_to(port);
		::close(lfd);

		::io_uring ring{};
		if (::io_uring_queue_init(64, &ring, 0) < 0) {
			::close(csock);
			server_stop.test_and_set(memory_order_release);
			server.join();
			println(cerr, "io_uring_queue_init failed");
			return 1;
		}
		CompletionTable ct;
		FileReader files{&ring, &ct, pack_ud};
		FileHandle sock = FileHandle::from_fd(csock);

		try {
			(void)run_callback(files, sock, cfg.warmup, 0);
			uint64_t const ns = (which == 0) ? run_callback(files, sock, cfg.iterations, cfg.warmup) :
											   run_coroutine(files, sock, cfg.iterations, cfg.warmup);
			double const per = static_cast<double>(ns) / static_cast<double>(cfg.iterations);
			string_view const label = (which == 0) ? "callback" : "coroutine";
			if (cfg.csv) {
				if (which == 0) {
					println("style,iterations,total_ns,ns_per_iter");
				}
				println("{},{},{},{:.1f}", label, cfg.iterations, ns, per);
			} else {
				if (which == 0) {
					println("iterations: {}, warmup: {}", cfg.iterations, cfg.warmup);
				}
				println("  {:<10} {:>8.1f} ns/iter ({} ns total)", label, per, ns);
			}
		} catch (exception const &e) { println(cerr, "error: {}", e.what()); }

		::io_uring_queue_exit(&ring);
		server_stop.test_and_set(memory_order_release);
		::shutdown(sock.raw_fd(), SHUT_RDWR);
		(void)sock.release_fd();
		::close(csock);
		server.join();
	}
}
