export module conflux.net.proxy;

import std;

import conflux.types;
import conflux.work;
import conflux.net.router;
import conflux.socket_io;

namespace wroot = conflux::work::root;

export struct ProxyOptions {
	std::string upstream_host;
	std::uint16_t upstream_port{80};
	std::string path_prefix{};
	bool preserve_host{false};
	bool upstream_tls{false};
	int timeout_sec{10};
};

export conflux::http::Response blocking_proxy(RequestView const &req, ProxyOptions const &opts);

export wroot::Task<conflux::http::Response>
async_proxy(RequestView const &req, ProxyOptions const &opts, SocketTaskRing &ring);
