import conflux.net.router;

template<class T>
concept has_global_context_middleware_function = ::ContextMiddlewareFunction<T>;

static_assert(has_global_context_middleware_function<int>);
