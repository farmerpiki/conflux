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

module conflux.net.http_server:send;

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

// Submit WRITEV for a mapped-file response.
// Adjusts iovecs to skip bytes already sent (conn.written).
// When the body is large enough for SEND_ZC, keep the header send separate so
// the body can use zero-copy after the header CQE drains.
void Ring::queue_send_mapped(
	int fd) {
	auto &conn = conn_for(fd);
	std::size_t skip = conn.written;
	std::size_t ni{};

	// iov[0]: remaining header bytes
	if (skip < conn.own_response.size()) {
		std::span<char> const hdr_span{conn.own_response};
		if (send_zc_enabled_ && conn.mapped_file && conn.mapped_file->size >= send_zc_threshold_) {
			auto submit_header = [&]<RingFd Handle>(Handle handle) {
				return submit_send_borrowed(
					raw_,
					handle,
					hdr_span.subspan(skip).data(),
					hdr_span.subspan(skip).size(),
					pack(Op::Send, conn.gen, fd));
			};
			bool const submitted = accepted_sockets_direct ?
									   submit_header(DirectFd::from_direct(static_cast<std::uint32_t>(fd))) :
									   submit_header(OsFd::from_os(fd));
			if (!submitted) {
				defer_op([this, fd] { queue_send_mapped(fd); });
			}
			return;
		}
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
		conn.writev_iov[ni++] = {
			.iov_base = static_cast<void *>(hdr_span.subspan(skip).data()),
			.iov_len = hdr_span.subspan(skip).size()};
		skip = 0;
	} else {
		skip -= conn.own_response.size();
	}
	// iov[1]: remaining file bytes (honouring offset for range requests)
	if (conn.mapped_file && skip < conn.mapped_file->size) {
		auto const win = conn.mapped_file->window();
		// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
		conn.writev_iov[ni++] = {
			.iov_base = const_cast<void *>(static_cast<void const *>(win.subspan(skip).data())),
			.iov_len = win.size() - skip};
	}

	if (ni == 0) {
		return;
	}
	auto submit_tail = [&]<RingFd Handle>(Handle handle) {
		if (send_zc_enabled_ && ni == 1 && conn.written >= conn.own_response.size()) {
			auto const body_len = conn.writev_iov[0].iov_len;
			if (body_len >= send_zc_threshold_) {
				++zc_counters_.attempts;
				++zc_counters_.mapped_attempts;
				zc_counters_.bytes_requested += body_len;
				if (submit_send_zc_borrowed(
						raw_,
						handle,
						conn.writev_iov[0].iov_base,
						body_len,
						pack(Op::SendZc, conn.gen, fd),
						send_zc_report_usage_)) {
					return true;
				}
				++zc_counters_.fallback_regular_send;
				defer_op([this, fd, g = conn.gen] {
					if (conn_for(fd).gen == g) {
						queue_send_mapped(fd);
					}
				});
				return true;
			}
		}
		if (!submit_writev_borrowed(
				raw_,
				handle,
				conn.writev_iov.data(),
				static_cast<unsigned>(ni),
				pack(Op::Send, conn.gen, fd))) {
			defer_op([this, fd] { queue_send_mapped(fd); });
		}
		return true;
	};
	(void)(accepted_sockets_direct ? submit_tail(DirectFd::from_direct(static_cast<std::uint32_t>(fd))) :
									 submit_tail(OsFd::from_os(fd)));
}

// triggered from handle_send once the header bytes are acked.
void Ring::queue_send_streamed(
	int fd) {
	auto &conn = conn_for(fd);
	if (conn.streamed_headers_sent) {
		// Phase 2: start splice (or continue by re-arming if already flying).
		if (!conn.streamed_splice_in_flight) {
			start_streamed_body(fd);
		}
		return;
	}
	auto const hdr_view = std::span{conn.own_response}.subspan(conn.written);
	auto submit_header = [&]<RingFd Handle>(Handle handle) {
		return submit_send_borrowed(raw_, handle, hdr_view.data(), hdr_view.size(), pack(Op::Send, conn.gen, fd));
	};
	bool const submitted = accepted_sockets_direct ?
							   submit_header(DirectFd::from_direct(static_cast<std::uint32_t>(fd))) :
							   submit_header(OsFd::from_os(fd));
	if (!submitted) {
		defer_op([this, fd] { queue_send_streamed(fd); });
	}
}

