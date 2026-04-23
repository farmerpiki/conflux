// HTTP reverse proxy: forwards requests to an upstream server over a blocking
// TCP socket. When a WorkPool is supplied, the blocking upstream exchange runs
// off-ring and wakes the server back through a deferred response handle.
module;

export module conflux.net.proxy;
import std;
import conflux.types;
export import conflux.work;
import conflux.net.client;
import conflux.net.router;
import conflux.utils;
using namespace std;

export struct ProxyOptions {
	string upstream_host;
	u16 upstream_port{80};
	// Strip this prefix from the request path before forwarding.
	string path_prefix;
	// Forward the original Host header. When false, use upstream_host.
	bool preserve_host{false};
	// Connect/recv timeout in seconds (0 = blocking until close).
	int timeout_sec{10};
	// Optional pool for off-ring upstream work. When null, proxying stays synchronous.
	shared_ptr<WorkPool> work_pool{};
};

HttpResponse perform_proxy_request(
	HttpRequestView const &req,
	ProxyOptions const &opts) {
	string up_path{req.path};
	if (!opts.path_prefix.empty() && up_path.starts_with(opts.path_prefix)) {
		up_path.erase(0, opts.path_prefix.size());
	}
	if (up_path.empty()) {
		up_path = "/";
	}

	ClientRequest creq{
		.method = string{req.method},
		.host = opts.upstream_host,
		.port = opts.upstream_port,
		.path = move(up_path),
		.headers = HttpFields{true},
		.body = string{req.body},
		.timeout_sec = opts.timeout_sec,
		.host_override = opts.preserve_host ? string{req.headers["host"]} : string{},
	};
	static constexpr array<string_view, 8> kHopByHop{
		"connection",
		"keep-alive",
		"proxy-authenticate",
		"proxy-authorization",
		"te",
		"trailers",
		"transfer-encoding",
		"upgrade",
	};
	for (auto const &[name, header_value]: req.headers) {
		if (name != "host" && !ranges::contains(kHopByHop, name)) {
			creq.headers.emplace_back(string{name}, string{header_value});
		}
	}

	auto xff = string{req.headers["x-forwarded-for"]};
	if (xff.empty()) {
		creq.headers["x-forwarded-for"] = string{req.remote_addr};
	} else {
		creq.headers["x-forwarded-for"] = format("{}, {}", xff, req.remote_addr);
	}

	auto response = http_request(creq);
	if (!response) {
		return HttpResponse::internal_error(format("proxy: {}", response.error()));
	}

	HttpResponse out;
	out.status = response->status;
	out.status_text = move(response->status_text);
	out.content_type = move(response->content_type);
	out.headers = move(response->headers);
	out.set_cookies = move(response->set_cookies);
	out.set_text_body(move(response->body));
	return out;
}

// Returns a handler (not middleware) that proxies all requests to the upstream.
// Mount it with router.get("/api/{*path}", proxy_handler({...})) etc.
export Router::Handler proxy_handler(
	ProxyOptions opts) {
	return [opts = move(opts)](HttpRequestView const &req) -> HttpResponse {
		if (!opts.work_pool) {
			return perform_proxy_request(req, opts);
		}

		auto deferred = make_shared<DeferredResponse>();
		auto owned = req.to_owned();
		if (!opts.work_pool->enqueue([deferred, opts, owned = move(owned)]() mutable {
				deferred->complete(perform_proxy_request(HttpRequestView{owned}, opts));
			})) {
			return HttpResponse::internal_error("proxy: work pool");
		}
		return HttpResponse::deferred(move(deferred));
	};
}
