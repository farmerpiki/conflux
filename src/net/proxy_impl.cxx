module;
#include <memory>

module conflux.net.proxy;

import std;
import conflux.net.http.types;
import conflux.net.http.request;
import conflux.net.client;
import conflux.net.async_client;
import conflux.utils;

namespace http = conflux::http;
using http::HttpClient;
using http::HttpClientOptions;
using http::HttpTimeouts;

namespace proxy_detail {

[[nodiscard]] static S build_upstream_url(
	SV path,
	ProxyOptions const &opts) {
	S up_path{path};
	if (!opts.path_prefix.empty() && up_path.starts_with(opts.path_prefix)) {
		up_path.erase(0, opts.path_prefix.size());
	}
	if (up_path.empty()) {
		up_path = "/";
	}
	SV const scheme = opts.upstream_tls ? "https" : "http";
	return format("{}://{}:{}{}", scheme, opts.upstream_host, opts.upstream_port, up_path);
}

[[nodiscard]] static HttpClientOptions make_client_opts(
	ProxyOptions const &opts) {
	HttpTimeouts t{};
	t.connect = chrono::milliseconds{opts.timeout_sec * 1000};
	t.first_byte = chrono::milliseconds{opts.timeout_sec * 1000};
	t.between_bytes = chrono::milliseconds{opts.timeout_sec * 1000};
	HttpClientOptions co{};
	co.default_timeouts = t;
	return co;
}

[[nodiscard]] static http::HttpRequest::Builder apply_headers(
	http::HttpRequest::Builder builder,
	HttpRequestView const &req,
	ProxyOptions const &opts) {
	for (auto const &[name, value]: req.headers) {
		if (name == "host") {
			continue;
		}
		if (conflux::http::is_hop_by_hop_header(name)) {
			continue;
		}
		builder.header(name, value);
	}
	auto xff = S{req.headers["x-forwarded-for"]};
	if (xff.empty()) {
		builder.header("X-Forwarded-For", req.remote_addr);
	} else {
		builder.header("X-Forwarded-For", format("{}, {}", xff, req.remote_addr));
	}
	if (opts.preserve_host) {
		builder.header("Host", req.headers["host"]);
	}
	if (!req.body.empty()) {
		builder.body_view(req.body);
	}
	return builder;
}

[[nodiscard]] static HttpResponse build_response(
	http::HttpResponse &&r) {
	HttpResponse out;
	out.status = r.head.status;
	out.status_text = move(r.head.status_text);
	out.headers = move(r.head.headers);
	out.set_cookies = move(r.head.set_cookies);
	if (auto const ct = out.headers["content-type"]; !ct.empty()) {
		out.content_type = S{ct};
	}
	out.set_text_body(move(r.body));
	return out;
}

[[nodiscard]] HttpResponse perform_proxy_request(
	HttpRequestView const &req,
	ProxyOptions const &opts) {
	auto co = make_client_opts(opts);
	co.default_timeouts.write = co.default_timeouts.connect;
	HttpClient client{move(co)};
	auto builder = apply_headers(http::HttpRequest::method(req.method, build_upstream_url(req.path, opts)), req, opts);
	builder.timeouts(client.options().default_timeouts);
	auto result = client.blocking_send(move(builder).build());
	if (!result) {
		return HttpResponse::bad_gateway(
			format("proxy: {} ({})", result.error().message, static_cast<int>(result.error().kind)));
	}
	return build_response(move(*result));
}

[[nodiscard]] wroot::Task<HttpResponse> perform_proxy_request_async(
	HttpRequest const &req,
	ProxyOptions const &opts,
	SocketTaskRing &ring) {
	auto co = make_client_opts(opts);
	co.default_timeouts.write = co.default_timeouts.connect;
	HttpClient client{move(co)};
	auto builder = apply_headers(
		http::HttpRequest::method(req.method, build_upstream_url(req.path, opts)),
		HttpRequestView{req},
		opts);
	builder.timeouts(client.options().default_timeouts);
	auto result = co_await http::async_send(client, ring, move(builder).build());
	if (!result) {
		co_return HttpResponse::bad_gateway(
			format("proxy: {} ({})", result.error().message, static_cast<int>(result.error().kind)));
	}
	co_return build_response(move(*result));
}

} // namespace proxy_detail

HttpResponse blocking_proxy(
	HttpRequestView const &req,
	ProxyOptions const &opts) {
	return proxy_detail::perform_proxy_request(req, opts);
}

HttpResponse proxy_sync(
	HttpRequestView const &req,
	ProxyOptions const &opts) {
	return blocking_proxy(req, opts);
}

wroot::Task<HttpResponse> async_proxy(
	HttpRequest const &req,
	ProxyOptions const &opts,
	SocketTaskRing &ring) {
	co_return co_await proxy_detail::perform_proxy_request_async(req, opts, ring);
}

wroot::Task<HttpResponse> proxy_async(
	HttpRequest const &req,
	ProxyOptions const &opts,
	SocketTaskRing &ring) {
	co_return co_await async_proxy(req, opts, ring);
}
