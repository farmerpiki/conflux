module;
#include <cerrno>
#include <cstring>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#if CONFLUX_HAS_TLS
	#include <openssl/ssl.h>
#endif
#include "cpu_features.hxx"
#include "simd_backend.hxx"

export module conflux.net.http.realtime;

#ifdef CONFLUX_BUILD_FUZZ
	#define CONFLUX_FUZZ_EXPORT export
#else
	#define CONFLUX_FUZZ_EXPORT
#endif

import std;
import conflux.types;
import conflux.net.http.types;
export import conflux.net.http.server_types;
import conflux.crypto;
import conflux.utils;
#if CONFLUX_HAS_TLS
import conflux.net.tls;
#endif

export namespace conflux::http {

enum class SseOverflowPolicy : std::uint8_t {
	DropNewest,
	DropOldest,
	Disconnect,
};

constexpr conflux::http::OverflowPolicy to_overflow_policy(
	SseOverflowPolicy policy) noexcept {
	switch (policy) {
	case SseOverflowPolicy::DropNewest: return conflux::http::OverflowPolicy::drop_newest;
	case SseOverflowPolicy::DropOldest: return conflux::http::OverflowPolicy::drop_oldest;
	case SseOverflowPolicy::Disconnect: return conflux::http::OverflowPolicy::close_connection;
	}
	return conflux::http::OverflowPolicy::drop_newest;
}

constexpr SseOverflowPolicy sse_overflow_policy(
	conflux::http::OverflowPolicy policy) noexcept {
	switch (policy) {
	case conflux::http::OverflowPolicy::drop_oldest     : return SseOverflowPolicy::DropOldest;
	case conflux::http::OverflowPolicy::close_connection: return SseOverflowPolicy::Disconnect;
	case conflux::http::OverflowPolicy::drop_newest     :
	case conflux::http::OverflowPolicy::reject          :
	case conflux::http::OverflowPolicy::backpressure    : return SseOverflowPolicy::DropNewest;
	}
	return SseOverflowPolicy::DropNewest;
}

} // namespace conflux::http

namespace {

void ignore_noexcept_destructor_failure() noexcept {}

} // namespace

export namespace conflux::http {

struct SsePressureMetrics {
	std::uint64_t dropped_newest{};
	std::uint64_t dropped_oldest{};
	std::uint64_t disconnected_for_pressure{};
};

class SseChannel {
private:
	int efd_{};
	std::mutex mtx_{};
	std::queue<std::string> pending_{};
	std::atomic_flag closed_{};
	std::size_t queued_bytes_{0};
	std::size_t max_queue_bytes_{};
	SseOverflowPolicy overflow_{SseOverflowPolicy::DropNewest};
	std::atomic<std::size_t> dropped_{0};
	std::atomic<std::uint64_t> dropped_newest_{0};
	std::atomic<std::uint64_t> dropped_oldest_{0};
	std::atomic<std::uint64_t> disconnected_for_pressure_{0};
	std::vector<std::function<void()>> close_callbacks_{};

public:
	static constexpr std::size_t kDefaultMaxQueueBytes = std::size_t{4} * 1024 * 1024;

