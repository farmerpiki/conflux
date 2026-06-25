module;
#include <cstddef>

module conflux.net.http_server:dispatch;

import std;
import conflux.types;
import std.compat;

import conflux.net.http.types;
import conflux.net.http.response;
import conflux.net.http.server_types;
import conflux.net.http1_parser;
import conflux.net.http.parse_helpers;
import conflux.net.http_server_helpers;
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
	bool has_host{};
	bool has_content_length{};
	bool has_content_type{};
	bool has_cookie{};
	bool connection_close{};
	bool connection_keep_alive{};
	bool expect_continue{};
	bool expect_unsupported{};
};

enum class ParseDispatchStatus : std::uint8_t {
	Incomplete,
	Rejected,
	Ok,
};

enum class BodyDispatchStatus : std::uint8_t {
	Incomplete,
	Rejected,
	Ready,
};

struct BodyDispatchResult {
	BodyDispatchStatus status{BodyDispatchStatus::Ready};
	std::string_view body;
	std::size_t stream_bytes{};
};

void note_common_header(
	CommonHeaderSummary &summary,
	std::string_view name,
	std::string_view field_value) {
	if (conflux::http::ascii_iequals(name, "host")) {
		++summary.host_count;
		if (!summary.has_host) {
			summary.host = field_value;
			summary.has_host = true;
		}
		return;
	}
	if (conflux::http::ascii_iequals(name, "content-length")) {
		++summary.content_length_count;
		if (!summary.has_content_length) {
			summary.content_length = field_value;
			summary.has_content_length = true;
		}
		return;
	}
	if (conflux::http::ascii_iequals(name, "transfer-encoding")) {
		++summary.transfer_encoding_count;
		return;
	}
	if (conflux::http::ascii_iequals(name, "content-type")) {
		if (!summary.has_content_type) {
			summary.content_type = field_value;
			summary.has_content_type = true;
		}
		return;
	}
	if (conflux::http::ascii_iequals(name, "cookie")) {
		if (!summary.has_cookie) {
			summary.cookie = field_value;
			summary.has_cookie = true;
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
		auto const expect_state = conflux::http::parse_expect_value(field_value);
		if (expect_state == conflux::http::ExpectState::unsupported) {
			summary.expect_unsupported = true;
			return;
		}
		if (expect_state == conflux::http::ExpectState::continue_100) {
			summary.expect_continue = true;
		}
	}
}

void reset_http1_response_state(
	Conn &conn) {
	conn.has_response = false;
	conn.written = 0;
	conn.mapped_file.reset();
	conn.mapped_total = 0;
	conn.mapped_delivered = 0;
	conn.zc_tls_bypass_counted = false;
	conn.is_sse = false;
	conn.sse_headers_sent = false;
}

[[nodiscard]] conflux::http::ExpectState common_expect_state(
	CommonHeaderSummary const &summary) noexcept {
	if (summary.expect_unsupported) {
		return conflux::http::ExpectState::unsupported;
	}
	if (summary.expect_continue) {
		return conflux::http::ExpectState::continue_100;
	}
	return conflux::http::ExpectState::none;
}

void emit_rejection(
	Conn &conn,
	std::string_view raw,
	Ring &ring,
	conflux::http::HttpRejectReason reason,
	std::string_view alt_svc) {
	auto r = conflux::http::make_rejection_response(reason);
	{
		std::scoped_lock lk{ring.metrics_mu_};
		conflux::http::note_rejection(ring.rejection_counters_, reason);
	}
	if (ring.observability_hooks_.rejection) {
		ring.observability_hooks_.rejection(reason, r.status);
	}
	conn.own_response = conflux::http::format_response(r, alt_svc, true);
	conn.has_response = true;
	conn.close_after_send = true;
	conn.request_bytes = raw.size();
}

[[nodiscard]] ParseDispatchStatus parse_or_reject_http1_request(
	Conn &conn,
	std::string_view raw,
	Ring &ring,
	conflux::http::ParserLimits const &limits,
	conflux::http1::ParsedRequest &parsed) {
	switch (conflux::http1::parse_request(raw, limits, parsed)) {
	case conflux::http1::ParseStatus::Incomplete: return ParseDispatchStatus::Incomplete;
	case conflux::http1::ParseStatus::UriTooLong:
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::request_line_too_large, ring.alt_svc_header);
		return ParseDispatchStatus::Rejected;
	case conflux::http1::ParseStatus::HeaderLineTooLarge:
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::header_line_too_large, ring.alt_svc_header);
		return ParseDispatchStatus::Rejected;
	case conflux::http1::ParseStatus::HeaderBlockTooLarge:
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::header_block_too_large, ring.alt_svc_header);
		return ParseDispatchStatus::Rejected;
	case conflux::http1::ParseStatus::TooManyHeaders:
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::too_many_headers, ring.alt_svc_header);
		return ParseDispatchStatus::Rejected;
	case conflux::http1::ParseStatus::BadRequest:
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::malformed_request, ring.alt_svc_header);
		return ParseDispatchStatus::Rejected;
	case conflux::http1::ParseStatus::Ok: return ParseDispatchStatus::Ok;
	}
	return ParseDispatchStatus::Rejected;
}

