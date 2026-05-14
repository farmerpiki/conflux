export module conflux.net.http_server;

import conflux.types;
import conflux.net.router;
import conflux.net.vhost;
import conflux.net.config;

export enum class RunStatus : u8 {
	stopped_normally,
	fatal_cq_overflow,
	fatal_cq_overflow_no_nodrop,
	fatal_submit_wait_ebadr,
	fatal_internal_exception,

};

export struct SendZcMetrics {
	u64 attempts{};
	u64 bytes_requested{};
	u64 bytes_sent{};
	u64 notifications{};
	u64 copied_notifications{};
	u64 sends_without_notification{};
	u64 errors_enomem{};
	u64 errors_other{};
	u64 fallback_regular_send{};
	u64 adaptive_disable_count{};
};

export enum class SendZcPendingAction : u8 {
	none,
	complete_response,
	resubmit_response,
	close_after_error,
};

export enum class SendZcCqeAction : u8 {
	none,
	complete_response,
	resubmit_response,
	close_after_error,
	close_after_notification,
};

export struct SendZcCqeState {
	bool waiting_notification{};
	bool close_after_notification{};
	SendZcPendingAction after_notification{SendZcPendingAction::none};
};

export struct SendZcCqeInput {
	int result{};
	bool notification{};
	bool more{};
	bool copied{};
	bool enomem_error{};
	SZ written_before{};
	SZ response_total{};
};

export struct SendZcCqeOutcome {
	SendZcCqeAction action{SendZcCqeAction::none};
	SZ bytes_sent{};
	bool adaptive_disabled{};
};

export [[nodiscard]] SendZcCqeOutcome observe_send_zc_cqe(
	SendZcCqeState &state,
	SendZcMetrics &metrics,
	SendZcCqeInput input,
	bool &send_zc_enabled) noexcept {
	SendZcCqeOutcome out{};
	if (input.notification) {
		++metrics.notifications;
		if (input.copied) {
			++metrics.copied_notifications;
			if (send_zc_enabled
				&& metrics.attempts >= 1024
				&& metrics.bytes_requested >= SZ{16} * 1024 * 1024
				&& metrics.copied_notifications * 10 > metrics.notifications * 9) {
				send_zc_enabled = false;
				++metrics.adaptive_disable_count;
				out.adaptive_disabled = true;
			}
		}
		state.waiting_notification = false;
		if (state.close_after_notification) {
			state.close_after_notification = false;
			state.after_notification = SendZcPendingAction::none;
			out.action = SendZcCqeAction::close_after_notification;
			return out;
		}
		auto const action = state.after_notification;
		state.after_notification = SendZcPendingAction::none;
		switch (action) {
		case SendZcPendingAction::complete_response: out.action = SendZcCqeAction::complete_response; break;
		case SendZcPendingAction::resubmit_response: out.action = SendZcCqeAction::resubmit_response; break;
		case SendZcPendingAction::close_after_error: out.action = SendZcCqeAction::close_after_error; break;
		default: break;
		}
		return out;
	}

	auto const note_error = [&] {
		if (input.enomem_error) {
			++metrics.errors_enomem;
		} else {
			++metrics.errors_other;
		}
	};

	if (input.more) {
		state.waiting_notification = true;
		if (input.result < 0) {
			note_error();
			state.after_notification = SendZcPendingAction::close_after_error;
			return out;
		}
		out.bytes_sent = static_cast<SZ>(input.result);
		metrics.bytes_sent += out.bytes_sent;
		state.after_notification = input.written_before + out.bytes_sent >= input.response_total
			? SendZcPendingAction::complete_response
			: SendZcPendingAction::resubmit_response;
		return out;
	}

	++metrics.sends_without_notification;
	if (input.result < 0) {
		note_error();
		out.action = SendZcCqeAction::close_after_error;
		return out;
	}
	out.bytes_sent = static_cast<SZ>(input.result);
	metrics.bytes_sent += out.bytes_sent;
	out.action = input.written_before + out.bytes_sent < input.response_total
		? SendZcCqeAction::resubmit_response
		: SendZcCqeAction::complete_response;
	return out;
}

export struct HttpServerMetrics {
	u64 sq_dropped{};
	u64 cq_overflow{};
	u64 accepted_direct_failures{};
	u64 zc_notifications_pending{};
	u64 recv_bundle_cqes{};
	u64 recv_bundle_slices{};
	u64 recv_bundle_bytes{};
	SendZcMetrics send_zc{};
};

export class HttpServer {
	struct Impl;
	Impl *impl_{};
	void initialize(Config const &cfg);

public:
	explicit HttpServer(Config const &cfg, Router &&router);
	explicit HttpServer(Config const &cfg, VHostRouter &&vhost_router);
	~HttpServer();

	// Thread-safe and async-signal-safe. Wakes every ring via its shutdown eventfd.
	void request_shutdown() noexcept;
	// Thread-safe normal shutdown. Wakes rings, stops HTTP/3 listener if present,
	// and lets run() drain in-flight responses before exiting.
	void shutdown();
	[[nodiscard]] RunStatus run() noexcept;
	// Snapshot counters accumulated by all rings. Intended after run() returns;
	// no synchronization is provided for concurrent calls while rings are active.
	[[nodiscard]] HttpServerMetrics metrics() const noexcept;
	// Blocks until ring 0 has bound and called listen(); returns the actual port.
	// Safe to call from any thread after run() has been dispatched.
	[[nodiscard]] u16 port() const;

	HttpServer(HttpServer const &) = delete;
	HttpServer &operator =(HttpServer const &) = delete;
	HttpServer(HttpServer &&) = delete;
	HttpServer &operator =(HttpServer &&) = delete;
};
