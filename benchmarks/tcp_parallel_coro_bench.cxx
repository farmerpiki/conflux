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
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.json;

namespace {

constexpr u64 pack_ud(
	u32 slot,
	u32 gen) noexcept {
	return (static_cast<u64>(gen) << 32U) | slot;
}

struct Config {
	V<SZ> parallelism{1, 2, 4, 8, 16, 32};
	SZ iterations = 20'000; // per connection
	SZ warmup = 500;
	bool json_out = false;
	S config_path;
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

struct RingConfig {
	S label = "default";
	u32 flags = 0;
	unsigned entries = 256;
};

RingConfig load_ring_config(
	S const &path) {
	RingConfig rc;
	if (path.empty()) {
		return rc;
	}
	std::ifstream const f{path};
	if (!f) {
		throw RE{format("cannot open config {}", path)};
	}
	std::stringstream ss;
	ss << f.rdbuf();
	auto parsed = conflux::json::parse(ss.str());
	if (!parsed) {
		throw RE{format("json parse failed: {}", path)};
	}
	auto const &root = parsed->root();
	rc.label = fs::path{path}.stem().string();
	if (auto obj = root.as_object(); obj) {
		if (auto entries_node = obj->find_member("ring_entries")) {
			if (auto num = entries_node->as_number(); num) {
				if (auto v = num->to_i64(); v) {
					rc.entries = static_cast<unsigned>(*v);
				}
			}
		}
		auto maybe_flags = obj->find_member("io_uring_flags");
		auto set = [&](SV key, u32 bit) {
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

S flags_str(
	u32 f) {
	S s;
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

namespace {

u64 parse_u64(
	char const *s) noexcept {
	SV const sv{s};
	u64 v{};
	from_chars(sv.data(), sv.data() + sv.size(), v);
	return v;
}

} // namespace

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (SZ i = 1; i < args.size(); ++i) {
		SV const a = args[i];
		if (a == "--iterations" && i + 1 < args.size()) {
			cfg.iterations = parse_u64(args[++i]);
		} else if (a == "--warmup" && i + 1 < args.size()) {
			cfg.warmup = parse_u64(args[++i]);
		} else if (a == "--parallel" && i + 1 < args.size()) {
			cfg.parallelism.clear();
			SV const list = args[++i];
			SZ pos = 0;
			while (pos < list.size()) {
				SZ const comma = list.find(',', pos);
				SZ const end = (comma == SV::npos) ? list.size() : comma;
				SZ v = 0;
				auto const r = from_chars(list.data() + pos, list.data() + end, v);
				if (r.ec != errc{} || v == 0) {
					throw RE{"bad --parallel list"};
				}
				cfg.parallelism.push_back(v);
				pos = end + 1;
			}
		} else if (a == "--json") {
			cfg.json_out = true;
		} else if (a == "--config" && i + 1 < args.size()) {
			cfg.config_path = args[++i];
		} else if (a == "--help" || a == "-h") {
			println(
				"Usage: conflux_tcp_parallel_coro_bench [--iterations N] [--warmup N] "
				"[--parallel 1,2,4,8,16] [--config path.json] [--json]");
			std::exit(0);
		}
	}
	return cfg;
}

// Shared join primitive: N workers, last one resolves the task.
struct Barrier {
	Atom<SZ> remaining;
	std::shared_ptr<conflux::work::root::TaskSource<void>> src;
	conflux::work::root::Task<void> done;

	explicit Barrier(
		SZ n)
		: remaining{n} {
		auto [task, source] = conflux::work::root::make_task_source<void>(
			conflux::work::root::SubmitOptions{.enable_cancellation = false});
		done = std::move(task);
		src = std::make_shared<conflux::work::root::TaskSource<void>>(std::move(source));
	}
};

struct BarrierTicket {
	Barrier *barrier{};

	~BarrierTicket() {
		if (barrier != nullptr && barrier->remaining.fetch_sub(1, memory_order_acq_rel) == 1) {
			(void)barrier->src->try_set_value(conflux::work::root::Success<void>{});
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
	A<byte, 64> buf{};
	SZ held = 0;
	A<char, 24> out{};
	try {
		for (;;) {
			SZ scan = 0;
			while (scan < held) {
				auto view = span{buf}.subspan(scan, held - scan);
				auto it = ranges::find(view, static_cast<byte>('\n'));
				if (it == view.end()) {
					break;
				}
				SZ const msg_end = scan + static_cast<SZ>(it - view.begin());
				u64 n = 0;
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
				SZ const out_len = static_cast<SZ>(conv.ptr - out.data()) + 1;
				SZ sent = 0;
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
	u16 &port_out) {
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
	u16 port) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
	*r.ptr = '\n';
	return static_cast<SZ>(r.ptr - out.data()) + 1;
}

u64 decode_line(
	SV line) {
	u64 n = 0;
	from_chars(line.data(), line.data() + line.size(), n);
	return n;
}

struct AsyncLineReader {
	FileReader &files;
	FileHandle const &handle;
	A<byte, 128> buf{};
	SZ held = 0;

	Task<SV> read_line() {
		for (;;) {
			auto view = span{buf}.first(held);
			auto it = ranges::find(view, static_cast<byte>('\n'));
			if (it != view.end()) {
				auto const end = static_cast<SZ>(it - view.begin());
				co_return SV{reinterpret_cast<char const *>(buf.data()), end};
			}
			auto got = co_await files.read_into(handle, 0, span{buf.data() + held, buf.size() - held});
			if (got == 0) {
				throw RE{"eof"};
			}
			held += got;
		}
	}

	void consume_line(
		SZ line_len) {
		SZ const drop = line_len + 1;
		consume_prefix(buf, held, drop);
	}
};

Task<void> worker(
	FileReader &files,
	FileHandle sock,
	SZ iters,
	Barrier *b) {
	BarrierTicket const ticket{b};
	AsyncLineReader reader{.files = files, .handle = sock};
	A<char, 24> out{};
	u64 n = 0;
	try {
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
	u64 ns;
	SZ total_iters;
};

RunResult run_parallel(
	FileReader &files,
	SZ parallelism,
	SZ iters_per_conn,
	int listen_fd) {
	V<FileHandle> client_socks;
	V<FileHandle> server_socks;
	client_socks.reserve(parallelism);
	server_socks.reserve(parallelism);

	u16 port = 0;
	sockaddr_in la{};
	socklen_t slen = sizeof(la);
	::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&la), &slen);
	port = ::ntohs(la.sin_port);

	for (SZ i = 0; i < parallelism; ++i) {
		int const cfd = connect_to(port);
		int const afd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
		if (afd < 0) {
			::close(cfd);
			throw RE{"accept"};
		}
		int one = 1;
		::setsockopt(afd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		client_socks.push_back(FileHandle::from_fd(cfd));
		server_socks.push_back(FileHandle::from_fd(afd));
	}

	Barrier b{parallelism * 2};

	auto const t0 = chrono::steady_clock::now();
	for (SZ i = 0; i < parallelism; ++i) {
		co_spawn(async_server(files, std::move(server_socks[i]), &b));
		co_spawn(spawn_arm(worker(files, std::move(client_socks[i]), iters_per_conn, &b)));
	}
	block_on(files, std::move(b.done));
	auto const t1 = chrono::steady_clock::now();

	return {
		.ns = static_cast<u64>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count()),
		.total_iters = parallelism * iters_per_conn};
}

} // namespace

int main(
	int argc,
	char **argv) try {
	if (argc >= 2 && SV{argv[1]} == "--bench-info") {
		std::print("{}\n", R"({"name":"tcp_parallel_coro","parser":"tcp_parallel","configs":[]})");
		return 0;
	}
	auto cfg = parse_args(span{argv, static_cast<SZ>(argc)});

	u16 port = 0;
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

	if (!cfg.json_out) {
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

	for (SZ p: cfg.parallelism) {
		auto const r = run_parallel(files, p, cfg.iterations, lfd);
		double const per = static_cast<double>(r.ns) / static_cast<double>(r.total_iters);
		double const tput = static_cast<double>(r.total_iters) / (static_cast<double>(r.ns) / 1e9);
		if (cfg.json_out) {
			println(
				"{{\"config\":\"{}\",\"variant\":\"P{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},\"flags\":\"{}\",\"ring_entries\":{},\"iters_per_conn\":{},\"throughput_iter_per_s\":{:.0f}}}",
				rc.label,
				p,
				r.total_iters,
				r.ns,
				per,
				flags_str(rc.flags),
				rc.entries,
				cfg.iterations,
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
