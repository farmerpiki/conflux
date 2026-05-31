import conflux.net.router;

template<class T>
concept has_global_view_middleware = ::ViewMiddleware<T>;

static_assert(has_global_view_middleware<int>);
