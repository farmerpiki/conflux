import conflux.net.router;

template<class T>
concept has_global_context_handler_function = ::ContextHandlerFunction<T>;

static_assert(has_global_context_handler_function<int>);
