module;

module conflux.net.router_static;

import std;
import conflux.types;
import conflux.net.http.server_types;
import conflux.net.http.response;
import conflux.net.http.static_files;
import conflux.net.http.static_core;
import conflux.net.http.static_async;
import conflux.net.config;
import conflux.file_io_sync;

[[nodiscard]] StaticRouteRegistration make_static_route_registration(
	std::string_view url_prefix,
	std::string root_dir,
	StaticOptions const &sopts,
	conflux::http::StaticFileCacheConfig const &static_file_cache,
	StaticCacheStore &static_cache) {
	while (!root_dir.empty() && root_dir.back() == '/') {
		root_dir.pop_back();
	}

	std::string pattern;
	pattern.reserve(url_prefix.size() + sizeof("/{*file}") - 1);
	pattern.append(url_prefix.data(), url_prefix.size());
	pattern += "/{*file}";
	auto effective_sopts = sopts;
	if (!effective_sopts.file_cache.enabled) {
		effective_sopts.file_cache = static_file_cache;
	}
	auto root_dir_file = blocking_open_directory(root_dir);
	auto root_dir_fd = std::make_shared<UniqueFd>(root_dir_file ? std::move(*root_dir_file) : UniqueFd{});
	auto rd = std::move(root_dir);

	StaticRouteRegistration routes{
		.pattern = std::move(pattern),
		.get = [rd, root_dir_fd, sopts = effective_sopts, &static_cache](RequestView const &req) -> Response {
			return handle_static_get_request(rd, root_dir_fd->fd(), sopts, req, static_cache);
		},
	};

	if (effective_sopts.allow_put) {
		routes.put = StaticRouteHandler{
			[rd, root_dir_fd, sopts = effective_sopts, &static_cache](RequestView const &req) -> Response {
				return handle_static_put(rd, root_dir_fd->fd(), sopts, req, static_cache);
			}};
	}

	if (effective_sopts.allow_delete) {
		routes.del = StaticRouteHandler{
			[rd, root_dir_fd, sopts = effective_sopts, &static_cache](RequestView const &req) -> Response {
				return handle_static_delete(rd, root_dir_fd->fd(), sopts, req, static_cache);
			}};
	}

	return routes;
}
