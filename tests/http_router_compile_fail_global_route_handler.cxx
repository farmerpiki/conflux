import conflux.net.router;

template<class T>
concept has_global_route_handler = ::RouteHandler<T>;

static_assert(has_global_route_handler<int>);
