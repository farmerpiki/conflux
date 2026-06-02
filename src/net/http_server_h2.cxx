module;
#include <cstddef> // must precede openssl/nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" blocks re-include c++config.h

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#if CONFLUX_HAS_TLS
	#include <openssl/err.h>
	#include <openssl/ssl.h>
#endif
#if CONFLUX_HAS_HTTP2
	#include <nghttp2/nghttp2.h>
#endif
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

module conflux.net.http_server:h2;

import std;
import conflux.types;
import std.compat;

import conflux.net.http.types;
import conflux.net.http.response;
import conflux.net.router;
import conflux.file_map;
import conflux.net.detail.direct_slot_pool;
import conflux.net.vhost;
import conflux.net.config;
import conflux.net.http1_parser;
import conflux.net.http.parse_helpers;
import conflux.net.http_server_helpers;
import conflux.net.http_server_config;
import conflux.uring;
import conflux.uring.completion;
import conflux.work;
import conflux.file_io;
import conflux.socket_io;
import conflux.utils;
#if CONFLUX_HAS_HTTP2
import conflux.net.http2;
#endif
#if CONFLUX_HAS_HTTP3
import conflux.net.http3;
#endif
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif
import :state;
using namespace conflux::socket_io;

#if CONFLUX_HTTP_TRACE
	#define HTTP_TRACE(MSG) conflux::utils::eprintln(std::format("http_trace {}", (MSG)))
#else
	#define HTTP_TRACE(MSG) ((void)0)
#endif

#if CONFLUX_HAS_HTTP2
static constexpr std::size_t kH2PendingSendCap = std::size_t{64} * 1024;
struct H2RequestLease {
	std::string method{};
	std::string path{};
	conflux::http::HttpFields headers{};
	std::string body{};
	conflux::http::HttpFieldsView query{};
	conflux::http::HttpFieldsView form{};
	conflux::http::HttpFieldsView cookies{};
	std::vector<conflux::http::UploadedFile> files{};
};

static void h2_queue_raw_rst_stream(
	Conn &conn,
	std::int32_t stream_id,
	std::uint32_t error_code) {
	if (stream_id <= 0 || conn.h2_pending_send.size() + 13 > kH2PendingSendCap) {
		return;
	}
	auto const id = static_cast<std::uint32_t>(stream_id) & 0x7fffffffU;
	std::array<char, 13> frame{
		char{0},
		char{0},
		char{4},
		static_cast<char>(NGHTTP2_RST_STREAM),
		char{0},
		static_cast<char>((id >> 24U) & 0x7fU),
		static_cast<char>((id >> 16U) & 0xffU),
		static_cast<char>((id >> 8U) & 0xffU),
		static_cast<char>(id & 0xffU),
		static_cast<char>((error_code >> 24U) & 0xffU),
		static_cast<char>((error_code >> 16U) & 0xffU),
		static_cast<char>((error_code >> 8U) & 0xffU),
		static_cast<char>(error_code & 0xffU),
	};
	conn.h2_pending_send.append(frame.data(), frame.size());
}
// ---------------------------------------------------------------------------
void Ring::h2_reject_stream(
	nghttp2_session *session,
	H2Stream &stream,
	std::int32_t stream_id,
	std::uint32_t error_code) {
	stream.rejected = true;
	nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, stream_id, error_code);
}

