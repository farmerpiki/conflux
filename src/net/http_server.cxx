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

#ifdef CONFLUX_BUILD_FUZZ
export void parse_urlencoded(SV data, HttpFieldsView &out);
export i64 decode_chunked(SV data, SZ max_body_size, SZ max_chunks, S &body);
#endif

export class HttpServer {
	struct Impl;
	Impl *impl_{};
	void initialize(Config const &cfg);

public:
	explicit HttpServer(Config const &cfg, Router &&router);
	explicit HttpServer(Config const &cfg, VHostRouter &&vhost_router);
	~HttpServer();

	// Thread-safe and async-signal-safe. Signals all rings to stop accepting,
	// drain in-flight responses, and exit. run() returns once all rings stop.
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
