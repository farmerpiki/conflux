// Scaling benchmark: N coroutines in parallel on a single io_uring, each
// driving its own TCP connection through the increment loop. Shows how well
// the runtime overlaps independent async work.
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
import conflux.json;

using namespace std;

namespace {

constexpr uint64_t pack_ud(
	uint32_t slot,
	uint32_t gen) noexcept {
	return (static_cast<uint64_t>(gen) << 32U) | slot;
}

struct Config {
	vector<size_t> parallelism{1, 2, 4, 8, 16, 32};
	size_t iterations = 20'000; // per connection
	size_t warmup = 500;
	bool csv = false;
	string config_path;
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

struct RingConfig {
	string label = "default";
	uint32_t flags = 0;
	unsigned entries = 256;
};

RingConfig load_ring_config(
	string const &path) {
	RingConfig rc;
	if (path.empty()) {
		return rc;
	}
	ifstream const f{path};
	if (!f) {
		throw runtime_error{format("cannot open config {}", path)};
	}
	stringstream ss;
	ss << f.rdbuf();
	auto parsed = conflux::json::parse(ss.str());
	if (!parsed) {
		throw runtime_error{format("json parse failed: {}", path)};
	}
	auto const &root = parsed->root();
	rc.label = filesystem::path{path}.stem().string();
	if (auto obj = root.as_object(); obj) {
		if (auto entries_node = obj->find_member("ring_entries")) {
			if (auto num = entries_node->as_number(); num) {
				if (auto v = num->to_i64(); v) {
					rc.entries = static_cast<unsigned>(*v);
				}
			}
		}
		auto maybe_flags = obj->find_member("io_uring_flags");
		auto set = [&](string_view key, uint32_t bit) {
			if (!maybe_flags) {
				return;
			}
			if (auto fobj = maybe_flags->as_object(); fobj) {
				if (auto flag_node = fobj->find_member(key)) {
					if (auto b = flag_node->as_bool(); b && *b) {
						rc.flags |= bit;
					}
				}
			}
		};
		set("single_issuer", IORING_SETUP_SINGLE_ISSUER);
		set("defer_taskrun", IORING_SETUP_DEFER_TASKRUN);
		set("sqpoll", IORING_SETUP_SQPOLL);
		set("coop_taskrun", IORING_SETUP_COOP_TASKRUN);
		set("taskrun_flag", IORING_SETUP_TASKRUN_FLAG);
		set("submit_all", IORING_SETUP_SUBMIT_ALL);
	}
	return rc;
}

string flags_str(
	uint32_t f) {
	string s;
	auto app = [&](char const *n) {
		if (!s.empty()) {
			s += '|';
		}
		s += n;
	};
	if ((f & IORING_SETUP_SINGLE_ISSUER) != 0U) {
		app("single_issuer");
	}
	if ((f & IORING_SETUP_DEFER_TASKRUN) != 0U) {
		app("defer_taskrun");
	}
	if ((f & IORING_SETUP_SQPOLL) != 0U) {
		app("sqpoll");
	}
	if ((f & IORING_SETUP_COOP_TASKRUN) != 0U) {
		app("coop_taskrun");
	}
	if ((f & IORING_SETUP_TASKRUN_FLAG) != 0U) {
		app("taskrun_flag");
	}
	if ((f & IORING_SETUP_SUBMIT_ALL) != 0U) {
		app("submit_all");
	}
	if (s.empty()) {
		s = "none";
	}
	return s;
}

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (size_t i = 1; i < args.size(); ++i) {
		string_view const a = args[i];
		if (a == "--iterations" && i + 1 < args.size()) {
			cfg.iterations = stoull(args[++i]);
		} else if (a == "--warmup" && i + 1 < args.size()) {
			cfg.warmup = stoull(args[++i]);
		} else if (a == "--parallel" && i + 1 < args.size()) {
			cfg.parallelism.clear();
			string_view const list = args[++i];
			size_t pos = 0;
			while (pos < list.size()) {
				size_t const comma = list.find(',', pos);
				size_t const end = (comma == string_view::npos) ? list.size() : comma;
				size_t v = 0;
				auto const r = from_chars(list.data() + pos, list.data() + end, v);
				if (r.ec != errc{} || v == 0) {
					throw runtime_error{"bad --parallel list"};
				}
				cfg.parallelism.push_back(v);
				pos = end + 1;
			}
		} else if (a == "--csv") {
			cfg.csv = true;
		} else if (a == "--config" && i + 1 < args.size()) {
			cfg.config_path = args[++i];
		} else if (a == "--help" || a == "-h") {
			println(
				"Usage: conflux_tcp_parallel_coro_bench [--iterations N] [--warmup N] "
				"[--parallel 1,2,4,8,16] [--config path.json] [--csv]");
			exit(0);
		}
	}
	return cfg;
}

// Shared join primitive: N workers, last one resolves the flow.
struct Barrier {
	atomic<size_t> remaining;
	FlowSource<void> src;

