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

module conflux.net.http_server:recv;

import std;
import conflux.types;
import std.compat;

import conflux.net.http.types;
import conflux.net.router;
import conflux.file_map;
import conflux.net.detail.direct_slot_pool;
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
using namespace conflux::socket_io;

#if CONFLUX_HTTP_TRACE
	#define HTTP_TRACE(MSG) conflux::utils::eprintln(std::format("http_trace {}", (MSG)))
#else
	#define HTTP_TRACE(MSG) ((void)0)
#endif

void Ring::discard_recv_bufs(
	int res,
	conflux::uring::CqeFlags flags) noexcept {
	HTTP_TRACE(
		std::format(
			"discard_recv_bufs res={} flags=0x{:x} has_buf={} mode={}",
			res,
			flags.raw(),
			cqe_has_buffer(flags),
			buffer_ring_mode_name(buf_ring_->mode())));
	if (!cqe_has_buffer(flags)) {
		return;
	}
	if (res <= 0) {
		if (buf_ring_->mode() != BufferRingMode::incremental) {
			(void)buf_ring_->recycle_selected_buffer(cqe_buffer_id(flags));
		}
		return;
	}
	auto payload = try_recv_payload_from_cqe(*buf_ring_, res, flags, use_recv_bundle);
	if (!payload) [[unlikely]] {
		return;
	}
	note_recv_payload(*payload);
	payload->recycle_all();
}

void Ring::discard_recv_bufs(
	RecvComp &rc) noexcept {
	discard_recv_bufs(rc.res, rc.flags);
	rc.flags = {};
}

void Ring::retire_incremental_partial(
	int fd,
	std::uint32_t gen,
	Conn &conn) noexcept {
	if (!conn.have_incremental_buf_id) {
		return;
	}
	retired_incremental_recv.insert_or_assign(
		pack_fd_gen(fd, gen),
		Ring::RetiredIncrementalBuf{conn.incremental_buf_id, true});
	conn.have_incremental_buf_id = false;
}

void Ring::reclaim_retired_incremental_recv(
	int fd,
	std::uint32_t gen) noexcept {
	if (buf_ring_->mode() != BufferRingMode::incremental) {
		return;
	}
	auto it = retired_incremental_recv.find(pack_fd_gen(fd, gen));
	if (it == retired_incremental_recv.end() || !it->second.present) {
		return;
	}
	buf_ring_->reclaim_incremental_partial(it->second.id);
	retired_incremental_recv.erase(it);
}

void Ring::clear_retired_incremental_if_final(
	int fd,
	std::uint32_t gen,
	conflux::uring::CqeFlags flags) noexcept {
	if (buf_ring_->mode() != BufferRingMode::incremental) {
		return;
	}
	if (!cqe_has_buffer(flags) || cqe_has_buf_more(flags)) {
		return;
	}
	auto const key = pack_fd_gen(fd, gen);
	auto it = retired_incremental_recv.find(key);
	if (it == retired_incremental_recv.end()) {
		return;
	}
	std::uint16_t const id = cqe_buffer_id(flags);
	if (it->second.id != id) [[unlikely]] {
		return;
	}
	retired_incremental_recv.erase(it);
}

void Ring::handle_recv_cqe(
	int fd,
	int res,
	conflux::uring::CqeFlags flg,
	std::uint32_t gen) {
	HTTP_TRACE(
		std::format(
			"recv_cqe fd={} res={} flg=0x{:x} gen={} mode={} direct={}",
			fd,
			res,
			flg.raw(),
			gen,
			buffer_ring_mode_name(buf_ring_->mode()),
			accepted_sockets_direct));
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size()) {
		discard_recv_bufs(res, flg);
		return;
	}
	bool const gen_match = fd_table[ufd].gen == gen;
	bool const ws_pending = ws_cancel_handoffs.find(fd) != ws_cancel_handoffs.end();
	if (!gen_match && !ws_pending) {
		if (res <= 0 && !cqe_has_buffer(flg)) {
			reclaim_retired_incremental_recv(fd, gen);
		} else if (res > 0 && cqe_has_buffer(flg)) {
			discard_recv_bufs(res, flg);
			clear_retired_incremental_if_final(fd, gen, flg);
			return;
		}
		discard_recv_bufs(res, flg);
		return;
	}
	if (gen_match && fd_table[ufd].close_after_send) [[unlikely]] {
		if (!cqe_has_more(flg)) {
			fd_table[ufd].recv_armed = false;
		}
		if (res <= 0 && !cqe_has_buffer(flg)) {
			reclaim_retired_incremental_recv(fd, gen);
		} else if (cqe_has_buffer(flg)) {
			discard_recv_bufs(res, flg);
		}
		return;
	}
	if (!cqe_has_more(flg) && gen_match) {
		fd_table[ufd].recv_armed = false;
	}
	if (res > 0 && gen_match && fd_table[ufd].fd >= 0) {
		auto &conn = fd_table[ufd];
		conn.last_recv_cqe_flags = flg;
		conn.have_last_recv_cqe_flags = true;
	}
	recvs.push_back({fd, res, gen, flg});
}

