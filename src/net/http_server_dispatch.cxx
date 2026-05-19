module;
#include <cstddef>

module conflux.net.http_server:dispatch;

import std;
import conflux.types;
import std.compat;

import conflux.net.http.types;
import conflux.net.http1_parser;
import conflux.net.http_server_helpers;
import conflux.utils;
import :state;

namespace {

enum class ParseError : std::uint8_t {
	None,
	BadRequest,
	UriTooLong,
	HeaderFieldsTooLarge,

};
void emit_parse_error(
	Conn &conn,
	std::string_view raw,
	ParseError err,
	std::string_view alt_svc) {
	HttpResponse r;
	switch (err) {
	case ParseError::UriTooLong          : r = HttpResponse::uri_too_long(); break;
	case ParseError::HeaderFieldsTooLarge: r = HttpResponse::header_fields_too_large(); break;
	case ParseError::BadRequest          :
	default                              : r = HttpResponse::bad_request(); break;
	}
	conn.own_response = format_response(r, alt_svc, true);
	conn.has_response = true;
	conn.close_after_send = true;
	conn.request_bytes = raw.size();
}

} // namespace
void dispatch_request(
	Conn &conn,
	std::string_view raw,
	Ring const &ring,
	std::size_t max_body_size,
	bool http_redirect_to_https,
	std::vector<std::string> const &https_redirect_hosts,
	ParserLimits const &limits) {
	conn.has_response = false;
	conn.written = 0;
	conn.mapped_file.reset();
	conn.mapped_total = 0;
	conn.mapped_delivered = 0;
	conn.zc_tls_bypass_counted = false;
	conn.is_sse = false;
	conn.sse_headers_sent = false;

	conflux::http1::ParsedRequest parsed;
	switch (conflux::http1::parse_request(raw, limits, parsed)) {
	case conflux::http1::ParseStatus::Incomplete: return;
	case conflux::http1::ParseStatus::UriTooLong:
		emit_parse_error(conn, raw, ParseError::UriTooLong, ring.alt_svc_header);
		return;
	case conflux::http1::ParseStatus::HeaderFieldsTooLarge:
		emit_parse_error(conn, raw, ParseError::HeaderFieldsTooLarge, ring.alt_svc_header);
		return;
	case conflux::http1::ParseStatus::BadRequest:
		emit_parse_error(conn, raw, ParseError::BadRequest, ring.alt_svc_header);
		return;
	case conflux::http1::ParseStatus::Ok: break;
	}
	auto const header_end = parsed.header_end_offset;

	std::string_view const method = parsed.method;
	std::string_view path = parsed.target;
	std::string_view redirect_query;
	std::string_view const version = parsed.version;
	HttpFieldsView const params;
	HttpFieldsView headers{true};
	HttpFieldsView query;
	HttpFieldsView form;
	HttpFieldsView cookies;
	std::vector<UploadedFile> files;
	std::string_view body;

	if (auto q = path.find('?'); q != std::string_view::npos) {
		redirect_query = path.substr(q);
		parse_urlencoded(path.substr(q + 1), query);
		path = path.substr(0, q);
	}

	headers.reserve(parsed.headers.size());
	for (auto const &[name, field_value]: parsed.headers) {
		headers.emplace_back(name, field_value);
	}

	if (path.starts_with("https://")) {
		auto slash = path.find('/', 8);
		path = (slash != std::string_view::npos) ? path.substr(slash) : std::string_view{"/"};
	} else if (path.starts_with("http://")) {
		auto slash = path.find('/', 7);
		path = (slash != std::string_view::npos) ? path.substr(slash) : std::string_view{"/"};
	}
	std::string redirect_target{path.empty() ? std::string_view{"/"} : path};
	redirect_target += redirect_query;

	if (version == "HTTP/1.1") {
		if (headers.count("host") != 1) {
			emit_parse_error(conn, raw, ParseError::BadRequest, ring.alt_svc_header);
			return;
		}
	}

	if (http_redirect_to_https && !conn.is_tls) {
		auto host = headers["host"];
		auto strip_host_port = [](std::string_view h) -> std::string_view {
			if (h.starts_with('[')) {
				auto b = h.find(']');
				if (b != std::string_view::npos) {
					auto c = h.find(':', b + 1);
					// strip port if present, then strip surrounding brackets
					std::string_view inner = c != std::string_view::npos ? h.substr(0, c) : h;
					if (inner.starts_with('[') && inner.ends_with(']')) {
						inner.remove_prefix(1);
						inner.remove_suffix(1);
					}
					return inner;
				}
				return h;
			}
			auto c = h.rfind(':');
			return c != std::string_view::npos ? h.substr(0, c) : h;
		};
		auto const host_bare = strip_host_port(host);
		std::string_view canonical_host;
		for (auto const &h: https_redirect_hosts) {
			if (conflux::http::ascii_iequals(h, host_bare)) {
				canonical_host = h;
				break;
			}
		}
		if (host.empty() || canonical_host.empty()) {
			auto r = HttpResponse{};
			r.status = kHttpBadRequest;
			r.status_text = "Bad Request";
			r.content_type = "text/plain; charset=utf-8";
			r.set_text_body("Bad Request");
			conn.own_response = format_response(r, ring.alt_svc_header, true);
			conn.has_response = true;
			conn.close_after_send = true;
			conn.request_bytes = raw.size();
			return;
		}
		conn.own_response = format_response(
			HttpResponse::redirect(format("https://{}{}", canonical_host, redirect_target), 308),
			ring.alt_svc_header,
			true);
		conn.has_response = true;
		conn.close_after_send = true;
		conn.request_bytes = raw.size();
		return;
	}

	auto body_start = header_end + 4;
	std::size_t body_stream_bytes = 0;

	auto const content_length_count = headers.count("content-length");
	auto const transfer_encoding_count = headers.count("transfer-encoding");
	if (content_length_count != 0 && transfer_encoding_count != 0) {
		emit_parse_error(conn, raw, ParseError::BadRequest, ring.alt_svc_header);
		return;
	}
	if (content_length_count > 1) {
		emit_parse_error(conn, raw, ParseError::BadRequest, ring.alt_svc_header);
		return;
	}
	if (transfer_encoding_count != 0 && !has_valid_chunked_transfer_encoding(headers)) {
		emit_parse_error(conn, raw, ParseError::BadRequest, ring.alt_svc_header);
		return;
	}

	auto const expect_state = parse_expect_header(headers);
	if (expect_state == ExpectState::unsupported) {
		HttpResponse r;
		r.status = 417;
		r.status_text = "Expectation Failed";
		r.content_type = "text/html; charset=utf-8";
		r.set_text_body("<html><body><h1>417 Expectation Failed</h1></body></html>");
		conn.own_response = format_response(r, ring.alt_svc_header, true);
		conn.has_response = true;
		conn.close_after_send = true;
		conn.request_bytes = raw.size();
		return;
	}
	auto const queue_continue = [&] {
		conn.own_response = "HTTP/1.1 100 Continue\r\n\r\n";
		conn.has_response = true;
		conn.written = 0;
		conn.request_bytes = 0;
		conn.expect_continue_sent = true;
	};

	if (content_length_count != 0) {
		auto cl = headers.get("content-length").value_or(std::string_view{});
		std::size_t content_length{};
		auto const *cl_end = std::ranges::next(cl.data(), ssize(cl));
		auto [ptr, ec] = from_chars(cl.data(), cl_end, content_length);
		if (ec != errc{} || ptr != cl_end) {
			conn.own_response = format_response(HttpResponse::bad_request(), ring.alt_svc_header, true);
			conn.has_response = true;
			conn.close_after_send = true;
			conn.request_bytes = raw.size();
			return;
		}
		if (content_length > max_body_size) {
			conn.own_response = format_response(HttpResponse::content_too_large(), ring.alt_svc_header, true);
			conn.has_response = true;
			conn.close_after_send = true;
			conn.request_bytes = raw.size();
			return;
		}
		if (raw.size() - body_start < content_length) {
			if (expect_state == ExpectState::continue_100 && !conn.expect_continue_sent) {
				queue_continue();
			}
			return;
		}
		body = raw.substr(body_start, content_length);
		body_stream_bytes = content_length;
	} else if (transfer_encoding_count != 0) {
		auto rc = decode_chunked_incremental(raw, body_start, max_body_size, limits.max_chunks, conn.chunked_decode);
		if (rc == 0) {
			if (expect_state == ExpectState::continue_100 && !conn.expect_continue_sent) {
				queue_continue();
			}
			return;
		}
		if (rc == -1) {
			conn.own_response = format_response(HttpResponse::bad_request(), ring.alt_svc_header, true);
			conn.has_response = true;
			conn.close_after_send = true;
			conn.request_bytes = raw.size();
			return;
		}
		if (rc == -2) {
			conn.own_response = format_response(HttpResponse::content_too_large(), ring.alt_svc_header, true);
			conn.has_response = true;
			conn.close_after_send = true;
			conn.request_bytes = raw.size();
			return;
		}
		body = conn.chunked_decode.body;
		body_stream_bytes = static_cast<std::size_t>(rc);
	}

	conn.expect_continue_sent = false;

	if (headers["content-type"].starts_with("application/x-www-form-urlencoded")) {
		parse_urlencoded(body, form);
	}

	auto ct_header = headers["content-type"];
	if (ct_header.starts_with("multipart/form-data")) {
		auto boundary = extract_param(ct_header, "boundary");
		if (!boundary.empty()) {
			parse_multipart(body, boundary, form, files);
		}
	}

	if (auto cookie = headers["cookie"]; !cookie.empty()) {
		parse_cookies(cookie, cookies);
	}

	{
		bool keep_alive = (version == "HTTP/1.1");
		if (has_connection_token(headers, "close")) {
			keep_alive = false;
		} else if (has_connection_token(headers, "keep-alive")) {
			keep_alive = true;
		}
		conn.close_after_send = !keep_alive;
	}

	conn.request_bytes = header_end + 4 + body_stream_bytes;

	HttpRequestView const
		req{method, path, version, conn.remote_addr, conn.is_tls, params, headers, query, form, cookies, files, body};
	auto const handler_started = std::chrono::steady_clock::now();
	HttpResponse resp;
	try {
		if (auto async = ring.try_dispatch_context(req)) {
			resp = move(*async);
		} else {
			resp = ring.dispatch(req);
		}
	} catch (exception const &e) { resp = HttpResponse::internal_error(e.what()); } catch (...) {
		resp = HttpResponse::internal_error();
	}
	if (ring.slow_handler_diagnostics) {
		auto const elapsed_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - handler_started)
				.count();
		if (elapsed_ms >= static_cast<std::int64_t>(ring.slow_handler_warn_ms)) {
			eprintln(format(
				"warning: slow handler on ring thread (method={}, path={}, elapsed_ms={})",
				method,
				path,
				elapsed_ms));
		}
	}
	if (resp.is_deferred()) {
#if CONFLUX_HAS_HTTP2
		if (conn.is_h2) {
			conn.own_response = format_response(
				HttpResponse::internal_error("deferred responses unsupported over HTTP/2"),
				ring.alt_svc_header,
				true);
			conn.has_response = true;
			return;
		}
#endif
		conn.is_deferred = true;
		conn.deferred_head_only = resp.head_only;
		conn.deferred_efd = resp.deferred_response_ptr()->eventfd_fd();
		conn.deferred_response = resp.take_deferred_response();
		conn.has_response = false;
	} else if (resp.is_ws_upgrade()) {
		conn.is_ws = true;
		conn.ws_upgrade = resp.ws_upgrade_ptr();
		conn.ws_work_pool = ring.resolve_ws_work_pool(req);
		conn.saved_req = req.to_owned();
		conn.close_after_send = false;
		conn.own_response = format_response(resp);
		conn.has_response = true;
	} else if (resp.is_sse()) {
		conn.close_after_send = true;
		conn.is_sse = true;
		conn.sse_efd = resp.sse_channel_ptr()->eventfd_fd();
		conn.sse_channel = resp.take_sse_channel();
		conn.own_response = std::string{format_sse_headers(conn.close_after_send)};
		conn.has_response = true;
	} else if (resp.is_mapped_file()) {
		conn.own_response = format_response(resp, ring.alt_svc_header, conn.close_after_send);
		if (resp.head_only) {
			conn.has_response = true;
		} else {
			conn.mapped_file = resp.take_mapped_file();
			conn.mapped_total = conn.own_response.size() + conn.mapped_file->size;
			conn.mapped_delivered = 0;
			conn.has_response = false;
		}
	} else if (resp.is_streamed_file()) {
		conn.own_response = format_response(resp, ring.alt_svc_header, conn.close_after_send);
		if (resp.head_only) {
			conn.has_response = true;
		} else {
			conn.streamed_file = resp.take_streamed_file();
			conn.streamed_headers_sent = false;
			conn.streamed_delivered = 0;
			conn.streamed_splice_in_flight = false;
			conn.has_response = true;
		}
	} else {
		conn.own_response = format_response(resp, ring.alt_svc_header, conn.close_after_send);
		conn.has_response = true;
	}
}
