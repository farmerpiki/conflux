export module conflux.net.http_server;

import conflux.types;
import std;
export import conflux.net.http.server_types;
import conflux.net.router;
import conflux.net.vhost;
import conflux.net.config;

export class HttpServer {
	struct Impl;
	Impl *impl_{};
	void initialize(conflux::http::Config const &cfg);

public:
	explicit HttpServer(conflux::http::Config const &cfg, Router &&router);
	explicit HttpServer(conflux::http::Config const &cfg, VHostRouter &&vhost_router);
	~HttpServer();

	[[nodiscard]] static std::expected<std::unique_ptr<HttpServer>, std::string>
	try_create(conflux::http::Config const &cfg, Router &&router);
	[[nodiscard]] static std::expected<std::unique_ptr<HttpServer>, std::string>
	try_create(conflux::http::Config const &cfg, VHostRouter &&vhost_router);

	// Thread-safe and async-signal-safe. Wakes every ring via its shutdown eventfd.
	void request_shutdown() noexcept;
	// Thread-safe normal shutdown. Wakes rings, stops HTTP/3 listener if present,
	// and lets run() drain in-flight responses before exiting.
	void shutdown();
	[[nodiscard]] conflux::http::DrainReport drain(conflux::http::DrainOptions options = {});
	[[nodiscard]] conflux::http::RunStatus run() noexcept;
	// Snapshot counters accumulated by all rings. Intended after run() returns;
	// no synchronization is provided for concurrent calls while rings are active.
	[[nodiscard]] HttpServerMetrics metrics() const noexcept;
	[[nodiscard]] std::string startup_report() const;
	void set_observability_hooks(HttpServerObservabilityHooks hooks);
	// Blocks until ring 0 has bound and called listen(); returns the actual port.
	// Safe to call from any std::thread after run() has been dispatched.
	[[nodiscard]] std::uint16_t port() const;

	HttpServer(HttpServer const &) = delete;
	HttpServer &operator =(HttpServer const &) = delete;
	HttpServer(HttpServer &&) = delete;
	HttpServer &operator =(HttpServer &&) = delete;
};