[[nodiscard]] bool Ring::h2_prevalidate_client_frames(
	Conn &conn,
	std::string_view bytes) {
	if (conn.h2_session == nullptr) {
		return true;
	}
	std::size_t offset = 0;
	constexpr std::string_view client_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
	if (!conn.h2_client_preface_seen && bytes.starts_with(client_preface)) {
		offset = client_preface.size();
		conn.h2_client_preface_seen = true;
	}
	while (bytes.size() - offset >= 9) {
		auto const *frame = reinterpret_cast<unsigned char const *>(bytes.data() + offset);
		auto const length = (std::size_t{frame[0]} << 16U) | (std::size_t{frame[1]} << 8U) | std::size_t{frame[2]};
		if (bytes.size() - offset - 9 < length) {
			return true;
		}
		auto const type = frame[3];
		auto const stream_id = static_cast<std::int32_t>(
			(std::uint32_t{frame[5] & 0x7fU} << 24U)
			| (std::uint32_t{frame[6]} << 16U)
			| (std::uint32_t{frame[7]} << 8U)
			| std::uint32_t{frame[8]});
		auto const payload = std::string_view{bytes.data() + offset + 9, length};

		if (type == NGHTTP2_PRIORITY) {
			if (stream_id == 0) {
				nghttp2_session_terminate_session(conn.h2_session, NGHTTP2_PROTOCOL_ERROR);
				return false;
			}
			if (length != 5) {
				h2_queue_raw_rst_stream(conn, stream_id, NGHTTP2_FRAME_SIZE_ERROR);
				return false;
			}
			if (payload.size() >= 4) {
				auto const dependency = static_cast<std::int32_t>(
					(std::uint32_t{static_cast<unsigned char>(payload[0]) & 0x7fU} << 24U)
					| (std::uint32_t{static_cast<unsigned char>(payload[1])} << 16U)
					| (std::uint32_t{static_cast<unsigned char>(payload[2])} << 8U)
					| std::uint32_t{static_cast<unsigned char>(payload[3])});
				if (dependency == stream_id) {
					h2_queue_raw_rst_stream(conn, stream_id, NGHTTP2_PROTOCOL_ERROR);
					return false;
				}
			}
		}
		if (type == NGHTTP2_RST_STREAM && stream_id != 0) {
			conn.h2_closed_streams.insert(stream_id);
			if (conn.h2_closed_streams.size() > 256) {
				conn.h2_closed_streams.erase(conn.h2_closed_streams.begin());
			}
		}
		if (type == NGHTTP2_WINDOW_UPDATE && stream_id != 0 && payload.size() == 4) {
			auto const increment = (std::uint32_t{static_cast<unsigned char>(payload[0]) & 0x7fU} << 24U)
								 | (std::uint32_t{static_cast<unsigned char>(payload[1])} << 16U)
								 | (std::uint32_t{static_cast<unsigned char>(payload[2])} << 8U)
								 | std::uint32_t{static_cast<unsigned char>(payload[3])};
			auto &total = conn.h2_stream_window_updates[stream_id];
			if (increment > std::numeric_limits<std::int32_t>::max() - total) {
				h2_queue_raw_rst_stream(conn, stream_id, NGHTTP2_FLOW_CONTROL_ERROR);
				return false;
			}
			total += increment;
		}

		if ((type == NGHTTP2_DATA || type == NGHTTP2_HEADERS) && stream_id != 0) {
			if (conn.h2_closed_streams.contains(stream_id)) {
				if (type == NGHTTP2_HEADERS) {
					nghttp2_session_terminate_session(conn.h2_session, NGHTTP2_STREAM_CLOSED);
				} else {
					h2_queue_raw_rst_stream(conn, stream_id, NGHTTP2_STREAM_CLOSED);
				}
				return false;
			}
			if (type == NGHTTP2_HEADERS && !conn.h2_streams.contains(stream_id)) {
				if (stream_id < conn.h2_max_client_stream_id) {
					nghttp2_session_terminate_session(conn.h2_session, NGHTTP2_PROTOCOL_ERROR);
					return false;
				}
				if ((stream_id & 1) != 0) {
					conn.h2_max_client_stream_id = std::max(conn.h2_max_client_stream_id, stream_id);
				}
			}
		}

		offset += 9 + length;
	}
	return true;
}

