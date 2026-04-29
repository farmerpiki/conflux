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
import conflux.types;
import conflux.work;
import conflux.file_io;

namespace {

constexpr u64 pack_ud(
	u32 slot,
	u32 gen) noexcept {
	return (static_cast<u64>(gen) << 32U) | slot;
}

struct Config {
	SZ iterations = 100'000;
	SZ warmup = 5'000;
	bool csv = false;
};

template<class T, SZ N>
void consume_prefix(
	A<T, N> &buf,
	SZ &held,
	SZ drop) {
	if (drop >= held) {
		held = 0;
		return;
	}
	SZ const remain = held - drop;
	for (SZ i = 0; i < remain; ++i) {
		buf[i] = buf[i + drop];
	}
	held = remain;
}

namespace {

u64 parse_u64(
	char const *s) noexcept {
	SV sv{s};
	u64 v{};
	from_chars(sv.data(), sv.data() + sv.size(), v);
	return v;
}

} // namespace

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (SZ i = 1; i < args.size(); ++i) {
		SV a = args[i];
		if (a == "--iterations" && i + 1 < args.size()) {
			cfg.iterations = parse_u64(args[++i]);
		} else if (a == "--warmup" && i + 1 < args.size()) {
			cfg.warmup = parse_u64(args[++i]);
		} else if (a == "--csv") {
			cfg.csv = true;
		} else if (a == "--help" || a == "-h") {
			println("Usage: conflux_tcp_increment_coro_bench [--iterations N] [--warmup N] [--csv]");
			std::exit(0);
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

	A<char, 64> buf{};
	SZ held = 0;
	while (!stop.test(memory_order_acquire)) {
		ssize_t got = ::read(cfd, buf.data() + held, buf.size() - held);
		if (got <= 0) {
			break;
		}
		held += static_cast<SZ>(got);
		SZ scan = 0;
		while (scan < held) {
			auto view = span{buf}.subspan(scan, held - scan);
			auto it = ranges::find(view, '\n');
			if (it == view.end()) {
				break;
			}
			SZ const msg_end = scan + static_cast<SZ>(it - view.begin());
			u64 n = 0;
			auto const parsed = from_chars(buf.data() + scan, buf.data() + msg_end, n);
			if (parsed.ec != errc{}) {
				::close(cfd);
				return;
			}
			++n;
			A<char, 24> out{};
			auto const conv = to_chars(out.data(), out.data() + out.size() - 1, n);
			if (conv.ec != errc{}) {
				::close(cfd);
				return;
			}
			*conv.ptr = '\n';
			SZ const out_len = static_cast<SZ>(conv.ptr - out.data()) + 1;
			SZ sent = 0;
			while (sent < out_len) {
				ssize_t const w = ::write(cfd, out.data() + sent, out_len - sent);
				if (w <= 0) {
					::close(cfd);
					return;
				}
				sent += static_cast<SZ>(w);
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
	u16 &port_out) {
	int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw RE{"socket"};
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw RE{"bind"};
	}
	socklen_t slen = sizeof(addr);
	if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &slen) < 0) {
		::close(fd);
		throw RE{"getsockname"};
	}
	port_out = ::ntohs(addr.sin_port);
	if (::listen(fd, 16) < 0) {
		::close(fd);
		throw RE{"listen"};
	}
	return fd;
}

int connect_to(
	u16 port) {
	int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw RE{"socket"};
	}
	int one = 1;
	::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = ::htons(port);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw RE{"connect"};
	}
	return fd;
}

SZ encode_line(
	span<char> out,
	u64 n) {
	auto const r = to_chars(out.data(), out.data() + out.size() - 1, n);
	if (r.ec != errc{}) {
		throw RE{"to_chars"};
	}
	*r.ptr = '\n';
	return static_cast<SZ>(r.ptr - out.data()) + 1;
}

u64 decode_line(
	SV line) {
	u64 n = 0;
	auto const r = from_chars(line.data(), line.data() + line.size(), n);
	if (r.ec != errc{}) {
		throw RE{"from_chars"};
	}
	return n;
}

struct AsyncLineReader {
	FileReader &files;
	FileHandle const &handle;
	A<byte, 128> buf{};
	SZ held = 0;

	Flow<SV> read_line() { return read_line_impl(); }

private:
	Flow<SV> read_line_impl() {
		FlowSource<SV> src;
		auto flow = src.flow();
		step(src);
		return flow;
	}

	void step(
		FlowSource<SV> dst) {
		auto view = span{buf}.first(held);
		auto it = ranges::find(view, static_cast<byte>('\n'));
		if (it != view.end()) {
			auto const end = static_cast<SZ>(it - view.begin());
			dst.resolve(SV{reinterpret_cast<char const *>(buf.data()), end});
			return;
		}
		auto read_flow = files.read_into(handle, 0, span{buf.data() + held, buf.size() - held});
		auto chained = move(read_flow)
					 | then([this, dst](SZ got) mutable {
						   if (got == 0) {
							   dst.reject(make_exception_ptr(RE{"eof"}));
							   return;
						   }
						   held += got;
						   step(dst);
					   })
					 | on_error([dst](EP e) { dst.reject(e); });
		(void)chained;
	}

public:
	void consume_line(
		SZ line_len) {
		SZ const drop = line_len + 1; // includes '\n'
		consume_prefix(buf, held, drop);
	}
};

u64 run_callback(
	FileReader &files,
	FileHandle const &sock,
	SZ iters,
	u64 start) {
	AsyncLineReader reader{.files = files, .handle = sock};
	A<char, 24> out{};
	u64 n = start;
	auto const t0 = chrono::steady_clock::now();
	for (SZ i = 0; i < iters; ++i) {
		SZ const len = encode_line(out, n);
		block_on(files, files.write_into(sock, 0, as_bytes(span{out.data(), len})));
		auto line = block_on(files, reader.read_line());
		u64 const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw RE{format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

Task<u64> coro_loop(
	FileReader &files,
	FileHandle const &sock,
	SZ iters,
	u64 start) {
	AsyncLineReader reader{.files = files, .handle = sock};
	A<char, 24> out{};
	u64 n = start;
	for (SZ i = 0; i < iters; ++i) {
		SZ const len = encode_line(out, n);
		co_await files.write_into(sock, 0, as_bytes(span{out.data(), len}));
		auto line = co_await reader.read_line();
		u64 const got = decode_line(line);
		reader.consume_line(line.size());
		if (got != n + 1) {
			throw RE{format("expected {} got {}", n + 1, got)};
		}
		n = got;
	}
	co_return n;
}

u64 run_coroutine(
	FileReader &files,
	FileHandle const &sock,
	SZ iters,
	u64 start) {
	auto const t0 = chrono::steady_clock::now();
	(void)block_on(files, coro_loop(files, sock, iters, start));
	auto const t1 = chrono::steady_clock::now();
	return static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
}

} // namespace

int main(
	int argc,
	char **argv) {
	auto cfg = parse_args(span{argv, static_cast<SZ>(argc)});

	for (int which = 0; which < 2; ++which) {
		u16 port = 0;
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
			u64 const ns = (which == 0) ? run_callback(files, sock, cfg.iterations, cfg.warmup) :
										  run_coroutine(files, sock, cfg.iterations, cfg.warmup);
			double const per = static_cast<double>(ns) / static_cast<double>(cfg.iterations);
			SV const label = (which == 0) ? "callback" : "coroutine";
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