// Acquire a pipe P and submit the splice chain via FileReader. Completion
// calls back into handle_streamed_splice_done on the ring std::thread.
void Ring::start_streamed_body(
	int fd) {
	auto &conn = conn_for(fd);
	if (!conn.streamed_file || !files || !splice_pipes) {
		queue_close(fd);
		return;
	}
	auto pipe = splice_pipes->try_acquire();
	if (!pipe) {
		// All pipe pairs in-flight; retry from next CQE drain once one is returned.
		defer_op([this, fd] { start_streamed_body(fd); });
		return;
	}
	auto const remaining = conn.streamed_file->send_size - conn.streamed_delivered;
	auto const off = conn.streamed_file->send_offset + conn.streamed_delivered;
	conn.streamed_splice_in_flight = true;
	auto const conn_gen = conn.gen;
	do_streamed_splice(
		this,
		fd,
		conn_gen,
		files->splice_to_fd(
			*conn.streamed_file->handle,
			off,
			static_cast<std::size_t>(remaining),
			fd,
			std::move(*pipe),
			accepted_sockets_direct))
		.detach();
}

#if CONFLUX_HAS_TLS
// TLS streamed body: acquire a FixedBuffer, read_fixed a chunk of the file,
// SSL_write it into wbio, flush and re-queue the TLS send. Pipelining depth
// is effectively 1 per connection — suitable for unbuffered streaming.
void Ring::start_streamed_tls_chunk(
	int fd) {
	auto &conn = conn_for(fd);
	if (!conn.streamed_file || !files || !fixed_buffers) {
		queue_close(fd);
		return;
	}
	auto buf = fixed_buffers->try_acquire();
	if (!buf) {
		// All slabs in-flight; retry from the next CQE drain once one is returned.
		defer_op([this, fd] { start_streamed_tls_chunk(fd); });
		return;
	}
	auto const remaining = conn.streamed_file->send_size - conn.streamed_delivered;
	auto const off = conn.streamed_file->send_offset + conn.streamed_delivered;
	auto const want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buf->size()));
	FixedBuffer b = std::move(*buf);
	auto const conn_gen = conn.gen;
	conn.streamed_splice_in_flight = true;
	auto &fh = *conn.streamed_file->handle;
	do_streamed_tls_chunk(this, fd, conn_gen, want, files->read_fixed(fh, off, std::move(b), want)).detach();
}

void Ring::on_streamed_tls_chunk_done(
	int fd,
	std::uint32_t conn_gen,
	FixedBuffer buf,
	std::size_t bytes,
	std::exception_ptr const &err) {
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != conn_gen) {
		return;
	}
	auto &conn = fd_table[ufd];
	conn.streamed_splice_in_flight = false;
	if (err || conn.ssl == nullptr || !conn.streamed_file) {
		if (conn.streamed_file) {
			conn.streamed_file->notify_failed();
		}
		conn.streamed_file.reset();
		queue_close(fd);
		return;
	}
	if (bytes == 0) {
		// EOF earlier than std::expected — treat as done; trailing bytes won't
		// be invented.
		conn.streamed_file->notify_failed();
		conn.streamed_file.reset();
		queue_close(fd);
		return;
	}
	auto view = buf.view().subspan(0, bytes);
	auto const w = SSL_write(conn.ssl.get(), view.data(), static_cast<int>(view.size()));
	if (w <= 0) {
		conn.streamed_file->notify_failed();
		conn.streamed_file.reset();
		queue_close(fd);
		return;
	}
	conn.streamed_delivered += static_cast<std::uint64_t>(w);
	tls_flush_wbio(conn);
	conn.tls_sending_response = true;
	tls_queue_send(conn);
	// `buf` drops here → slab returned to pool.
}

void Ring::write_mapped_tls_chunk(
	int fd,
	Conn &conn) {
	if (!conn.mapped_file || conn.ssl == nullptr) {
		queue_close(fd);
		return;
	}
	auto const win = conn.mapped_file->window();
	auto const remaining = win.size() - conn.mapped_delivered;
	if (remaining == 0) {
		return;
	}
	static constexpr std::uint64_t kMappedTlsChunk{64UL * 1024U};
	auto const want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kMappedTlsChunk));
	auto const *data = reinterpret_cast<char const *>(win.data()) + conn.mapped_delivered;
	auto const w = SSL_write(conn.ssl.get(), data, static_cast<int>(want));
	if (w <= 0) {
		conn.mapped_file.reset();
		queue_close(fd);
		return;
	}
	conn.mapped_delivered += static_cast<std::uint64_t>(w);
	tls_flush_wbio(conn);
	conn.tls_sending_response = true;
	tls_queue_send(conn);
}