[[nodiscard]] bool Ring::h2_valid_regular_header_name(
	std::string_view name) noexcept {
	if (!conflux::http::is_valid_header_name(name)) {
		return false;
	}
	for (char const c: name) {
		if (c >= 'A' && c <= 'Z') {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool Ring::h2_forbidden_connection_header(
	std::string_view name) noexcept {
	return name == "connection"
		|| name == "keep-alive"
		|| name == "proxy-connection"
		|| name == "transfer-encoding"
		|| name == "upgrade";
}

ssize_t Ring::h2_send_cb(
	nghttp2_session * /*unused*/,
	std::uint8_t const *data,
	std::size_t length,
	int /*unused*/,
	void *user_data) {
	auto *ctx = static_cast<H2ConnCtx *>(user_data);
	auto &conn = ctx->ring->conn_for(ctx->fd);
	if (conn.h2_pending_send.size() >= kH2PendingSendCap) {
		return NGHTTP2_ERR_WOULDBLOCK;
	}
	auto const available = kH2PendingSendCap - conn.h2_pending_send.size();
	auto const to_copy = std::min(length, available);
	conn.h2_pending_send.append(reinterpret_cast<char const *>(data), to_copy);
	return static_cast<ssize_t>(to_copy);
}

int Ring::h2_on_begin_headers_cb(
	nghttp2_session * /*unused*/,
	nghttp2_frame const *frame,
	void *user_data) {
	if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
		return 0;
	}
	auto *ctx = static_cast<H2ConnCtx *>(user_data);
	auto &conn = ctx->ring->conn_for(ctx->fd);
	conn.h2_streams.emplace(frame->hd.stream_id, H2Stream{});
	return 0;
}

// Populate H2Stream fields from pseudo-headers and regular headers.
int Ring::h2_on_header_cb(
	nghttp2_session *session,
	nghttp2_frame const *frame,
	std::uint8_t const *name,
	std::size_t namelen,
	std::uint8_t const *header_value,
	std::size_t valuelen,
	std::uint8_t /*unused*/,
	void *user_data) {
	auto *ctx = static_cast<H2ConnCtx *>(user_data);
	auto &conn = ctx->ring->conn_for(ctx->fd);
	auto it = conn.h2_streams.find(frame->hd.stream_id);
	if (it == conn.h2_streams.end()) {
		return 0;
	}
	auto &stream = it->second;
	if (stream.rejected) {
		return 0;
	}
	std::string_view const n{reinterpret_cast<char const *>(name), namelen};
	std::string_view const v{reinterpret_cast<char const *>(header_value), valuelen};
	if (stream.header_count == std::numeric_limits<std::size_t>::max()
		|| namelen > std::numeric_limits<std::size_t>::max() - valuelen
		|| stream.header_list_size > std::numeric_limits<std::size_t>::max() - namelen - valuelen) {
		h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_ENHANCE_YOUR_CALM);
		return 0;
	}
	++stream.header_count;
	stream.header_list_size += namelen + valuelen;
	if (stream.header_count > ctx->ring->parser_limits.max_headers
		|| stream.header_list_size > ctx->ring->parser_limits.max_header_block_size) {
		h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_ENHANCE_YOUR_CALM);
		return 0;
	}
	if (n.starts_with(":")) {
		if (stream.regular_header_seen) {
			h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
			return 0;
		}
		if (n == ":method") {
			if (stream.seen_method) {
				h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
				return 0;
			}
			stream.seen_method = true;
			stream.method = std::string{v};
		} else if (n == ":path") {
			if (stream.seen_path) {
				h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
				return 0;
			}
			stream.seen_path = true;
			stream.path = std::string{v};
		} else if (n == ":scheme") {
			if (stream.seen_scheme) {
				h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
				return 0;
			}
			stream.seen_scheme = true;
			stream.scheme = std::string{v};
		} else if (n == ":authority") {
			if (stream.seen_authority) {
				h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
				return 0;
			}
			stream.seen_authority = true;
			stream.authority = std::string{v};
		} else {
			h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
			return 0;
		}
	} else {
		stream.regular_header_seen = true;
		if (!h2_valid_regular_header_name(n) || h2_forbidden_connection_header(n)) {
			h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
			return 0;
		}
		if (n == "te" && v != "trailers") {
			h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
			return 0;
		}
		if (n == "content-length") {
			if (stream.seen_content_length) {
				h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
				return 0;
			}
			auto parsed_content_length = conflux::http::parse_content_length_limited(v, ctx->ring->max_body_size);
			if (parsed_content_length) {
				stream.expected_body_size = *parsed_content_length;
			} else {
				h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_CANCEL);
				return 0;
			}
			stream.seen_content_length = true;
		}
		stream.headers.emplace_back(std::string{n}, std::string{v});
	}
	return 0;
}

