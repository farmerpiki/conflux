import conflux.net.router;

template<class T>
concept has_global_request_handler = ::RequestHandler<T>;

static_assert(has_global_request_handler<int>);
