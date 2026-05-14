export module conflux.net.router_static;

import std;
import conflux.types;
import conflux.net.http.server_types;
import conflux.net.http.response;
import conflux.net.http.static_files;
import conflux.net.http.static_core;
import conflux.net.config;

export using StaticRouteHandler = CloneableFunction<HttpResponse(HttpRequestView const &)>;

export struct StaticRouteRegistration {
	S pattern{};
	StaticRouteHandler get{};
	Opt<StaticRouteHandler> put{};
	Opt<StaticRouteHandler> del{};
};

export [[nodiscard]] StaticRouteRegistration make_static_route_registration(
	SV url_prefix,
	S root_dir,
	StaticOptions const &sopts,
	StaticFileCacheConfig const &static_file_cache,
	StaticCacheStore &static_cache);
