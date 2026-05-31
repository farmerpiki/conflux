import conflux.net.router;

template<class T>
concept has_global_async_middleware = ::AsyncMiddleware<T>;

static_assert(has_global_async_middleware<int>);