template<typename Buf>
bool Ring::append_recv_buf_to(
	Buf &dst,
	RecvComp &rc) {
	auto payload = try_recv_payload_from_cqe(*buf_ring_, rc.res, rc.flags, use_recv_bundle);
	rc.flags = {};
	if (!payload) [[unlikely]] {
		return false;
	}
	if (payload->multi_buffer()) {
		note_recv_payload(*payload);
	}
	for (auto const &chunk: *payload) {
		dst.append(reinterpret_cast<char const *>(chunk.bytes.data()), chunk.bytes.size());
	}
	payload->recycle_all();
	return true;
}

void Ring::discard_stale_recv_completion(
	RecvComp &rc,
	conflux::uring::CqeFlags orig_flags) {
	discard_recv_bufs(rc);
	if (rc.res <= 0 && !cqe_has_buffer(orig_flags)) {
		reclaim_retired_incremental_recv(rc.fd, rc.gen);
	} else if (rc.res > 0 && cqe_has_buffer(orig_flags)) {
		clear_retired_incremental_if_final(rc.fd, rc.gen, orig_flags);
	}
}

void Ring::discard_closing_recv_completion(
	RecvComp &rc,
	conflux::uring::CqeFlags orig_flags) {
	discard_recv_bufs(rc);
	if (rc.res <= 0 && !cqe_has_buffer(orig_flags)) {
		reclaim_retired_incremental_recv(rc.fd, rc.gen);
	} else if (cqe_has_buffer(orig_flags)) {
		clear_retired_incremental_if_final(rc.fd, rc.gen, orig_flags);
	}
}

bool Ring::handle_ws_cancel_recv_handoff(
	RecvComp &rc,
	WsInstallEntry &entry,
	conflux::uring::CqeFlags orig_flags) {
	if (!append_recv_buf_to(entry.initial_buf, rc)) {
		return false;
	}
	clear_retired_incremental_if_final(rc.fd, rc.gen, orig_flags);
	return true;
}

std::optional<std::size_t> Ring::append_recv_payload_to_conn(
	Conn &conn,
	RecvComp &rc) {
#if CONFLUX_HAS_TLS
	if (conn.ssl != nullptr) {
		if (!append_recv_buf_to(conn.tls_rx_cipher, rc)) {
			return std::nullopt;
		}
		return conn.tls_rx_cipher.size();
	}
#endif
	if (!append_recv_buf_to(conn.partial, rc)) {
		return std::nullopt;
	}
	return conn.partial.size();
}

bool Ring::update_incremental_recv_state(
	int fd,
	Conn &conn,
	conflux::uring::CqeFlags orig_flags) {
	if (buf_ring_->mode() != BufferRingMode::incremental || !cqe_has_buffer(orig_flags)) {
		return true;
	}
	if (cqe_has_buf_more(orig_flags)) {
		conn.incremental_buf_id = cqe_buffer_id(orig_flags);
		conn.have_incremental_buf_id = true;
		if (!cqe_has_more(orig_flags)) [[unlikely]] {
			conflux::utils::eprintln(std::format("incremental ring fault: fd={} !MORE+BUF_MORE; closing", fd));
			queue_close(fd);
			return false;
		}
		return true;
	}
	conn.have_incremental_buf_id = false;
	return true;
}

void Ring::mark_request_receive_started(
	Conn &conn) {
	conn.last_activity = std::chrono::steady_clock::now();
	if (!conn.is_tls && !conn.partial.empty() && !conn.request_in_progress) {
		conn.request_started = conn.last_activity;
		conn.request_in_progress = true;
	}
}

