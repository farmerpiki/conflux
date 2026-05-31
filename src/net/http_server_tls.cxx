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

module conflux.net.http_server:tls;

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
using conflux::uring::DirectFd;
using conflux::uring::OsFd;
using conflux::uring::RingFd;

#if CONFLUX_HTTP_TRACE
	#define HTTP_TRACE(MSG) eprintln(std::format("http_trace {}", (MSG)))
#else
	#define HTTP_TRACE(MSG) ((void)0)
#endif

#if CONFLUX_HAS_TLS
void Ring::tls_flush_wbio(
	Conn &conn) {
	std::array<char, 4096> buf{};
	int n{};
	while ((n = BIO_read(SSL_get_wbio(conn.ssl.get()), buf.data(), static_cast<int>(buf.size()))) > 0) {
		conn.tls_send_pending.append(buf.data(), static_cast<std::size_t>(n));
	}
}

bool Ring::tls_feed_rbio(
	Conn &conn) {
	if (conn.tls_rx_cipher.empty()) {
		return true;
	}
	BIO *const rbio = SSL_get_rbio(conn.ssl.get());
	if (rbio == nullptr) {
		return false;
	}
	std::string in = std::move(conn.tls_rx_cipher);
	conn.tls_rx_cipher.clear();
	std::size_t off{};
	while (off < in.size()) {
		auto const want = static_cast<int>(
			std::min<std::size_t>(in.size() - off, static_cast<std::size_t>(std::numeric_limits<int>::max())));
		int const written = BIO_write(rbio, in.data() + off, want);
		if (written <= 0) {
			conn.tls_rx_cipher.append(in.data() + off, in.size() - off);
			return false;
		}
		off += static_cast<std::size_t>(written);
	}
	return true;
}

void Ring::tls_queue_send(
	Conn &conn) {
	if (conn.send_queued) {
		return;
	}

	if (conn.tls_send_inflight.empty()) {
		conn.tls_send_inflight = std::move(conn.tls_send_pending);
		conn.tls_send_pending.clear();
		conn.tls_send_off = 0;
	}

	if (conn.tls_send_inflight.empty()) {
		return;
	}

	auto const view = std::span{conn.tls_send_inflight}.subspan(conn.tls_send_off);
	auto const fd = conn.fd;
	auto const gen = conn.gen;
	auto submit_tls_send = [&]<RingFd Handle>(Handle handle) {
		return submit_send_borrowed(raw_, handle, view.data(), view.size(), pack(Op::Send, gen, fd));
	};

	conn.send_queued = true;
	bool const submitted = accepted_sockets_direct ?
							   submit_tls_send(DirectFd::from_direct(static_cast<std::uint32_t>(fd))) :
							   submit_tls_send(OsFd::from_os(fd));
	if (!submitted) {
		conn.send_queued = false;
		defer_op([this, fd, gen] {
			auto const ufd = static_cast<std::size_t>(fd);
			if (ufd < fd_table.size() && fd_table[ufd].gen == gen && fd_table[ufd].fd >= 0) {
				tls_queue_send(fd_table[ufd]);
			}
		});
	}
}

void Ring::begin_tls_peer_shutdown_wait(
	int fd,
	Conn &conn) {
	conn.tls_shutdown_after_send = false;
	conn.tls_wait_peer_shutdown = true;
	conn.close_after_send = false;
	conn.has_response = false;
	conn.own_response.clear();
	conn.written = 0;
	conn.request_bytes = 0;
	conn.partial.clear();
	conn.chunked_decode.reset();
	conn.request_in_progress = false;
	conn.expect_continue_sent = false;
	if (!conn.recv_armed) {
		queue_multishot_recv(fd);
	}
}

void Ring::queue_tls_shutdown(
	int fd,
	Conn &conn) {
	if (conn.ssl == nullptr) {
		queue_close(fd);
		return;
	}
	conn.tls_shutdown_after_send = true;
	auto const shutdown_rc = SSL_shutdown(conn.ssl.get());
	conn.tls_wait_peer_shutdown = shutdown_rc != 1;
	tls_flush_wbio(conn);
	if (!conn.tls_send_pending.empty() || !conn.tls_send_inflight.empty()) {
		tls_queue_send(conn);
		return;
	}
	if (!conn.tls_wait_peer_shutdown) {
		conn.tls_shutdown_after_send = false;
		queue_close(fd);
		return;
	}
	begin_tls_peer_shutdown_wait(fd, conn);
}
#endif // CONFLUX_HAS_TLS