[[nodiscard]] bool validate_http1_host_headers(
	Conn &conn,
	std::string_view raw,
	Ring &ring,
	std::string_view version,
	CommonHeaderSummary const &common_headers) {
	if (version != "HTTP/1.1") {
		return true;
	}
	auto const host_count = common_headers.host_count;
	if (host_count == 0) {
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::missing_host, ring.alt_svc_header);
		return false;
	}
	if (host_count > 1) {
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::duplicate_host, ring.alt_svc_header);
		return false;
	}
	return true;
}

[[nodiscard]] bool maybe_make_https_redirect_response(
	Conn &conn,
	std::string_view raw,
	Ring &ring,
	bool http_redirect_to_https,
	std::vector<std::string> const &https_redirect_hosts,
	CommonHeaderSummary const &common_headers,
	std::string_view path,
	std::string_view redirect_query) {
	if (!http_redirect_to_https || conn.is_tls) {
		return false;
	}
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
		auto r = conflux::http::Response::text("Bad Request", conflux::http::kHttpBadRequest);
		conn.own_response = conflux::http::format_response(r, ring.alt_svc_header, true);
		conn.has_response = true;
		conn.close_after_send = true;
		conn.request_bytes = raw.size();
		return true;
	}
	std::string redirect_target{path.empty() ? std::string_view{"/"} : path};
	redirect_target += redirect_query;
	conn.own_response = conflux::http::format_response(
		conflux::http::Response::redirect(std::format("https://{}{}", canonical_host, redirect_target), 308),
		ring.alt_svc_header,
		true);
	conn.has_response = true;
	conn.close_after_send = true;
	conn.request_bytes = raw.size();
	return true;
}

void queue_expect_continue_response(
	Conn &conn) {
	conn.own_response = "HTTP/1.1 100 Continue\r\n\r\n";
	conn.has_response = true;
	conn.written = 0;
	conn.request_bytes = 0;
	conn.expect_continue_sent = true;
}

[[nodiscard]] BodyDispatchResult resolve_http1_body_view(
	Conn &conn,
	std::string_view raw,
	Ring &ring,
	std::size_t body_start,
	std::size_t max_body_size,
	conflux::http::ParserLimits const &limits,
	conflux::http::HttpFieldsView const &headers,
	CommonHeaderSummary const &common_headers) {
	auto const content_length_count = common_headers.content_length_count;
	auto const transfer_encoding_count = common_headers.transfer_encoding_count;
	if (content_length_count != 0 && transfer_encoding_count != 0) {
		emit_rejection(
			conn,
			raw,
			ring,
			conflux::http::HttpRejectReason::content_length_with_transfer_encoding,
			ring.alt_svc_header);
		return {.status = BodyDispatchStatus::Rejected};
	}
	if (content_length_count > 1) {
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::duplicate_content_length, ring.alt_svc_header);
		return {.status = BodyDispatchStatus::Rejected};
	}
	if (transfer_encoding_count > 1) {
		emit_rejection(
			conn,
			raw,
			ring,
			conflux::http::HttpRejectReason::invalid_transfer_encoding,
			ring.alt_svc_header);
		return {.status = BodyDispatchStatus::Rejected};
	}
	if (transfer_encoding_count != 0 && !conflux::http::has_valid_chunked_transfer_encoding(headers)) {
		emit_rejection(
			conn,
			raw,
			ring,
			conflux::http::HttpRejectReason::unsupported_transfer_encoding,
			ring.alt_svc_header);
		return {.status = BodyDispatchStatus::Rejected};
	}

	auto const expect_state = common_expect_state(common_headers);
	if (expect_state == conflux::http::ExpectState::unsupported) {
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::expectation_failed, ring.alt_svc_header);
		return {.status = BodyDispatchStatus::Rejected};
	}
	if (content_length_count != 0) {
		auto cl = common_headers.content_length;
		auto parsed_content_length = conflux::http::parse_content_length_limited(cl, max_body_size);
		if (!parsed_content_length
			&& parsed_content_length.error() == conflux::http::ContentLengthParseError::malformed) {
			emit_rejection(
				conn,
				raw,
				ring,
				conflux::http::HttpRejectReason::malformed_content_length,
				ring.alt_svc_header);
			return {.status = BodyDispatchStatus::Rejected};
		}
		if (!parsed_content_length) {
			emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::body_too_large, ring.alt_svc_header);
			return {.status = BodyDispatchStatus::Rejected};
		}
		auto const content_length = *parsed_content_length;
		if (raw.size() - body_start < content_length) {
			if (expect_state == conflux::http::ExpectState::continue_100 && !conn.expect_continue_sent) {
				queue_expect_continue_response(conn);
			}
			return {.status = BodyDispatchStatus::Incomplete};
		}
		return {
			.status = BodyDispatchStatus::Ready,
			.body = raw.substr(body_start, content_length),
			.stream_bytes = content_length};
	}
	if (transfer_encoding_count != 0) {
		auto rc = conflux::http::decode_chunked_incremental(
			raw,
			body_start,
			max_body_size,
			limits.max_chunks,
			conn.chunked_decode);
		if (rc == 0) {
			if (expect_state == conflux::http::ExpectState::continue_100 && !conn.expect_continue_sent) {
				queue_expect_continue_response(conn);
			}
			return {.status = BodyDispatchStatus::Incomplete};
		}
		if (rc == -1) {
			emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::invalid_chunk, ring.alt_svc_header);
			return {.status = BodyDispatchStatus::Rejected};
		}
		if (rc == -2) {
			emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::body_too_large, ring.alt_svc_header);
			return {.status = BodyDispatchStatus::Rejected};
		}
		return {
			.status = BodyDispatchStatus::Ready,
			.body = conn.chunked_decode.body,
			.stream_bytes = static_cast<std::size_t>(rc)};
	}
	return {.status = BodyDispatchStatus::Ready};
}

