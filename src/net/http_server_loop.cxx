module;
#include <cstddef> // must precede openssl/nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" blocks re-include c++config.h

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
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
import conflux.net.router;
import conflux.file_map;
import conflux.net.direct_slot_pool;
import conflux.net.vhost;
import conflux.net.config;
import conflux.net.http1_parser;
import conflux.net.http_server_helpers;
import conflux.net.http_server_config;
import conflux.uring;
import conflux.uring.completion;
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

#if CONFLUX_HTTP_TRACE
	#define HTTP_TRACE(MSG) eprintln(std::format("http_trace {}", (MSG)))
#else
	#define HTTP_TRACE(MSG) ((void)0)
#endif

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

[[nodiscard]] HttpResponse Ring::dispatch(
	HttpRequestView const &req) const {
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

[[nodiscard]] std::optional<HttpResponse> Ring::try_dispatch_context(
	HttpRequestView const &req) const {
	if (!client_task_ring_) {
		return std::nullopt;
	}
	if (!has_context_routes()) {
		return std::nullopt;
	}
	RequestContext const ctx{*client_task_ring_};
	HttpRequest const owned = req.to_owned();
	if (vhost_router != nullptr) {
		return vhost_router->dispatch_context(owned, ctx);
	}
	return router->dispatch_context(owned, ctx);
}

[[nodiscard]] std::shared_ptr<WorkPool> Ring::resolve_ws_work_pool(
	HttpRequestView const &req) const {
	if (vhost_router != nullptr) {
		return vhost_router->resolved_work_pool(req.headers["host"]);
	}
	return router->work_pool();
}

void Ring::clear_deferred_wait(
	int deferred_efd) {
	if (deferred_efd >= 0) {
		deferred_waits.erase(deferred_efd);
	}
}

void Ring::queue_deferred_wait(
	int conn_fd,
	int deferred_efd,
	std::shared_ptr<DeferredResponse> response,
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
	io_uring_prep_read(sqe, deferred_efd, buf.get(), sizeof(std::uint64_t), 0);
	io_uring_sqe_set_data64(sqe, ud);
	in_flight_read_bufs[ud] = std::move(buf);
}

// Must be called from the std::thread that will run run_loop() (SINGLE_ISSUER).
// `wq_fd`: when non-zero, sets IORING_SETUP_ATTACH_WQ so this ring shares
// the parent ring's kernel io-wq. Pass ring[0].ring.ring_fd for rings 1..N.
void Ring::init(
	std::uint16_t port,
	unsigned entries,
	std::uint32_t uring_flags,
	std::uint32_t wq_fd,
	bool no_mmap) {
	io_uring_params params{};
	params.flags = uring_flags;
	if (wq_fd != 0) {
		params.flags |= IORING_SETUP_ATTACH_WQ;
		params.wq_fd = wq_fd;
	}
	requested_setup_flags_ = params.flags;
	active_setup_flags_ = 0;
	stripped_setup_flags_ = 0;
	if (no_mmap) {
		ssize_t const sz = io_uring_mlock_size(entries, params.flags);
		if (sz <= 0) {
			throw std::runtime_error{"io_uring_mlock_size failed"};
		}
		auto *raw = static_cast<std::byte *>(
			::aligned_alloc(static_cast<std::size_t>(sysconf(_SC_PAGESIZE)), static_cast<std::size_t>(sz)));
		if (raw == nullptr) {
			throw std::bad_alloc{};
		}
		ring_mem = {raw, ::free};
		if (io_uring_queue_init_mem(entries, &ring, &params, raw, static_cast<std::size_t>(sz)) < 0) {
			throw std::runtime_error{"io_uring_queue_init_mem failed"};
		}
		active_setup_flags_ = params.flags;
	} else {
		for (;;) {
			int const rc = ::io_uring_queue_init_params(entries, &ring, &params);
			if (rc == 0) {
				active_setup_flags_ = params.flags;
				break;
			}
			if (rc != -EINVAL) {
				throw std::runtime_error{std::format("io_uring_queue_init_params: {}", strerror(-rc))};
			}
			auto const stripped = next_uring_setup_flag_to_strip(params.flags);
			if (!stripped) {
				throw std::runtime_error{"io_uring_queue_init_params: no supported flag combination"};
			}
			params.flags &= ~*stripped;
			stripped_setup_flags_ |= *stripped;
		}
	}
	caps = detect_caps(conflux::uring::RingRef{ring});
	use_recv_bundle =
		use_recv_bundle && !use_recv_incremental_buf && caps.recvsend_bundle && CONFLUX_ENABLE_RECV_BUNDLE;
	client_task_ring_.emplace(
		SocketRawRing{ring},
		client_ct_,
		UserDataFn{[](std::uint32_t slot, std::uint32_t gen) noexcept -> std::uint64_t {
			return pack(Op::ClientRing, gen, static_cast<int>(slot));
		}});

	listen_fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0) {
		throw std::system_error{errno, std::system_category(), "socket"};
	}

	int opt = 1;
	int v6only = 0;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
	::setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
	::setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

	sockaddr_in6 addr{};
	addr.sin6_family = AF_INET6;
	addr.sin6_addr = in6addr_any;
	addr.sin6_port = htons(port);

	if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		throw std::system_error{errno, std::system_category(), "bind"};
	}
	if (::listen(listen_fd, SOMAXCONN) < 0) {
		throw std::system_error{errno, std::system_category(), "listen"};
	}

	// Read back actual port (port=0 → OS-assigned). Signal before io_uring
	// setup so callers can discover the port while rings initialise.
	{
		sockaddr_in6 local{};
		socklen_t llen = sizeof(local);
		if (getsockname(listen_fd, reinterpret_cast<sockaddr *>(&local), &llen) == 0) {
			bound_port = ntohs(local.sin6_port);
		}
		if (port_signal != nullptr) {
			port_signal->store(bound_port, std::memory_order_release);
			port_signal->notify_all();
		}
	}

	direct_fds_ = std::make_unique<DirectFdTable>(conflux::uring::RingRef{ring}, MAX_FILES);
	if (direct_fds_->registered() && direct_fds_->install(static_cast<std::uint32_t>(listen_fd), listen_fd)) {
		listen_fixed = true;
		caps.socket_direct_alloc = caps.op_socket && direct_fds_->registered();
		direct_slots_ = std::make_unique<DirectSlotPool>(direct_fds_->capacity());
		if (!direct_slots_->install_os_fd(static_cast<std::uint32_t>(listen_fd), listen_fd)) {
			direct_slots_.reset();
			listen_fixed = false;
			caps.socket_direct_alloc = false;
		}
	}
	accepted_sockets_direct = direct_accept_enabled_ && listen_fixed && caps.accept_direct_supported;

	// file_io pools: constructed here so register_buffers_sparse runs before
	// buf_ring setup (both touch io_uring internal state; ordering is
	// defensive — buf_ring uses a separate bgid). Install FileReader only
	// when both streaming paths have usable resources; otherwise serve_static
	// falls back to the mmap path instead of selecting an async response that
	// cannot deliver its body.
	if (file_io_slabs > 0 && file_io_pipe_pairs > 0) {
		auto const total_buf_slots =
			static_cast<unsigned>(file_io_slabs + (send_fixed_buffers_enabled ? send_buffer_slabs : std::size_t{0}));
		auto table = std::make_unique<RegisteredBufferTable>(&ring, total_buf_slots);
		if (table->ok()) {
			auto file_pool = std::make_unique<FixedBufferPool>(table.get(), 0, file_io_slabs, file_io_slab_bytes);
			auto pipes = std::make_unique<PipePool>(file_io_pipe_pairs);
			if (file_pool->ok() && file_pool->capacity() > 0 && pipes->capacity() > 0) {
				file_completions = std::make_unique<CompletionTable>();
				buf_table = std::move(table);
				fixed_buffers = std::move(file_pool);
				splice_pipes = std::move(pipes);
				files = std::make_unique<FileReader>(
					&ring,
					file_completions.get(),
					[](std::uint32_t slot, std::uint32_t gen) noexcept {
						return pack(Op::FileIo, gen, static_cast<int>(slot));
					});
				if (send_fixed_buffers_enabled && send_buffer_slabs > 0) {
					auto sp = std::make_unique<FixedBufferPool>(
						buf_table.get(),
						static_cast<unsigned>(file_io_slabs),
						send_buffer_slabs,
						send_buffer_bytes);
					if (sp->ok() && sp->capacity() > 0) {
						send_buffers = std::move(sp);
						send_fixed_buffers_supported = true;
					}
				}
			}
		}
	}

	buf_ring_ = std::make_unique<BufferRing>(
		conflux::uring::RingRef{ring},
		BufferRingOptions{
			.count = entries * 4,
			.buf_size = BUF_SIZE,
			.group_id = 0,
			.huge_pages = true,
			.mode = use_recv_incremental_buf ? BufferRingMode::incremental :
					use_recv_bundle          ? BufferRingMode::recv_bundle :
											   BufferRingMode::classic_one_cqe_per_buffer,
		},
		caps);

	fd_table.reserve(FD_TABLE_RESERVE);
	recvs.reserve(entries);
}

