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
#include <conflux/detail/discard.hxx>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

module conflux.net.http_server:diag;

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

[[nodiscard]] bool Ring::ring_integrity_suspect() const noexcept {
	return raw_.ring().cq_has_overflow();
}

void Ring::note_cq_overflow() noexcept {
	saw_overflow_since_last_resize_ = true;
}

void Ring::note_recv_bundle_slices(
	RecvSlices const &slices) noexcept {
	if (!use_recv_bundle || !slices.valid()) {
		return;
	}
	++recv_bundle_cqes_;
	recv_bundle_slices_ += slices.count();
	recv_bundle_bytes_ += slices.total_size();
}

void Ring::note_recv_payload(
	RecvPayload const &payload) noexcept {
	if (!use_recv_bundle || !payload.valid() || !payload.multi_buffer()) {
		return;
	}
	++recv_bundle_cqes_;
	recv_bundle_slices_ += payload.chunk_count();
	recv_bundle_bytes_ += payload.total_size();
}

[[nodiscard]] HttpServerMetrics Ring::metrics_snapshot() const noexcept {
	HttpServerMetrics m{};
	if (ring.ring_fd >= 0) {
		m.sq_dropped = ring.sq.kdropped != nullptr ? *ring.sq.kdropped : 0;
		m.cq_overflow = ring.cq.koverflow != nullptr ? *ring.cq.koverflow : 0;
	}
	m.accepted_direct_failures = accepted_direct_failures_;
	m.zc_capable_rings = CONFLUX_ENABLE_SEND_ZC && caps.send_zc ? 1 : 0;
	m.zc_enabled_rings = send_zc_enabled_ ? 1 : 0;
	m.recv_bundle_cqes = recv_bundle_cqes_;
	m.recv_bundle_slices = recv_bundle_slices_;
	m.recv_bundle_bytes = recv_bundle_bytes_;
	m.send_zc = zc_counters_.snapshot();
	for (Conn const &conn: fd_table) {
		if (conn.fd >= 0 && conn.zc_state.waiting_notification) {
			++m.zc_notifications_pending;
		}
	}
	return m;
}

void Ring::try_grow_cq_after_overflow() noexcept {
	if (!CONFLUX_ENABLE_RING_GROWTH) {
		return;
	}
	if (!saw_overflow_since_last_resize_ || cq_resize_unsupported_ || ring_integrity_suspect()) {
		return;
	}
	auto const rr = raw_.ring();
	std::uint32_t const cur = rr.cq_entries();
	if (cur == 0 || cur >= (1u << 20)) {
		cq_resize_unsupported_ = true;
		saw_overflow_since_last_resize_ = false;
		return;
	}
	std::uint32_t const target = std::min<std::uint32_t>(cur * 2u, 1u << 20);
	auto resized = rr.grow_cq_to(target);
	if (resized) {
		saw_overflow_since_last_resize_ = false;
		if (startup_banner) {
			eprintln(std::format("ring_cq_resized={}->{} after overflow", cur, target));
		}
		return;
	}
	int const err = resized.error();
	if (err == -EBUSY) {
		return;
	}
	// Ring resize requires kernel/liburing support and DEFER_TASKRUN, and is
	// unavailable for NO_MMAP rings. Treat permanent unsupported cases as a
	// capability miss; the existing NODROP drain path remains the fallback.
	saw_overflow_since_last_resize_ = false;
	if (err == -ENOSYS || err == -EINVAL || err == -EOPNOTSUPP) {
		cq_resize_unsupported_ = true;
	}
	if (startup_banner) {
		eprintln(std::format("ring_cq_resize_skipped={}->{} err={}", cur, target, err));
	}
}

void Ring::enter_ring_fatal(
	ServerFatalReason reason) noexcept {
	ring_fatal_ = true;
	shutting_down = true;
	pending_ops.clear();
	fatal_reason_ = reason;
	fatal_cq_overflow_count_ = raw_.ring().cq_overflow_count();
}

void Ring::close_tracked_fds_sync() noexcept {
	for (auto &conn: fd_table) {
		if (conn.fd >= 0) {
			::close(conn.fd);
			conn.fd = -1;
		}
	}
}

void Ring::recycle_recv_buffer_direct(
	io_uring_cqe const *cqe) noexcept {
	HTTP_TRACE(
		std::format(
			"direct_recv_cqe fd={} res={} flags=0x{:x} has_buf={} mode={}",
			std::get<2>(unpack(cqe->user_data)),
			cqe->res,
			cqe->flags,
			(cqe->flags & IORING_CQE_F_BUFFER) != 0u,
			buffer_ring_mode_name(buf_ring_->mode())));
	if ((cqe->flags & IORING_CQE_F_BUFFER) == 0u) {
		return;
	}
	if (cqe->res <= 0) {
		if (buf_ring_->mode() != BufferRingMode::incremental) {
			(void)buf_ring_->recycle_selected_buffer(cqe_buffer_id(cqe->flags));
		}
		return;
	}
	auto payload = try_recv_payload_from_cqe(*buf_ring_, cqe->res, cqe->flags, use_recv_bundle);
	if (!payload) [[unlikely]] {
		return;
	}
	if (payload->multi_buffer()) {
		note_recv_payload(*payload);
	}
	payload->recycle_all();
}

