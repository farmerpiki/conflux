export module conflux.net.router_static;

import std;
import conflux.types;
import conflux.net.http.server_types;
import conflux.net.http.response;
import conflux.net.http.static_files;
import conflux.net.http.static_core;
import conflux.net.config;

export namespace conflux::http {

using StaticRouteHandler = conflux::http::CloneableFunction<Response(conflux::http::RequestView const &)>;

struct StaticRouteRegistration {
	std::string pattern{};
	StaticRouteHandler get{};
	std::optional<StaticRouteHandler> put{};
	std::optional<StaticRouteHandler> del{};
};

[[nodiscard]] StaticRouteRegistration make_static_route_registration(
	std::string_view url_prefix,
	std::string root_dir,
	conflux::http::StaticOptions const &sopts,
	conflux::http::StaticFileCacheConfig const &static_file_cache,
	conflux::http::detail::StaticCacheStore &static_cache);

} // namespace conflux::http
