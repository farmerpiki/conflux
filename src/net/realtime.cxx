module;
#include <cerrno>
#include <cstring>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#if CONFLUX_HAS_TLS
	#include <openssl/ssl.h>
#endif
#if defined(CONFLUX_STDSIMD)
extern "C" {
void conflux_ws_unmask_stdsimd(unsigned char *data, __SIZE_TYPE__ n, unsigned char const *mask4);
}
#endif

export module conflux.net.http.realtime;

#ifdef CONFLUX_BUILD_FUZZ
	#define CONFLUX_FUZZ_EXPORT export
#else
	#define CONFLUX_FUZZ_EXPORT
#endif

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.crypto;
import conflux.utils;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif

export enum class SseOverflowPolicy : u8 {
	DropNewest,
	DropOldest,
	Disconnect,
};
export class SseChannel {
private:
	int efd_{};
	mutex mtx_{};
	std::queue<S> pending_{};
	atomic_flag closed_{};
	SZ queued_bytes_{0};
	SZ max_queue_bytes_{};
	SseOverflowPolicy overflow_{SseOverflowPolicy::DropNewest};
	Atom<SZ> dropped_{0};

public:
	static constexpr SZ kDefaultMaxQueueBytes = SZ{4} * 1024 * 1024;

	SseChannel(SseChannel const &) = delete;
	SseChannel &operator =(SseChannel const &) = delete;
	SseChannel(SseChannel &&) = delete;
	SseChannel &operator =(SseChannel &&) = delete;
	explicit SseChannel(
		SZ max_queue_bytes = kDefaultMaxQueueBytes,
		SseOverflowPolicy overflow = SseOverflowPolicy::DropNewest)
		: efd_(::eventfd(0, EFD_CLOEXEC))
		, max_queue_bytes_(max_queue_bytes)
		, overflow_(overflow) {
		if (efd_ < 0) {
			throw SE{errno, system_category(), "eventfd"};
		}
	}
	~SseChannel() noexcept {
		try {
			close();
		} catch (...) {} // NOLINT(bugprone-empty-catch): dtor must not propagate
		::close(efd_);
	}
	// Returns true if the frame was enqueued, false if dropped (overflow).
	// Also returns false if the channel is closed, regardless of policy.
	// Channel takes ownership of frame.
	[[nodiscard]] bool send(
		S frame) {
		bool enqueued = false;
		bool wake = false;
		{
			SL const lk{mtx_};
			if (closed_.test()) {
				return false;
			}
			SZ const frame_bytes = frame.size();
			SZ const would_be = queued_bytes_ + frame_bytes;
			if (would_be > max_queue_bytes_ && max_queue_bytes_ != 0) {
				switch (overflow_) {
				case SseOverflowPolicy::DropNewest: dropped_.fetch_add(1, memory_order_relaxed); return false;
				case SseOverflowPolicy::DropOldest:
					while (!pending_.empty() && queued_bytes_ + frame_bytes > max_queue_bytes_) {
						queued_bytes_ -= pending_.front().size();
						pending_.pop();
						dropped_.fetch_add(1, memory_order_relaxed);
					}
					break;
				case SseOverflowPolicy::Disconnect:
					closed_.test_and_set();
					dropped_.fetch_add(1, memory_order_relaxed);
					wake = true;
					break;
				}
			}
			if (!closed_.test()) {
				queued_bytes_ += frame_bytes;
				pending_.push(move(frame));
				enqueued = true;
				wake = true;
			}
		}
		if (wake) {
			u64 v = 1;
			if (::write(efd_, &v, sizeof(v)) < 0 && errno != EAGAIN) {
				eprintln(format("SseChannel::send: eventfd write: {}", strerror(errno)));
			}
		}
		return enqueued;
	}
	// Zero-copy intent: caller owns the backing buffer and is responsible for
	// keeping it alive until the frame is flushed to the socket. Currently
	// copies into the queue; when the queue migrates to SV storage this
	// contract becomes a hard lifetime requirement.
	[[nodiscard]] bool send_view(
		SV frame) {
		return send(S{frame});
	}
	[[nodiscard]] bool send_event(
		SV type,
		SV data) {
		// Reject newlines in type and data: they would break SSE framing and
		// allow injection of arbitrary events.
		auto has_nl = [](SV s) { return s.find('\n') != SV::npos || s.find('\r') != SV::npos; };
		if (has_nl(type) || has_nl(data)) {
			throw std::invalid_argument{"SseChannel::send_event: type and data must not contain newlines"};
		}
		return send(format("event: {}\ndata: {}\n\n", type, data));
	}
	void close() {
		if (closed_.test_and_set()) {
			return;
		} // already closed
		u64 v = 1;
		if (::write(efd_, &v, sizeof(v)) < 0 && errno != EAGAIN) {
			eprintln(format("SseChannel::close: eventfd write: {}", strerror(errno)));
		} // wake the io_uring poll
	}
	[[nodiscard]] S drain() {
		SL const lk{mtx_};
		S result;
		while (!pending_.empty()) {
			result += pending_.front();
			pending_.pop();
		}
		queued_bytes_ = 0;
		return result;
	}
	[[nodiscard]] bool is_closed() const noexcept { return closed_.test(); }
	[[nodiscard]] int eventfd_fd() const noexcept { return efd_; }
	[[nodiscard]] SZ dropped_count() const noexcept { return dropped_.load(memory_order_relaxed); }
	[[nodiscard]] SZ max_queue_bytes() const noexcept { return max_queue_bytes_; }
};

