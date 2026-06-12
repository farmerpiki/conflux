module;
#include <cstddef> // must precede openssl/nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" blocks re-include c++config.h

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#if CONFLUX_HAS_TLS
	#include <openssl/err.h>
	#include <openssl/ssl.h>
#endif
#if CONFLUX_HAS_HTTP2
	#include <nghttp2/nghttp2.h>
#endif
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

module conflux.net.http_server:loop;

import std;
import conflux.types;
import std.compat;

import conflux.net.http.types;
import conflux.net.http.response;
import conflux.net.router;
import conflux.file_map;
import conflux.net.detail.direct_slot_pool;
import conflux.net.vhost;
import conflux.net.config;
import conflux.net.http1_parser;
import conflux.net.http_server_helpers;
import conflux.net.http_server_config;
import conflux.small_function;
import conflux.uring;
import conflux.uring.completion;
import conflux.uring.handle;
import conflux.work;
import conflux.file_io;
import conflux.socket_io;
import conflux.utils;
#if CONFLUX_HAS_HTTP2
import conflux.net.http2;
#endif
#if CONFLUX_HAS_HTTP3
import conflux.net.http3;
#endif
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
import :state;
using namespace conflux::socket_io;
using conflux::uring::CompletionTable;
using conflux::uring::DirectFd;
using conflux::uring::OsFd;
using conflux::uring::UserDataFn;

#if CONFLUX_HTTP_TRACE
	#define HTTP_TRACE(MSG) conflux::utils::eprintln(std::format("http_trace {}", (MSG)))
#else
	#define HTTP_TRACE(MSG) ((void)0)
#endif

namespace conflux::http::server_detail {

struct RequestBufferDeleter {
	std::shared_ptr<RequestBufferPool> pool;
	void operator ()(
		std::string *ptr) noexcept {
		if (ptr == nullptr) {
			return;
		}
		try {
			ptr->clear();
			std::scoped_lock lock{pool->mutex};
			pool->buffers.push_back(std::move(*ptr));
		} catch (...) {} // NOLINT(bugprone-empty-catch): buffer-pool return is best-effort during deleter cleanup.
		delete ptr;
	}
};

} // namespace conflux::http::server_detail

std::uint64_t Ring::pack_fd_gen(
	int fd,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(fd)) << 32) | static_cast<std::uint64_t>(gen);
}

Ring::~Ring() {
	buf_ring_.reset();
	direct_fds_.reset();
	if (listen_fd >= 0) {
		::close(listen_fd);
	}
	io_uring_queue_exit(&ring);
}

[[nodiscard]] std::shared_ptr<std::string> Ring::acquire_request_buffer() {
	std::string initial;
	auto pool = request_buffer_pool;
	{
		std::scoped_lock lock{pool->mutex};
		if (!pool->buffers.empty()) {
			initial = std::move(pool->buffers.back());
			pool->buffers.pop_back();
		}
	}
	auto *storage = new std::string{std::move(initial)};
	return std::shared_ptr<std::string>{
		storage,
		conflux::http::server_detail::RequestBufferDeleter{.pool = std::move(pool)}};
}

[[nodiscard]] conflux::http::Response Ring::dispatch(
	conflux::http::RequestView const &req) const {
	if (vhost_router != nullptr) {
		return vhost_router->dispatch(req);
	}
	return router->dispatch(req);
}

[[nodiscard]] bool Ring::has_context_routes() const noexcept {
	if (vhost_router != nullptr) {
		return vhost_router->has_context_routes();
	}
	return router != nullptr && router->has_context_routes();
}

[[nodiscard]] std::optional<conflux::http::Response> Ring::try_dispatch_context(
	conflux::http::RequestView const &req) const {
	if (!client_task_ring_) {
		return std::nullopt;
	}
	if (!has_context_routes()) {
		return std::nullopt;
	}
	conflux::http::RequestContext const ctx{conflux::http::RequestRingRef{*client_task_ring_}};
	if (vhost_router != nullptr) {
		return vhost_router->dispatch_context(req, ctx);
	}
	return router->dispatch_context(req, ctx);
}

[[nodiscard]] std::shared_ptr<WorkPool> Ring::resolve_ws_work_pool(
	conflux::http::RequestView const &req) const {
	if (vhost_router != nullptr) {
		return vhost_router->resolved_work_pool(req.headers["host"]);
	}
	return router->work_pool();
}

