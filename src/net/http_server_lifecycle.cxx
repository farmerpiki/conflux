module;
#include <cstddef> // must precede openssl/nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" blocks re-include c++config.h

#include <liburing.h>
#if CONFLUX_HAS_TLS
	#include <openssl/ssl.h>
#endif
#if CONFLUX_HAS_HTTP2
	#include <nghttp2/nghttp2.h>
#endif
#include <sys/socket.h>
#include <unistd.h>

module conflux.net.http_server:lifecycle;

import std;
import std.compat;

import conflux.net.detail.direct_slot_pool;
import conflux.net.http.types;
import conflux.net.http_server_helpers;
import conflux.file_io;
import conflux.socket_io;
import conflux.uring;
import conflux.utils;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
import :state;

using namespace conflux::socket_io;
using conflux::uring::DirectFd;
using conflux::uring::OsFd;

#if CONFLUX_HTTP_TRACE
	#define HTTP_TRACE(MSG) conflux::utils::eprintln(std::format("http_trace {}", (MSG)))
#else
	#define HTTP_TRACE(MSG) ((void)0)
#endif

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

bool Ring::conn_uses_direct(
	int fd) const noexcept {
	auto const ufd = static_cast<std::size_t>(fd);
	return ufd < fd_table.size() && fd_table[ufd].accepted_direct;
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
	conn.accepted_direct = false;
	conn.recv_armed = false;
	conn.last_recv_cqe_flags = {};
	conn.have_last_recv_cqe_flags = false;
	conn.closing = false;
	conn.close_after_send = false;
	conn.has_response = false;
	conn.own_response.clear();
	conn.written = 0;
	conn.request_bytes = 0;
	conn.request_in_progress = false;
	conn.request_started = {};
	conn.is_sse = false;
	conn.sse_headers_sent = false;
	conn.is_ws = false;
	conn.is_deferred = false;
	conn.deferred_head_only = false;
	conn.sse_efd = -1;
	conn.sse_channel.reset();
	clear_deferred_wait(conn.deferred_efd);
	conn.deferred_efd = -1;
	if (conn.deferred_response) {
		conn.deferred_response->cancel_disconnect();
	}
	conn.deferred_response.reset();
	conn.deferred_request_storage.reset();
	conn.deferred_request_files.reset();
	conn.ws_upgrade.reset();
	conn.partial.clear();
	conn.chunked_decode.reset();
	conn.mapped_file.reset();
	conn.mapped_total = 0;
	conn.mapped_delivered = 0;
	if (conn.streamed_file) {
		conn.streamed_file->notify_failed();
	}
	conn.streamed_file.reset();
	conn.streamed_headers_sent = false;
	conn.streamed_delivered = 0;
	conn.streamed_splice_in_flight = false;
	conn.zc_state.waiting_notification = false;
	conn.zc_state.after_notification = conflux::http::SendZcPendingAction::none;
	conn.zc_state.close_after_notification = false;
	conn.zc_tls_bypass_counted = false;
	conn.send_buf = conflux::file_io::FixedBuffer{};
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
	conn.h2_closed_streams.clear();
	conn.h2_stream_window_updates.clear();
	conn.h2_max_client_stream_id = 0;
	conn.h2_client_preface_seen = false;
	conn.h2_pending_send.clear();
	conn.is_h2 = false;
	conn.h2_sse_stream_id = -1;
	conn.h2_sse_pending_wait = false;
#endif
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
	cancel_multishot_recv_or_defer(fd, old_gen);
}

void Ring::cancel_accept_or_defer(
	int fd) {
	if (fd < 0) {
		return;
	}
	if (!submit_cancel_by_ud(raw_, pack(Op::Accept, 0, fd), 0)) {
		defer_op([this, fd] { cancel_accept_or_defer(fd); });
	}
}

void Ring::cancel_accept_or_defer() {
	cancel_accept_or_defer(listen_fd);
}

void Ring::submit_direct_slot_close_or_defer(
	int fd) {
	auto const ud = pack(Op::DirectSlotClose, 0, fd);
	if (!submit_close(raw_, DirectFd::from_direct(static_cast<std::uint32_t>(fd)), ud)) {
		defer_op([this, fd] { submit_direct_slot_close_or_defer(fd); });
	}
}

