export module conflux.net.proxy;

import std;

import conflux.types;
import conflux.work;
import conflux.net.router;
import conflux.socket_io;

namespace wroot = conflux::work::root;

export namespace conflux::http {

struct ProxyOptions {
	std::string upstream_host;
	std::uint16_t upstream_port{80};
	std::string path_prefix{};
	bool preserve_host{false};
	bool upstream_tls{false};
	int timeout_sec{10};
};

conflux::http::Response blocking_proxy(conflux::http::RequestView const &req, ProxyOptions const &opts);

wroot::Task<conflux::http::Response>
async_proxy(conflux::http::RequestView const &req, ProxyOptions const &opts, SocketTaskRing &ring);

} // namespace conflux::http