// ---------------------------------------------------------------------------
// SseBroadcaster: fan-out pub/sub for SSE streams.
// ---------------------------------------------------------------------------
// Maintains a set of active SseChannel weak_ptrs.  broadcast() delivers an
// SSE event to every currently-connected subscriber.  Stale weak_ptrs are
// reaped on each broadcast call.
export class SseBroadcaster {
public:
	SseBroadcaster() = default;
	~SseBroadcaster() = default;
	SseBroadcaster(SseBroadcaster const &) = delete;
	SseBroadcaster &operator =(SseBroadcaster const &) = delete;
	SseBroadcaster(SseBroadcaster &&) = delete;
	SseBroadcaster &operator =(SseBroadcaster &&) = delete;
	// Register a new subscriber.  Returns the SP to pass to HttpResponse::sse().
	SP<SseChannel> subscribe() {
		auto ch = make_shared<SseChannel>();
		SL const lk{mtx_};
		channels_.emplace_back(ch);
		return ch;
	}
	// Broadcast an SSE event to all active subscribers.
	void broadcast(
		SV event,
		SV data) {
		auto frame = format("event: {}\ndata: {}\n\n", event, data);
		broadcast_raw(frame);
	}
	// Broadcast a data-only SSE message to all active subscribers.
	void broadcast_data(
		SV data) {
		auto frame = format("data: {}\n\n", data);
		broadcast_raw(frame);
	}
	// Number of currently-active subscribers (approximate; may include ones
	// that have just disconnected).
	[[nodiscard]] SZ subscriber_count() const {
		SL const lk{mtx_};
		return channels_.size();
	}

private:
	void broadcast_raw(
		S const &frame) {
		SL const lk{mtx_};
		// Erase stale weak_ptrs while delivering to live ones.
		std::erase_if(channels_, [&](weak_ptr<SseChannel> const &wch) {
			auto ch = wch.lock();
			if (!ch || ch->is_closed()) {
				return true;
			}
			auto _ = ch->send(frame);
			return false;
		});
	}
	mutable mutex mtx_;
	V<weak_ptr<SseChannel>> channels_;
};

// WebSocket support
// ---------------------------------------------------------------------------

