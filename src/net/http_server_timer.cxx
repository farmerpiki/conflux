module;
#include <cstddef> // must precede openssl/nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" blocks re-include c++config.h

#if CONFLUX_HAS_TLS
	#include <openssl/ssl.h>
#endif
#if CONFLUX_HAS_HTTP2
	#include <nghttp2/nghttp2.h>
#endif
#include <liburing.h>

module conflux.net.http_server:timer;

import std;
import std.compat;

import conflux.net.http.types;
import conflux.net.http.response;
import conflux.net.http_server_helpers;
import conflux.uring;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
import :state;

using conflux::uring::DirectFd;
using conflux::uring::OsFd;

namespace {

[[nodiscard]] bool incomplete_h1_headers(
	Conn const &conn) {
	auto const bytes = conn.partial.view();
	return !bytes.empty() && bytes.find("\r\n\r\n") == std::string_view::npos;
}

void emit_timeout_rejection(
	Conn &conn,
	Ring &ring,
	conflux::http::HttpRejectReason reason) {
	auto r = conflux::http::make_rejection_response(reason);
	{
		std::scoped_lock lk{ring.metrics_mu_};
		conflux::http::note_rejection(ring.rejection_counters_, reason);
	}
	if (ring.observability_hooks_.rejection) {
		ring.observability_hooks_.rejection(reason, r.status);
	}
	conn.own_response = conflux::http::format_response(r, ring.alt_svc_header, true);
	conn.has_response = true;
	conn.close_after_send = true;
}

} // namespace