std::size_t Ring::raw_receive_cap() const noexcept {
	auto bounded_add = [](std::size_t a, std::size_t b) noexcept {
		if (a > std::numeric_limits<std::size_t>::max() - b) {
			return std::numeric_limits<std::size_t>::max();
		}
		return a + b;
	};
	auto bounded_mul = [](std::size_t a, std::size_t b) noexcept {
		if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
			return std::numeric_limits<std::size_t>::max();
		}
		return a * b;
	};
	std::size_t cap = max_body_size;
	cap = bounded_add(cap, parser_limits.max_header_block_size);
	cap = bounded_add(cap, parser_limits.max_request_line_size);
	cap = bounded_add(cap, bounded_mul(parser_limits.max_chunks, conflux::http::kMaxChunkSizeLineBytes + 4));
	cap = bounded_add(cap, conflux::http::kMaxChunkTrailerBytes);
	return bounded_add(cap, 6);
}

void Ring::reject_oversized_receive_buffer(
	int fd,
	Conn &conn) {
	{
		std::scoped_lock lk{metrics_mu_};
		++rejection_counters_.body_too_large;
	}
	if (observability_hooks_.rejection) {
		observability_hooks_.rejection(
			conflux::http::HttpRejectReason::body_too_large,
			conflux::http::kHttpRequestEntityTooLarge);
	}
	conn.own_response =
		"HTTP/1.1 413 Content Too Large\r\n"
		"Content-Type: application/problem+json\r\n"
		"Content-Length: 78\r\n"
		"Connection: close\r\n\r\n"
		"{\"code\":\"body_too_large\",\"detail\":\"request body exceeds the configured limit\"}";
	conn.has_response = true;
	conn.close_after_send = true;
	start_response_send(fd, conn);
}

bool Ring::maybe_start_tls_sniff(
	[[maybe_unused]] Conn &conn) {
#if CONFLUX_HAS_TLS
	if (!conn.ssl && conn.tls_hs_done && !conn.partial.empty()) {
		if (static_cast<unsigned char>(conn.partial.front()) == 0x16U) {
			conn.ssl.reset(SSL_new(ssl_ctx));
			if (conn.ssl) {
				BIO *rbio = BIO_new(BIO_s_mem());
				if (rbio != nullptr) {
					BIO_set_mem_eof_return(rbio, -1);
				}
				BIO *wbio = BIO_new(BIO_s_mem());
				if (rbio == nullptr || wbio == nullptr) {
					if (rbio != nullptr) {
						BIO_free(rbio);
					}
					if (wbio != nullptr) {
						BIO_free(wbio);
					}
					conn.ssl.reset();
					queue_close(conn.fd);
					return false;
				}
				SSL_set_bio(conn.ssl.get(), rbio, wbio);
				SSL_set_accept_state(conn.ssl.get());
			} else {
				queue_close(conn.fd);
				return false;
			}
			conn.is_tls = true;
			conn.tls_hs_done = false;
			conn.tls_rx_cipher.append(conn.partial.data(), conn.partial.size());
			conn.partial.clear();
		} else {
			conn.tls_hs_done = false;
		}
	}
#endif
	return true;
}

void Ring::phase1_copy_recv_bufs() {
	for (auto &rc: recvs) {
		auto const ufd = static_cast<std::size_t>(rc.fd);
		auto ws_it = ws_cancel_handoffs.find(rc.fd);
		bool const ws_pending = ws_it != ws_cancel_handoffs.end()
#if CONFLUX_HAS_TLS
							 && ws_it->second.ssl == nullptr
#endif
			;
		if (rc.res <= 0
			|| ufd >= fd_table.size()
			|| (!ws_pending && (fd_table[ufd].gen != rc.gen || fd_table[ufd].fd < 0))) {
			auto const orig_flags = rc.flags;
			discard_stale_recv_completion(rc, orig_flags);
			continue;
		}
		if (ws_pending && (fd_table[ufd].gen != rc.gen || fd_table[ufd].fd < 0)) {
			auto const orig_flags = rc.flags;
			handle_ws_cancel_recv_handoff(rc, ws_it->second, orig_flags);
			continue;
		}
		auto &conn = fd_table[ufd];
		if (conn.close_after_send) [[unlikely]] {
			auto const orig_flags = rc.flags;
			discard_closing_recv_completion(rc, orig_flags);
			continue;
		}
		auto const orig_flags = rc.flags;
		auto const recv_buffered = append_recv_payload_to_conn(conn, rc);
		if (!recv_buffered) [[unlikely]] {
			queue_close(static_cast<int>(ufd));
			continue;
		}
		if (!update_incremental_recv_state(static_cast<int>(ufd), conn, orig_flags)) {
			continue;
		}
		mark_request_receive_started(conn);
		if (conn.send_queued) {
			continue;
		}
		if (*recv_buffered > raw_receive_cap()) {
			reject_oversized_receive_buffer(rc.fd, conn);
			continue;
		}
		if (!maybe_start_tls_sniff(conn)) {
			continue;
		}
	}
}