#endif
void Ring::on_streamed_splice_done(
	int fd,
	std::uint32_t conn_gen,
	std::size_t delivered,
	std::exception_ptr const &err) {
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != conn_gen) {
		return;
	}
	auto &conn = fd_table[ufd];
	conn.streamed_splice_in_flight = false;
	if (err || !conn.streamed_file) {
		if (conn.streamed_file) {
			conn.streamed_file->notify_failed();
		}
		conn.streamed_file.reset();
		queue_close(fd);
		return;
	}
	conn.streamed_delivered += delivered;
	if (conn.streamed_delivered < conn.streamed_file->send_size) {
		// Short splice — kick another leg from the event queue.
		defer_start_streamed_body_if_current(fd, conn.gen);
		return;
	}
	// Fully streamed — release handle (ring will close via close_async
	// on drop; FileHandle from_fd path closes synchronously, acceptable
	// since data is already delivered).
	conn.streamed_file->notify_complete();
	conn.streamed_file.reset();
	conn.written = 0;
	conn.send_queued = false;
	conn.has_response = false;
	conn.own_response.clear();
#if CONFLUX_HAS_TLS
	// kTLS file body went through splice, not tls_queue_send — clear manually.
	if (conn.ktls_send) {
		conn.tls_sending_response = false;
	}
#endif
	defer_handle_send_complete_if_current(fd, conn.gen);
}

#if CONFLUX_HAS_TLS
[[nodiscard]] bool Ring::tls_write_plaintext(
	int fd,
	Conn &conn,
	std::string_view bytes) {
	char const *data = bytes.data();
	auto remaining = static_cast<int>(bytes.size());
	while (remaining > 0) {
		auto const w = SSL_write(conn.ssl.get(), data, remaining);
		if (w <= 0) {
			queue_close(fd);
			return false;
		}
		data += w;
		remaining -= w;
	}
	tls_flush_wbio(conn);
	conn.tls_sending_response = true;
	tls_queue_send(conn);
	return true;
}

#endif
void Ring::note_send_zc_tls_bypass_if_candidate(
	Conn &conn) noexcept {
	if (!send_zc_enabled_ || conn.zc_tls_bypass_counted) {
		return;
	}
	std::size_t candidate_bytes{};
	if (conn.mapped_file) {
		candidate_bytes = static_cast<std::size_t>(conn.mapped_file->size);
	} else if (conn.streamed_file) {
		candidate_bytes = static_cast<std::size_t>(conn.streamed_file->send_size);
	} else if (conn.has_response && conn.own_response.size() >= conn.written) {
		candidate_bytes = conn.own_response.size() - conn.written;
	}
	if (candidate_bytes >= send_zc_threshold_) {
		++zc_counters_.tls_bypass;
		zc_counters_.tls_bypass_bytes += candidate_bytes;
		conn.zc_tls_bypass_counted = true;
	}
}

