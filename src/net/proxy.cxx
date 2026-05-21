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

export Response blocking_proxy(RequestView const &req, ProxyOptions const &opts);

export wroot::Task<Response> async_proxy(Request const &req, ProxyOptions const &opts, SocketTaskRing &ring);