namespace ws_detail {

// Compute Sec-WebSocket-Accept from Sec-WebSocket-Key.
export S ws_accept_key(
	SV client_key) {
	static constexpr SV kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	S input{client_key};
	input += kMagic;
	auto digest = sha1(to_unsigned_span(input));
	return base64_encode(span{digest.data(), digest.size()});
}
bool is_valid_client_key(
	SV key) {
	if (key.size() != 24) {
		return false;
	}
	auto decoded = base64_decode(key);
	return decoded.size() == 16 && base64_encode(to_unsigned_span(decoded)) == key;
}
export bool is_valid_handshake(
	HttpRequestView const &req) {
	return conflux::http::header_token_contains(req.headers["upgrade"], "websocket")
		&& conflux::http::header_token_contains(req.headers["connection"], "upgrade")
		&& trim(req.headers["sec-websocket-version"]) == "13"
		&& is_valid_client_key(trim(req.headers["sec-websocket-key"]));
}
// Build a complete WebSocket frame (server→client, unmasked) in one buffer so
// the transport call below emits header+payload as a single TCP segment / TLS record.
S ws_build_frame(
	u8 opcode,
	span<byte const> payload) {
	A<u8, 10> hdr{};
	SZ hdr_len = 0;
	hdr[hdr_len++] = 0x80U | opcode; // FIN + opcode
	SZ const len = payload.size();
	if (len < 126) {
		hdr[hdr_len++] = static_cast<u8>(len);
	} else if (len <= 0xFFFF) {
		hdr[hdr_len++] = 126;
		hdr[hdr_len++] = static_cast<u8>(len >> 8);
		hdr[hdr_len++] = static_cast<u8>(len & 0xFF);
	} else {
		hdr[hdr_len++] = 127;
		for (int s = 56; s >= 0; s -= 8) {
			hdr[hdr_len++] = static_cast<u8>((len >> s) & 0xFF);
		}
	}
	S frame;
	frame.reserve(hdr_len + len);
	frame.append(reinterpret_cast<char const *>(hdr.data()), hdr_len);
	frame.append(reinterpret_cast<char const *>(payload.data()), len);
	return frame;
}
bool ws_send_frame(
	int fd,
	u8 opcode,
	span<byte const> payload) {
	auto frame = ws_detail::ws_build_frame(opcode, payload);
	SZ sent = 0;
	while (sent < frame.size()) {
		auto n = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		sent += static_cast<SZ>(n);
	}
	return true;
}
#if CONFLUX_HAS_TLS
bool ws_tls_send_frame(
	SSL *ssl,
	u8 opcode,
	span<byte const> payload) {
	auto frame = ws_detail::ws_build_frame(opcode, payload);
	SZ sent = 0;
	while (sent < frame.size()) {
		auto const chunk = min<SZ>(frame.size() - sent, static_cast<SZ>(NL<int>::max()));
		int const n = SSL_write(ssl, frame.data() + sent, static_cast<int>(chunk));
		if (n > 0) {
			sent += static_cast<SZ>(n);
			continue;
		}
		int const err = SSL_get_error(ssl, n);
		if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
			continue;
		}
		return false;
	}
	return true;
}
#endif // CONFLUX_HAS_TLS
[[nodiscard]] bool is_valid_close_code(
	u16 code) {
	if (code < 1000U) {
		return false;
	}
	if (code <= 1003U) {
		return true;
	}
	if (code <= 1006U) {
		return false;
	}
	if (code <= 1014U) {
		return true;
	}
	if (code <= 4999U) {
		return code >= 3000U;
	}
	return false;
}
CONFLUX_FUZZ_EXPORT bool utf8_is_valid(
	SV s) {
	SZ i = 0;
	while (i < s.size()) {
		auto const b = static_cast<u8>(s[i]);
		SZ extra{};
		u32 min_cp{};
		u32 cp{};
		if (b < 0x80U) {
			++i;
			continue;
		}
		if ((b & 0xE0U) == 0xC0U) {
			extra = 1;
			min_cp = 0x80U;
			cp = b & 0x1FU;
		} else if ((b & 0xF0U) == 0xE0U) {
			extra = 2;
			min_cp = 0x800U;
			cp = b & 0x0FU;
		} else if ((b & 0xF8U) == 0xF0U) {
			extra = 3;
			min_cp = 0x10000U;
			cp = b & 0x07U;
		} else {
			return false;
		}
		if (i + extra >= s.size()) {
			return false;
		}
		for (SZ k = 1; k <= extra; ++k) {
			auto const c = static_cast<u8>(s[i + k]);
			if ((c & 0xC0U) != 0x80U) {
				return false;
			}
			cp = (cp << 6U) | (c & 0x3FU);
		}
		if (cp < min_cp) {
			return false;
		}
		if (cp >= 0xD800U && cp <= 0xDFFFU) {
			return false;
		}
		if (cp > 0x10FFFFU) {
			return false;
		}
		i += extra + 1;
	}
	return true;
}
CONFLUX_FUZZ_EXPORT struct FrameHeader {
	u8 opcode{};
	bool fin{};
	bool masked{};
	u64 payload_len{};
	A<u8, 4> mask{};
	SZ header_size{};
};

