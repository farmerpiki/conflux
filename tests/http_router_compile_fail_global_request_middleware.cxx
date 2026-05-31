import conflux.net.router;

template<class T>
concept has_global_request_middleware = ::RequestMiddleware<T>;

static_assert(has_global_request_middleware<int>);