void Ring::arm_timer() {
	if (shutting_down) {
		bool pending_force_close = false;
		bool drain_deadline_pending = false;
		auto const now = std::chrono::steady_clock::now();
		if (drain_control != nullptr
			&& drain_control->active.load(std::memory_order_acquire)
			&& now < drain_control->deadline) {
			drain_deadline_pending = true;
		}
		for (auto const &conn: fd_table) {
			if (conn.fd >= 0 && conn.send_queued && conn.close_after_send) {
				pending_force_close = true;
				break;
			}
		}
		if (!pending_force_close && !drain_deadline_pending && request_timeout_ms == 0 && tls_sniff_timeout_ms == 0) {
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

bool Ring::handle_drain_deadline(
	std::chrono::steady_clock::time_point now) {
	bool const drain_active = drain_control != nullptr && drain_control->active.load(std::memory_order_acquire);
	if (!drain_active || now < drain_control->deadline) {
		return false;
	}
	drain_control->deadline_hit.store(true, std::memory_order_release);
	{
		std::scoped_lock lk{metrics_mu_};
		++pressure_counters_.drain_deadline_hit;
	}
	for (auto &conn: fd_table) {
		if (conn.fd >= 0 && !conn.closing) {
			if (conn.deferred_response) {
				conn.deferred_response->cancel_shutdown();
			}
			drain_control->forced_closed.fetch_add(1, std::memory_order_relaxed);
			{
				std::scoped_lock lk{metrics_mu_};
				++pressure_counters_.drain_forced_close;
			}
			queue_close(conn.fd);
		}
	}
	return true;
}

void Ring::expire_deferred_waits(
	std::chrono::steady_clock::time_point now) {
	for (auto &[_, wait]: deferred_waits) {
		if (wait.response) {
			wait.response->expire_if_past_deadline(now);
		}
	}
}

bool Ring::close_if_shutdown_send_deadline(
	Conn &conn,
	std::chrono::steady_clock::time_point now) {
	if (shutting_down && conn.send_queued && conn.close_after_send && now >= conn.close_after_send_deadline) {
		queue_close(conn.fd);
		return true;
	}
	return false;
}

#if CONFLUX_HAS_TLS
bool Ring::close_if_tls_sniff_timeout(
	Conn &conn,
	std::chrono::steady_clock::time_point now,
	std::chrono::milliseconds sniff_limit) {
	bool const sniff_undecided = conn.ssl == nullptr && conn.tls_hs_done && conn.partial.empty();
	if (!sniff_undecided || tls_sniff_timeout_ms == 0) {
		return false;
	}
	if (now - conn.last_activity > sniff_limit) {
		queue_close(conn.fd);
	}
	return true;
}
#endif

void Ring::handle_request_timeout(
	Conn &conn,
	std::chrono::steady_clock::time_point now,
	std::chrono::milliseconds req_limit) {
	if (request_timeout_ms == 0) {
		return;
	}
	auto const ref = conn.request_in_progress ? conn.request_started : conn.last_activity;
	if (now - ref <= req_limit) {
		return;
	}
	if (conn.request_in_progress) {
		auto const fd = conn.fd;
		auto reason = conflux::http::HttpRejectReason::body_timeout;
		if (incomplete_h1_headers(conn)) {
			reason = conflux::http::HttpRejectReason::header_timeout;
		}
		invalidate_recv_if_armed(fd);
		emit_timeout_rejection(conn, *this, reason);
		start_response_send(fd, conn);
	} else {
		queue_close(conn.fd);
	}
}

void Ring::handle_connection_timer(
	Conn &conn,
	std::chrono::steady_clock::time_point now,
	std::chrono::milliseconds req_limit,
	[[maybe_unused]] std::chrono::milliseconds sniff_limit) {
	if (conn.fd < 0) {
		return;
	}
	if (close_if_shutdown_send_deadline(conn, now) || conn.is_sse) {
		return;
	}
	if (conn.is_deferred) {
		if (conn.deferred_response) {
			conn.deferred_response->expire_if_past_deadline(now);
		}
		return;
	}
	if (conn.send_queued) {
		return;
	}
#if CONFLUX_HAS_TLS
	if (close_if_tls_sniff_timeout(conn, now, sniff_limit)) {
		return;
	}
#endif
	handle_request_timeout(conn, now, req_limit);
}

void Ring::handle_timer() {
	bool const drain_active = drain_control != nullptr && drain_control->active.load(std::memory_order_acquire);
	if (request_timeout_ms == 0 && tls_sniff_timeout_ms == 0 && !drain_active) {
		return;
	}
	auto now = std::chrono::steady_clock::now();
	if (handle_drain_deadline(now)) {
		arm_timer();
		return;
	}
	auto req_limit = std::chrono::milliseconds{request_timeout_ms};
	auto sniff_limit = std::chrono::milliseconds{tls_sniff_timeout_ms};
	expire_deferred_waits(now);
	for (auto &conn: fd_table) {
		handle_connection_timer(conn, now, req_limit, sniff_limit);
	}
	arm_timer(); // re-arm for next tick
}

void Ring::handle_shutdown() {
	shutting_down = true;
	auto *drain =
		drain_control != nullptr && drain_control->active.load(std::memory_order_acquire) ? drain_control : nullptr;
	{
		std::scoped_lock lk{metrics_mu_};
		++pressure_counters_.drain_started;
	}
	if (drain != nullptr) {
		drain->accepted_before_stop.fetch_add(
			static_cast<std::uint64_t>(std::ranges::count_if(fd_table, [](Conn const &conn) { return conn.fd >= 0; })),
			std::memory_order_relaxed);
	}
	if (drain == nullptr || drain->options.stop_accepting) {
		cancel_accept_or_defer();
		close_listen_socket();
	}
	auto const now = std::chrono::steady_clock::now();
	for (std::size_t i = 0; i < fd_table.size(); ++i) {
		auto &conn = fd_table[i];
		if (conn.fd < 0) {
			continue;
		}
		if (conn.sse_channel) {
			bool const close_stream = drain == nullptr
								   || !drain->options.finish_streams
								   || drain->options.sse_policy != conflux::http::DrainStreamPolicy::leave_open;
			if (close_stream) {
				conn.sse_channel->close();
				{
					std::scoped_lock lk{metrics_mu_};
					++pressure_counters_.connections_closed_for_pressure;
				}
				if (drain != nullptr) {
					drain->streams_closed.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}
		if (conn.is_ws) {
			{
				std::scoped_lock lk{metrics_mu_};
				++pressure_counters_.websocket_closed_for_pressure;
			}
			if (drain != nullptr) {
				drain->streams_closed.fetch_add(1, std::memory_order_relaxed);
			}
		}
		bool const finish_send = drain == nullptr || drain->options.finish_requests;
		bool const response_ready = response_send_ready(conn);
		bool const response_pending = conn.request_in_progress || conn.send_queued || response_ready;
		if (response_pending && finish_send) {
			if (drain != nullptr
				&& conn.request_in_progress
				&& !conn.send_queued
				&& !response_ready
				&& conn.request_bytes > 0) {
				drain->requests_finished.fetch_add(1, std::memory_order_relaxed);
			}
			conn.close_after_send = true;
			conn.close_after_send_deadline =
				drain != nullptr ? drain->deadline : now + shutdown_close_after_send_timeout;
			if (conn.recv_armed) {
				cancel_multishot_recv_or_defer(static_cast<int>(i), conn.gen);
			}
			if (response_ready) {
				start_response_send(static_cast<int>(i), conn);
			}
		} else {
			bool const idle = !conn.request_in_progress && !conn.send_queued;
			if (drain != nullptr && idle && !drain->options.close_idle) {
				continue;
			}
			if (drain != nullptr && idle) {
				drain->idle_closed.fetch_add(1, std::memory_order_relaxed);
			}
			if (conn.deferred_response) {
				conn.deferred_response->cancel_shutdown();
			}
			queue_close(static_cast<int>(i));
		}
	}
	if (drain != nullptr && drain->options.websocket_policy != conflux::http::DrainStreamPolicy::leave_open) {
		auto const closed = shutdown_active_ws_for_pressure();
		drain->accepted_before_stop.fetch_add(closed, std::memory_order_relaxed);
		{
			std::scoped_lock lk{metrics_mu_};
			pressure_counters_.websocket_closed_for_pressure += closed;
		}
		drain->streams_closed.fetch_add(closed, std::memory_order_relaxed);
	}
	arm_timer();
}
