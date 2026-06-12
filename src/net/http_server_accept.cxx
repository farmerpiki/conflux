module;
#include <cstddef> // must precede openssl/nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" blocks re-include c++config.h

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#if CONFLUX_HAS_TLS
	#include <openssl/ssl.h>
#endif
#if CONFLUX_HAS_HTTP2
	#include <nghttp2/nghttp2.h>
#endif
#include <sys/socket.h>

module conflux.net.http_server:accept;

import std;
import std.compat;

import conflux.net.http_server_helpers;
import conflux.socket_io;
import conflux.utils;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
import :state;
using namespace conflux::socket_io;

#if CONFLUX_HTTP_TRACE
	#define HTTP_TRACE(MSG) conflux::utils::eprintln(std::format("http_trace {}", (MSG)))
#else
	#define HTTP_TRACE(MSG) ((void)0)
#endif

void Ring::handle_accept_error(
	[[maybe_unused]] int res,
	[[maybe_unused]] conflux::uring::CqeFlags flg) {
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
}

void Ring::reject_accepted_socket_during_shutdown(
	int fd,
	conflux::uring::CqeFlags flg) {
	{
		std::scoped_lock lk{metrics_mu_};
		++pressure_counters_.accept_rejected;
	}
	if (accepted_sockets_direct) {
		if (direct_slots_ && direct_slots_->adopt_kernel_allocated(static_cast<std::uint32_t>(fd))) {
			if (!direct_slots_->mark_closing(static_cast<std::uint32_t>(fd))) {
				conflux::utils::eprintln(std::format("handle_accept shutdown: mark_closing failed slot={}", fd));
			}
		}
		submit_direct_slot_close_or_defer(fd);
	} else {
		submit_os_close_or_defer(fd);
	}
	if (!cqe_has_more(flg)
		&& drain_control != nullptr
		&& drain_control->active.load(std::memory_order_acquire)
		&& !drain_control->options.stop_accepting) {
		queue_multishot_accept();
	}
}

bool Ring::adopt_direct_accept_slot_or_disable(
	int fd) {
	if (!accepted_sockets_direct || !direct_slots_) {
		return true;
	}
	if (direct_slots_->adopt_kernel_allocated(static_cast<std::uint32_t>(fd))) {
		return true;
	}
	++accepted_direct_failures_;
	conflux::utils::eprintln(
		std::format("handle_accept: adopt_kernel_allocated failed slot={} — stopping direct accept", fd));
	accepted_sockets_direct = false;
	submit_cancel_by_ud(raw_, pack(Op::Accept, 0, listen_fd), 0);
	submit_direct_slot_close_or_defer(fd);
	return false;
}

void Ring::reset_accepted_connection(
	int fd,
	Conn &conn,
	bool accepted_direct) {
	++conn.gen;
	conn.fd = fd;
	conn.accepted_direct = accepted_direct;
	conn.recv_armed = false;
	conn.last_recv_cqe_flags = {};
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
	conn.deferred_request_storage.reset();
	conn.deferred_request_files.reset();
	conn.ws_upgrade.reset();
	conn.partial.clear();
	conn.chunked_decode.reset();
	conn.mapped_file.reset();
	conn.mapped_total = 0;
	conn.mapped_delivered = 0;
	conn.last_activity = std::chrono::steady_clock::now();
}

void Ring::record_accepted_peer_address(
	int fd,
	Conn &conn) {
	if (!conn.accepted_direct) {
		sockaddr_in6 peer_addr{};
		socklen_t peer_len = sizeof(peer_addr);
		if (::getpeername(fd, reinterpret_cast<sockaddr *>(&peer_addr), &peer_len) == 0) {
			conn.remote_addr = conflux::utils::ip_to_string(peer_addr.sin6_addr);
		} else {
			conn.remote_addr.clear();
		}
		return;
	}
	conn.remote_addr = conflux::utils::ip_to_string(client_addr.sin6_addr);
}

void Ring::reset_connection_protocol_state(
	Conn &conn) {
	conn.is_tls = false;
#if CONFLUX_HAS_TLS
	if (conn.ssl != nullptr) {
		conn.ssl.reset();
	}
	conn.tls_rx_cipher.clear();
	conn.tls_send_pending.clear();
	conn.tls_send_inflight.clear();
	conn.tls_send_off = 0;
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

void Ring::apply_accepted_socket_options(
	int fd,
	Conn const &conn) {
	if (!conn.accepted_direct) {
		::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &tcp_opt_one_, sizeof tcp_opt_one_);
		::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &tcp_opt_one_, sizeof tcp_opt_one_);
		if (busy_poll_us_ > 0) {
			::setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll_us_, sizeof busy_poll_us_);
		}
		if (prefer_busy_poll_) {
			::setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &tcp_opt_one_, sizeof tcp_opt_one_);
		}
	}
}

void Ring::arm_accepted_connection_recv(
	int fd) {
	if (!conn_uses_direct(fd)) {
		queue_multishot_recv(fd);
		return;
	}
	queue_direct_accept_setup(fd);
}

void Ring::handle_accept(
	int res,
	conflux::uring::CqeFlags flg) {
	if (res < 0) {
		handle_accept_error(res, flg);
		return;
	}
	if (shutting_down) {
		reject_accepted_socket_during_shutdown(res, flg);
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
	if (!adopt_direct_accept_slot_or_disable(res)) {
		return;
	}
	auto &conn = conn_for(res);
	reset_accepted_connection(res, conn, accepted_sockets_direct);
	record_accepted_peer_address(res, conn);
	reset_connection_protocol_state(conn);
	apply_accepted_socket_options(res, conn);
	arm_accepted_connection_recv(res);
	if (!cqe_has_more(flg)) {
		queue_multishot_accept();
	}
}