// Accumulate DATA frame body bytes into stream.body.
int Ring::h2_on_data_chunk_cb(
	nghttp2_session *session,
	std::uint8_t /*unused*/,
	std::int32_t stream_id,
	std::uint8_t const *data,
	std::size_t len,
	void *user_data) {
	auto *ctx = static_cast<H2ConnCtx *>(user_data);
	auto &conn = ctx->ring->conn_for(ctx->fd);
	auto it = conn.h2_streams.find(stream_id);
	if (it != conn.h2_streams.end()) {
		auto &stream = it->second;
		if (stream.rejected) {
			return 0;
		}
		if (len > ctx->ring->max_body_size || stream.body.size() > ctx->ring->max_body_size - len) {
			h2_reject_stream(session, stream, stream_id, NGHTTP2_CANCEL);
			return 0;
		}
		if (stream.seen_content_length && len > stream.expected_body_size - stream.body.size()) {
			h2_reject_stream(session, stream, stream_id, NGHTTP2_PROTOCOL_ERROR);
			return 0;
		}
		if (!stream.body_reserved && stream.expected_body_size > 0) {
			stream.body.reserve(stream.expected_body_size);
			stream.body_reserved = true;
		}
		stream.body.append(reinterpret_cast<char const *>(data), len);
	}
	return 0;
}

// Data provider: feed response body bytes to nghttp2's framing layer.
// Handles both static responses and SSE streaming.
ssize_t Ring::h2_read_cb(
	nghttp2_session *session,
	std::int32_t stream_id,
	std::uint8_t *buf,
	std::size_t length,
	std::uint32_t *data_flags,
	nghttp2_data_source *source,
	void * /*user_data*/) {
	auto &stream = *static_cast<H2Stream *>(source->ptr);

	// SSE streaming path: drain from the channel, defer when empty.
	if (stream.sse_channel) {
		if (stream.h2_sse_buf.empty()) {
			stream.h2_sse_buf = stream.sse_channel->drain();
		}
		if (stream.h2_sse_buf.empty()) {
			if (stream.sse_channel->is_closed()) {
				*data_flags |= NGHTTP2_DATA_FLAG_EOF;
				return 0;
			}
			return NGHTTP2_ERR_DEFERRED;
		}
		auto to_copy = std::min(stream.h2_sse_buf.size(), length);
		std::copy_n(stream.h2_sse_buf.data(), to_copy, buf);
		stream.h2_sse_buf.erase(0, to_copy);
		// Don't set EOF — channel may produce more events.
		return static_cast<ssize_t>(to_copy);
	}

	// Static response body path.
	auto remaining = stream.response_body.size() - stream.response_off;
	auto to_copy = std::min(remaining, length);
	memcpy(buf, std::span{stream.response_body}.subspan(stream.response_off).data(), to_copy);
	stream.response_off += to_copy;
	if (stream.response_off >= stream.response_body.size()) {
		// If the response carries trailers, suppress the END_STREAM flag on the
		// DATA frame so we can send a HEADERS frame with the trailers after.
		if (!stream.response_trailers.empty()) {
			*data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
			*data_flags |= NGHTTP2_DATA_FLAG_EOF;
			std::vector<nghttp2_nv> nva;
			nva.reserve(stream.response_trailers.size());
			for (auto const &[n, v]: stream.response_trailers) {
				nva.push_back(
					{reinterpret_cast<std::uint8_t *>(const_cast<char *>(n.data())),
					 reinterpret_cast<std::uint8_t *>(const_cast<char *>(v.data())),
					 n.size(),
					 v.size(),
					 NGHTTP2_NV_FLAG_NONE});
			}
			nghttp2_submit_trailer(session, stream_id, nva.data(), nva.size());
			stream.response_trailers.clear();
		} else {
			*data_flags |= NGHTTP2_DATA_FLAG_EOF;
		}
	}
	return static_cast<ssize_t>(to_copy);
}