	SseChannel(SseChannel const &) = delete;
	SseChannel &operator =(SseChannel const &) = delete;
	SseChannel(SseChannel &&) = delete;
	SseChannel &operator =(SseChannel &&) = delete;
	explicit SseChannel(
		std::size_t max_queue_bytes = kDefaultMaxQueueBytes,
		SseOverflowPolicy overflow = SseOverflowPolicy::DropNewest)
		: efd_(::eventfd(0, EFD_CLOEXEC))
		, max_queue_bytes_(max_queue_bytes)
		, overflow_(overflow) {
		if (efd_ < 0) {
			throw std::system_error{errno, std::system_category(), "eventfd"};
		}
	}
	~SseChannel() noexcept {
		try {
			close();
		} catch (...) { ignore_noexcept_destructor_failure(); }
		::close(efd_);
	}
	// Returns true if the frame was enqueued, false if dropped (overflow).
	// Also returns false if the channel is closed, regardless of policy.
	// Channel takes ownership of frame.
	[[nodiscard]] bool send(
		std::string frame) {
		bool enqueued = false;
		bool wake = false;
		{
			std::scoped_lock const lk{mtx_};
			if (closed_.test()) {
				return false;
			}
			std::size_t const frame_bytes = frame.size();
			std::size_t const would_be = queued_bytes_ + frame_bytes;
			if (would_be > max_queue_bytes_ && max_queue_bytes_ != 0) {
				switch (overflow_) {
				case SseOverflowPolicy::DropNewest:
					dropped_.fetch_add(1, std::memory_order_relaxed);
					dropped_newest_.fetch_add(1, std::memory_order_relaxed);
					return false;
				case SseOverflowPolicy::DropOldest:
					while (!pending_.empty() && queued_bytes_ + frame_bytes > max_queue_bytes_) {
						queued_bytes_ -= pending_.front().size();
						pending_.pop();
						dropped_.fetch_add(1, std::memory_order_relaxed);
						dropped_oldest_.fetch_add(1, std::memory_order_relaxed);
					}
					break;
				case SseOverflowPolicy::Disconnect:
					closed_.test_and_set();
					dropped_.fetch_add(1, std::memory_order_relaxed);
					disconnected_for_pressure_.fetch_add(1, std::memory_order_relaxed);
					wake = true;
					break;
				}
			}
			if (!closed_.test()) {
				queued_bytes_ += frame_bytes;
				pending_.push(std::move(frame));
				enqueued = true;
				wake = true;
			}
		}
		if (wake) {
			std::uint64_t v = 1;
			if (::write(efd_, &v, sizeof(v)) < 0 && errno != EAGAIN) {
				conflux::utils::eprintln(std::format("SseChannel::send: eventfd write: {}", strerror(errno)));
			}
		}
		return enqueued;
	}
	// Copy-safe view overload: callers may mutate or release the source buffer
	// immediately after this returns.
	[[nodiscard]] bool send_view(
		std::string_view frame) {
		return send(std::string{frame});
	}
	[[nodiscard]] bool send_event(
		std::string_view type,
		std::string_view data) {
		// Reject newlines in type and data: they would break SSE framing and
		// allow injection of arbitrary events.
		auto has_nl = [](std::string_view s) {
			return s.find('\n') != std::string_view::npos || s.find('\r') != std::string_view::npos;
		};
		if (has_nl(type) || has_nl(data)) {
			throw std::invalid_argument{"SseChannel::send_event: type and data must not contain newlines"};
		}
		return send(std::format("event: {}\ndata: {}\n\n", type, data));
	}
	void close() {
		if (closed_.test_and_set()) {
			return;
		} // already closed
		std::vector<std::function<void()>> callbacks;
		{
			std::scoped_lock const lk{mtx_};
			callbacks = std::move(close_callbacks_);
		}
		for (auto &callback: callbacks) {
			try {
				callback();
			} catch (...) {} // NOLINT(bugprone-empty-catch): event notification callbacks are best-effort.
		}
		std::uint64_t v = 1;
		if (::write(efd_, &v, sizeof(v)) < 0 && errno != EAGAIN) {
			conflux::utils::eprintln(std::format("SseChannel::close: eventfd write: {}", strerror(errno)));
		} // wake the io_uring poll
	}
	void on_close(
		std::function<void()> callback) {
		if (closed_.test()) {
			try {
				callback();
			} catch (...) {} // NOLINT(bugprone-empty-catch): close observer callbacks are best-effort.
			return;
		}
		std::scoped_lock const lk{mtx_};
		if (closed_.test()) {
			try {
				callback();
			} catch (...) {} // NOLINT(bugprone-empty-catch): close observer callbacks are best-effort.
			return;
		}
		close_callbacks_.push_back(std::move(callback));
	}
	[[nodiscard]] std::string drain() {
		std::scoped_lock const lk{mtx_};
		std::string result;
		while (!pending_.empty()) {
			result += pending_.front();
			pending_.pop();
		}
		queued_bytes_ = 0;
		return result;
	}
	[[nodiscard]] bool is_closed() const noexcept { return closed_.test(); }
	[[nodiscard]] int eventfd_fd() const noexcept { return efd_; }
	[[nodiscard]] std::size_t dropped_count() const noexcept { return dropped_.load(std::memory_order_relaxed); }
	[[nodiscard]] SsePressureMetrics pressure_metrics() const noexcept {
		return SsePressureMetrics{
			.dropped_newest = dropped_newest_.load(std::memory_order_relaxed),
			.dropped_oldest = dropped_oldest_.load(std::memory_order_relaxed),
			.disconnected_for_pressure = disconnected_for_pressure_.load(std::memory_order_relaxed),
		};
	}
	[[nodiscard]] SseOverflowPolicy overflow_policy() const noexcept { return overflow_; }
	[[nodiscard]] conflux::http::OverflowPolicy overflow_policy_vocabulary() const noexcept {
		return to_overflow_policy(overflow_);
	}
	[[nodiscard]] std::size_t max_queue_bytes() const noexcept { return max_queue_bytes_; }
};

// ---------------------------------------------------------------------------
// SseBroadcaster: fan-out pub/sub for SSE streams.
// ---------------------------------------------------------------------------
// Maintains a set of active SseChannel weak_ptrs.  broadcast() delivers an
// SSE event to every currently-connected subscriber.  Stale weak_ptrs are
// reaped on each broadcast call.
class SseBroadcaster {
public:
	SseBroadcaster() = default;
	~SseBroadcaster() = default;
	SseBroadcaster(SseBroadcaster const &) = delete;
	SseBroadcaster &operator =(SseBroadcaster const &) = delete;
	SseBroadcaster(SseBroadcaster &&) = delete;
	SseBroadcaster &operator =(SseBroadcaster &&) = delete;
	// Register a new subscriber.  Returns the SP to pass to Response::sse().
	std::shared_ptr<SseChannel> subscribe() {
		auto ch = std::make_shared<SseChannel>();
		std::scoped_lock const lk{mtx_};
		channels_.emplace_back(ch);
		return ch;
	}
	// Broadcast an SSE event to all active subscribers.
	void broadcast(
		std::string_view event,
		std::string_view data) {
		auto frame = std::format("event: {}\ndata: {}\n\n", event, data);
		broadcast_raw(frame);
	}
	// Broadcast a data-only SSE message to all active subscribers.
	void broadcast_data(
		std::string_view data) {
		auto frame = std::format("data: {}\n\n", data);
		broadcast_raw(frame);
	}
	// Number of currently-active subscribers (approximate; may include ones
	// that have just disconnected).
	[[nodiscard]] std::size_t subscriber_count() const {
		std::scoped_lock const lk{mtx_};
		return channels_.size();
	}

private:
	void broadcast_raw(
		std::string const &frame) {
		std::scoped_lock const lk{mtx_};
		// Erase stale weak_ptrs while delivering to live ones.
		std::erase_if(channels_, [&](std::weak_ptr<SseChannel> const &wch) {
			auto ch = wch.lock();
			if (!ch || ch->is_closed()) {
				return true;
			}
			auto _ = ch->send(frame);
			return false;
		});
	}
	mutable std::mutex mtx_;
	std::vector<std::weak_ptr<SseChannel>> channels_;
};

} // namespace conflux::http

