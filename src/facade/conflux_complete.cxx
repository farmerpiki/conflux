export module conflux.complete;

export import conflux.extended;

#if CONFLUX_SURFACE_HAS_URING
export import conflux.uring;
export import conflux.uring.completion;
export import conflux.uring.fd;
export import conflux.uring.flow;
export import conflux.uring.handle;
#endif

#if CONFLUX_SURFACE_HAS_URING_TIMEOUT
export import conflux.uring.timeout;
#endif

#if CONFLUX_SURFACE_HAS_FILE_IO
export import conflux.file_io;
export import conflux.file_io.buffers;
export import conflux.file_io.driver;
export import conflux.file_io.iopoll;
export import conflux.file_io.pipe_pool;
export import conflux.file_io.reader;
#endif

#if CONFLUX_SURFACE_HAS_SOCKET_IO
export import conflux.socket_io;
export import conflux.socket_io.blocking;
export import conflux.socket_io.coro;
#endif

#if CONFLUX_SURFACE_HAS_DNS
export import conflux.net.dns;
#endif

#if CONFLUX_SURFACE_HAS_DNS_BRIDGE
export import conflux.dns_bridge;
#endif

#if CONFLUX_SURFACE_HAS_NET_IO_BUFFER
export import conflux.net.io_buffer;
#endif

#if CONFLUX_SURFACE_HAS_NET_CANCEL
export import conflux.net.cancel;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_FACADE
export import conflux.net.http;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_SERVER
export import conflux.net.http.server;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_PROTOCOL
export import conflux.net.http.protocol;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_PARSE_HELPERS
export import conflux.net.http.parse_helpers;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_ROUTER
export import conflux.net.router;
export import conflux.net.router_dispatch;
export import conflux.net.router_match;
export import conflux.net.router_static;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_STATIC
export import conflux.net.http.static_async;
export import conflux.net.http.static_files;
#endif

#if CONFLUX_SURFACE_HAS_HTTP2
export import conflux.net.http2;
#endif

#if CONFLUX_SURFACE_HAS_HTTP3
export import conflux.net.http3;
#endif

#if CONFLUX_SURFACE_HAS_FILE_WATCH
export import conflux.file_watch;
#endif

#if CONFLUX_SURFACE_HAS_SMTP
export import conflux.net.smtp;
#endif