Conn &Ring::conn_for(
	int fd) {
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size()) {
		if (ufd >= 1000000U) [[unlikely]] {
			::close(fd);
			thread_local Conn dead{};
			dead = Conn{};
			return dead;
		}
		fd_table.resize(ufd + 1);
	}
	return fd_table[ufd];
}

void Ring::conn_erase(
	int fd,
	std::uint32_t gen) {
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size()) {
		return;
	}
	auto &conn = fd_table[ufd];
	if (conn.gen != gen) {
		return;
	}
	if (conn.is_sse && conn.sse_channel) {
		conn.sse_channel->close(); // notify handler std::thread
	}
	retire_incremental_partial(fd, gen, conn);
	++conn.gen; // prevent a second Close CQE from erasing the next tenant
	conn.fd = -1;
	conn.recv_armed = false;
	conn.last_recv_cqe_flags = 0;
	conn.have_last_recv_cqe_flags = false;
	conn.closing = false;
	conn.close_after_send = false;
	conn.is_sse = false;
	conn.sse_headers_sent = false;
	conn.is_ws = false;
	conn.is_deferred = false;
	conn.sse_efd = -1;
	conn.sse_channel.reset();
	clear_deferred_wait(conn.deferred_efd);
	conn.deferred_efd = -1;
	conn.deferred_response.reset();
	conn.ws_upgrade.reset();
	conn.partial.clear();
	conn.chunked_decode.reset();
	conn.mapped_file.reset();
	conn.mapped_total = 0;
	conn.mapped_delivered = 0;
	conn.zc_state.waiting_notification = false;
	conn.zc_state.after_notification = SendZcPendingAction::none;
	conn.zc_state.close_after_notification = false;
	conn.zc_tls_bypass_counted = false;
	conn.send_buf = FixedBuffer{};
	conn.send_buf_base_written = 0;
	conn.send_buf_len = 0;
	conn.is_tls = false;
