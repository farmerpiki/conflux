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

module conflux.net.http_server:cqe;

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

void Ring::handle_sse_poll(
	int fd,
	int res,
	std::uint32_t gen) {
	in_flight_read_bufs.erase(pack(Op::SsePoll, gen, fd));
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != gen) {
		return;
	}
	auto &conn = fd_table[ufd];

	// res == sizeof(std::uint64_t) == 8 → io_uring read the eventfd counter;
	// res < 0 → error (fd closed, cancelled, etc.) → tear down.
	if (res <= 0) {
		queue_close(fd);
		return;
	}

	// io_uring already read (and reset) the eventfd counter into sse_read_buf.

#if CONFLUX_HAS_HTTP2
	// H2 SSE: resume the deferred data stream and drive the nghttp2 send loop.
	if (conn.is_h2 && conn.h2_sse_stream_id >= 0) {
		if (conn.h2_session != nullptr) {
			int const r = nghttp2_session_resume_data(conn.h2_session, conn.h2_sse_stream_id);
			if (r == 0) {
				h2_do_send(conn);
			}
		}
		// Re-arm if the channel is still open.
		if (conn.sse_channel && !conn.sse_channel->is_closed() && conn.h2_sse_stream_id >= 0) {
			queue_sse_wait(fd);
		}
		return;
	}
#endif

	auto data = conn.sse_channel->drain();
	if (!data.empty()) {
		conn.own_response = format_http_chunk(data);
		conn.has_response = true;
		conn.written = 0;
		start_response_send(fd, conn);
		// handle_send will re-arm wait or close after chunk is delivered.
	} else if (conn.sse_channel->is_closed()) {
		conn.own_response = "0\r\n\r\n";
		conn.has_response = true;
		conn.written = 0;
		conn.is_sse = false;
		conn.close_after_send = true;
		start_response_send(fd, conn);
	} else {
		queue_sse_wait(fd); // spurious wakeup, re-arm
	}
}

void Ring::handle_deferred_poll(
	int deferred_efd,
	int res,
	std::uint32_t gen) {
	in_flight_read_bufs.erase(pack(Op::DeferredPoll, gen, deferred_efd));
	auto it = deferred_waits.find(deferred_efd);
	if (it == deferred_waits.end()) {
		return;
	}

	auto const fd = it->second.conn_fd;
	auto const stream_id = it->second.stream_id;
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != gen) {
		deferred_waits.erase(it);
		return;
	}
	auto &conn = fd_table[ufd];
	if (res <= 0 || !it->second.response) {
		deferred_waits.erase(it);
		queue_close(fd);
		return;
	}

	auto ready = it->second.response->take_ready();
	if (!ready) {
		queue_deferred_wait(fd, deferred_efd, it->second.response, stream_id);
		return;
	}

	deferred_waits.erase(it);
	conn.last_activity = std::chrono::steady_clock::now();
	if (stream_id >= 0) {
#if CONFLUX_HAS_HTTP2
		auto stream_it = conn.h2_streams.find(stream_id);
		if (!conn.is_h2 || conn.h2_session == nullptr || stream_it == conn.h2_streams.end()) {
			return;
		}
		stream_it->second.deferred_efd = -1;
		h2_submit_response(conn, stream_id, std::move(*ready));
		h2_do_send(conn);
		if (conn.h2_sse_pending_wait) {
			conn.h2_sse_pending_wait = false;
			queue_sse_wait(fd);
		}
#endif
		return;
	}

	conn.is_deferred = false;
	conn.deferred_efd = -1;
	conn.deferred_response.reset();
	if (conn.deferred_head_only) {
		ready->head_only = true;
	}
	conn.deferred_head_only = false;
	if (ready->is_mapped_file()) {
		conn.own_response = format_response(*ready, alt_svc_header, conn.close_after_send);
		if (ready->head_only) {
			conn.has_response = true;
		} else {
			conn.mapped_file = ready->take_mapped_file();
			conn.mapped_total = conn.own_response.size() + conn.mapped_file->size;
			conn.mapped_delivered = 0;
			conn.has_response = false;
		}
	} else if (ready->is_streamed_file()) {
		conn.own_response = format_response(*ready, alt_svc_header, conn.close_after_send);
		if (ready->head_only) {
			conn.has_response = true;
		} else {
			conn.streamed_file = ready->take_streamed_file();
			conn.streamed_headers_sent = false;
			conn.streamed_delivered = 0;
			conn.streamed_splice_in_flight = false;
			conn.has_response = true;
		}
	} else {
		conn.own_response = format_response(*ready, alt_svc_header, conn.close_after_send);
		conn.has_response = true;
	}
	conn.written = 0;
	start_response_send(fd, conn);
}

