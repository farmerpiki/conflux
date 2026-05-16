export module conflux.net.http_server;

import conflux.types;
export import conflux.net.http.server_types;
import conflux.net.router;
import conflux.net.vhost;
import conflux.net.config;

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
