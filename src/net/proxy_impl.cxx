module;

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

[[nodiscard]] static bool path_prefix_matches_segment(
	std::string_view path,
	std::string_view prefix) noexcept {
	return !prefix.empty()
		&& prefix != "/"
		&& (path == prefix || (path.starts_with(prefix) && path[prefix.size()] == '/'));
}

[[nodiscard]] static std::string build_upstream_url(
	std::string_view path,
	ProxyOptions const &opts) {
	std::string up_path{path};
	if (path_prefix_matches_segment(up_path, opts.path_prefix)) {
		up_path.erase(0, opts.path_prefix.size());
	}
	if (up_path.empty()) {
		up_path = "/";
	}
	std::string_view const scheme = opts.upstream_tls ? "https" : "http";
	return std::format("{}://{}:{}{}", scheme, opts.upstream_host, opts.upstream_port, up_path);
}

[[nodiscard]] static HttpClientOptions make_client_opts(
	ProxyOptions const &opts) {
	HttpTimeouts t{};
	t.connect = std::chrono::milliseconds{opts.timeout_sec * 1000};
	t.first_byte = std::chrono::milliseconds{opts.timeout_sec * 1000};
	t.between_bytes = std::chrono::milliseconds{opts.timeout_sec * 1000};
	HttpClientOptions co{};
	co.default_timeouts = t;
	return co;
}

[[nodiscard]] static http::ClientRequest::Builder apply_headers(
	http::ClientRequest::Builder builder,
	RequestView const &req,
	ProxyOptions const &opts) {
	for (auto const &[name, value]: req.headers) {
		if (conflux::http::ascii_iequals(name, "host")) {
			continue;
		}
		if (conflux::http::is_hop_by_hop_header(name)) {
			continue;
		}
		builder.header(name, value);
	}
	auto xff = std::string{req.headers["x-forwarded-for"]};
	if (xff.empty()) {
		builder.header("X-Forwarded-For", req.remote_addr);
	} else {
		builder.header("X-Forwarded-For", std::format("{}, {}", xff, req.remote_addr));
	}
	if (opts.preserve_host) {
		builder.header("Host", req.headers["host"]);
	}
	if (!req.body.empty()) {
		builder.body_view(req.body);
	}
	return builder;
}

[[nodiscard]] static Response build_response(
	http::ClientResponse &&r) {
	Response out;
	out.status = r.head.status;
	out.status_text = std::move(r.head.status_text);
	out.headers = std::move(r.head.headers);
	out.set_cookies = std::move(r.head.set_cookies);
	if (auto const ct = out.headers["content-type"]; !ct.empty()) {
		out.content_type = std::string{ct};
	}
	out.set_text_body(std::move(r.body));
	return out;
}

[[nodiscard]] Response perform_proxy_request(
	RequestView const &req,
	ProxyOptions const &opts) {
	auto co = make_client_opts(opts);
	co.default_timeouts.write = co.default_timeouts.connect;
	HttpClient client{std::move(co)};
	auto builder =
		apply_headers(http::ClientRequest::method(req.method, build_upstream_url(req.path, opts)), req, opts);
	builder.timeouts(client.options().default_timeouts);
	auto result = client.blocking_send(std::move(builder).build());
	if (!result) {
		return Response::bad_gateway(
			format("proxy: {} ({})", result.error().message, static_cast<int>(result.error().kind)));
	}
	return build_response(std::move(*result));
}

[[nodiscard]] wroot::Task<Response> perform_proxy_request_async(
	RequestView const &req,
	ProxyOptions const &opts,
	SocketTaskRing &ring) {
	auto co = make_client_opts(opts);
	co.default_timeouts.write = co.default_timeouts.connect;
	HttpClient client{std::move(co)};
	auto builder =
		apply_headers(http::ClientRequest::method(req.method, build_upstream_url(req.path, opts)), req, opts);
	builder.timeouts(client.options().default_timeouts);
	auto result = co_await http::async_send(client, ring, std::move(builder).build());
	if (!result) {
		co_return Response::bad_gateway(
			format("proxy: {} ({})", result.error().message, static_cast<int>(result.error().kind)));
	}
	co_return build_response(std::move(*result));
}

} // namespace proxy_detail

Response blocking_proxy(
	RequestView const &req,
	ProxyOptions const &opts) {
	return proxy_detail::perform_proxy_request(req, opts);
}

wroot::Task<Response> async_proxy(
	RequestView const &req,
	ProxyOptions const &opts,
	SocketTaskRing &ring) {
	co_return co_await proxy_detail::perform_proxy_request_async(req, opts, ring);
}