void Ring::finish_ready_ws_handoffs() {
	for (int const fd: ws_cancel_ready) {
		auto it = ws_cancel_handoffs.find(fd);
		if (it == ws_cancel_handoffs.end()) {
			continue;
		}
		auto entry = std::move(it->second);
		ws_cancel_handoffs.erase(it);
#if CONFLUX_HAS_TLS
		if (entry.ssl != nullptr) {
			finish_tls_ws_handoff(fd, std::move(entry));
			continue;
		}
#endif
		finish_plain_ws_handoff(fd, std::move(entry));
	}
	ws_cancel_ready.clear();
}

void Ring::phase2_build_responses() {
	for (auto &rc: recvs) {
		if (rc.res <= 0) {
			continue;
		}
		auto const ufd = static_cast<std::size_t>(rc.fd);
		if (ufd >= fd_table.size() || fd_table[ufd].gen != rc.gen || fd_table[ufd].fd < 0) {
			continue;
		}
		auto &conn = fd_table[ufd];
#if CONFLUX_HAS_TLS
		if (conn.tls_wait_peer_shutdown) {
			continue;
		}
#endif
#if CONFLUX_HAS_HTTP2
		if (conn.is_h2) {
			if (conn.h2_session == nullptr) {
				h2_setup_conn(conn);
			}
			if (!conn.partial.empty()) {
				if (!h2_prevalidate_client_frames(conn, conn.partial.view())) {
					conn.partial.clear();
					h2_do_send(conn);
					continue;
				}
				auto n = nghttp2_session_mem_recv(
					conn.h2_session,
					reinterpret_cast<std::uint8_t const *>(conn.partial.data()),
					conn.partial.size());
				conn.partial.clear();
				if (n < 0) {
					queue_close(conn.fd);
					continue;
				}
			}
			h2_do_send(conn);
			// Arm SSE eventfd poll for new H2 SSE streams.
			if (conn.h2_sse_pending_wait) {
				conn.h2_sse_pending_wait = false;
				queue_sse_wait(conn.fd);
			}
			continue;
		}
#endif
		// Skip SSE/WS connections — their I/O is driven by separate loops.
		if (!conn.has_response && !conn.is_deferred && !conn.is_sse && !conn.is_ws) {
			dispatch_request(
				conn,
				conn.partial.view(),
				*this,
				max_body_size,
				http_redirect_to_https,
				https_redirect_hosts,
				parser_limits);
		}
	}
}

// Phase 3: return unconsumed buffers + dispatch send/close.
void Ring::phase3_dispatch() {
	for (auto &rc: recvs) {
		if (cqe_has_buffer(rc.flags)) {
			discard_recv_bufs(rc);
		}

		auto const ufd = static_cast<std::size_t>(rc.fd);
		if (ufd >= fd_table.size() || fd_table[ufd].gen != rc.gen) {
			continue;
		}
		auto &conn = fd_table[ufd];
		if (rc.res <= 0) {
			if (!conn.send_queued) {
				queue_close(rc.fd);
			}
			continue;
		}
		if (conn.fd < 0) {
			queue_close(rc.fd);
		} else if (response_send_ready(conn)) {
			start_response_send(rc.fd, conn);
		} else if (conn.is_deferred && !conn.send_queued) {
			queue_deferred_wait(rc.fd);
		} else if (
			!conn.is_sse
			&& !conn.is_ws
			&& !conn.is_deferred
			&& !conn.has_response
			&& !conn.mapped_file
			&& !conn.recv_armed) {
			// Normal connection: re-arm recv.
			// SSE/WS connections: their own I/O loops drive further work.
			queue_multishot_recv(rc.fd);
		}
	}
}