void Ring::h2_submit_response(
	Conn &conn,
	std::int32_t stream_id,
	conflux::http::Response resp) {
	auto it = conn.h2_streams.find(stream_id);
	if (it == conn.h2_streams.end()) {
		return;
	}
	auto &stream = it->second;

	if (resp.is_deferred()) {
		resp = conflux::http::Response::internal_error("nested deferred responses unsupported over HTTP/2");
	}
	if (resp.is_ws_upgrade()) {
		resp = conflux::http::Response::internal_error("websocket upgrades unsupported over HTTP/2");
	}
	if (resp.is_mapped_file()) {
		resp = conflux::http::Response::internal_error("mapped files unsupported over HTTP/2");
	}

	bool const is_sse_resp = resp.is_sse();
	std::string const status_str = std::to_string(resp.status);
	std::string const clen_str = std::to_string(resp.content_length());
	std::vector<std::pair<std::string, std::string>> nv_storage;
	nv_storage.reserve(3 + resp.headers.size() + resp.set_cookies.size());
	nv_storage.emplace_back(":status", status_str);
	nv_storage.emplace_back("content-type", resp.content_type);
	if (!is_sse_resp) {
		nv_storage.emplace_back("content-length", clen_str);
	}
	for (auto const &[k, v]: resp.headers) {
		nv_storage.emplace_back(k, v);
	}
	for (auto const &sc: resp.set_cookies) {
		nv_storage.emplace_back("set-cookie", sc);
	}
	if (conn.h2_ctx != nullptr && conn.h2_ctx->ring != nullptr && !conn.h2_ctx->ring->alt_svc_header.empty()) {
		nv_storage.emplace_back("alt-svc", conn.h2_ctx->ring->alt_svc_header);
	}

	std::vector<nghttp2_nv> nva;
	nva.reserve(nv_storage.size());
	for (auto &[n, v]: nv_storage) {
		nva.push_back(
			{reinterpret_cast<std::uint8_t *>(n.data()),
			 reinterpret_cast<std::uint8_t *>(v.data()),
			 n.size(),
			 v.size(),
			 NGHTTP2_NV_FLAG_NONE});
	}

	stream.response_body = resp.take_text_body();
	stream.response_off = 0;
	stream.response_trailers = std::move(resp.trailers);

	nghttp2_data_provider prd{};
	prd.read_callback = h2_read_cb;
	prd.source.ptr = &stream;

	if (is_sse_resp) {
		stream.sse_channel = resp.take_sse_channel();
		conn.sse_efd = stream.sse_channel->eventfd_fd();
		conn.sse_channel = stream.sse_channel;
		conn.h2_sse_stream_id = stream_id;
		conn.h2_sse_pending_wait = true;
	}

	nghttp2_submit_response(
		conn.h2_session,
		stream_id,
		nva.data(),
		nva.size(),
		(stream.response_body.empty() && !is_sse_resp) ? nullptr : &prd);
}