void Ring::queue_send(
	int fd) {
	auto &conn = conn_for(fd);
#if CONFLUX_HAS_TLS
	if (conn.ssl != nullptr) {
		note_send_zc_tls_bypass_if_candidate(conn);
		// TLS path: encrypt plaintext into the memory BIO, then send the
		// resulting TLS records through io_uring. Static streamed-file
		// responses need an explicit header phase; the body is pulled from
		// file_io only after the encrypted header batch is actually drained.
		if (conn.streamed_file) {
			if (!conn.streamed_headers_sent) {
				auto const hdr = std::string_view{conn.own_response}.substr(conn.written);
				if (hdr.empty()) {
					conn.streamed_headers_sent = true;
					if (conn.ktls_send && splice_pipes) {
						start_streamed_body(fd);
					} else {
						start_streamed_tls_chunk(fd);
					}
					return;
				}
				if (!tls_write_plaintext(fd, conn, hdr)) {
					return;
				}
				conn.written = conn.own_response.size();
				conn.streamed_headers_sent = true;
			}
			return;
		}
		if (conn.mapped_file && !conn.has_response) {
			if (conn.written < conn.own_response.size()) {
				auto const hdr = std::string_view{conn.own_response}.substr(conn.written);
				if (!tls_write_plaintext(fd, conn, hdr)) {
					return;
				}
				conn.written = conn.own_response.size();
				return;
			}
			write_mapped_tls_chunk(fd, conn);
			return;
		}
		if (!conn.has_response) {
			return;
		}
		if (!tls_write_plaintext(fd, conn, conn.own_response)) {
			return;
		}
		return;
	}
#endif
	if (!conn.mapped_file && !conn.streamed_file && !conn.has_response) {
		return;
	}
	conn.send_queued = true;
	if (conn.mapped_file) {
		queue_send_mapped(fd);
		return;
	}
	if (conn.streamed_file) {
		queue_send_streamed(fd);
		return;
	}
	auto const gen = conn.gen;
	auto const &resp = conn.own_response;
	std::size_t const len = resp.size() - conn.written;
	auto submit_response = [&]<RingFd Handle>(Handle handle) {
		if (conn.send_buf.valid()) {
			assert(conn.written >= conn.send_buf_base_written);
			auto const local_off = conn.written - conn.send_buf_base_written;
			assert(local_off <= conn.send_buf_len);
			auto const remaining = conn.send_buf.view().subspan(local_off, conn.send_buf_len - local_off);
			if (!submit_send_fixed_borrowed(
					raw_,
					handle,
					conn.send_buf.slot(),
					remaining.data(),
					remaining.size(),
					pack(Op::Send, gen, fd))) {
				defer_op([this, fd, gen] {
					auto const ufd = static_cast<std::size_t>(fd);
					if (ufd < fd_table.size() && fd_table[ufd].gen == gen) {
						queue_send(fd);
					}
				});
			}
			return;
		}
		auto const resp_view = std::span{resp}.subspan(conn.written);
		if (send_zc_enabled_ && resp_view.size() >= send_zc_threshold_) {
			++zc_counters_.attempts;
			++zc_counters_.plain_attempts;
			zc_counters_.bytes_requested += resp_view.size();
			if (submit_send_zc_borrowed(
					raw_,
					handle,
					resp_view.data(),
					resp_view.size(),
					pack(Op::SendZc, gen, fd),
					send_zc_report_usage_)) {
				return;
			}
			++zc_counters_.fallback_regular_send;
			defer_op([this, fd, gen] {
				auto const ufd = static_cast<std::size_t>(fd);
				if (ufd < fd_table.size() && fd_table[ufd].gen == gen) {
					queue_send(fd);
				}
			});
			return;
		}
		if (send_buffers && send_fixed_buffers_supported && len <= send_buffers->slab_bytes()) {
			auto buf = send_buffers->try_acquire();
			if (buf) {
				auto const view = buf->view().subspan(0, len);
				std::memcpy(view.data(), resp.data() + conn.written, len);
				if (submit_send_fixed_borrowed(
						raw_,
						handle,
						buf->slot(),
						view.data(),
						view.size(),
						pack(Op::Send, gen, fd))) {
					conn.send_buf = std::move(*buf);
					conn.send_buf_base_written = conn.written;
					conn.send_buf_len = len;
					return;
				}
				defer_op([this, fd, gen] {
					auto const ufd = static_cast<std::size_t>(fd);
					if (ufd < fd_table.size() && fd_table[ufd].gen == gen) {
						queue_send(fd);
					}
				});
				return;
			}
		}
		if (!submit_send_borrowed(raw_, handle, resp_view.data(), resp_view.size(), pack(Op::Send, gen, fd))) {
			defer_op([this, fd, gen] {
				auto const ufd = static_cast<std::size_t>(fd);
				if (ufd < fd_table.size() && fd_table[ufd].gen == gen) {
					queue_send(fd);
				}
			});
		}
	};
	if (accepted_sockets_direct) {
		submit_response(DirectFd::from_direct(static_cast<std::uint32_t>(fd)));
	} else {
		submit_response(OsFd::from_os(fd));
	}
}

[[nodiscard]] bool Ring::response_send_ready(
	Conn const &conn) noexcept {
	return conn.has_response || conn.mapped_file != nullptr || conn.streamed_file != nullptr;
}

void Ring::start_response_send(
	int fd,
	Conn &conn) {
	if (conn.send_queued || !response_send_ready(conn)) {
		return;
	}
	queue_send(fd);
}