CONFLUX_FUZZ_EXPORT enum class FrameParseStatus : u8 {
	Ok,
	Incomplete,
	ProtocolError,
	ControlTooLarge,
};
CONFLUX_FUZZ_EXPORT FrameParseStatus parse_frame_header(
	span<byte const> buf,
	FrameHeader &out) {
	if (buf.size() < 2) {
		return FrameParseStatus::Incomplete;
	}
	auto const b0 = to_integer<u8>(buf[0]);
	auto const b1 = to_integer<u8>(buf[1]);
	out.fin = (b0 & 0x80U) != 0;
	out.opcode = b0 & 0x0FU;
	out.masked = (b1 & 0x80U) != 0;
	u64 plen = b1 & 0x7FU;
	bool const is_control = (out.opcode & 0x08U) != 0;

	if ((b0 & 0x70U) != 0) {
		return FrameParseStatus::ProtocolError;
	}
	if ((out.opcode >= 0x3U && out.opcode <= 0x7U) || out.opcode >= 0xBU) {
		return FrameParseStatus::ProtocolError;
	}
	if (is_control && (!out.fin || plen > 125)) {
		return FrameParseStatus::ControlTooLarge;
	}
	if (!out.masked) {
		return FrameParseStatus::ProtocolError;
	}

	SZ off = 2;
	if (plen == 126) {
		if (buf.size() < off + 2) {
			return FrameParseStatus::Incomplete;
		}
		plen = (static_cast<u64>(to_integer<u8>(buf[off])) << 8U) | static_cast<u64>(to_integer<u8>(buf[off + 1]));
		if (plen < 126) {
			return FrameParseStatus::ProtocolError;
		}
		off += 2;
	} else if (plen == 127) {
		if (buf.size() < off + 8) {
			return FrameParseStatus::Incomplete;
		}
		plen = 0;
		for (SZ i = 0; i < 8; ++i) {
			plen = (plen << 8U) | to_integer<u8>(buf[off + i]);
		}
		if (plen <= 0xFFFF) {
			return FrameParseStatus::ProtocolError;
		}
		off += 8;
	}
	if (buf.size() < off + 4) {
		return FrameParseStatus::Incomplete;
	}
	out.mask = {
		to_integer<u8>(buf[off]),
		to_integer<u8>(buf[off + 1]),
		to_integer<u8>(buf[off + 2]),
		to_integer<u8>(buf[off + 3])};
	off += 4;
	out.payload_len = plen;
	out.header_size = off;
	return FrameParseStatus::Ok;
}

} // namespace ws_detail
// WebSocket connection object passed to the WsHandler callback.
// Thread-safe for concurrent send; recv is single-consumer.
export class WsConn {
public:
	enum class Opcode : u8 {
		Text = 1,
		Binary = 2,
		Close = 8,
		Ping = 9,
		Pong = 10,
	};
	struct Frame {
		Opcode opcode{};
		S payload;
	};
	WsConn(WsConn const &) = delete;
	WsConn &operator =(WsConn const &) = delete;
	WsConn(WsConn &&) = delete;
	WsConn &operator =(WsConn &&) = delete;
	explicit WsConn(
		int fd,
		S initial_buf = {})
		: fd_(fd)
		, buf_(move(initial_buf)) {}
#if CONFLUX_HAS_TLS
	// TLS variant: ssl must already have the handshake complete and be wired to
	// a socket BIO (SSL_set_fd called by the server before handing off).
	// initial_buf carries any plaintext bytes already decrypted before handoff.
	explicit WsConn(
		int fd,
		SSL *ssl,
		S initial_buf)
		: fd_(fd)
		, ssl_(ssl)
		, buf_(move(initial_buf)) {}
#endif
	~WsConn() noexcept {
		stop_keepalive();
		if (!closed_.test_and_set()) {
			::shutdown(fd_, SHUT_WR);
		}
	}
	Opt<Frame> recv() {
		while (true) {
			if (!fill(2)) {
				return nullopt;
			}
			ws_detail::FrameHeader hdr{};
			// First parse pass on 2 bytes surfaces protocol errors (rsv, opcode,
			// unmasked, control-size) without waiting for mask bytes — the wire
			// may legitimately have no mask for a rejected frame.
			auto const pre = ws_detail::parse_frame_header(as_bytes(span{buf_.data(), 2}), hdr);
			auto emit_protocol_close = [&]() {
				auto const b0 = static_cast<u8>(buf_[0]);
				if ((b0 & 0x70U) != 0) {
					close(1002, "rsv bits set");
				} else if (u8 const op = b0 & 0x0FU; (op >= 0x3U && op <= 0x7U) || op >= 0xBU) {
					close(1002, "reserved opcode");
				} else {
					close(1002, "unmasked frame");
				}
			};
			if (pre == ws_detail::FrameParseStatus::ProtocolError) {
				emit_protocol_close();
				return nullopt;
			}
			if (pre == ws_detail::FrameParseStatus::ControlTooLarge) {
				close(1002, "invalid control frame");
				return nullopt;
			}
			// pre is Ok (no extended length) or Incomplete (need extended length + mask).
			auto const b1 = static_cast<u8>(buf_[1]);
			u64 const len7 = b1 & 0x7FU;
			SZ const header_needed = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0) + 4;
			if (!fill(header_needed)) {
				return nullopt;
			}
			auto const status = ws_detail::parse_frame_header(as_bytes(span{buf_.data(), header_needed}), hdr);
			if (status != ws_detail::FrameParseStatus::Ok) {
				if (status == ws_detail::FrameParseStatus::ProtocolError) {
					close(1002, "invalid frame header");
				} else if (status == ws_detail::FrameParseStatus::ControlTooLarge) {
					close(1002, "invalid control frame");
				}
				return nullopt;
			}
			consume(hdr.header_size);

			bool const fin = hdr.fin;
			u8 const opcode_raw = hdr.opcode;
			u64 const plen = hdr.payload_len;
			bool const is_control = (opcode_raw & 0x08U) != 0;
			A<u8, 4> const mask_key = hdr.mask;

			if (plen > kMaxMessageSize) {
				close(1009, "message too big");
				return nullopt;
			}
			if (!is_control && (frag_payload_.size() + plen) > kMaxMessageSize) {
				close(1009, "message too big");
				return nullopt;
			}
			if (!fill(static_cast<SZ>(plen))) {
				return nullopt;
			}
			S payload(buf_.data(), static_cast<SZ>(plen));
			consume(static_cast<SZ>(plen));
#if defined(CONFLUX_STDSIMD)
			conflux_ws_unmask_stdsimd(
				reinterpret_cast<unsigned char *>(payload.data()),
				payload.size(),
				mask_key.data());
#else
			for (SZ i = 0; i < payload.size(); ++i) {
				payload[i] = static_cast<char>(
					static_cast<unsigned char>(payload[i])
					^ mask_key[i & 3]); // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			}
#endif

			if (opcode_raw == 0x9U) {
				do_send_frame(10, as_bytes(span{payload}));
				continue;
			}
			if (opcode_raw == 0xAU) {
				continue;
			}
			if (opcode_raw == 0x8U) {
				if (plen == 1) {
					close(1002, "invalid close payload");
					return nullopt;
				}
				u16 echo_code = 1000;
				if (plen >= 2) {
					echo_code = static_cast<u16>(
						(static_cast<unsigned>(static_cast<u8>(payload[0])) << 8U)
						| static_cast<unsigned>(static_cast<u8>(payload[1])));
					if (!ws_detail::is_valid_close_code(echo_code)) {
						close(1002, "invalid close code");
						return nullopt;
					}
					if (payload.size() > 2 && !ws_detail::utf8_is_valid(SV{payload}.substr(2))) {
						close(1007, "invalid utf-8");
						return nullopt;
					}
				}
				close(echo_code, {});
				return nullopt;
			}

			if (opcode_raw == 0x0U) {
				if (!frag_opcode_) {
					close(1002, "unexpected continuation");
					return nullopt;
				}
				frag_payload_.append(payload);
				if (!fin) {
					continue;
				}
				auto const final_op = *frag_opcode_;
				S final_payload = move(frag_payload_);
				frag_opcode_.reset();
				frag_payload_.clear();
				if (final_op == Opcode::Text && !ws_detail::utf8_is_valid(final_payload)) {
					close(1007, "invalid utf-8");
					return nullopt;
				}
				return Frame{.opcode = final_op, .payload = move(final_payload)};
			}

			if (opcode_raw != 0x1U && opcode_raw != 0x2U) {
				close(1002, "reserved opcode");
				return nullopt;
			}
			if (frag_opcode_) {
				close(1002, "nested data frame");
				return nullopt;
			}
			auto const opcode = static_cast<Opcode>(opcode_raw);
			if (!fin) {
				frag_opcode_ = opcode;
				frag_payload_ = move(payload);
				continue;
			}
			if (opcode == Opcode::Text && !ws_detail::utf8_is_valid(payload)) {
				close(1007, "invalid utf-8");
				return nullopt;
			}
			return Frame{.opcode = opcode, .payload = move(payload)};
		}
	}
	[[nodiscard]] bool send_text(
		SV data) {
		SL const lk{send_mtx_};
		return do_send_frame(1, as_bytes(span{data}));
	}
	[[nodiscard]] bool send_binary(
		span<byte const> data) {
		SL const lk{send_mtx_};
		return do_send_frame(2, data);
	}
	[[nodiscard]] bool send_ping(
		SV data = {}) {
		if (data.size() > 125) {
			throw std::invalid_argument{"WsConn::send_ping: payload exceeds 125-byte control frame limit"};
		}
		SL const lk{send_mtx_};
		return do_send_frame(9, as_bytes(span{data}));
	}
	void close(
		u16 code = 1000,
		SV reason = {}) {
		if (!ws_detail::is_valid_close_code(code)) {
			throw std::invalid_argument{"WsConn::close: invalid close code"};
		}
		if (reason.size() > 123) {
			throw std::invalid_argument{"WsConn::close: reason exceeds 123-byte limit (control frame payload max 125)"};
		}
		if (!ws_detail::utf8_is_valid(reason)) {
			throw std::invalid_argument{"WsConn::close: reason must be valid UTF-8"};
		}
		if (closed_.test_and_set()) {
			return;
		}
		stop_keepalive();
		A<char, 2> code_bytes{static_cast<char>(code >> 8), static_cast<char>(code & 0xFF)};
		S payload{code_bytes.data(), 2};
		payload += reason;
		{
			SL const lk{send_mtx_};
			do_send_frame(8, as_bytes(span{payload}));
		}
#if CONFLUX_HAS_TLS
		if (ssl_) {
			// Do NOT call SSL_shutdown(): in blocking mode it waits for the peer's
			// close_notify, deadlocking against a client that sent a WS close frame
			// but hasn't yet issued a TLS close_notify.  WS close frames are
			// application-level; just free the SSL object and shut the socket.
			ssl_.reset();
		}
#endif
		::shutdown(fd_, SHUT_WR);
	}
	[[nodiscard]] bool is_open() const noexcept { return !closed_.test(); }
	[[nodiscard]] int fd() const noexcept { return fd_; }
	// Start a background keepalive thread that sends a Ping frame every
	// interval_ms milliseconds.  The thread exits when the connection closes.
	// Multiple calls are ignored after the first.
	void start_keepalive(
		unsigned interval_ms) {
		if (keepalive_thread_.joinable()) {
			return; // already started
		}
		keepalive_thread_ = jthread([this, interval_ms](std::stop_token const &st) {
			std::unique_lock lk{keepalive_mtx_};
			while (is_open()) {
				if (keepalive_cv_.wait_for(lk, st, chrono::milliseconds{interval_ms}, [this] { return !is_open(); })) {
					break;
				}
				lk.unlock();
				auto _ = send_ping();
				lk.lock();
			}
		});
	}

private:
	void stop_keepalive() noexcept {
		if (!keepalive_thread_.joinable()) {
			return;
		}
		keepalive_thread_.request_stop();
		keepalive_cv_.notify_all();
	}
	static constexpr u64 kMaxMessageSize = 16ULL * 1024 * 1024;

