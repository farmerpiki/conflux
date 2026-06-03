export module conflux.extended;

export import conflux.curated;

#if CONFLUX_SURFACE_HAS_WORK
export import conflux.work;
#endif

#if CONFLUX_SURFACE_HAS_FILE_IO_SYNC
export import conflux.file_io_sync;
#endif

#if CONFLUX_SURFACE_HAS_FILE_MAP
export import conflux.file_map;
#endif

#if CONFLUX_SURFACE_HAS_JSON_BOUNDARY && !CONFLUX_SURFACE_HAS_JSON_REFLECT_PROVIDER
export import conflux.json.boundary;
#endif

#if CONFLUX_SURFACE_HAS_JSON_NATIVE_PROVIDER && !CONFLUX_SURFACE_HAS_JSON_REFLECT_PROVIDER
export import conflux.json.native_provider;
#endif

#if CONFLUX_SURFACE_HAS_JSON_REFLECT && !CONFLUX_SURFACE_HAS_JSON_REFLECT_PROVIDER
export import conflux.json.reflect;
#endif

#if CONFLUX_SURFACE_HAS_JSON_REFLECT_PROVIDER
export import conflux.json.reflect_provider;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_CONFIG
export import conflux.net.config;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_CLIENT
export import conflux.net.http.client;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_APP
export import conflux.http.extended;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_AUTH
export import conflux.net.auth;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_POLICY
export import conflux.net.cors;
export import conflux.net.rate_limit;
export import conflux.net.security;
export import conflux.net.forwarded;
export import conflux.net.request_id;
export import conflux.net.ip_filter;
export import conflux.net.cache_control;
export import conflux.net.trailing_slash;
export import conflux.net.redirect;
export import conflux.net.cookie_signing;
export import conflux.net.csrf;
export import conflux.net.etag;
export import conflux.net.response_cache;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_OBSERVABILITY
export import conflux.net.observability;
export import conflux.net.structured_log;
export import conflux.net.tracing;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_METRICS
export import conflux.net.metrics;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_OPENAPI
export import conflux.net.openapi;
#endif

#if CONFLUX_SURFACE_HAS_HTTP_VHOST
export import conflux.net.vhost;
#endif

#if CONFLUX_SURFACE_HAS_TLS_JWT
export import conflux.net.jwt;
#endif

#if CONFLUX_SURFACE_HAS_CRYPTO
export import conflux.crypto;
#endif

#if CONFLUX_SURFACE_HAS_TEMPLATES
export import conflux.templates;
#endif

#if CONFLUX_SURFACE_HAS_TEMPLATES_WATCH
export import conflux.templates.watch;
#endif

#if CONFLUX_SURFACE_HAS_PROCESS
export import conflux.process;
#endif

#if CONFLUX_SURFACE_HAS_DB
export import conflux.pg;
#endif