void Ring::handle_conn_close(
	int fd,
	int res,
	std::uint32_t gen) {
	HTTP_TRACE(std::format("conn_close fd={} res={} gen={} direct={}", fd, res, gen, accepted_sockets_direct));
	if (direct_slots_ && accepted_sockets_direct) {
		auto const slot = static_cast<std::uint32_t>(fd);
		if (res >= 0 || res == -EBADF) {
			if (res == -EBADF) {
				HTTP_TRACE(std::format("conn_close_direct_empty slot={} gen={}", slot, gen));
			}
			if (!direct_slots_->release_closed(slot)) {
				eprintln(std::format("handle_conn_close: release_closed failed slot={}", slot));
			}
		} else {
			direct_slots_->poison(slot, res);
		}
	}
	conn_erase(fd, gen);
}

void Ring::handle_direct_slot_close(
	int fd,
	int res) {
	if (!direct_slots_) {
		return;
	}
	auto const slot = static_cast<std::uint32_t>(fd);
	if (res >= 0 || res == -EBADF) {
		if (res == -EBADF) {
			HTTP_TRACE(std::format("direct_slot_close_empty slot={}", slot));
		}
		if (!direct_slots_->release_closed(slot)) {
			eprintln(std::format("handle_direct_slot_close: release_closed failed slot={}", slot));
		}
	} else {
		direct_slots_->poison(slot, res);
	}
}

void Ring::dispatch_cqe(
	Op op,
	int fd,
	int res,
	std::uint32_t flg,
	std::uint32_t gen) {
	switch (op) {
	case Op::Accept      : handle_accept(res, flg); break;
	case Op::Recv        : handle_recv_cqe(fd, res, flg, gen); break;
	case Op::Send        : handle_send(fd, res, gen); break;
	case Op::Close       : handle_conn_close(fd, res, gen); break;
	case Op::SsePoll     : handle_sse_poll(fd, res, gen); break;
	case Op::DeferredPoll: handle_deferred_poll(fd, res, gen); break;
	case Op::Shutdown    : handle_shutdown(); break;
	case Op::FdShutdown  : handle_fd_shutdown(fd, res, gen); break;
	case Op::Timer       : handle_timer(); break;
	case Op::FileIo:
		if (file_completions) {
			file_completions->dispatch(static_cast<std::uint32_t>(fd), gen, res, flg);
		}
		break;
	case Op::ClientRing     : client_ct_.dispatch(static_cast<std::uint32_t>(fd), gen, res, flg); break;
	case Op::WsCancel       : handle_ws_cancel(fd); break;
	case Op::FixedFdInstall : handle_fixed_fd_install(fd, res); break;
	case Op::DirectSlotClose: handle_direct_slot_close(fd, res); break;
	case Op::SendZc         : handle_send_zc(fd, res, flg, gen); break;
	case Op::Nop            : break;
	}
}

// Phase 1: copy recv data out of provided/pinned recv buffers, return
// ownership immediately.  RecvPayload keeps the HTTP path independent of the
// concrete buffer backend so a later RECV_ZC backend can preserve this flow.
