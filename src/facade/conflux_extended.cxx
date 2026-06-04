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

#if CONFLUX_SURFACE_HAS_HTTP_APP
export import conflux.http.extended;
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