	explicit Barrier(
		size_t n)
		: remaining{n} {}
};

struct BarrierTicket {
	Barrier *barrier{};

	~BarrierTicket() {
		if (barrier != nullptr && barrier->remaining.fetch_sub(1, memory_order_acq_rel) == 1) {
			barrier->src.resolve();
		}
	}
};

// Async server coroutine: runs on the same io_uring as the client. Reads
// lines, increments, writes back — no blocking syscalls, no cross-thread
// wakeups.
Task<void> async_server(
	FileReader &files,
	FileHandle sock,
	Barrier *done) {
	BarrierTicket const ticket{done};
	array<byte, 64> buf{};
	size_t held = 0;
	array<char, 24> out{};
	try {
		for (;;) {
			size_t scan = 0;
			while (scan < held) {
				auto it = find(
					buf.begin() + static_cast<ptrdiff_t>(scan),
					buf.begin() + static_cast<ptrdiff_t>(held),
					static_cast<byte>('\n'));
				if (it == buf.begin() + static_cast<ptrdiff_t>(held)) {
					break;
				}
				size_t const msg_end = static_cast<size_t>(it - buf.begin());
				uint64_t n = 0;
				auto const parsed = from_chars(
					reinterpret_cast<char const *>(buf.data()) + scan,
					reinterpret_cast<char const *>(buf.data()) + msg_end,
					n);
				if (parsed.ec != errc{}) {
					co_return;
				}
				++n;
				auto const conv = to_chars(out.data(), out.data() + out.size() - 1, n);
				*conv.ptr = '\n';
				size_t const out_len = static_cast<size_t>(conv.ptr - out.data()) + 1;
				size_t sent = 0;
				while (sent < out_len) {
					auto const w =
						co_await files.write_into(sock, 0, as_bytes(span{out.data() + sent, out_len - sent}));
					if (w == 0) {
						co_return;
					}
					sent += w;
				}
				scan = msg_end + 1;
			}
			if (scan > 0) {
				consume_prefix(buf, held, scan);
			}
			auto got = co_await files.read_into(sock, 0, span{buf.data() + held, buf.size() - held});
			if (got == 0) {
				co_return;
			}
			held += got;
		}
	} catch (...) { co_return; }
}

int start_listener(
	uint16_t &port_out) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
	socklen_t slen = sizeof(addr);
	::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &slen);
	port_out = ::ntohs(addr.sin_port);
	::listen(fd, 128);
	return fd;
}

int connect_to(
	uint16_t port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
	*r.ptr = '\n';
	return static_cast<size_t>(r.ptr - out.data()) + 1;
}

uint64_t decode_line(
	string_view line) {
	uint64_t n = 0;
	from_chars(line.data(), line.data() + line.size(), n);
	return n;
}

struct AsyncLineReader {
	FileReader &files;
	FileHandle const &handle;
	array<byte, 128> buf{};
	size_t held = 0;

	Task<string_view> read_line() {
		for (;;) {
			auto it = find(buf.begin(), buf.begin() + static_cast<ptrdiff_t>(held), static_cast<byte>('\n'));
			if (it != buf.begin() + static_cast<ptrdiff_t>(held)) {
				auto const end = static_cast<size_t>(it - buf.begin());
				co_return string_view{reinterpret_cast<char const *>(buf.data()), end};
			}
			auto got = co_await files.read_into(handle, 0, span{buf.data() + held, buf.size() - held});
			if (got == 0) {
				throw runtime_error{"eof"};
			}
			held += got;
		}
	}

	void consume_line(
		size_t line_len) {
		size_t const drop = line_len + 1;
		consume_prefix(buf, held, drop);
	}
};