// WebSocket support
// ---------------------------------------------------------------------------

export namespace conflux::http::detail {

// Compute Sec-WebSocket-Accept from Sec-WebSocket-Key.
std::string ws_accept_key(
	std::string_view client_key) {
	static constexpr std::string_view kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	std::string input{client_key};
	input += kMagic;
	auto digest = conflux::crypto::sha1(conflux::crypto::to_unsigned_span(input));
	return conflux::crypto::base64_encode(std::span{digest.data(), digest.size()});
}
bool is_valid_client_key(
	std::string_view key) {
	if (key.size() != 24) {
		return false;
	}
	auto decoded = conflux::crypto::base64_decode(key);
	return decoded.size() == 16 && conflux::crypto::base64_encode(conflux::crypto::to_unsigned_span(decoded)) == key;
}
bool is_valid_handshake(
	conflux::http::RequestView const &req) {
	return conflux::http::header_token_contains(req.headers["upgrade"], "websocket")
		&& conflux::http::header_token_contains(req.headers["connection"], "upgrade")
		&& conflux::utils::trim(req.headers["sec-websocket-version"]) == "13"
		&& is_valid_client_key(conflux::utils::trim(req.headers["sec-websocket-key"]));
}
// Build a complete WebSocket frame (server→client, unmasked) in one buffer so
// the transport call below emits header+payload as a single TCP segment / TLS record.
std::string ws_build_frame(
	std::uint8_t opcode,
	std::span<std::byte const> payload) {
	std::array<std::uint8_t, 10> hdr{};
	std::size_t hdr_len = 0;
	hdr[hdr_len++] = 0x80U | opcode; // FIN + opcode
	std::size_t const len = payload.size();
	if (len < 126) {
		hdr[hdr_len++] = static_cast<std::uint8_t>(len);
	} else if (len <= 0xFFFF) {
		hdr[hdr_len++] = 126;
		hdr[hdr_len++] = static_cast<std::uint8_t>(len >> 8);
		hdr[hdr_len++] = static_cast<std::uint8_t>(len & 0xFF);
	} else {
		hdr[hdr_len++] = 127;
		for (int s = 56; s >= 0; s -= 8) {
			hdr[hdr_len++] = static_cast<std::uint8_t>((len >> s) & 0xFF);
		}
	}
	std::string frame;
	frame.reserve(hdr_len + len);
	frame.append(reinterpret_cast<char const *>(hdr.data()), hdr_len);
	frame.append(reinterpret_cast<char const *>(payload.data()), len);
	return frame;
}
bool ws_send_frame(
	int fd,
	std::uint8_t opcode,
	std::span<std::byte const> payload) {
	auto frame = ws_build_frame(opcode, payload);
	std::size_t sent = 0;
	while (sent < frame.size()) {
		auto n = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		sent += static_cast<std::size_t>(n);
	}
	return true;
}
#if CONFLUX_HAS_TLS
bool ws_tls_send_frame(
	SSL *ssl,
	std::uint8_t opcode,
	std::span<std::byte const> payload) {
	auto frame = ws_build_frame(opcode, payload);
	std::size_t sent = 0;
	while (sent < frame.size()) {
		auto const chunk =
			std::min<std::size_t>(frame.size() - sent, static_cast<std::size_t>(std::numeric_limits<int>::max()));
		int const n = SSL_write(ssl, frame.data() + sent, static_cast<int>(chunk));
		if (n > 0) {
			sent += static_cast<std::size_t>(n);
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
	std::uint16_t code) {
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
	std::string_view s) {
	std::size_t i = 0;
	while (i < s.size()) {
		auto const b = static_cast<std::uint8_t>(s[i]);
		std::size_t extra{};
		std::uint32_t min_cp{};
		std::uint32_t cp{};
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
		for (std::size_t k = 1; k <= extra; ++k) {
			auto const c = static_cast<std::uint8_t>(s[i + k]);
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
	std::uint8_t opcode{};
	bool fin{};
	bool masked{};
	std::uint64_t payload_len{};
	std::array<std::uint8_t, 4> mask{};
	std::size_t header_size{};
};

CONFLUX_FUZZ_EXPORT enum class FrameParseStatus : std::uint8_t {
	Ok,
	Incomplete,
	ProtocolError,
	ControlTooLarge,
};
CONFLUX_FUZZ_EXPORT FrameParseStatus parse_frame_header(
	std::span<std::byte const> buf,
	FrameHeader &out) {
	if (buf.size() < 2) {
		return FrameParseStatus::Incomplete;
	}
	auto const b0 = to_integer<std::uint8_t>(buf[0]);
	auto const b1 = to_integer<std::uint8_t>(buf[1]);
	out.fin = (b0 & 0x80U) != 0;
	out.opcode = b0 & 0x0FU;
	out.masked = (b1 & 0x80U) != 0;
	std::uint64_t plen = b1 & 0x7FU;
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

	std::size_t off = 2;
	if (plen == 126) {
		if (buf.size() < off + 2) {
			return FrameParseStatus::Incomplete;
		}
		plen = (static_cast<std::uint64_t>(to_integer<std::uint8_t>(buf[off])) << 8U)
			 | static_cast<std::uint64_t>(to_integer<std::uint8_t>(buf[off + 1]));
		if (plen < 126) {
			return FrameParseStatus::ProtocolError;
		}
		off += 2;
	} else if (plen == 127) {
		if (buf.size() < off + 8) {
			return FrameParseStatus::Incomplete;
		}
		plen = 0;
		for (std::size_t i = 0; i < 8; ++i) {
			plen = (plen << 8U) | to_integer<std::uint8_t>(buf[off + i]);
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
		to_integer<std::uint8_t>(buf[off]),
		to_integer<std::uint8_t>(buf[off + 1]),
		to_integer<std::uint8_t>(buf[off + 2]),
		to_integer<std::uint8_t>(buf[off + 3])};
	off += 4;
	out.payload_len = plen;
	out.header_size = off;
	return FrameParseStatus::Ok;
}

} // namespace conflux::http::detail
// WebSocket connection object passed to the WsHandler callback.
// Thread-safe for concurrent send; recv is single-consumer.
export namespace conflux::http {

class WsConn {
public:
	enum class Opcode : std::uint8_t {
		Text = 1,
		Binary = 2,
		Close = 8,
		Ping = 9,
		Pong = 10,
	};
	struct Frame {
		Opcode opcode{};
		std::string payload;
	};
	WsConn(WsConn const &) = delete;
	WsConn &operator =(WsConn const &) = delete;
	WsConn(WsConn &&) = delete;
	WsConn &operator =(WsConn &&) = delete;
	explicit WsConn(
		int fd,
		std::string initial_buf = {},
		std::shared_ptr<std::atomic<std::uint64_t>> pressure_counter = {})
		: fd_(fd)
		, pressure_counter_(std::move(pressure_counter))
		, buf_(std::move(initial_buf)) {}
#if CONFLUX_HAS_TLS
	// TLS std::variant: ssl must already have the handshake complete and be wired to
	// a socket BIO (SSL_set_fd called by the server before handing off).
	// initial_buf carries any plaintext bytes already decrypted before handoff.
	explicit WsConn(
		int fd,
		SSL *ssl,
		std::string initial_buf,
		std::shared_ptr<std::atomic<std::uint64_t>> pressure_counter = {})
		: fd_(fd)
		, ssl_(ssl)
		, pressure_counter_(std::move(pressure_counter))
		, buf_(std::move(initial_buf)) {}
#endif
	~WsConn() noexcept {
		stop_keepalive();
		if (!closed_.test_and_set()) {
			notify_close_noexcept();
			::shutdown(fd_, SHUT_WR);
		}
	}
	std::optional<Frame> recv() {
		while (true) {
			detail::FrameHeader hdr{};
			if (!read_ws_frame_header(hdr)) {
				return std::nullopt;
			}
			auto payload = read_ws_payload(hdr);
			if (!payload) {
				return std::nullopt;
			}
			auto const opcode_raw = hdr.opcode;
			if ((opcode_raw & 0x08U) != 0) {
				if (handle_ws_control_frame(hdr, *payload)) {
					continue;
				}
				return std::nullopt;
			}
			if (opcode_raw == 0x0U) {
				auto frame = handle_ws_continuation_frame(hdr.fin, std::move(*payload));
				if (!frame && is_open()) {
					continue;
				}
				return frame;
			}
			auto frame = handle_ws_data_frame(hdr.fin, opcode_raw, std::move(*payload));
			if (!frame && is_open()) {
				continue;
			}
			return frame;
		}
	}
	[[nodiscard]] bool send_text(
		std::string_view data) {
		std::scoped_lock const lk{send_mtx_};
		return do_send_frame(1, std::as_bytes(std::span{data}));
	}
	[[nodiscard]] bool send_binary(
		std::span<std::byte const> data) {
		std::scoped_lock const lk{send_mtx_};
		return do_send_frame(2, data);
	}
	[[nodiscard]] bool send_ping(
		std::string_view data = {}) {
		if (data.size() > 125) {
			throw std::invalid_argument{"WsConn::send_ping: payload exceeds 125-std::byte control frame limit"};
		}
		std::scoped_lock const lk{send_mtx_};
		return do_send_frame(9, std::as_bytes(std::span{data}));
	}
	void close(
		std::uint16_t code = 1000,
		std::string_view reason = {}) {
		if (!detail::is_valid_close_code(code)) {
			throw std::invalid_argument{"WsConn::close: invalid close code"};
		}
		if (reason.size() > 123) {
			throw std::invalid_argument{
				"WsConn::close: reason exceeds 123-std::byte limit (control frame payload std::max 125)"};
		}
		if (!detail::utf8_is_valid(reason)) {
			throw std::invalid_argument{"WsConn::close: reason must be valid UTF-8"};
		}
		if (closed_.test_and_set()) {
			return;
		}
		notify_close_noexcept();
		stop_keepalive();
		std::array<char, 2> code_bytes{static_cast<char>(code >> 8), static_cast<char>(code & 0xFF)};
		std::string payload{code_bytes.data(), 2};
		payload += reason;
		{
			std::scoped_lock const lk{send_mtx_};
			do_send_frame(8, std::as_bytes(std::span{payload}));
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
	void on_close(
		std::function<void()> callback) {
		if (closed_.test()) {
			invoke_close_callback(callback);
			return;
		}
		std::scoped_lock const lk{close_mtx_};
		if (closed_.test()) {
			invoke_close_callback(callback);
			return;
		}
		close_callbacks_.push_back(std::move(callback));
	}
	// Start a background keepalive std::thread that sends a Ping frame every
	// interval_ms milliseconds.  The std::thread exits when the connection closes.
	// Multiple calls are ignored after the first.
	void start_keepalive(
		unsigned interval_ms) {
		if (keepalive_thread_.joinable()) {
			return; // already started
		}
		keepalive_thread_ = std::jthread([this, interval_ms](std::stop_token const &st) {
			std::unique_lock lk{keepalive_mtx_};
			while (is_open()) {
				if (keepalive_cv_.wait_for(lk, st, std::chrono::milliseconds{interval_ms}, [this] {
						return !is_open();
					})) {
					break;
				}
				lk.unlock();
				auto _ = send_ping();
				lk.lock();
			}
		});
	}

private:
	static void invoke_close_callback(
		std::function<void()> const &callback) noexcept {
		try {
			callback();
		} catch (...) {} // NOLINT(bugprone-empty-catch): close observer callbacks are best-effort.
	}
	void notify_close_noexcept() noexcept {
		std::vector<std::function<void()>> callbacks;
		{
			std::scoped_lock const lk{close_mtx_};
			callbacks = std::move(close_callbacks_);
		}
		for (auto &callback: callbacks) {
			invoke_close_callback(callback);
		}
	}
	void stop_keepalive() noexcept {
		if (!keepalive_thread_.joinable()) {
			return;
		}
		keepalive_thread_.request_stop();
		keepalive_cv_.notify_all();
	}
	void note_pressure_close_noexcept() noexcept {
		if (pressure_counter_ && !pressure_counted_.test_and_set()) {
			pressure_counter_->fetch_add(1, std::memory_order_relaxed);
		}
	}
	static constexpr std::uint64_t kMaxMessageSize = 16ULL * 1024 * 1024;

	int fd_;
#if CONFLUX_HAS_TLS
	conflux::net_tls::UniqueSsl ssl_;
#endif
	std::atomic_flag closed_{};
	std::atomic_flag pressure_counted_{};
	std::shared_ptr<std::atomic<std::uint64_t>> pressure_counter_;
	std::mutex send_mtx_;
	std::mutex close_mtx_;
	std::mutex keepalive_mtx_;
	std::condition_variable_any keepalive_cv_;
	std::jthread keepalive_thread_{};
	std::vector<std::function<void()>> close_callbacks_{};
	std::string buf_;
	std::size_t buf_pos_{};
	std::optional<Opcode> frag_opcode_{};
	std::string frag_payload_{};
	[[nodiscard]] std::size_t buffered_size() const noexcept { return buf_.size() - buf_pos_; }
	[[nodiscard]] char const *buffered_data() const noexcept { return buf_.data() + buf_pos_; }
	bool fill(
		std::size_t n) {
		while (buffered_size() < n) {
			if (buf_pos_ > 0 && (buf_pos_ >= 4096 || buf_pos_ * 2 >= buf_.size())) {
				buf_.erase(0, buf_pos_);
				buf_pos_ = 0;
			}
			std::array<char, 4096> tmp{};
#if CONFLUX_HAS_TLS
			if (ssl_) {
				auto rc = SSL_read(ssl_.get(), tmp.data(), static_cast<int>(tmp.size()));
				if (rc <= 0) {
					return false;
				}
				buf_.append(tmp.data(), static_cast<std::size_t>(rc));
				continue;
			}
#endif
			auto rc = ::recv(fd_, tmp.data(), tmp.size(), 0);
			if (rc <= 0) {
				return false;
			}
			buf_.append(tmp.data(), static_cast<std::size_t>(rc));
		}
		return true;
	}
	void consume(
		std::size_t n) {
		buf_pos_ += n;
		if (buf_pos_ >= buf_.size()) {
			buf_.clear();
			buf_pos_ = 0;
		}
	}
	void emit_ws_protocol_close() {
		auto const b0 = static_cast<std::uint8_t>(buf_[buf_pos_]);
		if ((b0 & 0x70U) != 0) {
			close(1002, "rsv bits set");
		} else if (std::uint8_t const op = b0 & 0x0FU; (op >= 0x3U && op <= 0x7U) || op >= 0xBU) {
			close(1002, "reserved opcode");
		} else {
			close(1002, "unmasked frame");
		}
	}
	[[nodiscard]] bool read_ws_frame_header(
		detail::FrameHeader &hdr) {
		if (!fill(2)) {
			return false;
		}
		auto const pre = detail::parse_frame_header(std::as_bytes(std::span{buffered_data(), 2}), hdr);
		if (pre == detail::FrameParseStatus::ProtocolError) {
			emit_ws_protocol_close();
			return false;
		}
		if (pre == detail::FrameParseStatus::ControlTooLarge) {
			close(1002, "invalid control frame");
			return false;
		}
		auto const b1 = static_cast<std::uint8_t>(buf_[buf_pos_ + 1]);
		std::uint64_t const len7 = b1 & 0x7FU;
		std::size_t const header_needed = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0) + 4;
		if (!fill(header_needed)) {
			return false;
		}
		auto const status = detail::parse_frame_header(std::as_bytes(std::span{buffered_data(), header_needed}), hdr);
		if (status != detail::FrameParseStatus::Ok) {
			if (status == detail::FrameParseStatus::ProtocolError) {
				close(1002, "invalid frame header");
			} else if (status == detail::FrameParseStatus::ControlTooLarge) {
				close(1002, "invalid control frame");
			}
			return false;
		}
		consume(hdr.header_size);
		return true;
	}
	[[nodiscard]] std::optional<std::string> read_ws_payload(
		detail::FrameHeader const &hdr) {
		auto const plen = hdr.payload_len;
		if (plen > kMaxMessageSize) {
			close(1009, "message too big");
			return std::nullopt;
		}
		if ((hdr.opcode & 0x08U) == 0 && (frag_payload_.size() + plen) > kMaxMessageSize) {
			close(1009, "message too big");
			return std::nullopt;
		}
		if (!fill(static_cast<std::size_t>(plen))) {
			return std::nullopt;
		}
		std::string payload(buffered_data(), static_cast<std::size_t>(plen));
		consume(static_cast<std::size_t>(plen));
		unmask_ws_payload(payload, hdr.mask);
		return payload;
	}
	static void unmask_ws_payload(
		std::string &payload,
		std::array<std::uint8_t, 4> const &mask_key) {
#if defined(CONFLUX_STDSIMD) && CONFLUX_SIMD_SELECTION_DIRECT
		constexpr std::size_t kStdsimdThreshold = 32;
		if (payload.size() >= kStdsimdThreshold) {
			conflux_ws_unmask_stdsimd(
				reinterpret_cast<unsigned char *>(payload.data()),
				payload.size(),
				mask_key.data());
		} else
#elif defined(CONFLUX_STDSIMD) && CONFLUX_SIMD_SELECTION_RUNTIME
		constexpr std::size_t kStdsimdThreshold = 32;
		if (payload.size() >= kStdsimdThreshold && conflux_cpu_supports_avx2()) {
			conflux_ws_unmask_stdsimd(
				reinterpret_cast<unsigned char *>(payload.data()),
				payload.size(),
				mask_key.data());
		} else
#endif
		{
			for (std::size_t i = 0; i < payload.size(); ++i) {
				payload[i] = static_cast<char>(
					static_cast<unsigned char>(payload[i])
					^ mask_key[i & 3]); // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			}
		}
	}
	[[nodiscard]] bool handle_ws_control_frame(
		detail::FrameHeader const &hdr,
		std::string const &payload) {
		if (hdr.opcode == 0x9U) {
			do_send_frame(10, std::as_bytes(std::span{payload}));
			return true;
		}
		if (hdr.opcode == 0xAU) {
			return true;
		}
		if (hdr.opcode != 0x8U) {
			close(1002, "reserved opcode");
			return false;
		}
		if (hdr.payload_len == 1) {
			close(1002, "invalid close payload");
			return false;
		}
		std::uint16_t echo_code = 1000;
		if (hdr.payload_len >= 2) {
			echo_code = static_cast<std::uint16_t>(
				(static_cast<unsigned>(static_cast<std::uint8_t>(payload[0])) << 8U)
				| static_cast<unsigned>(static_cast<std::uint8_t>(payload[1])));
			if (!detail::is_valid_close_code(echo_code)) {
				close(1002, "invalid close code");
				return false;
			}
			if (payload.size() > 2 && !detail::utf8_is_valid(std::string_view{payload}.substr(2))) {
				close(1007, "invalid utf-8");
				return false;
			}
		}
		close(echo_code, {});
		return false;
	}
	[[nodiscard]] std::optional<Frame> handle_ws_continuation_frame(
		bool fin,
		std::string payload) {
		if (!frag_opcode_) {
			close(1002, "std::unexpected continuation");
			return std::nullopt;
		}
		frag_payload_.append(payload);
		if (!fin) {
			return std::nullopt;
		}
		auto const final_op = *frag_opcode_;
		std::string final_payload = std::move(frag_payload_);
		frag_opcode_.reset();
		frag_payload_.clear();
		if (final_op == Opcode::Text && !detail::utf8_is_valid(final_payload)) {
			close(1007, "invalid utf-8");
			return std::nullopt;
		}
		return Frame{.opcode = final_op, .payload = std::move(final_payload)};
	}
	[[nodiscard]] std::optional<Frame> handle_ws_data_frame(
		bool fin,
		std::uint8_t opcode_raw,
		std::string payload) {
		if (opcode_raw != 0x1U && opcode_raw != 0x2U) {
			close(1002, "reserved opcode");
			return std::nullopt;
		}
		if (frag_opcode_) {
			close(1002, "nested data frame");
			return std::nullopt;
		}
		auto const opcode = static_cast<Opcode>(opcode_raw);
		if (!fin) {
			frag_opcode_ = opcode;
			frag_payload_ = std::move(payload);
			return std::nullopt;
		}
		if (opcode == Opcode::Text && !detail::utf8_is_valid(payload)) {
			close(1007, "invalid utf-8");
			return std::nullopt;
		}
		return Frame{.opcode = opcode, .payload = std::move(payload)};
	}
	// Send a WebSocket frame over either TLS or plain socket.
	bool do_send_frame(
		std::uint8_t opcode,
		std::span<std::byte const> payload) {
		bool ok = false;
#if CONFLUX_HAS_TLS
		if (ssl_) {
			ok = detail::ws_tls_send_frame(ssl_.get(), opcode, payload);
		} else
#endif
		{
			ok = detail::ws_send_frame(fd_, opcode, payload);
		}
		if (!ok) {
			note_pressure_close_noexcept();
		}
		return ok;
	}
};
// Token carried in Response.ws_upgrade to signal a 101 WebSocket upgrade.
struct WsUpgrade {
	std::string accept_key;
	conflux::http::CloneableFunction<void(conflux::http::RequestView const &, WsConn &)> handler;
};

} // namespace conflux::http
