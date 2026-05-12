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
	// Blocks until ring 0 has bound and called listen(); returns the actual port.
	// Safe to call from any thread after run() has been dispatched.
	[[nodiscard]] u16 port() const;

	HttpServer(HttpServer const &) = delete;
	HttpServer &operator =(HttpServer const &) = delete;
	HttpServer(HttpServer &&) = delete;
	HttpServer &operator =(HttpServer &&) = delete;
};