#if CONFLUX_HAS_TLS
	if (conn.ssl != nullptr) {
		conn.ssl.reset();
	}
	conn.tls_rx_cipher.clear();
	conn.tls_send_pending.clear();
	conn.tls_send_inflight.clear();
	conn.tls_send_off = 0;
	conn.tls_hs_done = false;
	conn.tls_sending_response = false;
	conn.tls_shutdown_after_send = false;
	conn.tls_wait_peer_shutdown = false;
#endif
#if CONFLUX_HAS_HTTP2
	if (conn.h2_session != nullptr) {
		nghttp2_session_del(conn.h2_session);
		conn.h2_session = nullptr;
	}
	conn.h2_ctx.reset();
	for (auto const &[_, stream]: conn.h2_streams) {
		clear_deferred_wait(stream.deferred_efd);
	}
	conn.h2_streams.clear();
	conn.h2_pending_send.clear();
	conn.is_h2 = false;
	conn.h2_sse_stream_id = -1;
	conn.h2_sse_pending_wait = false;
#endif
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
	conflux::work::root::detail::small_move_only_function<void()> op) {
	if (ring_fatal_) {
		return;
	}
	pending_ops.push_back(std::move(op));
}

void Ring::cancel_multishot_recv_or_defer(
	SocketHandle handle) {
	if (!submit_cancel_multishot_recv(raw_, handle, pack(Op::Nop, 0, 0))) {
		defer_op([this, handle] { cancel_multishot_recv_or_defer(handle); });
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
	auto listen_handle = listen_fixed ? SocketHandle::from_direct(static_cast<std::uint32_t>(listen_fd)) :
										SocketHandle::from_os(listen_fd);
	if (!submit_accept_multishot_borrowed(
			raw_,
			listen_handle,
			reinterpret_cast<sockaddr *>(&client_addr),
			&client_addr_len,
			pack(Op::Accept, 0, listen_fd),
			caps,
			accepted_sockets_direct)) {
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
	auto handle =
		accepted_sockets_direct ? SocketHandle::from_direct(static_cast<std::uint32_t>(fd)) : SocketHandle::from_os(fd);
	auto const arm = resolve_recv_arm_policy(conn);
	if (!submit_recv_multishot(raw_, handle, *buf_ring_, pack(Op::Recv, conn.gen, fd), use_recv_bundle, arm)) {
		defer_op([this, fd] { queue_multishot_recv(fd); });
		return;
	}
	conn.recv_armed = true;
}

void Ring::queue_direct_accept_setup(
	int fd) {
	auto &conn = conn_for(fd);
	auto handle = SocketHandle::from_direct(static_cast<std::uint32_t>(fd));
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

void Ring::invalidate_recv_if_armed(
	int fd) {
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size()) {
		return;
	}
	auto &conn = fd_table[ufd];
	if (conn.fd < 0 || !conn.recv_armed) {
		return;
	}
	std::uint32_t const old_gen = conn.gen;
	retire_incremental_partial(fd, old_gen, conn);
	++conn.gen;
	conn.recv_armed = false;
	auto handle = accepted_sockets_direct ? SocketHandle::from_direct(static_cast<std::uint32_t>(fd)) :
											SocketHandle::from_os(conn.fd);
	cancel_multishot_recv_or_defer(handle);
}

void Ring::cancel_accept_or_defer() {
	if (!submit_cancel_by_ud(raw_, pack(Op::Accept, 0, listen_fd), 0)) {
		defer_op([this] { cancel_accept_or_defer(); });
	}
}

void Ring::submit_conn_close_or_defer(
	int fd,
	std::uint32_t gen) {
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != gen) {
		return;
	}
	auto handle =
		accepted_sockets_direct ? SocketHandle::from_direct(static_cast<std::uint32_t>(fd)) : SocketHandle::from_os(fd);
	bool const submitted = accepted_sockets_direct ? submit_close_fast(
														 raw_,
														 handle,
														 pack(Op::Nop, 0, 0),
														 pack(Op::Close, gen, fd),
														 SocketCloseOptions{
															 .shutdown_write = true,
															 .skip_shutdown_success_cqe = true,
															 .allow_async_shutdown_for_os_fd = false,
														 }) :
													 submit_close(raw_, handle, pack(Op::Close, gen, fd));
	if (!submitted) {
		HTTP_TRACE(std::format("conn_close_defer fd={} gen={} direct={}", fd, gen, accepted_sockets_direct));
		defer_op([this, fd, gen] { submit_conn_close_or_defer(fd, gen); });
		return;
	}
	HTTP_TRACE(std::format("conn_close_queued fd={} gen={} direct={}", fd, gen, accepted_sockets_direct));
	fd_table[ufd].closing = true;
	if (direct_slots_ && accepted_sockets_direct) {
		if (!direct_slots_->mark_closing(static_cast<std::uint32_t>(fd))) {
			eprintln(std::format("submit_conn_close_or_defer: mark_closing failed slot={}", fd));
		}
	}
}

void Ring::submit_fd_shutdown_or_defer(
	int fd,
	std::uint32_t gen) {
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != gen) {
		return;
	}
	auto handle =
		accepted_sockets_direct ? SocketHandle::from_direct(static_cast<std::uint32_t>(fd)) : SocketHandle::from_os(fd);
	if (!submit_shutdown(raw_, handle, SHUT_WR, pack(Op::FdShutdown, gen, fd))) {
		HTTP_TRACE(std::format("fd_shutdown_defer fd={} gen={} direct={}", fd, gen, accepted_sockets_direct));
		defer_op([this, fd, gen] { submit_fd_shutdown_or_defer(fd, gen); });
		return;
	}
	HTTP_TRACE(std::format("fd_shutdown_queued fd={} gen={} direct={}", fd, gen, accepted_sockets_direct));
}