void Ring::clear_deferred_wait(
	int deferred_efd) {
	if (deferred_efd >= 0) {
		if (auto it = deferred_waits.find(deferred_efd); it != deferred_waits.end() && it->second.response) {
			it->second.response->cancel_disconnect();
		}
		deferred_waits.erase(deferred_efd);
	}
}

void Ring::queue_deferred_wait(
	int conn_fd,
	int deferred_efd,
	std::shared_ptr<conflux::http::DeferredResponse> response,
	std::int32_t stream_id) {
	if (deferred_efd < 0 || !response) {
		return;
	}
	auto *sqe = get_sqe();
	if (sqe == nullptr) {
		defer_op([this, conn_fd, deferred_efd, response, stream_id]() mutable {
			queue_deferred_wait(conn_fd, deferred_efd, std::move(response), stream_id);
		});
		return;
	}

	auto &wait = deferred_waits[deferred_efd];
	wait.conn_fd = conn_fd;
	wait.stream_id = stream_id;
	wait.response = std::move(response);

	auto const conn_gen = conn_for(conn_fd).gen;
	auto const ud = pack(Op::DeferredPoll, conn_gen, deferred_efd);
	auto buf = std::make_unique<std::uint64_t>(0);
	conflux::uring::Sqe{sqe}
		.prep_read(conflux::uring::SqeFd{deferred_efd}, buf.get(), sizeof(std::uint64_t), 0)
		.user_data(conflux::uring::UserData{ud});
	in_flight_read_bufs[ud] = std::move(buf);
}

io_uring_sqe *Ring::get_sqe() {
	if (ring_fatal_) {
		return nullptr;
	}
	auto sqe = raw_.try_get_sqe();
	return sqe ? sqe.raw() : nullptr;
}

// Defer an op whose SQE allocation failed. Replayed from run_loop once
// the CQE reap frees ring capacity.
void Ring::defer_op(
	conflux::detail::small_move_only_function<void()> op) {
	if (ring_fatal_) {
		return;
	}
	pending_ops.push_back(std::move(op));
}

void Ring::cancel_multishot_recv_or_defer(
	int fd,
	std::uint32_t gen) {
	auto const recv_ud = pack(Op::Recv, gen, fd);
	if (!submit_cancel_by_ud(raw_, recv_ud, pack(Op::Nop, 0, 0))) {
		defer_op([this, fd, gen] { cancel_multishot_recv_or_defer(fd, gen); });
	}
}

void Ring::drain_pending_ops() {
	std::size_t remaining = pending_ops.size();
	while (remaining > 0 && !pending_ops.empty()) {
		auto op = std::move(pending_ops.front());
		pending_ops.pop_front();
		--remaining;
		op();
	}
}

void Ring::defer_queue_send_if_current(
	int fd,
	std::uint32_t gen) {
	defer_op([this, fd, gen] {
		auto const ufd = static_cast<std::size_t>(fd);
		if (ufd < fd_table.size() && fd_table[ufd].gen == gen && fd_table[ufd].fd >= 0) {
			start_response_send(fd, fd_table[ufd]);
		}
	});
}

void Ring::defer_handle_send_complete_if_current(
	int fd,
	std::uint32_t gen) {
	defer_op([this, fd, gen] {
		auto const ufd = static_cast<std::size_t>(fd);
		if (ufd < fd_table.size() && fd_table[ufd].gen == gen && fd_table[ufd].fd >= 0) {
			handle_send_complete(fd, fd_table[ufd]);
		}
	});
}

void Ring::defer_start_streamed_body_if_current(
	int fd,
	std::uint32_t gen) {
	defer_op([this, fd, gen] {
		auto const ufd = static_cast<std::size_t>(fd);
		if (ufd < fd_table.size() && fd_table[ufd].gen == gen && fd_table[ufd].fd >= 0) {
			start_streamed_body(fd);
		}
	});
}

void Ring::queue_multishot_accept() {
	client_addr_len = sizeof(client_addr);
	// Direct accepted sockets cannot be queried with getpeername(), so keep
	// one in-flight accept owner for the shared peer-address buffer.
	bool const submitted = accepted_sockets_direct ? submit_accept_direct_borrowed(
														 raw_,
														 DirectFd::from_direct(static_cast<std::uint32_t>(listen_fd)),
														 reinterpret_cast<sockaddr *>(&client_addr),
														 &client_addr_len,
														 pack(Op::Accept, 0, listen_fd),
														 0,
														 IORING_FILE_INDEX_ALLOC) :
						   listen_fixed ? submit_accept_multishot_borrowed(
											  raw_,
											  DirectFd::from_direct(static_cast<std::uint32_t>(listen_fd)),
											  reinterpret_cast<sockaddr *>(&client_addr),
											  &client_addr_len,
											  pack(Op::Accept, 0, listen_fd),
											  caps,
											  false) :
										  submit_accept_multishot_borrowed(
											  raw_,
											  OsFd::from_os(listen_fd),
											  reinterpret_cast<sockaddr *>(&client_addr),
											  &client_addr_len,
											  pack(Op::Accept, 0, listen_fd),
											  caps,
											  false);
	if (!submitted) {
		defer_op([this] { queue_multishot_accept(); });
	}
}