bool Ring::handle_sse_send_complete(
	int fd,
	Conn &conn) {
	if (!conn.is_sse) {
		return false;
	}
	if (!conn.sse_headers_sent) {
		conn.sse_headers_sent = true;
	}
	auto remaining = conn.sse_channel->drain();
	if (!remaining.empty()) {
		conn.own_response = format_http_chunk(remaining);
		conn.has_response = true;
		conn.written = 0;
		defer_queue_send_if_current(fd, conn.gen);
	} else if (conn.sse_channel->is_closed()) {
		conn.own_response = "0\r\n\r\n";
		conn.has_response = true;
		conn.written = 0;
		conn.is_sse = false;
		conn.close_after_send = true;
		defer_queue_send_if_current(fd, conn.gen);
	} else {
		queue_sse_wait(fd);
	}
	return true;
}

void Ring::handle_http_response_send_complete(
	int fd,
	Conn &conn) {
	if (drain_control != nullptr && drain_control->active.load(std::memory_order_acquire)) {
		drain_control->requests_finished.fetch_add(1, std::memory_order_relaxed);
	}
	if (conn.close_after_send) {
		conn.close_after_send = false;
#if CONFLUX_HAS_TLS
		if (conn.ssl != nullptr) {
			queue_tls_shutdown(fd, conn);
			return;
		}
#endif
		queue_close(fd);
		return;
	}
	conn.partial.consume(conn.request_bytes);
	if (conn.request_bytes > 0) {
		conn.expect_continue_sent = false;
		conn.chunked_decode.reset();
	}
	conn.request_bytes = 0;
	if (conn.partial.empty()) {
		conn.request_in_progress = false;
	} else {
		conn.request_started = std::chrono::steady_clock::now();
	}
	if (!conn.partial.empty()) {
		dispatch_request(
			conn,
			conn.partial.view(),
			*this,
			max_body_size,
			http_redirect_to_https,
			https_redirect_hosts,
			parser_limits);
		if (response_send_ready(conn)) {
			if (!conn.send_queued) {
				defer_queue_send_if_current(fd, conn.gen);
			}
			return;
		}
		if (conn.is_deferred) {
			queue_deferred_wait(fd);
			return;
		}
	}
	if (!conn.recv_armed) {
		queue_multishot_recv(fd);
	}
}

void Ring::handle_send_complete(
	int fd,
	Conn &conn) {
	if (handle_sse_send_complete(fd, conn)) {
		return;
	}
	if (conn.is_ws) {
		handoff_plain_ws(conn, fd);
		return;
	}
	handle_http_response_send_complete(fd, conn);
}

void Ring::finish_plain_send(
	int fd,
	Conn &conn) {
	conn.written = 0;
	conn.send_queued = false;
	conn.has_response = false;
	conn.own_response.clear();
	conn.zc_tls_bypass_counted = false;
	conn.send_buf = FixedBuffer{};
	conn.send_buf_base_written = 0;
	conn.send_buf_len = 0;
	handle_send_complete(fd, conn);
}

void Ring::finish_mapped_send(
	int fd,
	Conn &conn) {
	conn.mapped_file.reset();
	conn.mapped_total = 0;
	conn.mapped_delivered = 0;
	conn.written = 0;
	conn.send_queued = false;
	conn.own_response.clear();
	conn.zc_tls_bypass_counted = false;
	handle_send_complete(fd, conn);
}

void Ring::fail_send(
	int fd,
	Conn &conn) {
	if (conn.mapped_file) {
		conn.mapped_file.reset();
	}
	if (conn.streamed_file) {
		conn.streamed_file->notify_failed();
		conn.streamed_file.reset();
	}
	queue_close(fd);
}

void Ring::handle_send(
	int fd,
	int res,
	std::uint32_t gen) {
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != gen) {
		return;
	}
	auto &conn = fd_table[ufd];

#if CONFLUX_HAS_TLS
	// TLS path: track progress through tls_send_buf.
	if (conn.ssl != nullptr) {
		if (res <= 0) {
			queue_close(fd);
			return;
		}

		conn.tls_send_off += static_cast<std::size_t>(res);

		if (conn.tls_send_off < conn.tls_send_inflight.size()) {
			conn.send_queued = false;
			tls_queue_send(conn);
			return;
		}

		conn.tls_send_inflight.clear();
		conn.tls_send_off = 0;
		conn.send_queued = false;

		handle_send_tls_complete(fd, conn);
		return;
	}