// A frame is fully received.  On END_STREAM, dispatch to the router and
// submit the HTTP/2 response via nghttp2_submit_response.
int Ring::h2_on_frame_recv_cb(
	nghttp2_session *session,
	nghttp2_frame const *frame,
	void *user_data) {
	// Only act on request streams that are now complete.
	if ((frame->hd.flags & NGHTTP2_FLAG_END_STREAM) == 0) {
		return 0;
	}
	if (frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA) {
		return 0;
	}

	auto *ctx = static_cast<H2ConnCtx *>(user_data);
	auto &conn = ctx->ring->conn_for(ctx->fd);
	auto it = conn.h2_streams.find(frame->hd.stream_id);
	if (it == conn.h2_streams.end()) {
		return 0;
	}
	auto &stream = it->second;
	if (stream.rejected) {
		return 0;
	}
	if (stream.end_stream_seen) {
		return 0;
	}
	stream.end_stream_seen = true;
	if (!stream.seen_method
		|| !stream.seen_path
		|| !stream.seen_scheme
		|| stream.method.empty()
		|| stream.path.empty()
		|| stream.scheme.empty()) {
		h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
		return 0;
	}
	if (stream.seen_content_length && stream.body.size() != stream.expected_body_size) {
		h2_reject_stream(session, stream, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
		return 0;
	}

	auto request_lease = std::make_shared<H2RequestLease>();
	request_lease->method = std::move(stream.method);
	request_lease->path = std::move(stream.path);
	request_lease->headers = std::move(stream.headers);
	request_lease->body = std::move(stream.body);

	std::string_view const method = request_lease->method;
	auto const target = conflux::http::split_path_query(request_lease->path);
	std::string_view const path = target.path;
	std::string_view const version = "HTTP/2";
	std::string_view const body = request_lease->body;
	conflux::http::HttpFieldsView const params;
	conflux::http::populate_request_parts(
		target,
		request_lease->headers,
		body,
		request_lease->query,
		request_lease->form,
		request_lease->cookies,
		request_lease->files);

	conflux::http::RequestView const req{
		method,
		path,
		version,
		conn.remote_addr,
		true,
		params,
		request_lease->headers,
		request_lease->query,
		request_lease->form,
		request_lease->cookies,
		request_lease->files,
		body};

	conflux::http::Response resp;
	try {
		resp = ctx->ring->dispatch(req);
	} catch (std::exception const &e) { resp = conflux::http::Response::internal_error(e.what()); } catch (...) {
		resp = conflux::http::Response::internal_error();
	}

	if (resp.is_deferred()) {
		auto deferred_response = resp.deferred_response_ptr();
		deferred_response->keep_alive(request_lease);
		stream.deferred_efd = deferred_response->eventfd_fd();
		ctx->ring
			->queue_deferred_wait(ctx->fd, stream.deferred_efd, resp.take_deferred_response(), frame->hd.stream_id);
		return 0;
	}
	h2_submit_response(conn, frame->hd.stream_id, std::move(resp));
	return 0;
}

int Ring::h2_on_invalid_frame_recv_cb(
	nghttp2_session *session,
	nghttp2_frame const *frame,
	int lib_error_code,
	void * /*user_data*/) {
	auto error_code = NGHTTP2_PROTOCOL_ERROR;
	if (lib_error_code == NGHTTP2_ERR_FRAME_SIZE_ERROR) {
		error_code = NGHTTP2_FRAME_SIZE_ERROR;
	} else if (lib_error_code == NGHTTP2_ERR_FLOW_CONTROL) {
		error_code = NGHTTP2_FLOW_CONTROL_ERROR;
	} else if (lib_error_code == NGHTTP2_ERR_STREAM_CLOSED) {
		error_code = NGHTTP2_STREAM_CLOSED;
	}
	if (frame->hd.stream_id == 0) {
		nghttp2_session_terminate_session(session, static_cast<std::uint32_t>(error_code));
		return 0;
	}
	nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, frame->hd.stream_id, static_cast<std::uint32_t>(error_code));
	return 0;
}

// Stream fully closed — release its state.
int Ring::h2_on_stream_close_cb(
	nghttp2_session * /*unused*/,
	std::int32_t stream_id,
	std::uint32_t /*EC*/,
	void *user_data) {
	auto *ctx = static_cast<H2ConnCtx *>(user_data);
	auto &conn = ctx->ring->conn_for(ctx->fd);
	if (auto it = conn.h2_streams.find(stream_id); it != conn.h2_streams.end()) {
		ctx->ring->clear_deferred_wait(it->second.deferred_efd);
	}
	conn.h2_closed_streams.insert(stream_id);
	if (conn.h2_closed_streams.size() > 256) {
		conn.h2_closed_streams.erase(conn.h2_closed_streams.begin());
	}
	// If this was the active H2 SSE stream, clear conn-level SSE state.
	if (conn.h2_sse_stream_id == stream_id) {
		conn.h2_sse_stream_id = -1;
		conn.h2_sse_pending_wait = false;
		conn.sse_efd = -1;
		conn.sse_channel.reset();
	}
	conn.h2_streams.erase(stream_id);
	return 0;
}

