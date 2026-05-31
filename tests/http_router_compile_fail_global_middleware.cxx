import conflux.net.router;

template<class T>
concept has_global_middleware = ::Middleware<T>;

static_assert(has_global_middleware<int>);