Task<void> worker(
	FileReader &files,
	FileHandle sock,
	size_t iters,
	Barrier *b) {
	BarrierTicket const ticket{b};
	AsyncLineReader reader{.files = files, .handle = sock};
	array<char, 24> out{};
	uint64_t n = 0;
	try {
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
	} catch (...) {}
	co_return;
}

// Wraps worker(...) so co_spawn discards the inner Task<void> while the
// barrier signal above fires once per coroutine.
Task<void> spawn_arm(
	Task<void> inner) {
	co_await std::move(inner);
	co_return;
}

struct RunResult {
	uint64_t ns;
	size_t total_iters;
};

RunResult run_parallel(
	FileReader &files,
	size_t parallelism,
	size_t iters_per_conn,
	int listen_fd) {
	vector<FileHandle> client_socks;
	vector<FileHandle> server_socks;
	client_socks.reserve(parallelism);
	server_socks.reserve(parallelism);

	uint16_t port = 0;
	sockaddr_in la{};
	socklen_t slen = sizeof(la);
	::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&la), &slen);
	port = ::ntohs(la.sin_port);

	for (size_t i = 0; i < parallelism; ++i) {
		int const cfd = connect_to(port);
		int const afd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
		if (afd < 0) {
			::close(cfd);
			throw runtime_error{"accept"};
		}
		int one = 1;
		::setsockopt(afd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		client_socks.push_back(FileHandle::from_fd(cfd));
		server_socks.push_back(FileHandle::from_fd(afd));
	}

	Barrier b{parallelism * 2};
	auto done = b.src.flow();

	auto const t0 = chrono::steady_clock::now();
	for (size_t i = 0; i < parallelism; ++i) {
		co_spawn(async_server(files, std::move(server_socks[i]), &b));
		co_spawn(spawn_arm(worker(files, std::move(client_socks[i]), iters_per_conn, &b)));
	}
	block_on(files, std::move(done));
	auto const t1 = chrono::steady_clock::now();

	return {
		.ns = static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count()),
		.total_iters = parallelism * iters_per_conn};
}

} // namespace

int main(
	int argc,
	char **argv) try {
	auto cfg = parse_args(span{argv, static_cast<size_t>(argc)});

	uint16_t port = 0;
	int const lfd = start_listener(port);
	(void)port;

	auto const rc = load_ring_config(cfg.config_path);
	::io_uring ring{};
	io_uring_params params{};
	params.flags = rc.flags;
	if (::io_uring_queue_init_params(rc.entries, &ring, &params) < 0) {
		::close(lfd);
		println(cerr, "io_uring_queue_init_params failed (flags={})", flags_str(rc.flags));
		return 1;
	}
	CompletionTable ct;
	FileReader files{&ring, &ct, pack_ud};

	if (cfg.csv) {
		println(
			"config,flags,ring_entries,parallel,iters_per_conn,total_iters,total_ns,ns_per_iter,throughput_iter_per_s");
	} else {
		println(
			"config: {}, flags: {}, ring_entries: {}, iterations/conn: {}, warmup: {}",
			rc.label,
			flags_str(rc.flags),
			rc.entries,
			cfg.iterations,
			cfg.warmup);
	}

	// Warmup with P=1.
	(void)run_parallel(files, 1, cfg.warmup, lfd);

	for (size_t p: cfg.parallelism) {
		auto const r = run_parallel(files, p, cfg.iterations, lfd);
		double const per = static_cast<double>(r.ns) / static_cast<double>(r.total_iters);
		double const tput = static_cast<double>(r.total_iters) / (static_cast<double>(r.ns) / 1e9);
		if (cfg.csv) {
			println(
				"{},{},{},{},{},{},{},{:.1f},{:.0f}",
				rc.label,
				flags_str(rc.flags),
				rc.entries,
				p,
				cfg.iterations,
				r.total_iters,
				r.ns,
				per,
				tput);
		} else {
			println(
				"  P={:<3}  {:>8.1f} ns/iter  {:>10.0f} iter/s  ({:>6.1f} ms total, {} iters)",
				p,
				per,
				tput,
				static_cast<double>(r.ns) / 1e6,
				r.total_iters);
		}
	}

	::shutdown(lfd, SHUT_RDWR);
	::close(lfd);

	::io_uring_queue_exit(&ring);
	return 0;
} catch (exception const &e) {
	println(cerr, "fatal: {}", e.what());
	return 1;
}
