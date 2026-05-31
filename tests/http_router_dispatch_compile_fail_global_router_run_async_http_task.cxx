import conflux.net.router_dispatch;

auto probe() {
	return &::router_run_async_http_task;
}