void Ring::submit_os_close_or_defer(
	int fd) {
	auto const ud = pack(Op::Close, 0, fd);
	if (!submit_close(raw_, OsFd::from_os(fd), ud)) {
		defer_op([this, fd] { submit_os_close_or_defer(fd); });
	}
}

void Ring::close_listen_socket() noexcept {
	if (listen_fd < 0) {
		return;
	}
	int const fd = listen_fd;
	if (listen_fixed && direct_fds_) {
		(void)direct_fds_->install(static_cast<std::uint32_t>(fd), -1);
		listen_fixed = false;
	}
	(void)::shutdown(fd, SHUT_RDWR);
	::close(fd);
	listen_fd = -1;
}

void Ring::submit_conn_close_or_defer(
	int fd,
	std::uint32_t gen) {
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != gen) {
		return;
	}
	bool const direct = fd_table[ufd].accepted_direct;
	bool const submitted = direct ? submit_close_fast(
										raw_,
										DirectFd::from_direct(static_cast<std::uint32_t>(fd)),
										pack(Op::Nop, 0, 0),
										pack(Op::Close, gen, fd),
										SocketCloseOptions{
											.shutdown_write = true,
											.skip_shutdown_success_cqe = true,
											.allow_async_shutdown_for_os_fd = false,
										}) :
									submit_close(raw_, OsFd::from_os(fd), pack(Op::Close, gen, fd));
	if (!submitted) {
		HTTP_TRACE(std::format("conn_close_defer fd={} gen={} direct={}", fd, gen, direct));
		defer_op([this, fd, gen] { submit_conn_close_or_defer(fd, gen); });
		return;
	}
	HTTP_TRACE(std::format("conn_close_queued fd={} gen={} direct={}", fd, gen, direct));
	fd_table[ufd].closing = true;
	if (direct_slots_ && direct) {
		if (!direct_slots_->mark_closing(static_cast<std::uint32_t>(fd))) {
			conflux::utils::eprintln(std::format("submit_conn_close_or_defer: mark_closing failed slot={}", fd));
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
	bool const direct = fd_table[ufd].accepted_direct;
	bool const submitted = direct ? submit_shutdown(
										raw_,
										DirectFd::from_direct(static_cast<std::uint32_t>(fd)),
										SHUT_WR,
										pack(Op::FdShutdown, gen, fd)) :
									submit_shutdown(raw_, OsFd::from_os(fd), SHUT_WR, pack(Op::FdShutdown, gen, fd));
	if (!submitted) {
		HTTP_TRACE(std::format("fd_shutdown_defer fd={} gen={} direct={}", fd, gen, direct));
		defer_op([this, fd, gen] { submit_fd_shutdown_or_defer(fd, gen); });
		return;
	}
	HTTP_TRACE(std::format("fd_shutdown_queued fd={} gen={} direct={}", fd, gen, direct));
}

void Ring::handle_fd_shutdown(
	int fd,
	[[maybe_unused]] int res,
	std::uint32_t gen) {
	HTTP_TRACE(
		std::format(
			"fd_shutdown fd={} res={} gen={} direct={} mode={}",
			fd,
			res,
			gen,
			conn_uses_direct(fd),
			buffer_ring_mode_name(buf_ring_->mode())));
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != gen) {
		return;
	}
	if (fd_table[ufd].send_queued) {
		fd_table[ufd].close_after_send = true;
		fd_table[ufd].closing = false;
		return;
	}
	submit_conn_close_or_defer(fd, gen);
}

void Ring::queue_close(
	int fd) {
	auto const ufd = static_cast<std::size_t>(fd);
	auto const gen = (ufd < fd_table.size()) ? fd_table[ufd].gen : std::uint32_t{0};
	bool const direct = ufd < fd_table.size() && fd_table[ufd].accepted_direct;
	HTTP_TRACE(
		std::format(
			"queue_close fd={} gen={} direct={} closing={} recv_armed={} zc_waiting={} mode={}",
			fd,
			gen,
			direct,
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
		if (fd_table[ufd].send_queued) {
			fd_table[ufd].close_after_send = true;
			submit_fd_shutdown_or_defer(fd, gen);
			return;
		}
	}

	if (direct) {
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
