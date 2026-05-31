import conflux.net.router;

template<class T>
concept has_global_view_handler = ::ViewHandler<T>;

static_assert(has_global_view_handler<int>);