void Ring::handle_fd_shutdown(
	int fd,
	int res,
	std::uint32_t gen) {
	HTTP_TRACE(
		std::format(
			"fd_shutdown fd={} res={} gen={} direct={} mode={}",
			fd,
			res,
			gen,
			accepted_sockets_direct,
			buffer_ring_mode_name(buf_ring_->mode())));
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != gen) {
		return;
	}
	submit_conn_close_or_defer(fd, gen);
	(void)res;
}

void Ring::queue_close(
	int fd) {
	auto const ufd = static_cast<std::size_t>(fd);
	auto const gen = (ufd < fd_table.size()) ? fd_table[ufd].gen : std::uint32_t{0};
	HTTP_TRACE(
		std::format(
			"queue_close fd={} gen={} direct={} closing={} recv_armed={} zc_waiting={} mode={}",
			fd,
			gen,
			accepted_sockets_direct,
			ufd < fd_table.size() ? fd_table[ufd].closing : false,
			ufd < fd_table.size() ? fd_table[ufd].recv_armed : false,
			ufd < fd_table.size() ? fd_table[ufd].zc_state.waiting_notification : false,
			buffer_ring_mode_name(buf_ring_->mode())));
	if (ufd < fd_table.size()) {
		if (fd_table[ufd].closing) {
			return;
		}
		if (fd_table[ufd].zc_state.waiting_notification) {
			fd_table[ufd].zc_state.close_after_notification = true;
			fd_table[ufd].closing = true;
			invalidate_recv_if_armed(fd);
			return;
		}
	}

	if (accepted_sockets_direct) {
		invalidate_recv_if_armed(fd);
		auto const direct_gen = (ufd < fd_table.size()) ? fd_table[ufd].gen : std::uint32_t{0};
		if (ufd < fd_table.size()) {
			if (fd_table[ufd].gen != direct_gen || fd_table[ufd].closing) {
				return;
			}
			fd_table[ufd].closing = true;
		}
		submit_conn_close_or_defer(fd, direct_gen);
		return;
	}

	invalidate_recv_if_armed(fd);
	auto const close_gen = (ufd < fd_table.size()) ? fd_table[ufd].gen : gen;
	if (ufd < fd_table.size()) {
		if (fd_table[ufd].gen != close_gen || fd_table[ufd].closing) {
			return;
		}
		fd_table[ufd].closing = true;
	}
	submit_fd_shutdown_or_defer(fd, close_gen);
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
	io_uring_prep_read(sqe, conn.sse_efd, buf.get(), sizeof(std::uint64_t), 0);
	io_uring_sqe_set_data64(sqe, ud);
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
	io_uring_prep_read(sqe, shutdown_efd, &shutdown_buf, sizeof(shutdown_buf), 0);
	io_uring_sqe_set_data64(sqe, pack(Op::Shutdown, 0, 0));
}

