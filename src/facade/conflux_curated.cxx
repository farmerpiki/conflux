export module conflux.curated;

export import conflux.features;

// Keep HTTP as an explicit leaf import for now. Re-exporting conflux.http
// through profile modules currently trips GCC 16 module namespace import ICEs.

#if CONFLUX_SURFACE_HAS_JSON
export import conflux.json;
#endif

#if CONFLUX_SURFACE_HAS_JSON_FILE
export import conflux.json.file;
#endif