#if CONFLUX_HAS_TLS
// Called when all bytes in tls_send_buf have been sent.  Drives the
// post-send state machine for TLS connections.
void Ring::handle_send_tls_complete(
	int fd,
	Conn &conn) {
	conn.send_queued = false;

	if (conn.tls_shutdown_after_send) {
		if (conn.tls_wait_peer_shutdown) {
			begin_tls_peer_shutdown_wait(fd, conn);
		} else {
			conn.tls_shutdown_after_send = false;
			queue_close(fd);
		}
		return;
	}

	#if CONFLUX_HAS_HTTP2
	if (conn.is_h2) {
		if (conn.close_after_send) {
			conn.close_after_send = false;
			queue_tls_shutdown(fd, conn);
			return;
		}
		// Drive any nghttp2 output queued while the previous send was in flight,
		// then re-arm recv if nothing new was sent.
		h2_do_send(conn);
		if (!conn.send_queued && !conn.recv_armed) {
			queue_multishot_recv(fd);
		}
		return;
	}
	#endif

	if (conn.tls_sending_response) {
		if (conn.mapped_file) {
			if (conn.mapped_delivered < conn.mapped_file->size) {
				write_mapped_tls_chunk(fd, conn);
				return;
			}
			conn.mapped_file.reset();
			conn.mapped_total = 0;
			conn.mapped_delivered = 0;
		}

		// file_io TLS streaming: after the header batch is acked, pull
		// plaintext via read_fixed and SSL_write one chunk; repeat until the
		// whole file is delivered. handle_send_tls_complete fires once per
		// tls_send_buf drain, so this naturally interleaves with TLS sends.
		if (conn.streamed_file) {
			if (!conn.streamed_headers_sent) {
				return;
			}
			if (conn.streamed_delivered < conn.streamed_file->send_size) {
				if (!conn.streamed_splice_in_flight) {
					if (conn.ktls_send && splice_pipes) {
						start_streamed_body(fd);
					} else {
						start_streamed_tls_chunk(fd);
					}
				}
				return;
			}
			conn.streamed_file->notify_complete();
			conn.streamed_file.reset();
			conn.streamed_headers_sent = false;
			conn.streamed_delivered = 0;
		}

		// An HTTP response (or SSE/WS payload) was fully delivered.
		conn.tls_sending_response = false;
		conn.has_response = false;
		conn.own_response.clear();
		conn.written = 0;

		if (handle_sse_send_complete(fd, conn)) {
			return;
		}
		if (conn.is_ws) {
			handoff_tls_ws(conn, fd);
			return;
		}
		handle_http_response_send_complete(fd, conn);
	} else {
		// TLS handshake data was sent (or a post-handshake alert).
		// If an HTTP response accumulated while we were busy, send it now.
		if (response_send_ready(conn)) {
			if (!conn.send_queued) {
				defer_queue_send_if_current(fd, conn.gen);
			}
		} else if (!conn.recv_armed) {
			queue_multishot_recv(fd);
		}
	}

	// A response path above may have already encrypted bytes and queued the
	// underlying TLS send. Do not immediately re-enter the send-start path.
	if (conn.send_queued) {
		return;
	}
	if (!conn.tls_send_pending.empty()) {
		tls_queue_send(conn);
	}
}

#endif // CONFLUX_HAS_TLS

#if CONFLUX_HAS_TLS
// Per-connection TLS recv handler: feeds ciphertext into OpenSSL, drives the
// handshake, and decrypts application data back into conn.partial.
void Ring::phase1b_tls_one(
	Conn &conn,
	RecvComp &rc) {
	if (!tls_feed_rbio(conn)) {
		queue_close(conn.fd);
		rc.res = -1;
		return;
	}

	// Drive the handshake until it completes or needs more data.
	if (!conn.tls_hs_done) {
		int const r = SSL_do_handshake(conn.ssl.get());
		tls_flush_wbio(conn);
		if (r == 1) {
			conn.tls_hs_done = true;
			conn.ktls_send = (BIO_get_ktls_send(SSL_get_wbio(conn.ssl.get())) != 0);
	#if CONFLUX_HAS_HTTP2
			conn.is_h2 = conflux::http::detail::http2_negotiated(conn.ssl.get());
	#endif
		} else {
			int const err = SSL_get_error(conn.ssl.get(), r);
			if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
				if (!conn.send_queued) {
					tls_queue_send(conn);
				}
				return; // wait for more handshake data from client
			}
			if (!conn.send_queued) {
				queue_close(conn.fd);
			}
			rc.res = -1;
			return;
		}
	}

	// Handshake done — decrypt application data into partial.
	std::array<char, BUF_SIZE> plain{};
	int n{};
	while ((n = SSL_read(conn.ssl.get(), plain.data(), static_cast<int>(plain.size()))) > 0) {
		conn.partial.append(plain.data(), static_cast<std::size_t>(n));
	}
	int const ssl_err = SSL_get_error(conn.ssl.get(), n);
	if (ssl_err == SSL_ERROR_ZERO_RETURN
		|| (ssl_err != SSL_ERROR_WANT_READ && ssl_err != SSL_ERROR_WANT_WRITE && ssl_err != SSL_ERROR_NONE)) {
		if (!conn.send_queued) {
			queue_close(conn.fd);
		}
		rc.res = -1;
	}

	#if CONFLUX_HAS_HTTP2
	if (!conn.is_h2 && !conn.partial.empty() && !conn.request_in_progress) {
	#else
	if (!conn.partial.empty() && !conn.request_in_progress) {
	#endif
		conn.request_started = std::chrono::steady_clock::now();
		conn.request_in_progress = true;
	}

	tls_flush_wbio(conn);
	if (!conn.send_queued) {
		tls_queue_send(conn);
	}
}

#endif // CONFLUX_HAS_TLS (phase1b_tls_one)
// Phase 1b: run TLS recv processing.
// Plain connections: no-op when TLS not compiled in.
void Ring::phase1b_process() {
#if CONFLUX_HAS_TLS
	for (auto &rc: recvs) {
		if (rc.res <= 0) {
			continue;
		}
		auto const ufd = static_cast<std::size_t>(rc.fd);
		if (ufd >= fd_table.size() || fd_table[ufd].gen != rc.gen) {
			continue;
		}
		auto &conn = fd_table[ufd];
		if (conn.ssl != nullptr) {
			phase1b_tls_one(conn, rc);
		}
	}
#endif
}
