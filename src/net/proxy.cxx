export module conflux.net.proxy;

import std;

import conflux.types;
import conflux.work;
import conflux.net.router;
import conflux.socket_io;

namespace wroot = conflux::work::root;

// async DNS not yet ring-safe; upstream_host must be a numeric IP or opts.work_pool must be set.
export struct ProxyOptions {
	std::string upstream_host;
	std::uint16_t upstream_port{80};
	std::string path_prefix{};
	bool preserve_host{false};
	bool upstream_tls{false};
	int timeout_sec{10};
};

export HttpResponse blocking_proxy(HttpRequestView const &req, ProxyOptions const &opts);

export wroot::Task<HttpResponse> async_proxy(HttpRequest const &req, ProxyOptions const &opts, SocketTaskRing &ring);
