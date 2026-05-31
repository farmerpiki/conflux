import conflux.net.router;

template<class T>
concept has_global_handler_result = ::HandlerResult<T>;

static_assert(has_global_handler_result<int>);
