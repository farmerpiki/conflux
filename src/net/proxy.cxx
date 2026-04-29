// HTTP reverse proxy: forwards requests to an upstream server.
// Uses HttpClient::send_blocking (Phase 1). Phase 2 will migrate to async.
module;

export module conflux.net.proxy;
import std;
import conflux.types;
export import conflux.work;
import conflux.net.http.types;
import conflux.net.http.request;
import conflux.net.client;
import conflux.net.router;
import conflux.utils;

namespace http = conflux::http;
using http::HttpClient;
using http::HttpClientOptions;
using http::HttpTimeouts;

export struct ProxyOptions {
	S upstream_host;
	u16 upstream_port{80};
	S path_prefix{};
	bool preserve_host{false};
	bool upstream_tls{false};
	int timeout_sec{10};
	SP<WorkPool> work_pool{};
};

namespace {

constexpr A<SV, 8> kHopByHop{
	"connection",
	"keep-alive",
	"proxy-authenticate",
	"proxy-authorization",
	"te",
	"trailers",
	"transfer-encoding",
	"upgrade",
};

} // namespace

namespace proxy_detail {

HttpResponse perform_proxy_request(
	HttpRequestView const &req,
	ProxyOptions const &opts) {
	S up_path{req.path};
	if (!opts.path_prefix.empty() && up_path.starts_with(opts.path_prefix)) {
		up_path.erase(0, opts.path_prefix.size());
	}
	if (up_path.empty()) {
		up_path = "/";
	}

	S const scheme = opts.upstream_tls ? "https" : "http";
	S const url_str = format("{}://{}:{}{}", scheme, opts.upstream_host, opts.upstream_port, up_path);

	auto builder = http::HttpRequest::method(req.method, url_str);

	// Forward non-hop-by-hop headers.
	for (auto const &[name, value]: req.headers) {
		if (name == "host") {
			continue;
		}
		if (ranges::contains(kHopByHop, name)) {
			continue;
		}
		builder.header(name, value);
	}

	// X-Forwarded-For.
	auto xff = S{req.headers["x-forwarded-for"]};
	if (xff.empty()) {
		builder.header("X-Forwarded-For", req.remote_addr);
	} else {
		builder.header("X-Forwarded-For", format("{}, {}", xff, req.remote_addr));
	}

	// Host header.
	if (opts.preserve_host) {
		builder.header("Host", req.headers["host"]);
	}

	if (!req.body.empty()) {
		builder.body_view(req.body);
	}

	HttpTimeouts timeouts{};
	timeouts.connect = chrono::milliseconds{opts.timeout_sec * 1000};
	timeouts.first_byte = chrono::milliseconds{opts.timeout_sec * 1000};
	timeouts.between_bytes = chrono::milliseconds{opts.timeout_sec * 1000};
	builder.timeouts(timeouts);

	HttpClientOptions client_opts{};
	client_opts.default_timeouts = timeouts;
	HttpClient client{std::move(client_opts)};

	auto result = client.send_blocking(std::move(builder).build());
	if (!result) {
		return HttpResponse::internal_error(
			format("proxy: {} ({})", result.error().message, static_cast<int>(result.error().kind)));
	}

	HttpResponse out;
	out.status = result->head.status;
	out.status_text = std::move(result->head.status_text);
	out.headers = std::move(result->head.headers);
	out.set_cookies = std::move(result->head.set_cookies);
	// Propagate content-type from headers (it's now in headers, not a separate field).
	if (auto const ct = out.headers["content-type"]; !ct.empty()) {
		out.content_type = S{ct};
	}
	out.set_text_body(std::move(result->body));
	return out;
}

} // namespace proxy_detail

export Router::Handler proxy_handler(
	ProxyOptions opts) {
	return [opts = std::move(opts)](HttpRequestView const &req) -> HttpResponse {
		if (!opts.work_pool) {
			return proxy_detail::perform_proxy_request(req, opts);
		}
		auto deferred = make_shared<DeferredResponse>();
		auto owned = req.to_owned();
		if (!opts.work_pool->enqueue([deferred, opts, owned = std::move(owned)]() mutable {
				deferred->complete(proxy_detail::perform_proxy_request(HttpRequestView{owned}, opts));
			})) {
			return HttpResponse::internal_error("proxy: work pool");
		}
		return HttpResponse::deferred(std::move(deferred));
	};
}
