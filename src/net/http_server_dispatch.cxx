module;
#include <cstddef>

module conflux.net.http_server:dispatch;

import std;
import conflux.types;
import std.compat;

import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http1_parser;
import conflux.net.http_server_helpers;
import conflux.utils;
import :state;

namespace {

struct CommonHeaderSummary {
	std::size_t host_count{};
	std::size_t content_length_count{};
	std::size_t transfer_encoding_count{};
	std::string_view host;
	std::string_view content_length;
	std::string_view content_type;
	std::string_view cookie;
	bool connection_close{};
	bool connection_keep_alive{};
	bool expect_continue{};
	bool expect_unsupported{};
};

void note_common_header(
	CommonHeaderSummary &summary,
	std::string_view name,
	std::string_view field_value) {
	if (conflux::http::ascii_iequals(name, "host")) {
		++summary.host_count;
		if (summary.host.empty()) {
			summary.host = field_value;
		}
		return;
	}
	if (conflux::http::ascii_iequals(name, "content-length")) {
		++summary.content_length_count;
		if (summary.content_length.empty()) {
			summary.content_length = field_value;
		}
		return;
	}
	if (conflux::http::ascii_iequals(name, "transfer-encoding")) {
		++summary.transfer_encoding_count;
		return;
	}
	if (conflux::http::ascii_iequals(name, "content-type")) {
		if (summary.content_type.empty()) {
			summary.content_type = field_value;
		}
		return;
	}
	if (conflux::http::ascii_iequals(name, "cookie")) {
		if (summary.cookie.empty()) {
			summary.cookie = field_value;
		}
		return;
	}
	if (conflux::http::ascii_iequals(name, "connection")) {
		if (conflux::http::header_token_contains(field_value, "close")) {
			summary.connection_close = true;
		}
		if (conflux::http::header_token_contains(field_value, "keep-alive")) {
			summary.connection_keep_alive = true;
		}
		return;
	}
	if (conflux::http::ascii_iequals(name, "expect")) {
		for (auto const token: conflux::http::header_tokens(field_value)) {
			if (token.empty()) {
				continue;
			}
			if (!conflux::http::ascii_iequals(token, "100-continue")) {
				summary.expect_unsupported = true;
				break;
			}
			summary.expect_continue = true;
		}
	}
}

[[nodiscard]] ExpectState common_expect_state(
	CommonHeaderSummary const &summary) noexcept {
	if (summary.expect_unsupported) {
		return ExpectState::unsupported;
	}
	if (summary.expect_continue) {
		return ExpectState::continue_100;
	}
	return ExpectState::none;
}

void note_rejection(
	HttpRejectionMetrics &metrics,
	HttpRejectReason reason) noexcept {
	switch (reason) {
	case HttpRejectReason::malformed_request       : ++metrics.malformed_request; break;
	case HttpRejectReason::request_line_too_large  : ++metrics.request_line_too_large; break;
	case HttpRejectReason::header_line_too_large   : ++metrics.header_line_too_large; break;
	case HttpRejectReason::header_block_too_large  : ++metrics.header_block_too_large; break;
	case HttpRejectReason::too_many_headers        : ++metrics.too_many_headers; break;
	case HttpRejectReason::missing_host            : ++metrics.missing_host; break;
	case HttpRejectReason::duplicate_host          : ++metrics.duplicate_host; break;
	case HttpRejectReason::malformed_content_length: ++metrics.malformed_content_length; break;
	case HttpRejectReason::duplicate_content_length: ++metrics.duplicate_content_length; break;
	case HttpRejectReason::content_length_with_transfer_encoding:
		++metrics.content_length_with_transfer_encoding;
		break;
	case HttpRejectReason::unsupported_transfer_encoding: ++metrics.unsupported_transfer_encoding; break;
	case HttpRejectReason::invalid_transfer_encoding    : ++metrics.invalid_transfer_encoding; break;
	case HttpRejectReason::invalid_chunk                : ++metrics.invalid_chunk; break;
	case HttpRejectReason::body_too_large               : ++metrics.body_too_large; break;
	case HttpRejectReason::expectation_failed           : ++metrics.expectation_failed; break;
	case HttpRejectReason::header_timeout               : ++metrics.header_timeout; break;
	case HttpRejectReason::body_timeout                 : ++metrics.body_timeout; break;
	case HttpRejectReason::none                         : break;
	}
}

void emit_rejection(
	Conn &conn,
	std::string_view raw,
	Ring &ring,
	HttpRejectReason reason,
	std::string_view alt_svc) {
	Response r;
	r.status = reject_reason_status(reason);
	switch (r.status) {
	case 400: r.status_text = "Bad Request"; break;
	case 408: r.status_text = "Request Timeout"; break;
	case 413: r.status_text = "Content Too Large"; break;
	case 414: r.status_text = "URI Too Long"; break;
	case 417: r.status_text = "Expectation Failed"; break;
	case 431: r.status_text = "Request Header Fields Too Large"; break;
	default : r.status_text = "Bad Request"; break;
	}
	r.content_type = "application/problem+json";
	r.set_text_body(
		std::format(
			R"({{"code":"{}","diagnostic_code":"{}","detail":"{}"}})",
			reject_reason_code(reason),
			reject_reason_diagnostic_code(reason),
			reject_reason_detail(reason)));
	note_rejection(ring.rejection_counters_, reason);
	if (ring.observability_hooks_.rejection) {
		ring.observability_hooks_.rejection(reason, r.status);
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
	Ring &ring,
	std::size_t max_body_size,
	bool http_redirect_to_https,
	std::vector<std::string> const &https_redirect_hosts,
	ParserLimits const &limits,
	std::shared_ptr<std::string> request_storage) {
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
		emit_rejection(conn, raw, ring, HttpRejectReason::request_line_too_large, ring.alt_svc_header);
		return;
	case conflux::http1::ParseStatus::HeaderLineTooLarge:
		emit_rejection(conn, raw, ring, HttpRejectReason::header_line_too_large, ring.alt_svc_header);
		return;
	case conflux::http1::ParseStatus::HeaderBlockTooLarge:
		emit_rejection(conn, raw, ring, HttpRejectReason::header_block_too_large, ring.alt_svc_header);
		return;
	case conflux::http1::ParseStatus::TooManyHeaders:
		emit_rejection(conn, raw, ring, HttpRejectReason::too_many_headers, ring.alt_svc_header);
		return;
	case conflux::http1::ParseStatus::BadRequest:
		emit_rejection(conn, raw, ring, HttpRejectReason::malformed_request, ring.alt_svc_header);
		return;
	case conflux::http1::ParseStatus::Ok: break;
	}
	auto const header_end = parsed.header_end_offset;

	std::string_view const method = parsed.method;
	auto const target = conflux::http::split_path_query(parsed.target);
	std::string_view path = conflux::http::origin_form_path_from_target(target.path);
	std::string_view const redirect_query = target.query_suffix;
	std::string_view const version = parsed.version;
	HttpFieldsView const params;
	HttpFieldsView headers{true};
	HttpFieldsView query;
	HttpFieldsView form;
	HttpFieldsView cookies;
	std::vector<UploadedFile> files;
	std::string_view body;

	if (!target.query_suffix.empty()) {
		parse_urlencoded(target.query, query);
	}

	CommonHeaderSummary common_headers;
	headers.reserve(parsed.headers.size());
	for (auto const &[name, field_value]: parsed.headers) {
		headers.emplace_back(name, field_value);
		note_common_header(common_headers, name, field_value);
	}

	if (version == "HTTP/1.1") {
		auto const host_count = common_headers.host_count;
		if (host_count == 0) {
			emit_rejection(conn, raw, ring, HttpRejectReason::missing_host, ring.alt_svc_header);
			return;
		}
		if (host_count > 1) {
			emit_rejection(conn, raw, ring, HttpRejectReason::duplicate_host, ring.alt_svc_header);
			return;
		}
	}

	if (http_redirect_to_https && !conn.is_tls) {
		auto host = common_headers.host;
		auto const host_bare = conflux::http::host_without_port_or_ipv6_brackets(host);
		std::string_view canonical_host;
		for (auto const &h: https_redirect_hosts) {
			if (conflux::http::ascii_iequals(h, host_bare)) {
				canonical_host = h;
				break;
			}
		}
		if (host.empty() || canonical_host.empty()) {
			auto r = Response::text("Bad Request", kHttpBadRequest);
			conn.own_response = format_response(r, ring.alt_svc_header, true);
			conn.has_response = true;
			conn.close_after_send = true;
			conn.request_bytes = raw.size();
			return;
		}
		std::string redirect_target{path.empty() ? std::string_view{"/"} : path};
		redirect_target += redirect_query;
		conn.own_response = format_response(
			Response::redirect(std::format("https://{}{}", canonical_host, redirect_target), 308),
			ring.alt_svc_header,
			true);
		conn.has_response = true;
		conn.close_after_send = true;
		conn.request_bytes = raw.size();
		return;
	}

	auto body_start = header_end + 4;
	std::size_t body_stream_bytes = 0;

	auto const content_length_count = common_headers.content_length_count;
	auto const transfer_encoding_count = common_headers.transfer_encoding_count;
	if (content_length_count != 0 && transfer_encoding_count != 0) {
		emit_rejection(conn, raw, ring, HttpRejectReason::content_length_with_transfer_encoding, ring.alt_svc_header);
		return;
	}
	if (content_length_count > 1) {
		emit_rejection(conn, raw, ring, HttpRejectReason::duplicate_content_length, ring.alt_svc_header);
		return;
	}
	if (transfer_encoding_count > 1) {
		emit_rejection(conn, raw, ring, HttpRejectReason::invalid_transfer_encoding, ring.alt_svc_header);
		return;
	}
	if (transfer_encoding_count != 0 && !has_valid_chunked_transfer_encoding(headers)) {
		emit_rejection(conn, raw, ring, HttpRejectReason::unsupported_transfer_encoding, ring.alt_svc_header);
		return;
	}

	auto const expect_state = common_expect_state(common_headers);
	if (expect_state == ExpectState::unsupported) {
		emit_rejection(conn, raw, ring, HttpRejectReason::expectation_failed, ring.alt_svc_header);
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
		auto cl = common_headers.content_length;
		std::size_t content_length{};
		auto const *cl_end = std::ranges::next(cl.data(), ssize(cl));
		auto [ptr, ec] = std::from_chars(cl.data(), cl_end, content_length);
		if (ec != std::errc{} || ptr != cl_end) {
			emit_rejection(conn, raw, ring, HttpRejectReason::malformed_content_length, ring.alt_svc_header);
			return;
		}
		if (content_length > max_body_size) {
			emit_rejection(conn, raw, ring, HttpRejectReason::body_too_large, ring.alt_svc_header);
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
			emit_rejection(conn, raw, ring, HttpRejectReason::invalid_chunk, ring.alt_svc_header);
			return;
		}
		if (rc == -2) {
			emit_rejection(conn, raw, ring, HttpRejectReason::body_too_large, ring.alt_svc_header);
			return;
		}
		body = conn.chunked_decode.body;
		body_stream_bytes = static_cast<std::size_t>(rc);
	}

	conn.expect_continue_sent = false;

	if (common_headers.content_type.starts_with("application/x-www-form-urlencoded")) {
		parse_urlencoded(body, form);
	}

	auto ct_header = common_headers.content_type;
	if (ct_header.starts_with("multipart/form-data")) {
		auto boundary = extract_param(ct_header, "boundary");
		if (!boundary.empty()) {
			parse_multipart(body, boundary, form, files);
		}
	}

	if (auto cookie = common_headers.cookie; !cookie.empty()) {
		parse_cookies(cookie, cookies);
	}

	{
		bool keep_alive = (version == "HTTP/1.1");
		if (common_headers.connection_close) {
			keep_alive = false;
		} else if (common_headers.connection_keep_alive) {
			keep_alive = true;
		}
		conn.close_after_send = !keep_alive;
	}

	conn.request_bytes = header_end + 4 + body_stream_bytes;
	if (!request_storage) {
		auto storage =
			conn.partial.cut_prefix(conn.request_bytes, ring.acquire_request_buffer(), ring.request_tail_scratch);
		dispatch_request(
			conn,
			*storage,
			ring,
			max_body_size,
			http_redirect_to_https,
			https_redirect_hosts,
			limits,
			std::move(storage));
		conn.request_bytes = 0;
		return;
	}

	std::shared_ptr<std::vector<UploadedFile>> request_files;
	std::span<UploadedFile const> file_views{files};
	if (!files.empty()) {
		request_files = std::make_shared<std::vector<UploadedFile>>(std::move(files));
		file_views = *request_files;
	}
	RequestView const req{
		method,
		path,
		version,
		conn.remote_addr,
		conn.is_tls,
		params,
		headers,
		query,
		form,
		cookies,
		file_views,
		body};
	auto const handler_started = std::chrono::steady_clock::now();
	Response resp;
	try {
		if (auto async = ring.try_dispatch_context(req)) {
			resp = std::move(*async);
		} else {
			resp = ring.dispatch(req);
		}
	} catch (std::exception const &e) { resp = Response::internal_error(e.what()); } catch (...) {
		resp = Response::internal_error();
	}
	if (ring.slow_handler_diagnostics) {
		auto const elapsed_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - handler_started)
				.count();
		if (elapsed_ms >= static_cast<std::int64_t>(ring.slow_handler_warn_ms)) {
			eprintln(
				std::format(
					"warning: slow handler on ring std::thread (method={}, path={}, elapsed_ms={})",
					method,
					path,
					elapsed_ms));
		}
	}
	if (resp.is_deferred()) {
#if CONFLUX_HAS_HTTP2
		if (conn.is_h2) {
			conn.own_response = format_response(
				Response::internal_error("deferred responses unsupported over HTTP/2"),
				ring.alt_svc_header,
				true);
			conn.has_response = true;
			return;
		}
#endif
		conn.is_deferred = true;
		conn.deferred_head_only = resp.head_only;
		auto deferred_response = resp.deferred_response_ptr();
		if (request_storage) {
			deferred_response->keep_alive(request_storage);
		}
		if (request_files) {
			deferred_response->keep_alive(request_files);
		}
		conn.deferred_efd = deferred_response->eventfd_fd();
		conn.deferred_response = resp.take_deferred_response();
		conn.deferred_request_storage = std::move(request_storage);
		if (request_files) {
			conn.deferred_request_files = std::move(request_files);
		}
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
			++ring.static_file_counters_.mapped_responses;
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
			++ring.static_file_counters_.streamed_responses;
		}
	} else {
		conn.own_response = format_response(resp, ring.alt_svc_header, conn.close_after_send);
		conn.has_response = true;
	}
}