void compact_chunked_request_buffer(
	Conn &conn,
	std::size_t body_start) {
	if (conn.chunked_decode.pos <= body_start) {
		return;
	}
	conn.partial.erase_view_range(body_start, conn.chunked_decode.pos);
	conn.chunked_decode.pos = body_start;
	conn.chunked_decode.body_start = body_start;
}

void apply_http1_keep_alive(
	Conn &conn,
	std::string_view version,
	CommonHeaderSummary const &common_headers) {
	bool keep_alive = (version == "HTTP/1.1");
	if (common_headers.connection_close) {
		keep_alive = false;
	} else if (common_headers.connection_keep_alive) {
		keep_alive = true;
	}
	conn.close_after_send = !keep_alive;
}

[[nodiscard]] conflux::http::UploadError upload_error(
	conflux::http::UploadErrorKind kind,
	std::string detail) {
	return conflux::http::UploadError{.kind = kind, .detail = std::move(detail)};
}

[[nodiscard]] conflux::http::Response invoke_http1_handler(
	Ring &ring,
	conflux::http::RequestView const &req,
	conflux::http::RequestView const &fallback_req,
	std::string_view method,
	std::string_view path,
	std::shared_ptr<conflux::http::detail::UploadBodyState> upload_body = {}) {
	auto const handler_started = std::chrono::steady_clock::now();
	conflux::http::Response resp;
	try {
		if (upload_body) {
			if (auto async = ring.try_dispatch_context(req, std::move(upload_body))) {
				resp = std::move(*async);
			} else {
				resp = ring.dispatch(fallback_req);
			}
		} else if (auto async = ring.try_dispatch_context(req)) {
			resp = std::move(*async);
		} else {
			resp = ring.dispatch(req);
		}
	} catch (std::exception const &e) { resp = conflux::http::Response::internal_error(e.what()); } catch (...) {
		resp = conflux::http::Response::internal_error();
	}
	if (ring.slow_handler_diagnostics) {
		auto const elapsed_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - handler_started)
				.count();
		if (elapsed_ms >= static_cast<std::int64_t>(ring.slow_handler_warn_ms)) {
			conflux::utils::eprintln(
				std::format(
					"warning: slow handler on ring std::thread (method={}, path={}, elapsed_ms={})",
					method,
					path,
					elapsed_ms));
		}
	}
	return resp;
}

void install_response_state(
	Conn &conn,
	conflux::http::Response &&resp,
	Ring &ring,
	conflux::http::RequestView const &req,
	std::shared_ptr<std::string> request_storage,
	std::shared_ptr<std::vector<conflux::http::UploadedFile>> request_files);