// Arm a one-shot periodic timer that fires every ~1 second for connection reaping.
void Ring::arm_timer() {
	if (shutting_down) {
		bool pending_force_close = false;
		for (auto const &conn: fd_table) {
			if (conn.fd >= 0 && conn.send_queued && conn.close_after_send) {
				pending_force_close = true;
				break;
			}
		}
		if (!pending_force_close && request_timeout_ms == 0 && tls_sniff_timeout_ms == 0) {
			return;
		}
	} else if (request_timeout_ms == 0 && tls_sniff_timeout_ms == 0) {
		return;
	}
	timer_ts.tv_sec = 1;
	timer_ts.tv_nsec = 0;
	if (!submit_timeout_borrowed(raw_, &timer_ts, pack(Op::Timer, 0, 0))) {
		defer_op([this] { arm_timer(); });
	}
}

void Ring::handle_timer() {
	if (request_timeout_ms == 0 && tls_sniff_timeout_ms == 0) {
		return;
	}
	auto now = std::chrono::steady_clock::now();
	auto req_limit = std::chrono::milliseconds{request_timeout_ms};
	auto sniff_limit = std::chrono::milliseconds{tls_sniff_timeout_ms};
	for (auto &conn: fd_table) {
		if (conn.fd < 0) {
			continue;
		}
		if (shutting_down && conn.send_queued && conn.close_after_send && now >= conn.close_after_send_deadline) {
			queue_close(conn.fd);
			continue;
		}
		if (conn.is_sse) {
			continue;
		} // SSE streams are exempt
		if (conn.is_deferred) {
			// Deferred responses self-expire: expire_if_past_deadline forces a 504 and
			// wakes the eventfd that the deferred-poll SQE is watching.
			if (conn.deferred_response) {
				conn.deferred_response->expire_if_past_deadline(now);
			}
			continue;
		}
		if (conn.send_queued) {
			continue;
		} // mid-send: handler already responded
		// TLS sniff-undecided sentinel: ssl==nullptr && tls_hs_done==true && partial empty.
		// Use the (usually shorter) sniff timeout to reap silent connections that opened
		// the TCP socket but never sent a std::byte.
		bool const sniff_undecided = conn.ssl == nullptr && conn.tls_hs_done && conn.partial.empty();
		if (sniff_undecided && tls_sniff_timeout_ms != 0) {
			if (now - conn.last_activity > sniff_limit) {
				queue_close(conn.fd);
			}
			continue;
		}
		if (request_timeout_ms != 0) {
			auto const ref = conn.request_in_progress ? conn.request_started : conn.last_activity;
			if (now - ref > req_limit) {
				queue_close(conn.fd);
			}
		}
	}
	arm_timer(); // re-arm for next tick
}