[[nodiscard]] RecvArmPolicy Ring::resolve_recv_arm_policy(
	Conn const &conn) const noexcept {
	return ::resolve_recv_arm_policy(
		auto_recv_arm_policy,
		caps.recv_poll_first,
		conn.have_last_recv_cqe_flags,
		conn.last_recv_cqe_flags);
}

void Ring::queue_multishot_recv(
	int fd) {
	auto &conn = conn_for(fd);
	auto const arm = resolve_recv_arm_policy(conn);
	bool const submitted = accepted_sockets_direct ? submit_recv_multishot(
														 raw_,
														 DirectFd::from_direct(static_cast<std::uint32_t>(fd)),
														 *buf_ring_,
														 pack(Op::Recv, conn.gen, fd),
														 use_recv_bundle,
														 arm) :
													 submit_recv_multishot(
														 raw_,
														 OsFd::from_os(fd),
														 *buf_ring_,
														 pack(Op::Recv, conn.gen, fd),
														 use_recv_bundle,
														 arm);
	if (!submitted) {
		defer_op([this, fd] { queue_multishot_recv(fd); });
		return;
	}
	conn.recv_armed = true;
}

void Ring::queue_direct_accept_setup(
	int fd) {
	auto &conn = conn_for(fd);
	auto handle = DirectFd::from_direct(static_cast<std::uint32_t>(fd));
	DirectTcpAcceptSetup setup{};
	bool const cmd_sock_opts = cmd_sock_setsockopt_enabled_ && caps.cmd_sock_setsockopt;
	setup.tcp_nodelay_once = cmd_sock_opts;
	setup.tcp_quickack_once = cmd_sock_opts;
	setup.prefer_busy_poll_once = prefer_busy_poll_ && cmd_sock_opts;
	setup.busy_poll_us_optval = busy_poll_us_ > 0 && cmd_sock_opts ? &busy_poll_us_ : nullptr;
	setup.recv_bundle = use_recv_bundle;
	setup.recv_arm_policy = resolve_recv_arm_policy(conn);
	setup.skip_sockopt_success_cqes = true;
	if (!submit_direct_tcp_accept_setup_recv(
			raw_,
			handle,
			*buf_ring_,
			pack(Op::Nop, 0, 0),
			pack(Op::Recv, conn.gen, fd),
			setup)) {
		defer_op([this, fd] { queue_direct_accept_setup(fd); });
		return;
	}
	conn.recv_armed = true;
}

void Ring::queue_sse_wait(
	int fd) {
	auto &conn = conn_for(fd);
	auto *sqe = get_sqe();
	if (sqe == nullptr) {
		defer_op([this, fd] { queue_sse_wait(fd); });
		return;
	}
	auto const ud = pack(Op::SsePoll, conn.gen, fd);
	auto buf = std::make_unique<std::uint64_t>(0);
	// Blocking read on the eventfd — io_uring uses io-wq when the fd
	// is not immediately readable.  Offset 0 is ignored for eventfd.
	conflux::uring::Sqe{sqe}
		.prep_read(conflux::uring::SqeFd{conn.sse_efd}, buf.get(), sizeof(std::uint64_t), 0)
		.user_data(conflux::uring::UserData{ud});
	in_flight_read_bufs[ud] = std::move(buf);
}

void Ring::queue_deferred_wait(
	int fd) {
	auto &conn = conn_for(fd);
	queue_deferred_wait(fd, conn.deferred_efd, conn.deferred_response);
}

void Ring::arm_shutdown_read() {
	auto *sqe = get_sqe();
	if (sqe == nullptr) {
		defer_op([this] { arm_shutdown_read(); });
		return;
	}
	conflux::uring::Sqe{sqe}
		.prep_read(conflux::uring::SqeFd{shutdown_efd}, &shutdown_buf, sizeof(shutdown_buf), 0)
		.user_data(conflux::uring::UserData{pack(Op::Shutdown, 0, 0)});
}

