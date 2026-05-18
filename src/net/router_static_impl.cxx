module;
#include <fcntl.h>
#include <unistd.h>

module conflux.net.router_static;

import std;
import conflux.types;
import conflux.net.http.server_types;
import conflux.net.http.response;
import conflux.net.http.static_files;
import conflux.net.http.static_core;
import conflux.net.http.static_async;
import conflux.net.config;

[[nodiscard]] StaticRouteRegistration make_static_route_registration(
	std::string_view url_prefix,
	std::string root_dir,
	StaticOptions const &sopts,
	StaticFileCacheConfig const &static_file_cache,
	StaticCacheStore &static_cache) {
	while (!root_dir.empty() && root_dir.back() == '/') {
		root_dir.pop_back();
	}

	auto pattern = std::string{url_prefix} + "/{*file}";
	auto effective_sopts = sopts;
	if (!effective_sopts.file_cache.enabled) {
		effective_sopts.file_cache = static_file_cache;
	}
	auto root_dir_fd = std::shared_ptr<int>{
		new int(::open(root_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)),
		[](int *fd) {
			if (fd != nullptr) {
				if (*fd >= 0) {
					::close(*fd);
				}
				delete fd;
			}
		}};
	auto rd = move(root_dir);

	StaticRouteRegistration routes{
		.pattern = move(pattern),
		.get = [rd, root_dir_fd, sopts = effective_sopts, &static_cache](HttpRequestView const &req) -> HttpResponse {
			return handle_static_get_request(rd, *root_dir_fd, sopts, req, static_cache);
		},
	};

	if (effective_sopts.allow_put) {
		routes.put = StaticRouteHandler{[rd, root_dir_fd, sopts = effective_sopts, &static_cache](HttpRequestView const &req) -> HttpResponse {
			return handle_static_put(rd, *root_dir_fd, sopts, req, static_cache);
		}};
	}

	if (effective_sopts.allow_delete) {
		routes.del = StaticRouteHandler{[rd, root_dir_fd, sopts = effective_sopts, &static_cache](HttpRequestView const &req) -> HttpResponse {
			return handle_static_delete(rd, *root_dir_fd, sopts, req, static_cache);
		}};
	}

	return routes;
}