void Ring::handle_shutdown() {
	shutting_down = true;
	cancel_accept_or_defer();
	auto const now = std::chrono::steady_clock::now();
	for (std::size_t i = 0; i < fd_table.size(); ++i) {
		auto &conn = fd_table[i];
		if (conn.fd < 0) {
			continue;
		}
		if (conn.sse_channel) {
			conn.sse_channel->close();
		}
		if (conn.send_queued) {
			conn.close_after_send = true;
			conn.close_after_send_deadline = now + shutdown_close_after_send_timeout;
			if (conn.recv_armed) {
				auto handle = accepted_sockets_direct ? SocketHandle::from_direct(static_cast<std::uint32_t>(i)) :
														SocketHandle::from_os(conn.fd);
				cancel_multishot_recv_or_defer(handle);
			}
		} else {
			queue_close(static_cast<int>(i));
		}
	}
	arm_timer();
}

void Ring::handle_accept(
	int res,
	std::uint32_t flg) {
	if (res < 0) {
		HTTP_TRACE(
			std::format(
				"accept_err res={} direct={} recv_bundle={} mode={} more={}",
				res,
				accepted_sockets_direct,
				use_recv_bundle,
				buffer_ring_mode_name(buf_ring_->mode()),
				cqe_has_more(flg)));
		if (!shutting_down) {
			queue_multishot_accept();
		}
		return;
	}
	HTTP_TRACE(
		std::format(
			"accept fd={} direct={} recv_bundle={} mode={} more={}",
			res,
			accepted_sockets_direct,
			use_recv_bundle,
			buffer_ring_mode_name(buf_ring_->mode()),
			cqe_has_more(flg)));
	if (accepted_sockets_direct && direct_slots_) {
		if (!direct_slots_->adopt_kernel_allocated(static_cast<std::uint32_t>(res))) {
			++accepted_direct_failures_;
			eprintln(std::format("handle_accept: adopt_kernel_allocated failed slot={} — stopping direct accept", res));
			accepted_sockets_direct = false;
			submit_cancel_by_ud(raw_, pack(Op::Accept, 0, listen_fd), 0);
			auto const ud = pack(Op::DirectSlotClose, 0, res);
			if (!submit_close(raw_, SocketHandle::from_direct(static_cast<std::uint32_t>(res)), ud)) {
				defer_op([this, res, ud] {
					submit_close(raw_, SocketHandle::from_direct(static_cast<std::uint32_t>(res)), ud);
				});
			}
			return;
		}
	}
	auto &conn = conn_for(res);
	++conn.gen;
	conn.fd = res;
	conn.recv_armed = false;
	conn.last_recv_cqe_flags = 0;
	conn.have_last_recv_cqe_flags = false;
	conn.have_incremental_buf_id = false;
	conn.send_queued = false;
	conn.closing = false;
	conn.close_after_send = false;
	conn.has_response = false;
	conn.written = 0;
	conn.is_sse = false;
	conn.sse_headers_sent = false;
	conn.is_deferred = false;
	conn.sse_efd = -1;
	conn.sse_channel.reset();
	conn.deferred_efd = -1;
	conn.deferred_response.reset();
	conn.ws_upgrade.reset();
	conn.partial.clear();
	conn.chunked_decode.reset();
	conn.mapped_file.reset();
	conn.mapped_total = 0;
	conn.mapped_delivered = 0;
	conn.last_activity = std::chrono::steady_clock::now();
	if (!accepted_sockets_direct) {
		sockaddr_in6 peer_addr{};
		socklen_t peer_len = sizeof(peer_addr);
		if (::getpeername(res, reinterpret_cast<sockaddr *>(&peer_addr), &peer_len) == 0) {
			conn.remote_addr = ip_to_string(peer_addr.sin6_addr);
		} else {
			conn.remote_addr.clear();
		}
	} else {
		conn.remote_addr = ip_to_string(client_addr.sin6_addr);
	}
	conn.is_tls = false;
#if CONFLUX_HAS_TLS
	// Free any SSL left by a prior tenant on this fd slot.
	if (conn.ssl != nullptr) {
		conn.ssl.reset();
	}
	conn.tls_rx_cipher.clear();
	conn.tls_send_pending.clear();
	conn.tls_send_inflight.clear();
	conn.tls_send_off = 0;
	// Sentinel: ssl==nullptr && tls_hs_done==true means "waiting for first std::byte".
	// SSL_new() is deferred to phase1_copy_recv_bufs after the first-std::byte sniff.
	// ssl_ctx==nullptr (plain-only server): tls_hs_done stays false — no sniff needed.
	conn.tls_hs_done = (ssl_ctx != nullptr);
	conn.tls_sending_response = false;
	conn.tls_shutdown_after_send = false;
	conn.tls_wait_peer_shutdown = false;
#endif
#if CONFLUX_HAS_HTTP2
	if (conn.h2_session != nullptr) {
		nghttp2_session_del(conn.h2_session);
		conn.h2_session = nullptr;
	}
	conn.h2_ctx.reset();
	conn.h2_streams.clear();
	conn.h2_pending_send.clear();
	conn.is_h2 = false;
	conn.h2_sse_stream_id = -1;
	conn.h2_sse_pending_wait = false;
#endif
	if (!accepted_sockets_direct) {
		::setsockopt(res, IPPROTO_TCP, TCP_NODELAY, &tcp_opt_one_, sizeof tcp_opt_one_);
		::setsockopt(res, IPPROTO_TCP, TCP_QUICKACK, &tcp_opt_one_, sizeof tcp_opt_one_);
		if (busy_poll_us_ > 0) {
			::setsockopt(res, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us_, sizeof busy_poll_us_);
		}
		if (prefer_busy_poll_) {
			::setsockopt(res, SOL_SOCKET, SO_PREFER_BUSY_POLL, &tcp_opt_one_, sizeof tcp_opt_one_);
		}
		queue_multishot_recv(res);
	} else {
		queue_direct_accept_setup(res);
	}
	if (!cqe_has_more(flg)) {
		queue_multishot_accept();
	}
}

