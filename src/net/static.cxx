export module conflux.net.http.static_files;

import std;
import conflux.types;
import conflux.work;
import conflux.net.config;

export struct StaticOptions {
	// Cache-Control header value. Empty = no Cache-Control header set.
	S cache_control{"max-age=3600, public"};
	// Serve pre-compressed .gz or .br sidecars when the client accepts them.
	bool precompressed{true};
	// Generate an HTML directory listing when no index.html is found.
	bool directory_listing{false};
	// When set, stat/open/mmap happen on this pool's threads via DeferredResponse,
	// keeping the io_uring thread free while slow disks resolve.
	SP<WorkPool> offload_pool{};
	// Small static file cache. Disabled by default to preserve existing memory
	// behavior unless callers opt in via Config/Router defaults or per route.
	StaticFileCacheConfig file_cache{};
	bool allow_put{false};
	bool allow_delete{false};
};