void Ring::dispatch_cqe_fatal(
	io_uring_cqe const *cqe) noexcept {
	try {
		auto const [op, accepted_fd, direct_slot] = unpack(cqe->user_data);
		CONFLUX_DISCARD(accepted_fd);
		CONFLUX_DISCARD(direct_slot);
		switch (op) {
		case Op::Recv: recycle_recv_buffer_direct(cqe); break;
		case Op::Accept:
			if (!accepted_sockets_direct && cqe->res >= 0) {
				::close(cqe->res);
			}
			break;
		case Op::Close:
		case Op::Send:
		case Op::SendZc:
		case Op::Timer:
		case Op::FileIo:
		case Op::SsePoll:
		case Op::DeferredPoll:
		case Op::Shutdown:
		case Op::FdShutdown:
		case Op::WsCancel:
		case Op::ClientRing:
		case Op::Nop         : break;
		case Op::FixedFdInstall:
			if (cqe->res >= 0) {
				::close(cqe->res);
			}
			break;
		default:
			{
				// unknown op — future Op additions must be handled here
				auto _ = std::fprintf(
					stderr,
					"dispatch_cqe_fatal: unknown op=%u ud=0x%llx\n",
					static_cast<unsigned>(static_cast<std::uint8_t>(op)),
					static_cast<unsigned long long>(cqe->user_data));
				break;
			}
		}
	} catch (std::exception const &e) {
		auto _ = std::fprintf(stderr, "dispatch_cqe_fatal: suppressed std::exception: %s\n", e.what());
	} catch (...) { auto _ = std::fputs("dispatch_cqe_fatal: suppressed unknown std::exception\n", stderr); }
}

void Ring::emit_ring_diagnostics() noexcept {
	try {
		auto const features_str = caps_to_log_string(caps);
		eprintln(std::format("ring_features={}", features_str.empty() ? "none" : features_str));
		std::uint32_t const overflow_now = raw_.ring().cq_overflow_count();
		eprintln(std::format("ring_cq_overflow={}", overflow_now));
		if (fatal_cq_overflow_count_ > 0) {
			eprintln(
				std::format(
					"ring_cq_overflow_delta={}",
					overflow_now > fatal_cq_overflow_count_ ? overflow_now - fatal_cq_overflow_count_ : 0u));
		}
		eprintln(std::format("ring_sq_busy={}", io_uring_sq_ready(&ring)));
		{
			std::uint32_t const v = ring.sq.kdropped != nullptr ? *ring.sq.kdropped : 0u;
			eprintln(std::format("ring_sq_dropped={}", v));
		}
		// Parse fdinfo for CqOverflowList (overflow list depth, Linux 6.x+)
		int const rfd = ring.ring_fd;
		if (rfd >= 0) {
			auto const path = std::format("/proc/self/fdinfo/{}", rfd);
			if (auto fdinfo = blocking_read_text_file_nothrow(path, std::size_t{64} * 1024)) {
				for (auto const line: LineRange{*fdinfo}) {
					if (line.text.starts_with("CqOverflowList:")) {
						auto pos = line.text.find(':');
						if (pos != std::string_view::npos) {
							eprintln(std::format("ring_cq_overflow_list={}", line.text.substr(pos + 1)));
						}
					}
				}
			}
		}
		if (fatal_reason_ != ServerFatalReason::none) {
			std::string_view reason_str;
			switch (fatal_reason_) {
			case ServerFatalReason::cq_overflow          : reason_str = "cq_overflow"; break;
			case ServerFatalReason::cq_overflow_no_nodrop: reason_str = "cq_overflow_no_nodrop"; break;
			case ServerFatalReason::submit_wait_ebadr    : reason_str = "submit_wait_ebadr"; break;
			case ServerFatalReason::internal_exception   : reason_str = "internal_exception"; break;
			default                                      : reason_str = "unknown"; break;
			}
			eprintln(std::format("ring_fatal_reason={}", reason_str));
		}
		if (overflow_flush_limit_hit_) {
			eprintln("ring_overflow_flush_limit_hit=1");
		}
	} catch (std::exception const &e) {
		auto _ = std::fprintf(stderr, "emit_ring_diagnostics: suppressed std::exception: %s\n", e.what());
	} catch (...) { auto _ = std::fputs("emit_ring_diagnostics: suppressed unknown std::exception\n", stderr); }
}

void Ring::flush_overflow_cqes_until_clear_or_limit() noexcept {
	static constexpr unsigned max_iters = 16;
	static constexpr unsigned BATCH = 256;
	for (unsigned i = 0; i < max_iters && ring_integrity_suspect(); ++i) {
		io_uring_get_events(&ring);
		std::array<io_uring_cqe *, BATCH> cqes{};
		unsigned const n = io_uring_peek_batch_cqe(&ring, cqes.data(), BATCH);
		if (n == 0) {
			break;
		}
		for (unsigned j = 0; j < n; ++j) {
			dispatch_cqe_fatal(cqes[j]); // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
		}
		io_uring_cq_advance(&ring, n);
	}
	if (ring_integrity_suspect()) {
		overflow_flush_limit_hit_ = true;
	}
	emit_ring_diagnostics();
	close_tracked_fds_sync();
}
