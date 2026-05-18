#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <memory>

import std;
import conflux.types;
import conflux.uring;
import conflux.uring.flow;
import conflux.socket_io;
import bench_common;
import conflux.net.direct_slot_pool;

using namespace std::literals;

#ifdef __clang__
	#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#else
	#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
namespace {
namespace ur = conflux::uring;
namespace uf = conflux::uring::flow;

// ── helpers ────────────────────────────────────────────────────────────────

struct ListenSock {
	int fd = -1;
	std::uint16_t port{};
};
[[nodiscard]] ListenSock make_listen_socket() {
	int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket"};
	}
	int one = 1;
	(void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	(void)::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"bind"};
	}
	if (::listen(fd, 4096) < 0) {
		::close(fd);
		throw std::runtime_error{"listen"};
	}
	socklen_t len = sizeof(addr);
	::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len);
	return {.fd = fd, .port = ntohs(addr.sin_port)};
}
[[nodiscard]] int connect_one(
	std::uint16_t port) {
	int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket"};
	}
	int one = 1;
	(void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	(void)::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"connect"};
	}
	return fd;
}
[[nodiscard]] int accept_one(
	int listen_fd) {
	int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
	if (fd < 0) {
		throw std::runtime_error{"accept"};
	}
	int one = 1;
	(void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	(void)::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
	return fd;
}
void wait_cqe(
	io_uring *ring,
	io_uring_cqe **out) {
	if (io_uring_wait_cqe(ring, out) != 0) {
		throw std::runtime_error{"io_uring_wait_cqe"};
	}
}

void submit_or_throw(
	SocketRawRing const &raw) {
	if (raw.submit() < 0) {
		throw std::runtime_error{"io_uring_submit"};
	}
}
struct RingGuard {
	io_uring ring{};
	explicit RingGuard(
		unsigned entries,
		unsigned flags = 0) {
		io_uring_params p{};
		p.flags = flags;
		if (io_uring_queue_init_params(entries, &ring, &p) < 0) {
			throw std::runtime_error{"io_uring_queue_init"};
		}
	}
	~RingGuard() { io_uring_queue_exit(&ring); }
	RingGuard(RingGuard const &) = delete;
	RingGuard &operator =(RingGuard const &) = delete;
	[[nodiscard]] io_uring *get() noexcept { return &ring; }
	[[nodiscard]] conflux::uring::IoUringCaps caps() const noexcept {
		return conflux::uring::detect_caps(conflux::uring::RingRef{const_cast<io_uring *>(&ring)});
	}
};
struct FdGuard {
	int fd = -1;
	explicit FdGuard(
		int f) noexcept
		: fd{f} {}
	~FdGuard() {
		if (fd >= 0) {
			::close(fd);
		}
	}
	FdGuard(FdGuard const &) = delete;
	FdGuard &operator =(FdGuard const &) = delete;
	FdGuard(
		FdGuard &&o) noexcept
		: fd{exchange(o.fd, -1)} {}
	FdGuard &operator =(
		FdGuard &&o) noexcept {
		if (this != &o) {
			if (fd >= 0) {
				::close(fd);
			}
			fd = exchange(o.fd, -1);
		}
		return *this;
	}
};
// drain all pending CQEs
void drain_cqes(
	io_uring *ring) {
	io_uring_cqe *cqe{};
	while (io_uring_peek_cqe(ring, &cqe) == 0) {
		io_uring_cqe_seen(ring, cqe);
	}
}
[[nodiscard]] io_uring_cqe make_flow_cqe(
	std::uint32_t flow_idx,
	std::uint32_t gen,
	std::uint8_t op_index,
	uf::FlowOpKind kind,
	std::int32_t res) {
	return io_uring_cqe{
		.user_data = uf::encode_tag(flow_idx, gen, op_index, kind),
		.res = res,
	};
}
// ── Variant definition ─────────────────────────────────────────────────────

using RunFn = std::function<void()>;
struct Variant {
	std::string_view name;
	RunFn run;
	RunFn setup;
	RunFn teardown;
};
BenchStats run_variant(
	Variant const &v,
	std::size_t iterations,
	std::size_t warmup,
	std::string_view config_name) {
	if (v.setup) {
		v.setup();
	}

	for (std::size_t i = 0; i < warmup; ++i) {
		v.run();
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
	return BenchStats{
		.config = config_name,
		.variant = v.name,
		.iterations = iterations,
		.total_ns = total,
		.ns_per_iter = ns_pi,
	};
}
// ── accept / close / lifecycle variants ────────────────────────────────────

// raw_accept_close: time single-shot accept + close cycle
void run_accept_close(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{64};
	SocketRawRing raw{rg.get()};

	auto v = Variant{
		.name = "raw_accept_close",
		.run =
			[&] {
				int cli = connect_one(ls.port);
				auto *sqe = raw.get_sqe();
				io_uring_prep_accept(sqe, ls.fd, nullptr, nullptr, 0);
				io_uring_sqe_set_data64(sqe, 1);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				int accepted = cqe->res;
				io_uring_cqe_seen(rg.get(), cqe);
				if (accepted >= 0) {
					submit_close(raw, SocketHandle::from_os(accepted), 2);
					submit_or_throw(raw);
					wait_cqe(rg.get(), &cqe);
					io_uring_cqe_seen(rg.get(), cqe);
				}
				::close(cli);
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// raw_accept_direct_close: single-shot accept into direct fd slot, close direct
void run_accept_direct_close(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{64};
	SocketRawRing raw{rg.get()};
	auto dft = make_unique<DirectFdTable>(rg.get(), 64);
	if (!dft->registered()) {
		return;
	}

	auto v = Variant{
		.name = "raw_accept_direct_close",
		.run =
			[&] {
				int cli = connect_one(ls.port);
				auto *sqe = raw.get_sqe();
				io_uring_prep_accept_direct(sqe, ls.fd, nullptr, nullptr, 0, IORING_FILE_INDEX_ALLOC);
				io_uring_sqe_set_data64(sqe, 1);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				int slot = cqe->res;
				io_uring_cqe_seen(rg.get(), cqe);
				if (slot >= 0) {
					submit_close(raw, SocketHandle::from_direct(static_cast<std::uint32_t>(slot)), 2);
					submit_or_throw(raw);
					wait_cqe(rg.get(), &cqe);
					io_uring_cqe_seen(rg.get(), cqe);
				}
				::close(cli);
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
	drain_cqes(rg.get());
	dft.reset();
}
// raw_accept_direct_reuse_cycle: accept-direct, close, reuse same slot, 10 per iter
void run_accept_direct_reuse_cycle(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{64};
	SocketRawRing raw{rg.get()};
	DirectFdTable dft{rg.get(), 64};
	if (!dft.registered()) {
		return;
	}

	constexpr int kCyclesPerIter = 10;

	auto v = Variant{
		.name = "raw_accept_direct_reuse_cycle",
		.run =
			[&] {
				for (int c = 0; c < kCyclesPerIter; ++c) {
					int cli = connect_one(ls.port);
					auto *sqe = raw.get_sqe();
					io_uring_prep_accept_direct(sqe, ls.fd, nullptr, nullptr, 0, IORING_FILE_INDEX_ALLOC);
					io_uring_sqe_set_data64(sqe, 1);
					submit_or_throw(raw);
					io_uring_cqe *cqe{};
					wait_cqe(rg.get(), &cqe);
					int slot = cqe->res;
					io_uring_cqe_seen(rg.get(), cqe);
					if (slot >= 0) {
						submit_close(raw, SocketHandle::from_direct(static_cast<std::uint32_t>(slot)), 2);
						submit_or_throw(raw);
						wait_cqe(rg.get(), &cqe);
						io_uring_cqe_seen(rg.get(), cqe);
					}
					::close(cli);
				}
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// raw_shutdown_close: linked shutdown(WR) + close pair
void run_shutdown_close(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{64};
	SocketRawRing raw{rg.get()};

	auto v = Variant{
		.name = "raw_shutdown_close",
		.run =
			[&] {
				int cli = connect_one(ls.port);
				int srv = accept_one(ls.fd);
				auto h = SocketHandle::from_os(srv);
				(void)submit_shutdown_close(raw, h, 1, 2);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				for (int n = 0; n < 2;) {
					wait_cqe(rg.get(), &cqe);
					io_uring_cqe_seen(rg.get(), cqe);
					++n;
				}
				::close(cli);
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// ── recv variants ──────────────────────────────────────────────────────────

// raw_multishot_recv_1conn: 1 conn, multishot recv, client sends 64B
void run_multishot_recv_1conn(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};
	BufferRing bufs{
		rg.get(),
		{.count = 256, .buf_size = 4096, .group_id = 0, .huge_pages = false},
		rg.caps()
    };

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	static constexpr auto kPayload = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"sv;

	std::atomic<bool> stop{false};
	thread sender{[&] {
		while (!stop.load(memory_order_relaxed)) {
			if (::send(cli, kPayload.data(), kPayload.size(), MSG_NOSIGNAL) <= 0) {
				break;
			}
		}
	}};

	auto v = Variant{
		.name = "raw_multishot_recv_1conn",
		.run =
			[&] {
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				bool const more = (cqe->flags & IORING_CQE_F_MORE) != 0;
				if (cqe->res > 0 && cqe_has_buffer(static_cast<std::uint32_t>(cqe->flags))) {
					bufs.recycle(cqe_buffer_id(static_cast<std::uint32_t>(cqe->flags)));
				}
				io_uring_cqe_seen(rg.get(), cqe);
				if (!more) {
					submit_recv_multishot(raw, SocketHandle::from_os(srv), bufs, 10);
					submit_or_throw(raw);
				}
			},
		.setup =
			[&] {
				submit_recv_multishot(raw, SocketHandle::from_os(srv), bufs, 10);
				submit_or_throw(raw);
			},
		.teardown =
			[&] {
				submit_cancel_fd(raw, SocketHandle::from_os(srv), 99);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				io_uring_cqe_seen(rg.get(), cqe);
				drain_cqes(rg.get());
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
	stop.store(true, memory_order_relaxed);
	::shutdown(cli, SHUT_RDWR);
	sender.join();
}
// raw_multishot_recv_Nconn: N conns, multishot recv on all (N = nproc, capped at 8)
void run_multishot_recv_Nconn(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto const kConns = static_cast<std::size_t>(min(thread::hardware_concurrency(), 8u));
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{1024};
	SocketRawRing raw{rg.get()};
	BufferRing bufs{
		rg.get(),
		{.count = 4096, .buf_size = 4096, .group_id = 0, .huge_pages = false},
		rg.caps()
    };

	std::vector<FdGuard> clients;
	std::vector<FdGuard> servers;
	clients.reserve(kConns);
	servers.reserve(kConns);
	for (std::size_t i = 0; i < kConns; ++i) {
		int c = connect_one(ls.port);
		int s = accept_one(ls.fd);
		clients.emplace_back(c);
		servers.emplace_back(s);
	}

	static constexpr auto kPayload = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"sv;

	std::atomic<bool> stop{false};
	std::vector<thread> senders;
	senders.reserve(kConns);
	for (std::size_t i = 0; i < kConns; ++i) {
		senders.emplace_back([&, fd = clients[i].fd] {
			while (!stop.load(memory_order_relaxed)) {
				if (::send(fd, kPayload.data(), kPayload.size(), MSG_NOSIGNAL) <= 0) {
					break;
				}
			}
		});
	}

	auto v = Variant{
		.name = "raw_multishot_recv_Nconn",
		.run =
			[&] {
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				bool const more = (cqe->flags & IORING_CQE_F_MORE) != 0;
				std::uint64_t const ud = io_uring_cqe_get_data64(cqe);
				if (cqe->res > 0 && cqe_has_buffer(static_cast<std::uint32_t>(cqe->flags))) {
					bufs.recycle(cqe_buffer_id(static_cast<std::uint32_t>(cqe->flags)));
				}
				io_uring_cqe_seen(rg.get(), cqe);
				if (!more && ud >= 100 && ud < 100 + kConns) {
					std::size_t const idx = ud - 100;
					submit_recv_multishot(raw, SocketHandle::from_os(servers[idx].fd), bufs, ud);
					submit_or_throw(raw);
				}
			},
		.setup =
			[&] {
				for (std::size_t i = 0; i < kConns; ++i) {
					submit_recv_multishot(raw, SocketHandle::from_os(servers[i].fd), bufs, 100 + i);
				}
				submit_or_throw(raw);
			},
		.teardown =
			[&] {
				for (std::size_t i = 0; i < kConns; ++i) {
					submit_cancel_fd(raw, SocketHandle::from_os(servers[i].fd), 200 + i);
				}
				submit_or_throw(raw);
				drain_cqes(rg.get());
			},
	};

	auto iters = min(args.iterations, std::size_t{50000});
	auto warmup = std::min(args.warmup, std::size_t{5000});
	auto s = run_variant(v, iters, warmup, config_name);
	bench_print(s, json, false);
	stop.store(true, memory_order_relaxed);
	for (auto &c: clients) {
		::shutdown(c.fd, SHUT_RDWR);
	}
	for (auto &t: senders) {
		t.join();
	}
}
// raw_recv_fixed_fd: recv on direct fd slot
void run_recv_fixed_fd(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};
	DirectFdTable dft{rg.get(), 64};
	if (!dft.registered()) {
		return;
	}
	BufferRing bufs{
		rg.get(),
		{.count = 256, .buf_size = 4096, .group_id = 0, .huge_pages = false},
		rg.caps()
    };

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	(void)dft.install(0, srv);

	static constexpr auto kPayload = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"sv;

	std::atomic<bool> stop{false};
	thread sender{[&] {
		while (!stop.load(memory_order_relaxed)) {
			if (::send(cli, kPayload.data(), kPayload.size(), MSG_NOSIGNAL) <= 0) {
				break;
			}
		}
	}};

	auto v = Variant{
		.name = "raw_recv_fixed_fd",
		.run =
			[&] {
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				bool const more = (cqe->flags & IORING_CQE_F_MORE) != 0;
				if (cqe->res > 0 && cqe_has_buffer(static_cast<std::uint32_t>(cqe->flags))) {
					bufs.recycle(cqe_buffer_id(static_cast<std::uint32_t>(cqe->flags)));
				}
				io_uring_cqe_seen(rg.get(), cqe);
				if (!more) {
					submit_recv_multishot(raw, SocketHandle::from_direct(0), bufs, 10);
					submit_or_throw(raw);
				}
			},
		.setup =
			[&] {
				submit_recv_multishot(raw, SocketHandle::from_direct(0), bufs, 10);
				submit_or_throw(raw);
			},
		.teardown =
			[&] {
				submit_cancel_fd(raw, SocketHandle::from_direct(0), 99);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				io_uring_cqe_seen(rg.get(), cqe);
				drain_cqes(rg.get());
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
	stop.store(true, memory_order_relaxed);
	::shutdown(cli, SHUT_RDWR);
	sender.join();
}
// ── send / writev variants ─────────────────────────────────────────────────

void run_send_variant(
	BenchArgs const &args,
	bool json,
	std::string_view config_name,
	std::string_view variant_name,
	std::string_view payload) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	std::atomic<bool> stop{false};
	thread reader{[&] {
		std::array<char, 65536> buf{};
		while (!stop.load(memory_order_relaxed)) {
			if (::recv(cli, buf.data(), buf.size(), 0) <= 0) {
				break;
			}
		}
	}};

	auto v = Variant{
		.name = variant_name,
		.run =
			[&] {
				submit_send_borrowed(raw, SocketHandle::from_os(srv), payload.data(), payload.size(), 1);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				io_uring_cqe_seen(rg.get(), cqe);
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
	stop.store(true, memory_order_relaxed);
	::shutdown(srv, SHUT_RDWR);
	reader.join();
}
static auto const kSend64 = std::string(64, 'X');
static auto const kSend4k = std::string(4096, 'Y');
// raw_send_fixed_fd: send 64B on direct fd slot
void run_send_fixed_fd(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};
	DirectFdTable dft{rg.get(), 64};
	if (!dft.registered()) {
		return;
	}

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	(void)dft.install(0, srv);

	std::atomic<bool> stop{false};
	thread reader{[&] {
		std::array<char, 65536> buf{};
		while (!stop.load(memory_order_relaxed)) {
			if (::recv(cli, buf.data(), buf.size(), 0) <= 0) {
				break;
			}
		}
	}};

	auto v = Variant{
		.name = "raw_send_fixed_fd",
		.run =
			[&] {
				submit_send_borrowed(raw, SocketHandle::from_direct(0), kSend64.data(), kSend64.size(), 1);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				io_uring_cqe_seen(rg.get(), cqe);
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
	stop.store(true, memory_order_relaxed);
	::shutdown(srv, SHUT_RDWR);
	reader.join();
}
// raw_writev_2seg / raw_writev_4seg
void run_writev_variant(
	BenchArgs const &args,
	bool json,
	std::string_view config_name,
	std::string_view variant_name,
	span<iovec const> iov) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	std::atomic<bool> stop{false};
	thread reader{[&] {
		std::array<char, 65536> buf{};
		while (!stop.load(memory_order_relaxed)) {
			if (::recv(cli, buf.data(), buf.size(), 0) <= 0) {
				break;
			}
		}
	}};

	auto v = Variant{
		.name = variant_name,
		.run =
			[&] {
				submit_writev_borrowed(
					raw,
					SocketHandle::from_os(srv),
					iov.data(),
					static_cast<unsigned>(iov.size()),
					1);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				io_uring_cqe_seen(rg.get(), cqe);
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
	stop.store(true, memory_order_relaxed);
	::shutdown(srv, SHUT_RDWR);
	reader.join();
}
static auto const kWritevSeg = std::string(512, 'W');
// ── direct fd management variants ──────────────────────────────────────────

// raw_direct_fd_install: install OS fd into direct table
void run_direct_fd_install(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};
	DirectFdTable dft{rg.get(), 64};
	if (!dft.registered()) {
		return;
	}

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	auto v = Variant{
		.name = "raw_direct_fd_install",
		.run =
			[&] {
				submit_fixed_fd_install(raw, 0, 1);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				int const installed = cqe->res;
				io_uring_cqe_seen(rg.get(), cqe);
				if (installed >= 0) {
					::close(installed);
				}
			},
	};

	(void)dft.install(0, srv);
	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// direct_fd_alloc_free: DirectFdTable install + clear (no io_uring SQE)
void run_direct_fd_alloc_free(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	RingGuard rg{64};

	auto v = Variant{
		.name = "direct_fd_alloc_free",
		.run = [&] { DirectFdTable dft{rg.get(), 64}; },
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// direct_fd_slot_exhaustion: fill table, free all, refill
void run_direct_fd_slot_exhaustion(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};

	constexpr std::uint32_t kSlots = 32;
	std::vector<FdGuard> clients;
	std::vector<FdGuard> servers;
	clients.reserve(kSlots);
	servers.reserve(kSlots);
	for (std::uint32_t i = 0; i < kSlots; ++i) {
		int c = connect_one(ls.port);
		int s = accept_one(ls.fd);
		clients.emplace_back(c);
		servers.emplace_back(s);
	}

	RingGuard rg{64};

	auto v = Variant{
		.name = "direct_fd_slot_exhaustion",
		.run =
			[&] {
				DirectFdTable dft{rg.get(), kSlots};
				if (!dft.registered()) {
					return;
				}
				for (std::uint32_t i = 0; i < kSlots; ++i) {
					(void)dft.install(i, servers[i].fd);
				}
				// table destroyed → unregisters
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// raw_stale_cqe_after_reuse: accept-direct, close, reuse slot, verify gen rejects old CQE
void run_stale_cqe_after_reuse(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	RingGuard rg{64};
	GenerationTable gen{64};

	auto v = Variant{
		.name = "raw_stale_cqe_after_reuse",
		.run =
			[&] {
				std::uint32_t slot = 0;
				auto g1 = gen.current(slot);
				auto g2 = gen.advance(slot);
				if (gen.alive(slot, g1)) {
					throw std::runtime_error{"stale gen accepted"};
				}
				if (!gen.alive(slot, g2)) {
					throw std::runtime_error{"current gen rejected"};
				}
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// ── cancel / timeout variants ──────────────────────────────────────────────

// raw_cancel_recv: start multishot recv, cancel by fd
void run_cancel_recv(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};
	BufferRing bufs{
		rg.get(),
		{.count = 256, .buf_size = 4096, .group_id = 0, .huge_pages = false},
		rg.caps()
    };

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	auto v = Variant{
		.name = "raw_cancel_recv",
		.run =
			[&] {
				submit_recv_multishot(raw, SocketHandle::from_os(srv), bufs, 10);
				submit_or_throw(raw);
				submit_cancel_fd(raw, SocketHandle::from_os(srv), 20);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				// wait for both cancel result and recv cancellation
				for (int n = 0; n < 2;) {
					wait_cqe(rg.get(), &cqe);
					io_uring_cqe_seen(rg.get(), cqe);
					++n;
				}
				drain_cqes(rg.get());
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// raw_cancel_by_user_data: start recv, cancel by ud
void run_cancel_by_user_data(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};
	BufferRing bufs{
		rg.get(),
		{.count = 256, .buf_size = 4096, .group_id = 0, .huge_pages = false},
		rg.caps()
    };

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	auto v = Variant{
		.name = "raw_cancel_by_user_data",
		.run =
			[&] {
				submit_recv_multishot(raw, SocketHandle::from_os(srv), bufs, 10);
				submit_or_throw(raw);
				submit_cancel_by_ud(raw, 10, 20);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				for (int n = 0; n < 2;) {
					wait_cqe(rg.get(), &cqe);
					io_uring_cqe_seen(rg.get(), cqe);
					++n;
				}
				drain_cqes(rg.get());
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// raw_link_timeout: recv with IO_LINK + timeout
void run_link_timeout(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	std::array<char, 64> recv_buf{};
	__kernel_timespec ts{.tv_sec = 0, .tv_nsec = 1000000}; // 1ms

	auto v = Variant{
		.name = "raw_link_timeout",
		.run =
			[&] {
				auto *sqe = raw.get_sqe();
				io_uring_prep_recv(sqe, srv, recv_buf.data(), recv_buf.size(), 0);
				io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
				io_uring_sqe_set_data64(sqe, 10);
				submit_link_timeout_borrowed(raw, &ts, 20);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				for (int n = 0; n < 2;) {
					wait_cqe(rg.get(), &cqe);
					io_uring_cqe_seen(rg.get(), cqe);
					++n;
				}
				drain_cqes(rg.get());
			},
	};

	auto iters = min(args.iterations, std::size_t{10000});
	auto warmup = std::min(args.warmup, std::size_t{1000});
	auto s = run_variant(v, iters, warmup, config_name);
	bench_print(s, json, false);
}
// ── setsockopt variants ────────────────────────────────────────────────────

void run_setsockopt_variant(
	BenchArgs const &args,
	bool json,
	std::string_view config_name,
	std::string_view variant_name,
	int level,
	int optname) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};
	DirectFdTable dft{rg.get(), 64};
	if (!dft.registered()) {
		return;
	}

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	(void)dft.install(0, srv);
	int optval = 1;

	auto v = Variant{
		.name = variant_name,
		.run =
			[&] {
				submit_setsockopt_borrowed(
					raw,
					SocketHandle::from_direct(0),
					level,
					optname,
					&optval,
					sizeof(optval),
					1);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				io_uring_cqe_seen(rg.get(), cqe);
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// ── buffer ring variants ───────────────────────────────────────────────────

// buf_ring_alloc_recycle: allocate RecvBuffer, immediately recycle
void run_buf_ring_alloc_recycle(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	RingGuard rg{64};
	BufferRing bufs{
		rg.get(),
		{.count = 256, .buf_size = 4096, .group_id = 0, .huge_pages = false},
		rg.caps()
    };

	std::uint16_t id = 0;
	auto v = Variant{
		.name = "buf_ring_alloc_recycle",
		.run =
			[&] {
				auto rb = bufs.lease(id, 64);
				// rb destructor recycles
				id = static_cast<std::uint16_t>((id + 1) % 256);
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// buf_ring_batch_recycle_16: batch recycle 16 buffers
void run_buf_ring_batch_recycle_16(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	RingGuard rg{64};
	BufferRing bufs{
		rg.get(),
		{.count = 256, .buf_size = 4096, .group_id = 0, .huge_pages = false},
		rg.caps()
    };

	std::array<std::uint16_t, 16> ids{};
	for (std::uint16_t i = 0; i < 16; ++i) {
		ids[i] = i;
	}

	auto v = Variant{
		.name = "buf_ring_batch_recycle_16",
		.run = [&] { bufs.recycle_batch(ids); },
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// buf_ring_exhaustion_recover: drain all buffers, recycle all, verify recv resumes
void run_buf_ring_exhaustion_recover(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};
	constexpr std::uint32_t kBufCount = 64;
	BufferRing bufs{
		rg.get(),
		{.count = kBufCount, .buf_size = 4096, .group_id = 0, .huge_pages = false},
		rg.caps()
    };

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	static constexpr auto kPayload = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"sv;

	auto v = Variant{
		.name = "buf_ring_exhaustion_recover",
		.run =
			[&] {
				// drain kBufCount buffers
				submit_recv_multishot(raw, SocketHandle::from_os(srv), bufs, 10);
				submit_or_throw(raw);
				std::vector<std::uint16_t> drained;
				drained.reserve(kBufCount);
				for (std::uint32_t i = 0; i < kBufCount; ++i) {
					(void)::send(cli, kPayload.data(), kPayload.size(), MSG_NOSIGNAL);
					io_uring_cqe *cqe{};
					wait_cqe(rg.get(), &cqe);
					if (cqe->res > 0 && cqe_has_buffer(static_cast<std::uint32_t>(cqe->flags))) {
						drained.push_back(cqe_buffer_id(static_cast<std::uint32_t>(cqe->flags)));
					}
					io_uring_cqe_seen(rg.get(), cqe);
				}
				// recycle all at once
				bufs.recycle_batch(drained);
				// cancel outstanding recv
				submit_cancel_fd(raw, SocketHandle::from_os(srv), 99);
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				wait_cqe(rg.get(), &cqe);
				io_uring_cqe_seen(rg.get(), cqe);
				drain_cqes(rg.get());
			},
	};

	auto iters = min(args.iterations, std::size_t{5000});
	auto warmup = std::min(args.warmup, std::size_t{500});
	auto s = run_variant(v, iters, warmup, config_name);
	bench_print(s, json, false);
}
// ── generation table variants ──────────────────────────────────────────────

// gen_table_check: check() hot loop
void run_gen_table_check(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	GenerationTable gen{1024};

	auto v = Variant{
		.name = "gen_table_check",
		.run =
			[&] {
				for (std::uint32_t i = 0; i < 1024; ++i) {
					(void)gen.alive(i, 0);
				}
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// gen_table_bump_check: advance() then alive() alternating
void run_gen_table_bump_check(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	GenerationTable gen{1024};

	std::uint32_t slot = 0;
	auto v = Variant{
		.name = "gen_table_bump_check",
		.run =
			[&] {
				auto g = gen.advance(slot);
				(void)gen.alive(slot, g);
				slot = (slot + 1) & 1023;
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// flow_deferred_close_abandon: force a deferred close and abandon it without
// resubmitting, exercising the shutdown cleanup path.
void run_flow_deferred_close_abandon(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	struct State {
		ur::Ring ring;
		uf::FlowRuntime rt;
		State()
			: ring{[] {
				auto r = ur::Ring::init(4, ur::SetupFlags{});
				if (!r) {
					throw std::runtime_error{"Ring::init failed for flow deferred-close bench"};
				}
				return move(*r);
			}()}
			, rt{ring, ur::detect_caps(ring.ref()), [](uf::FlowResult) noexcept {}} {}
	};
	auto state = make_shared<State>();

	auto v = Variant{
		.name = "flow_deferred_close_abandon",
		.run =
			[&] {
				char buf[4] = {};
				auto b = state->rt.flow();
				auto f = b.open_direct(ur::DirectSlot{0}, AT_FDCWD, uf::BorrowedPath{"/dev/null"}, O_RDWR);
				f.then_read(buf, 4, 0).then_write(buf, 4, 0).then_read(buf, 4, 0).close_if_opened();
				(void)b.submit();

				std::uint32_t const idx = 0;
				std::uint32_t const gen = 1;
				auto cqe0 = make_flow_cqe(idx, gen, 0, uf::FlowOpKind::open_direct, 0);
				auto cqe1 = make_flow_cqe(idx, gen, 1, uf::FlowOpKind::read, 4);
				auto cqe2 = make_flow_cqe(idx, gen, 2, uf::FlowOpKind::write, 4);
				auto cqe3 = make_flow_cqe(idx, gen, 3, uf::FlowOpKind::read, 4);
				state->rt.on_cqe(&cqe0);
				state->rt.on_cqe(&cqe1);
				state->rt.on_cqe(&cqe2);
				state->rt.on_cqe(&cqe3);

				(void)state->rt.abandon_deferred_closes([](auto) noexcept {});
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// recv_arm_policy_resolve: compare default_ / poll_first / adaptive policy selection
// on idle-style and bulk-style CQE flag traces.
void run_recv_arm_policy_resolve(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	static constexpr std::uint32_t kSockNonempty = IORING_CQE_F_SOCK_NONEMPTY;
	static constexpr std::array<std::uint32_t, 256> kIdleFlags{};
	static constexpr std::array<std::uint32_t, 256> kBulkFlags = [] {
		std::array<std::uint32_t, 256> flags{};
		flags.fill(kSockNonempty);
		return flags;
	}();

	enum class PolicyMode : std::uint8_t {
		default_,
		poll_first,
		adaptive,
	};
	auto run_case = [&](std::string_view variant_name, span<std::uint32_t const> flags, PolicyMode mode) {
		volatile std::uint64_t sink = 0;
		auto v = Variant{
			.name = variant_name,
			.run =
				[&] {
					for (std::uint32_t flg: flags) {
						RecvArmPolicy arm = RecvArmPolicy::default_;
						switch (mode) {
							case PolicyMode::default_  : arm = RecvArmPolicy::default_; break;
							case PolicyMode::poll_first: arm = RecvArmPolicy::poll_first; break;
							case PolicyMode::adaptive   : arm = resolve_recv_arm_policy(true, true, true, flg); break;
						}
						sink += static_cast<std::uint64_t>(arm);
					}
				},
		};
		auto s = run_variant(v, args.iterations, args.warmup, config_name);
		bench_print(s, json, false);
		auto _ = sink;
	};

	run_case("recv_arm_idle_default_256"sv, kIdleFlags, PolicyMode::default_);
	run_case("recv_arm_idle_poll_first_256"sv, kIdleFlags, PolicyMode::poll_first);
	run_case("recv_arm_idle_adaptive_256"sv, kIdleFlags, PolicyMode::adaptive);
	run_case("recv_arm_bulk_default_256"sv, kBulkFlags, PolicyMode::default_);
	run_case("recv_arm_bulk_poll_first_256"sv, kBulkFlags, PolicyMode::poll_first);
	run_case("recv_arm_bulk_adaptive_256"sv, kBulkFlags, PolicyMode::adaptive);
}
// ── submit batching variants ───────────────────────────────────────────────

// raw_batch_send_32: submit 32 sends, wait for 32 CQEs
void run_batch_send_32(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	static constexpr auto kPayload = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"sv;

	std::atomic<bool> stop{false};
	thread reader{[&] {
		std::array<char, 65536> buf{};
		while (!stop.load(memory_order_relaxed)) {
			if (::recv(cli, buf.data(), buf.size(), 0) <= 0) {
				break;
			}
		}
	}};

	auto v = Variant{
		.name = "raw_batch_send_32",
		.run =
			[&] {
				for (int i = 0; i < 32; ++i) {
					submit_send_borrowed(
						raw,
						SocketHandle::from_os(srv),
						kPayload.data(),
						kPayload.size(),
						static_cast<std::uint64_t>(i));
				}
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				for (int n = 0; n < 32;) {
					wait_cqe(rg.get(), &cqe);
					io_uring_cqe_seen(rg.get(), cqe);
					++n;
				}
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
	stop.store(true, memory_order_relaxed);
	::shutdown(srv, SHUT_RDWR);
	reader.join();
}
// raw_batch_recv_send_16: submit 16 recvs + 16 sends, wait for all
void run_batch_recv_send_16(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	auto ls = make_listen_socket();
	FdGuard lsg{ls.fd};
	RingGuard rg{256};
	SocketRawRing raw{rg.get()};

	int cli = connect_one(ls.port);
	FdGuard cg{cli};
	int srv = accept_one(ls.fd);
	FdGuard sg{srv};

	static constexpr auto kPayload = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"sv;
	std::array<std::array<char, 128>, 16> recv_bufs{};

	std::atomic<bool> stop{false};
	thread sender{[&] {
		while (!stop.load(memory_order_relaxed)) {
			if (::send(cli, kPayload.data(), kPayload.size(), MSG_NOSIGNAL) <= 0) {
				break;
			}
		}
	}};
	thread reader{[&] {
		std::array<char, 65536> buf{};
		while (!stop.load(memory_order_relaxed)) {
			if (::recv(cli, buf.data(), buf.size(), 0) <= 0) {
				break;
			}
		}
	}};

	auto v = Variant{
		.name = "raw_batch_recv_send_16",
		.run =
			[&] {
				for (std::size_t i = 0; i < 16; ++i) {
					submit_async_recv_borrowed(
						raw,
						SocketHandle::from_os(srv),
						recv_bufs[i].data(),
						recv_bufs[i].size(),
						static_cast<std::uint64_t>(i));
				}
				for (std::size_t i = 0; i < 16; ++i) {
					submit_send_borrowed(raw, SocketHandle::from_os(srv), kPayload.data(), kPayload.size(), 100 + i);
				}
				submit_or_throw(raw);
				io_uring_cqe *cqe{};
				for (int n = 0; n < 32;) {
					wait_cqe(rg.get(), &cqe);
					io_uring_cqe_seen(rg.get(), cqe);
					++n;
				}
			},
	};

	auto iters = min(args.iterations, std::size_t{50000});
	auto warmup = std::min(args.warmup, std::size_t{5000});
	auto s = run_variant(v, iters, warmup, config_name);
	bench_print(s, json, false);
	stop.store(true, memory_order_relaxed);
	::shutdown(srv, SHUT_RDWR);
	::shutdown(cli, SHUT_RDWR);
	sender.join();
	reader.join();
}
// buf_slices_from_cqe_classic: buffer_slices_from_cqe hot path, single-buffer CQE
void run_buf_slices_from_cqe_classic(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	RingGuard rg{64};
	BufferRing bufs{
		rg.get(),
		{.count = 256, .buf_size = 4096, .group_id = 0, .huge_pages = false},
		rg.caps()
    };

	auto v = Variant{
		.name = "buf_slices_from_cqe_classic",
		.run =
			[&] {
				std::uint16_t const id = bufs.ring_id_at(bufs.debug_head_pos());
				std::uint32_t const flags = IORING_CQE_F_BUFFER | (static_cast<std::uint32_t>(id) << IORING_CQE_BUFFER_SHIFT);
				auto slices = buffer_slices_from_cqe(bufs, 64, flags, false);
				slices.recycle_all();
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// buf_slices_from_cqe_bundle_3_full / _bundle_3_partial_tail
// Simulate a 3-buffer bundle CQE using BufferRingMode::recv_bundle.
// partial_tail variant: last buffer holds fewer bytes, exercising the length math.
void run_buf_slices_from_cqe_bundle(
	BenchArgs const &args,
	bool json,
	std::string_view config_name,
	std::string_view variant_name,
	int res) {
	RingGuard rg{64};
	constexpr std::size_t kBufSz = 4096;
	BufferRing bufs{
		rg.get(),
		{.count = 256, .buf_size = kBufSz, .group_id = 0, .huge_pages = false, .mode = BufferRingMode::recv_bundle},
		rg.caps()
    };

	auto v = Variant{
		.name = variant_name,
		.run =
			[&] {
				std::uint16_t const id = bufs.ring_id_at(bufs.debug_head_pos());
				std::uint32_t const flags = IORING_CQE_F_BUFFER | (static_cast<std::uint32_t>(id) << IORING_CQE_BUFFER_SHIFT);
				auto slices = buffer_slices_from_cqe(bufs, res, flags, true);
				volatile std::size_t acc = 0;
				for (auto s: slices) {
					acc += s.bytes.size();
				}
				auto _ = acc;
				slices.recycle_all();
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// buf_slice_from_incremental_cqe: N synthetic partial CQEs per buffer, final recycles
// incremental_1_full: N=1 — exercises incremental_offsets_ + final-CQE consume + final recycle
// incremental_2_half: N=2 — two half-fills, BUF_MORE on first
// incremental_4_quarter: N=4 — four quarter-fills, BUF_MORE on first three
void run_buf_slice_from_incremental_cqe(
	BenchArgs const &args,
	bool json,
	std::string_view config_name,
	std::string_view variant_name,
	int n) {
	RingGuard rg{64};
	if (!rg.caps().feat_pbuf_ring_inc) {
		return;
	}
	constexpr std::size_t kBufSz = 4096;
	BufferRing bufs{
		rg.get(),
		{.count = 256, .buf_size = kBufSz, .group_id = 0, .huge_pages = false, .mode = BufferRingMode::incremental},
		rg.caps()
    };

	std::size_t const chunk = kBufSz / static_cast<std::size_t>(n);

	volatile std::size_t acc = 0;
	auto v = Variant{
		.name = variant_name,
		.run =
			[&] {
				std::uint16_t const id = bufs.ring_id_at(bufs.debug_head_pos());
				for (int i = 0; i < n; ++i) {
					bool const is_last = (i == n - 1);
					std::size_t const res = is_last ? kBufSz - chunk * static_cast<std::size_t>(i) : chunk;
					std::uint32_t flags = IORING_CQE_F_BUFFER | (static_cast<std::uint32_t>(id) << IORING_CQE_BUFFER_SHIFT);
					if (!is_last) {
						flags |= IORING_CQE_F_BUF_MORE;
					}
					auto slice = buffer_slice_from_incremental_cqe(bufs, static_cast<int>(res), flags);
					acc += slice.size();
					slice.recycle_if_final();
				}
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// direct_slot_pool_acquire_release: acquire() + release_empty() cycle
void run_direct_slot_pool_acquire_release(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	DirectSlotPool pool{256};

	auto v = Variant{
		.name = "direct_slot_pool_acquire_release",
		.run =
			[&] {
				auto r = pool.acquire();
				if (r) {
					auto _ = pool.release_empty(*r);
				}
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}
// direct_slot_pool_full_lifecycle: adopt_kernel_allocated → mark_closing → release_closed
void run_direct_slot_pool_full_lifecycle(
	BenchArgs const &args,
	bool json,
	std::string_view config_name) {
	DirectSlotPool pool{256};
	std::uint32_t slot = 0;

	auto v = Variant{
		.name = "direct_slot_pool_full_lifecycle",
		.run =
			[&] {
				auto _ = pool.adopt_kernel_allocated(slot);
				auto _ = pool.mark_closing(slot);
				auto _ = pool.release_closed(slot);
				slot = (slot + 1) % 256;
			},
	};

	auto s = run_variant(v, args.iterations, args.warmup, config_name);
	bench_print(s, json, false);
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"socket_raw","parser":"standard","configs":[{"name":"default","extra":{},"args":["--iterations","100000","--warmup","10000"]}]})");

	auto const args = bench_parse_args(span{argv, static_cast<std::size_t>(argc)});
	auto const json = args.json_out;
	auto const config_name = args.config_name.empty() ? "default"sv : std::string_view{args.config_name};

	// ── accept / close / lifecycle ────
	run_accept_close(args, json, config_name);
	run_accept_direct_close(args, json, config_name);
	run_accept_direct_reuse_cycle(args, json, config_name);
	run_shutdown_close(args, json, config_name);

	// ── recv ────
	run_multishot_recv_1conn(args, json, config_name);
	run_multishot_recv_Nconn(args, json, config_name);
	run_recv_fixed_fd(args, json, config_name);

	// ── send / writev ────
	run_send_variant(args, json, config_name, "raw_send_64", kSend64);
	run_send_variant(args, json, config_name, "raw_send_4k", kSend4k);
	run_send_fixed_fd(args, json, config_name);

	iovec iov2[2]{
		{.iov_base = const_cast<char *>(kWritevSeg.data()), .iov_len = kWritevSeg.size()},
		{.iov_base = const_cast<char *>(kWritevSeg.data()), .iov_len = kWritevSeg.size()},
	};
	run_writev_variant(args, json, config_name, "raw_writev_2seg", iov2);

	iovec iov4[4]{
		{.iov_base = const_cast<char *>(kWritevSeg.data()), .iov_len = kWritevSeg.size()},
		{.iov_base = const_cast<char *>(kWritevSeg.data()), .iov_len = kWritevSeg.size()},
		{.iov_base = const_cast<char *>(kWritevSeg.data()), .iov_len = kWritevSeg.size()},
		{.iov_base = const_cast<char *>(kWritevSeg.data()), .iov_len = kWritevSeg.size()},
	};
	run_writev_variant(args, json, config_name, "raw_writev_4seg", iov4);

	// ── direct fd management ────
	run_direct_fd_install(args, json, config_name);
	run_direct_fd_alloc_free(args, json, config_name);
	run_direct_fd_slot_exhaustion(args, json, config_name);
	run_stale_cqe_after_reuse(args, json, config_name);

	// ── cancel / timeout ────
	run_cancel_recv(args, json, config_name);
	run_cancel_by_user_data(args, json, config_name);
	run_link_timeout(args, json, config_name);

	// ── setsockopt ────
	run_setsockopt_variant(args, json, config_name, "raw_setsockopt_nodelay", IPPROTO_TCP, TCP_NODELAY);
	run_setsockopt_variant(args, json, config_name, "raw_setsockopt_quickack", IPPROTO_TCP, TCP_QUICKACK);

	// ── buffer ring ────
	run_buf_ring_alloc_recycle(args, json, config_name);
	run_buf_ring_batch_recycle_16(args, json, config_name);
	run_buf_ring_exhaustion_recover(args, json, config_name);

	// ── generation table ────
	run_gen_table_check(args, json, config_name);
	run_gen_table_bump_check(args, json, config_name);
	run_flow_deferred_close_abandon(args, json, config_name);
	run_recv_arm_policy_resolve(args, json, config_name);

	// ── submit batching ────
	run_batch_send_32(args, json, config_name);
	run_batch_recv_send_16(args, json, config_name);

	// ── buffer slices from cqe ────
	run_buf_slices_from_cqe_classic(args, json, config_name);
	constexpr std::size_t kBufSz = 4096;
	run_buf_slices_from_cqe_bundle(
		args,
		json,
		config_name,
		"buf_slices_from_cqe_bundle_3_full",
		static_cast<int>(3 * kBufSz));
	run_buf_slices_from_cqe_bundle(
		args,
		json,
		config_name,
		"buf_slices_from_cqe_bundle_3_partial_tail",
		static_cast<int>(2 * kBufSz + 64));
	run_buf_slice_from_incremental_cqe(args, json, config_name, "incremental_1_full", 1);
	run_buf_slice_from_incremental_cqe(args, json, config_name, "incremental_2_half", 2);
	run_buf_slice_from_incremental_cqe(args, json, config_name, "incremental_4_quarter", 4);

	// ── direct slot pool ────
	run_direct_slot_pool_acquire_release(args, json, config_name);
	run_direct_slot_pool_full_lifecycle(args, json, config_name);
}