[[nodiscard]] bool dispatch_http1_upload_request(
	Conn &conn,
	std::string_view raw,
	Ring &ring,
	std::size_t body_start,
	std::size_t max_body_size,
	std::string_view method,
	conflux::http::PathQueryView target,
	std::string_view path,
	std::string_view version,
	conflux::http::HttpFieldsView const &headers,
	conflux::http::HttpFieldsView &query,
	conflux::http::HttpFieldsView &cookies,
	CommonHeaderSummary const &common_headers,
	std::shared_ptr<std::string> const &request_storage) {
	auto const content_length_count = common_headers.content_length_count;
	auto const transfer_encoding_count = common_headers.transfer_encoding_count;
	if (content_length_count != 0 && transfer_encoding_count != 0) {
		emit_rejection(
			conn,
			raw,
			ring,
			conflux::http::HttpRejectReason::content_length_with_transfer_encoding,
			ring.alt_svc_header);
		return true;
	}
	if (content_length_count > 1) {
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::duplicate_content_length, ring.alt_svc_header);
		return true;
	}
	if (transfer_encoding_count > 1) {
		emit_rejection(
			conn,
			raw,
			ring,
			conflux::http::HttpRejectReason::invalid_transfer_encoding,
			ring.alt_svc_header);
		return true;
	}
	if (transfer_encoding_count != 0 && !conflux::http::has_valid_chunked_transfer_encoding(headers)) {
		emit_rejection(
			conn,
			raw,
			ring,
			conflux::http::HttpRejectReason::unsupported_transfer_encoding,
			ring.alt_svc_header);
		return true;
	}
	auto const expect_state = common_expect_state(common_headers);
	if (expect_state == conflux::http::ExpectState::unsupported) {
		emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::expectation_failed, ring.alt_svc_header);
		return true;
	}
	std::optional<std::size_t> content_length;
	if (content_length_count != 0) {
		auto parsed_content_length =
			conflux::http::parse_content_length_limited(common_headers.content_length, max_body_size);
		if (!parsed_content_length
			&& parsed_content_length.error() == conflux::http::ContentLengthParseError::malformed) {
			emit_rejection(
				conn,
				raw,
				ring,
				conflux::http::HttpRejectReason::malformed_content_length,
				ring.alt_svc_header);
			return true;
		}
		if (!parsed_content_length) {
			emit_rejection(conn, raw, ring, conflux::http::HttpRejectReason::body_too_large, ring.alt_svc_header);
			return true;
		}
		content_length = *parsed_content_length;
	} else if (transfer_encoding_count == 0) {
		content_length = 0;
	}

	conflux::http::HttpFieldsView const params;
	conflux::http::HttpFieldsView upload_form;
	std::span<conflux::http::UploadedFile const> upload_files;
	std::vector<conflux::http::UploadedFile> ignored_files;
	std::string_view const body;
	conflux::http::populate_request_parts(target, headers, body, query, upload_form, cookies, ignored_files);
	conflux::http::RequestView const upload_req{
		method,
		path,
		version,
		conn.remote_addr,
		conn.is_tls,
		params,
		headers,
		query,
		upload_form,
		cookies,
		upload_files,
		{}};
	auto upload_body =
		std::make_shared<conflux::http::detail::UploadBodyState>(content_length, ring.upload_stream_queue_capacity);
	conflux::http::Response resp;
	try {
		auto dispatched = ring.try_dispatch_context(upload_req, upload_body);
		if (!dispatched) {
			return false;
		}
		resp = std::move(*dispatched);
	} catch (std::exception const &e) { resp = conflux::http::Response::internal_error(e.what()); } catch (...) {
		resp = conflux::http::Response::internal_error();
	}

	conn.request_bytes = body_start;
	auto storage = request_storage;
	if (!storage) {
		storage = conn.partial.cut_prefix(body_start, ring.acquire_request_buffer(), ring.request_tail_scratch);
	}
	conn.http1_upload_request_storage = storage;
	conn.http1_upload_body = upload_body;
	conn.http1_upload_content_length = content_length;
	conn.request_in_progress = true;
	conn.http1_upload_received = 0;
	conn.http1_upload_body_start = body_start;
	conn.http1_upload_chunk_remaining = 0;
	conn.http1_upload_chunk_count = 0;
	conn.http1_upload_trailer_lines = 0;
	conn.http1_upload_trailer_bytes = 0;
	conn.http1_upload_line.clear();
	conn.http1_upload_chunk_phase = Http1UploadChunkPhase::size_line;
	{
		std::scoped_lock lk{ring.metrics_mu_};
		++ring.upload_counters_.streams_started;
	}
	bool const cancel_upload_for_final_response = !resp.is_deferred();
	if (cancel_upload_for_final_response) {
		upload_body->abandon_consumer();
	}
	bool const send_expect_continue =
		expect_state == conflux::http::ExpectState::continue_100 && resp.is_deferred() && !conn.expect_continue_sent;
	install_response_state(conn, std::move(resp), ring, upload_req, std::move(storage), {});
	if (conn.is_deferred && conn.deferred_response) {
		if (auto ready = conn.deferred_response->take_ready()) {
			ring.install_http1_deferred_response(conn.fd, conn, std::move(*ready));
			return true;
		}
	}
	if (cancel_upload_for_final_response) {
		{
			std::scoped_lock lk{ring.metrics_mu_};
			++ring.upload_counters_.canceled_by_handler;
		}
		conn.close_after_send = true;
		conn.partial.clear();
		conn.http1_upload_body.reset();
		conn.http1_upload_request_storage.reset();
		conn.http1_upload_content_length.reset();
		conn.http1_upload_line.clear();
		conn.http1_upload_chunk_phase = Http1UploadChunkPhase::done;
		ring.start_response_send(conn.fd, conn);
		return true;
	}
	if (send_expect_continue) {
		conn.http1_continue_final_close_after_send = conn.close_after_send;
		queue_expect_continue_response(conn);
		conn.close_after_send = false;
		conn.request_bytes = body_start;
	}
	feed_http1_upload(conn, ring);
	return true;
}