// Called once a response (or chunk) has been fully delivered.
// Drives SSE/WS/normal post-send state machine.
RunStatus Ring::run_loop() {
	static constexpr unsigned BATCH = 256;
	if (ring_core_ >= 0) {
		cpu_set_t cs;
		CPU_ZERO(&cs);
		CPU_SET(static_cast<unsigned>(ring_core_), &cs);
		if (::sched_setaffinity(0, sizeof(cs), &cs) < 0) {
			eprintln(std::format("run_loop: sched_setaffinity ring_core={} failed errno={}", ring_core_, errno));
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
			eprintln(std::format("run_loop: IORING_REGISTER_IOWQ_AFF worker_core={} failed rc={}", worker_core_, rc));
		}
	}
	CurrentFileReaderScope const file_reader_scope{files.get()};

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
				return RunStatus::fatal_cq_overflow_no_nodrop;
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
				return RunStatus::fatal_submit_wait_ebadr;
			}
			if (ring_integrity_suspect() && !caps.feat_nodrop) {
				enter_ring_fatal(ServerFatalReason::cq_overflow_no_nodrop);
				emit_ring_diagnostics();
				close_tracked_fds_sync();
				return RunStatus::fatal_cq_overflow_no_nodrop;
			}
			continue;
		}

		std::array<io_uring_cqe *, BATCH> cqes{};
		unsigned const count = io_uring_peek_batch_cqe(&ring, cqes.data(), BATCH);

		bool const overflowed = ring_integrity_suspect();
		if (overflowed) {
			note_cq_overflow();
		} else {
			try_grow_cq_after_overflow();
		}

		if (count == 0) {
			if (overflowed && !caps.feat_nodrop) {
				enter_ring_fatal(ServerFatalReason::cq_overflow_no_nodrop);
				emit_ring_diagnostics();
				close_tracked_fds_sync();
				return RunStatus::fatal_cq_overflow_no_nodrop;
			}
			continue;
		}

		recvs.clear();

		for (unsigned i = 0; i < count; ++i) {
			// NOLINT(cppcoreguidelines-pro-bounds-constant-A-index): runtime batch index
			auto [op, cqe_gen, fd] =
				unpack(io_uring_cqe_get_data64(cqes[i])); // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			auto *cqe = cqes[i]; // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			dispatch_cqe(op, fd, cqe->res, cqe->flags, cqe_gen);
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
				return RunStatus::stopped_normally;
			}
		}
	}
}