#endif
	if (conn.mapped_file) {
		if (res <= 0) {
			fail_send(fd, conn);
			return;
		}
		conn.written += static_cast<std::size_t>(res);
		if (conn.written < conn.mapped_total) {
			queue_send_mapped(fd);
			return;
		}
		finish_mapped_send(fd, conn);
		return;
	}

	if (conn.streamed_file) {
		if (res <= 0) {
			fail_send(fd, conn);
			return;
		}
		conn.written += static_cast<std::size_t>(res);
		if (conn.written < conn.own_response.size()) {
			queue_send_streamed(fd); // headers: resubmit remainder
			return;
		}
		// Headers fully sent; kick off body streaming.
		conn.streamed_headers_sent = true;
		start_streamed_body(fd);
		return;
	}

	if (res == -EINVAL && conn.send_buf.valid()) {
		send_fixed_buffers_supported = false;
		conn.send_buf = FixedBuffer{};
		conn.send_buf_base_written = 0;
		conn.send_buf_len = 0;
		queue_send(fd);
		return;
	}
	if (res > 0) {
		if (!conn.has_response) {
			conn.send_buf = FixedBuffer{};
			conn.send_buf_base_written = 0;
			conn.send_buf_len = 0;
			conn.send_queued = false;
			if (!conn.recv_armed && !conn.is_sse && !conn.is_ws && !conn.is_deferred) {
				queue_multishot_recv(fd);
			}
			return;
		}
		conn.written += static_cast<std::size_t>(res);
		if (conn.written < conn.own_response.size()) {
			queue_send(fd);
			return;
		}
		finish_plain_send(fd, conn);
	} else {
		conn.send_buf = FixedBuffer{};
		conn.send_buf_base_written = 0;
		conn.send_buf_len = 0;
		queue_close(fd);
	}
}

void Ring::handle_send_zc(
	int fd,
	int res,
	std::uint32_t flags,
	std::uint32_t gen) {
	auto const ufd = static_cast<std::size_t>(fd);
	if (ufd >= fd_table.size() || fd_table[ufd].gen != gen) {
		return;
	}
	auto &conn = fd_table[ufd];
	auto const is_mapped = conn.mapped_file != nullptr;
	auto const total = is_mapped ? conn.mapped_total : conn.own_response.size();
	auto const outcome = observe_send_zc_cqe(
		conn.zc_state,
		zc_counters_,
		SendZcCqeInput{
			.result = res,
			.notification = (flags & IORING_CQE_F_NOTIF) != 0,
			.more = (flags & IORING_CQE_F_MORE) != 0,
			.copied = (static_cast<std::uint32_t>(res) & IORING_NOTIF_USAGE_ZC_COPIED) != 0,
			.enomem_error = res == -ENOMEM,
			.written_before = conn.written,
			.response_total = total,
		},
		send_zc_enabled_);
	conn.written += outcome.bytes_sent;
	switch (outcome.action) {
	case SendZcCqeAction::complete_response:
		if (is_mapped) {
			finish_mapped_send(fd, conn);
		} else {
			finish_plain_send(fd, conn);
		}
		break;
	case SendZcCqeAction::resubmit_response:
		if (is_mapped) {
			queue_send_mapped(fd);
		} else {
			queue_send(fd);
		}
		break;
	case SendZcCqeAction::close_after_error: fail_send(fd, conn); break;
	case SendZcCqeAction::close_after_notification:
		conn.own_response.clear();
		conn.mapped_file.reset();
		conn.closing = false; // queue_close early-returns when closing==true
		queue_close(fd);
		break;
	default: break;
	}
}

conflux::work::root::Task<void> do_streamed_splice(
	Ring *ring,
	int fd,
	std::uint32_t conn_gen,
	conflux::work::root::Task<std::size_t> splice_task) {
	try {
		auto const delivered = co_await std::move(splice_task);
		ring->on_streamed_splice_done(fd, conn_gen, delivered, {});
	} catch (...) { ring->on_streamed_splice_done(fd, conn_gen, std::size_t{0}, std::current_exception()); }
}

#if CONFLUX_HAS_TLS
conflux::work::root::Task<void> do_streamed_tls_chunk(
	Ring *ring,
	int fd,
	std::uint32_t conn_gen,
	std::size_t want,
	conflux::work::root::Task<FileReader::ReadFixedResult> read_task) {
	try {
		auto result = co_await std::move(read_task);
		ring->on_streamed_tls_chunk_done(fd, conn_gen, std::move(result.buffer), std::min(result.bytes, want), {});
	} catch (...) { ring->on_streamed_tls_chunk_done(fd, conn_gen, FixedBuffer{}, 0, std::current_exception()); }
}
#endif // CONFLUX_HAS_TLS