[[nodiscard]] bool maybe_stabilize_request_storage(
	Conn &conn,
	Ring &ring,
	std::size_t max_body_size,
	bool http_redirect_to_https,
	std::vector<std::string> const &https_redirect_hosts,
	conflux::http::ParserLimits const &limits,
	std::shared_ptr<std::string> const &request_storage) {
	if (request_storage) {
		return false;
	}
	auto storage =
		conn.partial.cut_prefix(conn.request_bytes, ring.acquire_request_buffer(), ring.request_tail_scratch);
	std::string_view const raw_request{*storage};
	dispatch_request(
		conn,
		raw_request,
		ring,
		max_body_size,
		http_redirect_to_https,
		https_redirect_hosts,
		limits,
		std::move(storage));
	conn.expect_continue_sent = false;
	conn.chunked_decode.reset();
	conn.request_bytes = 0;
	return true;
}

void install_response_state(
	Conn &conn,
	conflux::http::Response &&resp,
	Ring &ring,
	conflux::http::RequestView const &req,
	std::shared_ptr<std::string> request_storage,
	std::shared_ptr<std::vector<conflux::http::UploadedFile>> request_files) {
	if (resp.is_deferred()) {
#if CONFLUX_HAS_HTTP2
		if (conn.is_h2) {
			conn.own_response = conflux::http::format_response(
				conflux::http::Response::internal_error("deferred responses unsupported over HTTP/2"),
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
		conn.own_response = conflux::http::format_response(resp);
		conn.has_response = true;
	} else if (resp.is_sse()) {
		conn.close_after_send = true;
		conn.is_sse = true;
		conn.sse_efd = resp.sse_channel_ptr()->eventfd_fd();
		conn.sse_channel = resp.take_sse_channel();
		conn.own_response = std::string{conflux::http::format_sse_headers(conn.close_after_send)};
		conn.has_response = true;
	} else if (resp.is_mapped_file()) {
		conn.own_response = conflux::http::format_response(resp, ring.alt_svc_header, conn.close_after_send);
		if (resp.head_only) {
			conn.has_response = true;
		} else {
			conn.mapped_file = resp.take_mapped_file();
			conn.mapped_total = conn.own_response.size() + conn.mapped_file->window().size();
			conn.mapped_delivered = 0;
			conn.has_response = false;
			{
				std::scoped_lock lk{ring.metrics_mu_};
				++ring.static_file_counters_.mapped_responses;
			}
		}
	} else if (resp.is_streamed_file()) {
		conn.own_response = conflux::http::format_response(resp, ring.alt_svc_header, conn.close_after_send);
		if (resp.head_only) {
			conn.has_response = true;
		} else {
			conn.streamed_file = resp.take_streamed_file();
			conn.streamed_headers_sent = false;
			conn.streamed_delivered = 0;
			conn.streamed_splice_in_flight = false;
			conn.has_response = true;
			{
				std::scoped_lock lk{ring.metrics_mu_};
				++ring.static_file_counters_.streamed_responses;
			}
		}
	} else {
		conn.own_response = conflux::http::format_response(resp, ring.alt_svc_header, conn.close_after_send);
		conn.has_response = true;
	}
}

} // namespace
void dispatch_request(
	Conn &conn,
	std::string_view raw,
	Ring &ring,
	std::size_t max_body_size,
	bool http_redirect_to_https,
	std::vector<std::string> const &https_redirect_hosts,
	conflux::http::ParserLimits const &limits,
	std::shared_ptr<std::string> request_storage) {
	reset_http1_response_state(conn);

	conflux::http1::ParsedRequest parsed;
	auto const parse_status = parse_or_reject_http1_request(conn, raw, ring, limits, parsed);
	if (parse_status != ParseDispatchStatus::Ok) {
		return;
	}
	auto const header_end = parsed.header_end_offset;

	std::string_view const method = parsed.method;
	auto const target = conflux::http::split_path_query(parsed.target);
	std::string_view path = conflux::http::origin_form_path_from_target(target.path);
	std::string_view const redirect_query = target.query_suffix;
	std::string_view const version = parsed.version;
	conflux::http::HttpFieldsView const params;
	conflux::http::HttpFieldsView headers{true};
	conflux::http::HttpFieldsView query;
	conflux::http::HttpFieldsView form;
	conflux::http::HttpFieldsView cookies;
	std::vector<conflux::http::UploadedFile> files;
	std::string_view body;

	CommonHeaderSummary common_headers;
	headers.reserve(parsed.headers.size());
	for (auto const &[name, field_value]: parsed.headers) {
		headers.emplace_back(name, field_value);
		note_common_header(common_headers, name, field_value);
	}

	if (!validate_http1_host_headers(conn, raw, ring, version, common_headers)) {
		return;
	}

	if (maybe_make_https_redirect_response(
			conn,
			raw,
			ring,
			http_redirect_to_https,
			https_redirect_hosts,
			common_headers,
			path,
			redirect_query)) {
		return;
	}

	auto body_start = header_end + 4;
	apply_http1_keep_alive(conn, version, common_headers);
	if (dispatch_http1_upload_request(
			conn,
			raw,
			ring,
			body_start,
			max_body_size,
			method,
			target,
			path,
			version,
			headers,
			query,
			cookies,
			common_headers,
			request_storage)) {
		return;
	}
	auto body_result =
		resolve_http1_body_view(conn, raw, ring, body_start, max_body_size, limits, headers, common_headers);
	if (body_result.status != BodyDispatchStatus::Ready) {
		if (body_result.status == BodyDispatchStatus::Incomplete && common_headers.transfer_encoding_count != 0) {
			compact_chunked_request_buffer(conn, body_start);
		}
		return;
	}
	body = body_result.body;

	conn.expect_continue_sent = false;

	conflux::http::populate_request_parts(target, headers, body, query, form, cookies, files);

	conn.request_bytes = header_end + 4 + body_result.stream_bytes;
	if (maybe_stabilize_request_storage(
			conn,
			ring,
			max_body_size,
			http_redirect_to_https,
			https_redirect_hosts,
			limits,
			request_storage)) {
		return;
	}

	std::shared_ptr<std::vector<conflux::http::UploadedFile>> request_files;
	std::span<conflux::http::UploadedFile const> file_views{files};
	if (!files.empty()) {
		request_files = std::make_shared<std::vector<conflux::http::UploadedFile>>(std::move(files));
		file_views = *request_files;
	}
	conflux::http::RequestView const req{
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
	conflux::http::HttpFieldsView upload_form;
	std::span<conflux::http::UploadedFile const> upload_files;
	conflux::http::RequestView const upload_req{
		method,
		path,
		version,
		conn.remote_addr,
		conn.is_tls,
		params,
		headers,
		query,
		upload_form,
		cookies,
		upload_files,
		{}};
	auto upload_body = std::make_shared<conflux::http::detail::UploadBodyState>(
		common_headers.has_content_length ? std::optional<std::uint64_t>{body.size()} : std::nullopt,
		ring.upload_stream_queue_capacity);
	if (!body.empty()) {
		auto _ = upload_body->push(std::string{body});
	}
	upload_body->finish();
	auto resp = invoke_http1_handler(ring, upload_req, req, method, path, std::move(upload_body));
	install_response_state(conn, std::move(resp), ring, req, std::move(request_storage), std::move(request_files));
}

namespace {

void fail_http1_upload(
	Conn &conn,
	Ring &ring,
	conflux::http::UploadError error) {
	auto const kind = error.kind;
	auto response = conflux::http::upload_error_response(error);
	if (conn.http1_upload_body) {
		conn.http1_upload_body->fail(std::move(error));
	}
	conn.own_response = conflux::http::format_response(response, ring.alt_svc_header, true);
	conn.has_response = true;
	conn.close_after_send = true;
	conn.http1_upload_body.reset();
	conn.http1_upload_request_storage.reset();
	conn.http1_upload_content_length.reset();
	conn.http1_upload_line.clear();
	conn.http1_upload_chunk_phase = Http1UploadChunkPhase::done;
	{
		std::scoped_lock lk{ring.metrics_mu_};
		switch (kind) {
		case conflux::http::UploadErrorKind::disconnected  : ++ring.upload_counters_.disconnected; break;
		case conflux::http::UploadErrorKind::body_too_large: ++ring.upload_counters_.body_too_large; break;
		case conflux::http::UploadErrorKind::content_length_mismatch:
			++ring.upload_counters_.content_length_mismatch;
			break;
		default: break;
		}
	}
}

[[nodiscard]] bool push_http1_upload_chunk(
	Conn &conn,
	Ring &ring,
	std::string chunk) {
	if (chunk.empty()) {
		return true;
	}
	auto pushed = conn.http1_upload_body->push(std::move(chunk));
	if (!pushed) {
		if (pushed.error().kind == conflux::http::UploadErrorKind::io_error) {
			std::scoped_lock lk{ring.metrics_mu_};
			++ring.upload_counters_.queue_backpressure_events;
		}
		fail_http1_upload(conn, ring, std::move(pushed).error());
		return false;
	}
	return true;
}

void finish_http1_upload(
	Conn &conn,
	Ring &ring) {
	if (!conn.http1_upload_body) {
		return;
	}
	{
		std::scoped_lock lk{ring.metrics_mu_};
		ring.upload_counters_.bytes_consumed += conn.http1_upload_body->bytes_read();
	}
	conn.http1_upload_body->finish();
	conn.http1_upload_body.reset();
	conn.http1_upload_request_storage.reset();
	conn.http1_upload_content_length.reset();
	conn.http1_upload_line.clear();
	conn.http1_upload_chunk_phase = Http1UploadChunkPhase::done;
	conn.request_in_progress = false;
	if (conn.is_deferred && conn.deferred_response) {
		if (auto ready = conn.deferred_response->take_ready()) {
			ring.install_http1_deferred_response(conn.fd, conn, std::move(*ready));
		}
	}
}

[[nodiscard]] bool feed_http1_content_length_upload(
	Conn &conn,
	Ring &ring) {
	auto const total = *conn.http1_upload_content_length;
	while (conn.http1_upload_received < total && !conn.partial.empty()) {
		auto const remaining = total - conn.http1_upload_received;
		auto const n = std::min<std::size_t>(remaining, conn.partial.size());
		std::string chunk{conn.partial.data(), n};
		conn.partial.consume(n);
		conn.http1_upload_received += n;
		{
			std::scoped_lock lk{ring.metrics_mu_};
			ring.upload_counters_.bytes_received += n;
		}
		if (!push_http1_upload_chunk(conn, ring, std::move(chunk))) {
			return false;
		}
	}
	if (conn.http1_upload_received == total) {
		finish_http1_upload(conn, ring);
	}
	return true;
}

[[nodiscard]] bool consume_http1_upload_line_char(
	Conn &conn,
	Ring &ring,
	char c) {
	conn.partial.consume(1);
	if (conn.http1_upload_line.size() >= conflux::http::kMaxChunkSizeLineBytes) {
		fail_http1_upload(
			conn,
			ring,
			upload_error(conflux::http::UploadErrorKind::malformed_body, "chunk line too large"));
		return false;
	}
	conn.http1_upload_line.push_back(c);
	return true;
}

[[nodiscard]] bool feed_http1_chunked_upload(
	Conn &conn,
	Ring &ring) {
	while (conn.http1_upload_body && !conn.partial.empty()) {
		switch (conn.http1_upload_chunk_phase) {
		case Http1UploadChunkPhase::size_line:
			{
				char const c = conn.partial.front();
				if (!consume_http1_upload_line_char(conn, ring, c)) {
					return false;
				}
				if (c != '\n') {
					break;
				}
				if (conn.http1_upload_line.size() < 2
					|| conn.http1_upload_line[conn.http1_upload_line.size() - 2] != '\r') {
					fail_http1_upload(
						conn,
						ring,
						upload_error(conflux::http::UploadErrorKind::malformed_body, "chunk size line missing CRLF"));
					return false;
				}
				auto line = std::string_view{conn.http1_upload_line}.substr(0, conn.http1_upload_line.size() - 2);
				std::size_t chunk_size{};
				if (!conflux::http::parse_chunk_size_line(line, chunk_size)) {
					fail_http1_upload(
						conn,
						ring,
						upload_error(conflux::http::UploadErrorKind::malformed_body, "invalid chunk size"));
					return false;
				}
				conn.http1_upload_line.clear();
				if (++conn.http1_upload_chunk_count > ring.parser_limits.max_chunks) {
					fail_http1_upload(
						conn,
						ring,
						upload_error(conflux::http::UploadErrorKind::malformed_body, "too many upload chunks"));
					return false;
				}
				if (chunk_size == 0) {
					conn.http1_upload_chunk_phase = Http1UploadChunkPhase::trailer_line;
				} else {
					if (chunk_size > ring.max_body_size
						|| conn.http1_upload_received > ring.max_body_size - chunk_size) {
						fail_http1_upload(
							conn,
							ring,
							upload_error(conflux::http::UploadErrorKind::body_too_large, "upload exceeds body limit"));
						return false;
					}
					conn.http1_upload_chunk_remaining = chunk_size;
					conn.http1_upload_chunk_phase = Http1UploadChunkPhase::data;
				}
				break;
			}
		case Http1UploadChunkPhase::data:
			{
				auto const n = std::min(conn.partial.size(), conn.http1_upload_chunk_remaining);
				std::string chunk{conn.partial.data(), n};
				conn.partial.consume(n);
				conn.http1_upload_chunk_remaining -= n;
				conn.http1_upload_received += n;
				{
					std::scoped_lock lk{ring.metrics_mu_};
					ring.upload_counters_.bytes_received += n;
				}
				if (!push_http1_upload_chunk(conn, ring, std::move(chunk))) {
					return false;
				}
				if (conn.http1_upload_chunk_remaining == 0) {
					conn.http1_upload_chunk_phase = Http1UploadChunkPhase::data_cr;
				}
				break;
			}
		case Http1UploadChunkPhase::data_cr:
			if (conn.partial.front() != '\r') {
				fail_http1_upload(
					conn,
					ring,
					upload_error(conflux::http::UploadErrorKind::malformed_body, "chunk data missing CRLF"));
				return false;
			}
			conn.partial.consume(1);
			conn.http1_upload_chunk_phase = Http1UploadChunkPhase::data_lf;
			break;
		case Http1UploadChunkPhase::data_lf:
			if (conn.partial.front() != '\n') {
				fail_http1_upload(
					conn,
					ring,
					upload_error(conflux::http::UploadErrorKind::malformed_body, "chunk data missing LF"));
				return false;
			}
			conn.partial.consume(1);
			conn.http1_upload_chunk_phase = Http1UploadChunkPhase::size_line;
			break;
		case Http1UploadChunkPhase::trailer_line:
			{
				char const c = conn.partial.front();
				if (!consume_http1_upload_line_char(conn, ring, c)) {
					return false;
				}
				if (c != '\n') {
					break;
				}
				if (conn.http1_upload_line.size() < 2
					|| conn.http1_upload_line[conn.http1_upload_line.size() - 2] != '\r') {
					fail_http1_upload(
						conn,
						ring,
						upload_error(conflux::http::UploadErrorKind::malformed_body, "chunk trailer missing CRLF"));
					return false;
				}
				auto const line_bytes = conn.http1_upload_line.size();
				if (!conflux::http::accept_chunk_trailer_line(
						line_bytes,
						conn.http1_upload_trailer_lines,
						conn.http1_upload_trailer_bytes)) {
					fail_http1_upload(
						conn,
						ring,
						upload_error(conflux::http::UploadErrorKind::malformed_body, "chunk trailer too large"));
					return false;
				}
				bool const done = conn.http1_upload_line == "\r\n";
				conn.http1_upload_line.clear();
				if (done) {
					finish_http1_upload(conn, ring);
					return true;
				}
				break;
			}
		case Http1UploadChunkPhase::done: return true;
		}
	}
	return true;
}

} // namespace

void feed_http1_upload(
	Conn &conn,
	Ring &ring) {
	if (!conn.http1_upload_body) {
		return;
	}
	if (conn.http1_upload_body->consumer_abandoned()) {
		{
			std::scoped_lock lk{ring.metrics_mu_};
			++ring.upload_counters_.canceled_by_handler;
		}
		conn.close_after_send = true;
		conn.partial.clear();
		conn.http1_upload_body.reset();
		conn.http1_upload_request_storage.reset();
		conn.http1_upload_content_length.reset();
		return;
	}
	if (conn.http1_upload_content_length) {
		auto _ = feed_http1_content_length_upload(conn, ring);
		return;
	}
	auto _ = feed_http1_chunked_upload(conn, ring);
}