// ---------------------------------------------------------------------------
// H2 Ring methods
// ---------------------------------------------------------------------------

// SSL_write h2_pending_send into tls_send_buf, then queue a TLS send.
// No-op if nothing pending or a send is already in flight (data accumulates
// and will be flushed when handle_send_tls_complete's H2 branch runs next).
void Ring::h2_flush_pending(
	Conn &conn) {
	if (conn.h2_pending_send.empty() || conn.send_queued) {
		return;
	}
	char const *h2_data = conn.h2_pending_send.data();
	int h2_remaining = static_cast<int>(conn.h2_pending_send.size());
	while (h2_remaining > 0) {
		auto const w = SSL_write(conn.ssl.get(), h2_data, h2_remaining);
		if (w <= 0) {
			queue_close(conn.fd);
			return;
		}
		h2_data += w;
		h2_remaining -= w;
	}
	conn.h2_pending_send.clear();
	tls_flush_wbio(conn);
	tls_queue_send(conn);
}

// Drive nghttp2 output (all queued frames) and flush to io_uring.
void Ring::h2_do_send(
	Conn &conn) {
	if (conn.h2_session == nullptr) {
		return;
	}
	if (!conn.h2_pending_send.empty()) {
		h2_flush_pending(conn);
		if (conn.send_queued || conn.closing || !conn.h2_pending_send.empty()) {
			return;
		}
	}
	if (nghttp2_session_want_write(conn.h2_session) == 0) {
		h2_flush_pending(conn);
		if (!conn.send_queued && conn.h2_pending_send.empty() && nghttp2_session_want_read(conn.h2_session) == 0) {
			queue_close(conn.fd);
		}
		return;
	}
	if (nghttp2_session_send(conn.h2_session) != 0) {
		queue_close(conn.fd);
		return;
	}
	h2_flush_pending(conn);
}

// Create nghttp2 server session and submit the server connection preface
// (SETTINGS frame).  Does NOT flush — caller must call h2_do_send() after
// running nghttp2_session_mem_recv() so that the SETTINGS and SETTINGS_ACK
// are coalesced into a single TLS record.
void Ring::h2_setup_conn(
	Conn &conn) {
	nghttp2_session_callbacks *cbs = nullptr;
	nghttp2_session_callbacks_new(&cbs);
	nghttp2_session_callbacks_set_send_callback(cbs, h2_send_cb);
	nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, h2_on_begin_headers_cb);
	nghttp2_session_callbacks_set_on_header_callback(cbs, h2_on_header_cb);
	nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, h2_on_data_chunk_cb);
	nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, h2_on_frame_recv_cb);
	nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(cbs, h2_on_invalid_frame_recv_cb);
	nghttp2_session_callbacks_set_on_stream_close_callback(cbs, h2_on_stream_close_cb);

	conn.h2_ctx = std::make_unique<H2ConnCtx>(H2ConnCtx{.ring = this, .fd = conn.fd});
	if (nghttp2_session_server_new(&conn.h2_session, cbs, conn.h2_ctx.get()) != 0) {
		conn.h2_session = nullptr;
	}
	nghttp2_session_callbacks_del(cbs);
	if (conn.h2_session == nullptr) {
		queue_close(conn.fd);
		return;
	}

	constexpr std::uint32_t kH2MaxConcurrentStreams = 100;
	constexpr std::uint32_t kH2InitialWindowSize = 1U << 24;
	std::uint32_t const h2_max_header_list_size = static_cast<std::uint32_t>(std::min<std::size_t>(
		parser_limits.max_header_block_size,
		static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
	std::array<nghttp2_settings_entry, 3> const iv{
		{
         {.settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, .value = kH2MaxConcurrentStreams},
         {.settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, .value = kH2InitialWindowSize},
         {.settings_id = NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, .value = h2_max_header_list_size},
		 }
    };
	nghttp2_submit_settings(conn.h2_session, NGHTTP2_FLAG_NONE, iv.data(), iv.size());
	// Flush deferred to caller's h2_do_send().
}

#endif // CONFLUX_HAS_HTTP2
