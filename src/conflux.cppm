export module conflux;

#if CONFLUX_API_SURFACE_LEVEL == CONFLUX_API_SURFACE_CURATED
export import conflux.curated;
#elif CONFLUX_API_SURFACE_LEVEL == CONFLUX_API_SURFACE_EXTENDED
export import conflux.extended;
#elif CONFLUX_API_SURFACE_LEVEL == CONFLUX_API_SURFACE_COMPLETE
export import conflux.complete;
#else
	#error "invalid CONFLUX_API_SURFACE_LEVEL"
#endif