	int fd_;
#if CONFLUX_HAS_TLS
	UniqueSsl ssl_;
#endif
	atomic_flag closed_{};
	mutex send_mtx_;
	mutex keepalive_mtx_;
	std::condition_variable_any keepalive_cv_;
	jthread keepalive_thread_{};
	S buf_;
	Opt<Opcode> frag_opcode_{};
	S frag_payload_{};
	bool fill(
		SZ n) {
		while (buf_.size() < n) {
			A<char, 4096> tmp{};
#if CONFLUX_HAS_TLS
			if (ssl_) {
				auto rc = SSL_read(ssl_.get(), tmp.data(), static_cast<int>(tmp.size()));
				if (rc <= 0) {
					return false;
				}
				buf_.append(tmp.data(), static_cast<SZ>(rc));
				continue;
			}
#endif
			auto rc = ::recv(fd_, tmp.data(), tmp.size(), 0);
			if (rc <= 0) {
				return false;
			}
			buf_.append(tmp.data(), static_cast<SZ>(rc));
		}
		return true;
	}
	void consume(
		SZ n) {
		buf_.erase(0, n);
	}
	// Send a WebSocket frame over either TLS or plain socket.
	bool do_send_frame(
		u8 opcode,
		span<byte const> payload) {
#if CONFLUX_HAS_TLS
		if (ssl_) {
			return ws_detail::ws_tls_send_frame(ssl_.get(), opcode, payload);
		}
#endif
		return ws_detail::ws_send_frame(fd_, opcode, payload);
	}
};
// Token carried in HttpResponse.ws_upgrade to signal a 101 WebSocket upgrade.
export struct WsUpgrade {
	S accept_key;
	CloneableFunction<void(HttpRequestView const &, WsConn &)> handler;
};