// Called once a response (or chunk) has been fully delivered.
// Drives SSE/WS/normal post-send state machine.
conflux::http::RunStatus Ring::run_loop() {
	static constexpr unsigned BATCH = 256;
	if (ring_core_ >= 0) {
		cpu_set_t cs;
		CPU_ZERO(&cs);
		CPU_SET(static_cast<unsigned>(ring_core_), &cs);
		if (::sched_setaffinity(0, sizeof(cs), &cs) < 0) {
			conflux::utils::eprintln(
				std::format("run_loop: sched_setaffinity ring_core={} failed errno={}", ring_core_, errno));
		}
	}
	if (worker_core_ >= 0 && ring.ring_fd >= 0) {
		cpu_set_t cs;
		CPU_ZERO(&cs);
		CPU_SET(static_cast<unsigned>(worker_core_), &cs);
		auto const rc = ::io_uring_register(
			static_cast<unsigned>(ring.ring_fd),
			static_cast<unsigned>(IORING_REGISTER_IOWQ_AFF),
			&cs,
			static_cast<unsigned>(sizeof(cs)));
		if (rc < 0) {
			conflux::utils::eprintln(
				std::format("run_loop: IORING_REGISTER_IOWQ_AFF worker_core={} failed rc={}", worker_core_, rc));
		}
	}
	conflux::file_io::CurrentFileReaderScope const file_reader_scope{files.get()};

	queue_multishot_accept();
	arm_shutdown_read();
	arm_timer();
	auto _ = raw_.submit();

	for (;;) {
		if (ring_integrity_suspect()) {
			note_cq_overflow();
			if (!caps.feat_nodrop) {
				enter_ring_fatal(ServerFatalReason::cq_overflow_no_nodrop);
				emit_ring_diagnostics();
				close_tracked_fds_sync();
				return conflux::http::RunStatus::fatal_cq_overflow_no_nodrop;
			}
			// NODROP: overflow list non-empty but CQEs are not lost.
			// io_uring_submit_and_wait drains overflow into the ring; continue.
		} else {
			try_grow_cq_after_overflow();
		}

		drain_pending_ops();

		int const rc = io_uring_submit_and_wait(&ring, 1);
		if (rc < 0) {
			if (rc == -EINTR) {
				continue;
			}
			if (rc == -EBADR) {
				enter_ring_fatal(ServerFatalReason::submit_wait_ebadr);
				flush_overflow_cqes_until_clear_or_limit();
				return conflux::http::RunStatus::fatal_submit_wait_ebadr;
			}
			if (ring_integrity_suspect() && !caps.feat_nodrop) {
				enter_ring_fatal(ServerFatalReason::cq_overflow_no_nodrop);
				emit_ring_diagnostics();
				close_tracked_fds_sync();
				return conflux::http::RunStatus::fatal_cq_overflow_no_nodrop;
			}
			continue;
		}

		std::array<io_uring_cqe *, BATCH> cqes{};
		unsigned const count = io_uring_peek_batch_cqe(&ring, cqes.data(), BATCH);

		bool const overflowed = ring_integrity_suspect();
		if (overflowed) {
			note_cq_overflow();
		}

		if (count == 0) {
			if (overflowed && !caps.feat_nodrop) {
				enter_ring_fatal(ServerFatalReason::cq_overflow_no_nodrop);
				emit_ring_diagnostics();
				close_tracked_fds_sync();
				return conflux::http::RunStatus::fatal_cq_overflow_no_nodrop;
			}
			continue;
		}

		recvs.clear();

		for (unsigned i = 0; i < count; ++i) {
			// NOLINT(cppcoreguidelines-pro-bounds-constant-A-index): runtime batch index
			auto [op, cqe_gen, fd] =
				unpack(io_uring_cqe_get_data64(cqes[i])); // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			auto *cqe = cqes[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			dispatch_cqe(op, fd, cqe->res, conflux::uring::CqeFlags{cqe->flags}, cqe_gen);
		}
		io_uring_cq_advance(&ring, count);

		phase1_copy_recv_bufs();
		finish_ready_ws_handoffs();
		phase1b_process();
		phase2_build_responses();
		phase3_dispatch();

		if (shutting_down) {
			bool const any_open = std::ranges::any_of(fd_table, [](Conn const &c) { return c.fd >= 0; });
			if (!any_open) {
				if (file_completions && !file_completions->cancel_all()) {
					continue;
				}
				return conflux::http::RunStatus::stopped_normally;
			}
		}
	}
}
