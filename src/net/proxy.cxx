export module conflux.net.proxy;

import conflux.types;
import conflux.work;
import conflux.net.router;
import conflux.socket_io;

namespace wroot = conflux::work::root;

// async DNS not yet ring-safe; upstream_host must be a numeric IP or opts.work_pool must be set.
export struct ProxyOptions {
	S upstream_host;
	u16 upstream_port{80};
	S path_prefix{};
	bool preserve_host{false};
	bool upstream_tls{false};
	int timeout_sec{10};
};

export HttpResponse blocking_proxy(
	HttpRequestView const &req,
	ProxyOptions const &opts);

export wroot::Task<HttpResponse> async_proxy(
	HttpRequest const &req,
	ProxyOptions const &opts,
	SocketTaskRing &ring);
