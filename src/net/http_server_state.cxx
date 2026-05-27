module;
#include <cstddef> // must precede openssl/nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" blocks re-include c++config.h

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
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

module conflux.net.http_server:state;

import std;
import conflux.types;
import std.compat;
import conflux.net.http.server_types;
import conflux.net.http.parse_helpers;

import conflux.net.http.types;
import conflux.net.router;
import conflux.file_map;
import conflux.net.detail.direct_slot_pool;
import conflux.net.vhost;
import conflux.net.config;
import conflux.net.http_server_config;
import conflux.small_function;
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

enum class Op : std::uint8_t {
	Accept,
	Recv,
	Send,
	Close,
	SsePoll,
	DeferredPoll,
	Shutdown,
	FdShutdown,
	Timer,
	FileIo,
	WsCancel,
	FixedFdInstall,
	DirectSlotClose,
	ClientRing,
	Nop,
	SendZc,

};

struct SendZcCounters : SendZcMetrics {
	[[nodiscard]] SendZcMetrics snapshot() const noexcept { return static_cast<SendZcMetrics const &>(*this); }
};

enum class ServerFatalReason : std::uint8_t {
	none,
	cq_overflow,
	cq_overflow_no_nodrop,
	submit_wait_ebadr,
	internal_exception,

};

inline constexpr std::uint32_t OP_SHIFT = 56U;
inline constexpr std::uint32_t GEN_SHIFT = 24U;
inline constexpr std::uint64_t GEN_MASK = 0xFFFFFFFFULL;
inline constexpr std::uint64_t FD_MASK = 0x00FFFFFFULL;
constexpr std::uint64_t pack(
	Op op,
	std::uint32_t gen,
	int fd) noexcept {
	return (static_cast<std::uint64_t>(static_cast<std::uint8_t>(op)) << OP_SHIFT)
		 | ((static_cast<std::uint64_t>(gen) & GEN_MASK) << GEN_SHIFT)
		 | (static_cast<std::uint64_t>(static_cast<std::uint32_t>(fd)) & FD_MASK);
}
constexpr std::tuple<Op, std::uint32_t, int> unpack(
	std::uint64_t ud) noexcept {
	return {
		static_cast<Op>(ud >> OP_SHIFT),
		static_cast<std::uint32_t>((ud >> GEN_SHIFT) & GEN_MASK),
		static_cast<int>(ud & FD_MASK)};
}

struct PartialBuf {
	std::string buf{};
	std::size_t pos{0};
	[[nodiscard]] inline bool empty() const noexcept { return pos >= buf.size(); }
	[[nodiscard]] inline std::size_t size() const noexcept { return buf.size() - pos; }
	[[nodiscard]] inline char const *data() const noexcept { return buf.data() + pos; }
	[[nodiscard]] inline char front() const noexcept { return buf[pos]; }
	[[nodiscard]] inline std::string_view view() const noexcept { return {buf.data() + pos, buf.size() - pos}; }
	inline void append(
		char const *p,
		std::size_t n) {
		buf.append(p, n);
	}
	inline void consume(
		std::size_t n) noexcept {
		pos += n;
		if (pos >= buf.size()) {
			clear();
		}
	}
	inline void clear() noexcept {
		buf.clear();
		pos = 0;
	}
	[[nodiscard]] inline std::string take() {
		if (pos > 0) {
			buf.erase(0, pos);
		}
		pos = 0;
		return std::move(buf);
	}
	[[nodiscard]] inline std::shared_ptr<std::string> cut_prefix(
		std::size_t n,
		std::shared_ptr<std::string> out,
		std::string &tail_scratch) {
		tail_scratch.clear();
		auto const available = size();
		n = std::min(n, available);
		if (pos == 0) {
			*out = std::move(buf);
			if (out->size() > n) {
				tail_scratch.assign(out->data() + n, out->size() - n);
				out->resize(n);
			}
			buf = std::move(tail_scratch);
			pos = 0;
			return out;
		}
		out->assign(data(), n);
		if (available > n) {
			tail_scratch.assign(data() + n, available - n);
		}
		buf = std::move(tail_scratch);
		pos = 0;
		return out;
	}
};

struct RecvComp {
	int fd;
	int res;
	std::uint32_t gen;
	conflux::uring::CqeFlags flags;
};
struct RequestBufferPool {
	std::mutex mutex{};
	std::vector<std::string> buffers{};
};
inline constexpr std::size_t FD_TABLE_RESERVE = 4096;
inline constexpr unsigned DEFAULT_RING_ENTRIES = 1024U;
#if CONFLUX_HAS_HTTP2
struct Ring; // forward-declared so H2ConnCtx can hold Ring* while Conn precedes Ring
struct H2Stream {
	std::string method{};
	std::string path{};
	std::string scheme{"https"};
	std::string authority{};
	HttpFields headers{};
	std::string body{};
	std::size_t expected_body_size{};
	bool body_reserved{};
	bool end_stream_seen{};
	bool rejected{};
	bool regular_header_seen{};
	bool seen_method{};
	bool seen_path{};
	bool seen_scheme{};
	bool seen_authority{};
	bool seen_content_length{};
	std::size_t header_count{};
	std::size_t header_list_size{};
	// Response state for the data provider callback:
	std::string response_body{};
	std::size_t response_off{};
	HttpFields response_trailers{};
	int deferred_efd{-1};
	// SSE streaming state (non-null → H2 SSE stream):
	std::shared_ptr<conflux::http::SseChannel> sse_channel{};
	std::string h2_sse_buf{}; // overflow: drained SSE data not yet framed
};
struct H2ConnCtx {
	Ring *ring;
	int fd;
};
#endif // CONFLUX_HAS_HTTP2

struct Ring;
struct Conn;
void dispatch_request(
	Conn &conn,
	std::string_view raw,
	Ring &ring,
	std::size_t max_body_size,
	bool http_redirect_to_https,
	std::vector<std::string> const &https_redirect_hosts,
	conflux::http::ParserLimits const &limits,
	std::shared_ptr<std::string> request_storage = {});
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): field order mirrors connection state-machine phases.
struct alignas(
	64) Conn {
	int fd = -1;
	std::uint32_t gen = 0;
	bool recv_armed = false;
	std::uint16_t incremental_buf_id{};
	bool have_incremental_buf_id{};
	conflux::uring::CqeFlags last_recv_cqe_flags{};
	bool have_last_recv_cqe_flags{};
	bool send_queued = false;
	bool is_sse = false;
	bool sse_headers_sent = false;
	bool is_ws = false; // true → WebSocket upgrade; hand off fd to WS std::thread after send
	bool is_deferred = false;
	bool deferred_head_only = false; // HEAD on deferred route → strip body when ready
	bool closing = false; // close SQE already submitted for this generation
	bool close_after_send = false; // true → close instead of re-arming recv
	std::chrono::steady_clock::time_point close_after_send_deadline{}; // force-close grace deadline during shutdown
	int sse_efd = -1;
	std::shared_ptr<conflux::http::SseChannel> sse_channel{};
	int deferred_efd = -1;
	std::shared_ptr<DeferredResponse> deferred_response{};
	std::shared_ptr<std::string> deferred_request_storage{};
	std::shared_ptr<std::vector<UploadedFile>> deferred_request_files{};
	std::shared_ptr<conflux::http::WsUpgrade> ws_upgrade{}; // set when 101 pending; cleared after handoff
	std::shared_ptr<WorkPool> ws_work_pool{};
	Request saved_req{}; // copy of request saved for WS handler std::thread
	bool is_tls = false; // set after first-std::byte sniff; used by dispatch_request
#if CONFLUX_HAS_TLS
	// TLS state (null → plaintext connection)
	UniqueSsl ssl;
	std::string tls_rx_cipher{}; // encrypted bytes received from the socket, before SSL_read()
	std::string tls_send_pending{}; // encrypted bytes generated while no send can be submitted
	std::string tls_send_inflight{}; // stable borrowed storage for in-flight io_uring SEND
	std::size_t tls_send_off{}; // bytes of tls_send_inflight already sent
	bool tls_hs_done = false; // TLS handshake completed; also used as undecided sentinel
	bool tls_sending_response = false; // true → current tls_send_buf carries HTTP response data
	bool tls_shutdown_after_send = false; // true → send TLS close_notify after pending bytes drain
	bool tls_wait_peer_shutdown = false; // true → drain peer close_notify/FIN after our close_notify is sent
	bool ktls_send = false; // kTLS send offload active; splice_to_fd usable for TLS file body
#endif
	bool has_response = false;
	std::string own_response{};
	PartialBuf partial{};
	std::size_t written = 0;
	FixedBuffer send_buf{};
	std::size_t send_buf_base_written{};
	std::size_t send_buf_len{};
	std::size_t request_bytes = 0; // bytes consumed by current dispatched request
	std::chrono::steady_clock::time_point last_activity; // updated on accept and recv
	std::chrono::steady_clock::time_point request_started{};
	bool request_in_progress = false;
	bool expect_continue_sent = false;
	std::string remote_addr{}; // peer IP, set on accept
	ChunkedDecodeState chunked_decode{};
	// mmap path: non-null when current response has a zero-copy file region
	std::shared_ptr<MappedBody> mapped_file{};
	std::size_t mapped_total{}; // own_response.size() + mapped_file->size
	std::uint64_t mapped_delivered{};
	std::array<iovec, 2> writev_iov{}; // iovecs rebuilt per-send in queue_send_mapped

	// file_io streaming path: non-null when current response streams via splice
	// (plain HTTP) or read_fixed+SSL_write (TLS). Phase tracks whether headers
	// have been flushed to the socket.
	std::shared_ptr<conflux::http::StreamedFile> streamed_file{};
	bool streamed_headers_sent = false;
	std::uint64_t streamed_delivered = 0;
	bool streamed_splice_in_flight = false;
	SendZcCqeState zc_state{};
	bool zc_tls_bypass_counted = false;
#if CONFLUX_HAS_HTTP2
	bool is_h2{};
	nghttp2_session *h2_session = nullptr;
	std::unique_ptr<H2ConnCtx> h2_ctx;
	std::map<std::int32_t, H2Stream> h2_streams{};
	std::set<std::int32_t> h2_closed_streams{};
	std::map<std::int32_t, std::uint32_t> h2_stream_window_updates{};
	std::int32_t h2_max_client_stream_id{};
	bool h2_client_preface_seen{};
	std::string h2_pending_send{};
	std::int32_t h2_sse_stream_id{-1}; // stream_id of active H2 SSE stream (-1 = none)
	bool h2_sse_pending_wait{}; // set by on_frame_recv_cb to trigger queue_sse_wait after h2_do_send
#endif
};

#if CONFLUX_HTTP_TRACE
[[nodiscard]] inline char const *buffer_ring_mode_name(
	BufferRingMode mode) noexcept {
	switch (mode) {
	case BufferRingMode::classic_one_cqe_per_buffer: return "classic";
	case BufferRingMode::recv_bundle               : return "recv_bundle";
	case BufferRingMode::incremental               : return "incremental";
	}
	return "unknown";
}
	#define HTTP_TRACE(MSG) eprintln(std::format("http_trace {}", (MSG)))
#else
	#define HTTP_TRACE(MSG) ((void)0)
#endif

struct Ring;

conflux::work::root::Task<void>
do_streamed_splice(Ring *ring, int fd, std::uint32_t conn_gen, conflux::work::root::Task<std::size_t> splice_task);

conflux::work::root::Task<void> do_streamed_tls_chunk(
	Ring *ring,
	int fd,
	std::uint32_t conn_gen,
	std::size_t want,
	conflux::work::root::Task<FileReader::ReadFixedResult> read_task);
struct Ring {
	struct DrainControl {
		DrainOptions options{};
		std::chrono::steady_clock::time_point deadline{};
		std::atomic_bool active{false};
		std::atomic_bool deadline_hit{false};
		std::atomic<std::uint64_t> accepted_before_stop{0};
		std::atomic<std::uint64_t> idle_closed{0};
		std::atomic<std::uint64_t> requests_finished{0};
		std::atomic<std::uint64_t> streams_closed{0};
		std::atomic<std::uint64_t> forced_closed{0};
	};
	struct DeferredWait {
		int conn_fd{-1};
		std::int32_t stream_id{-1};
		std::shared_ptr<DeferredResponse> response{};
	};
	struct WsHandoffState {
		std::shared_ptr<conflux::http::WsUpgrade> upgrade{};
		std::shared_ptr<WorkPool> pool{};
		Request request{};
	};
	struct WsInstallEntry {
		WsHandoffState state{};
		std::string initial_buf{};
#if CONFLUX_HAS_TLS
		UniqueSsl ssl{};
#endif
	};
	struct ActiveWsRegistry {
		std::mutex mu{};
		std::unordered_set<int> fds{};
	};
	struct RetiredIncrementalBuf {
		std::uint16_t id{};
		bool present{};
	};
	static std::uint64_t pack_fd_gen(int fd, std::uint32_t gen) noexcept;
	static constexpr std::size_t BUF_SIZE = 8192;
	static constexpr std::uint32_t MAX_FILES = 65536;

	io_uring ring{};
	SocketRawRing raw_{conflux::uring::RingRef{ring}};
	mutable CompletionTable client_ct_{64};
	mutable std::optional<SocketTaskRing> client_task_ring_{};
	// Backing memory for the ring when no_mmap = true. Freed on destroy.
	std::unique_ptr<std::byte[], void (*)(void *)> ring_mem{nullptr, ::free};
	int listen_fd = -1;
	sockaddr_in6 client_addr{};
	socklen_t client_addr_len = sizeof(client_addr);

	std::shared_ptr<RequestBufferPool> request_buffer_pool{std::make_shared<RequestBufferPool>()};
	std::string request_tail_scratch{};
	std::vector<Conn> fd_table{};
	std::vector<RecvComp> recvs{};
	std::unordered_map<int, DeferredWait> deferred_waits{};
	std::unordered_map<std::uint64_t, std::unique_ptr<std::uint64_t>> in_flight_read_bufs{};
	std::unordered_map<int, WsInstallEntry> ws_cancel_handoffs{};
	std::unordered_map<int, WsInstallEntry> ws_installs{};
	std::shared_ptr<ActiveWsRegistry> active_ws_registry{std::make_shared<ActiveWsRegistry>()};
	std::unordered_map<std::uint64_t, RetiredIncrementalBuf> retired_incremental_recv{};
	std::vector<int> ws_cancel_ready{};

	std::unique_ptr<BufferRing> buf_ring_;
	std::unique_ptr<DirectFdTable> direct_fds_;
	std::unique_ptr<DirectSlotPool> direct_slots_;
	int tcp_opt_one_ = 1; // stable optval for cmd_sock SETSOCKOPT

	conflux::uring::IoUringCaps caps{};
	std::uint32_t requested_setup_flags_ = 0; // flags requested before adaptive EINVAL fallback
	std::uint32_t active_setup_flags_ = 0; // flags used by the successful io_uring setup call
	std::uint32_t stripped_setup_flags_ = 0; // requested flags removed during adaptive EINVAL fallback
	bool listen_fixed = false;
	bool accepted_sockets_direct = false;
	bool direct_accept_enabled_ = true;
	bool cmd_sock_setsockopt_enabled_ = true;
	bool startup_banner = false;
	bool shutting_down = false;
	bool ring_fatal_{false};
	ServerFatalReason fatal_reason_{ServerFatalReason::none};
	std::uint32_t fatal_cq_overflow_count_{0};
	bool overflow_flush_limit_hit_{false};
	bool saw_overflow_since_last_resize_{false};
	bool cq_resize_unsupported_{false};
	bool use_recv_bundle = false; // IORING_RECVSEND_BUNDLE on multishot recv
	bool use_recv_incremental_buf = false; // IOU_PBUF_RING_INC on buffer ring
	bool auto_recv_arm_policy = false; // adaptive poll_first via IORING_CQE_F_SOCK_NONEMPTY
	int busy_poll_us_ = 0; // SO_BUSY_POLL optval; 0=disabled
	bool prefer_busy_poll_ = false; // SO_PREFER_BUSY_POLL
	int ring_core_ = -1; // sched_setaffinity core for this ring std::thread; -1=disabled
	int worker_core_ = -1; // IORING_REGISTER_IOWQ_AFF core for io-wq; -1=disabled
	bool send_zc_enabled_ = false;
	std::size_t send_zc_threshold_ = 16384;
	bool send_zc_report_usage_ = true;
	SendZcCounters zc_counters_{};
	HttpRejectionMetrics rejection_counters_{};
	HttpServerMetrics::StaticFileMetrics static_file_counters_{};
	HttpPressureMetrics pressure_counters_{};
	std::shared_ptr<std::atomic<std::uint64_t>> ws_pressure_counter_{std::make_shared<std::atomic<std::uint64_t>>(0)};
	HttpServerObservabilityHooks observability_hooks_{};
	std::uint64_t accepted_direct_failures_{};
	std::uint64_t recv_bundle_cqes_{};
	std::uint64_t recv_bundle_slices_{};
	std::uint64_t recv_bundle_bytes_{};
	std::size_t max_body_size = std::size_t{1024} * 1024; // set from Config before run_loop()
	std::uint32_t request_timeout_ms = 30000; // set from Config before run_loop(); 0 = disabled
	std::uint32_t tls_sniff_timeout_ms = 10000; // set from Config before run_loop(); 0 = disabled
	bool slow_handler_diagnostics = false; // set from Config before run_loop()
	std::uint32_t slow_handler_warn_ms = 25; // set from Config before run_loop()
	bool http_redirect_to_https = false; // set from Config before run_loop()
	std::vector<std::string> https_redirect_hosts{}; // set from Config before run_loop()
	conflux::http::ParserLimits parser_limits{}; // set from Config before run_loop()
	std::string alt_svc_header{}; // "h3=\":443\"; ma=86400" when h3 enabled; empty otherwise

	int shutdown_efd = -1; // set by HttpServer before run_loop(); not owned
	DrainControl *drain_control = nullptr; // set by HttpServer before run_loop(); not owned
	std::uint64_t shutdown_buf = 0; // read target for the shutdown eventfd SQE

	// file_io pools — constructed after io_uring_queue_init. Shared by
	// static-file serving and any other caller that grabs files.get().
	std::unique_ptr<CompletionTable> file_completions{};
	std::unique_ptr<RegisteredBufferTable> buf_table{};
	std::unique_ptr<FixedBufferPool> fixed_buffers{};
	std::unique_ptr<FixedBufferPool> send_buffers{};
	std::unique_ptr<PipePool> splice_pipes{};
	std::unique_ptr<FileReader> files{};
	bool send_fixed_buffers_supported{false};

	// pool sizing — set from Config before run_loop()
	std::size_t file_io_slabs = 64;
	std::size_t file_io_slab_bytes = std::size_t{64} * 1024;
	std::size_t file_io_pipe_pairs = 16;
	std::size_t send_buffer_slabs = 64;
	std::size_t send_buffer_bytes = std::size_t{4} * 1024;
	bool send_fixed_buffers_enabled = false;

	__kernel_timespec timer_ts{}; // reused for the periodic timeout SQE
	static constexpr std::chrono::milliseconds shutdown_close_after_send_timeout{5000};

	// Software fallback queue for ops that could not obtain an SQE even after
	// a flush. Drained at the top of each run_loop iteration (after CQE reap
	// frees SQ slots). Each thunk re-invokes the original queue_* path so
	// conn state (gen, buffers) is re-read at replay time.
	std::deque<conflux::detail::small_move_only_function<void()>> pending_ops{};

	Router const *router = nullptr; // set before init(); not owned
	VHostRouter const *vhost_router = nullptr; // set before init(); not owned

	std::uint16_t bound_port{}; // actual port after bind (handles port=0)
	std::atomic<std::uint16_t> *port_signal = nullptr; // set for ring 0; signalled after listen()
#if CONFLUX_HAS_TLS
	SSL_CTX *ssl_ctx = nullptr; // non-owning; set by HttpServer before run(); null → plaintext
	// vhost_ctxs: hostname → SSL_CTX* for SNI switching (non-owning, owned by HttpServer::Impl)
	std::vector<std::pair<std::string, SSL_CTX *>> vhost_ctxs;
#endif

	Ring() = default;
	~Ring();
	Ring(Ring const &) = delete;
	Ring &operator =(Ring const &) = delete;
	Ring(Ring &&) = delete;
	Ring &operator =(Ring &&) = delete;
	[[nodiscard]] Response dispatch(RequestView const &req) const;
	[[nodiscard]] bool has_context_routes() const noexcept;
	[[nodiscard]] std::optional<Response> try_dispatch_context(RequestView const &req) const;
	[[nodiscard]] std::shared_ptr<std::string> acquire_request_buffer();
	[[nodiscard]] std::shared_ptr<WorkPool> resolve_ws_work_pool(RequestView const &req) const;
	void clear_deferred_wait(int deferred_efd);
	void queue_deferred_wait(
		int conn_fd,
		int deferred_efd,
		std::shared_ptr<DeferredResponse> response,
		std::int32_t stream_id = -1);
#if CONFLUX_HAS_TLS
	static void tls_flush_wbio(Conn &conn);
	static bool tls_feed_rbio(Conn &conn);
	// Submit an io_uring send for pending/in-flight TLS bytes.
	// Caller must NOT pre-set send_queued; this function owns the transition.
	void tls_queue_send(Conn &conn);
	void begin_tls_peer_shutdown_wait(int fd, Conn &conn);
	void queue_tls_shutdown(int fd, Conn &conn);
#endif // CONFLUX_HAS_TLS

#if CONFLUX_HAS_HTTP2
	// ---------------------------------------------------------------------------
	// nghttp2 static callbacks (passed as C function pointers; no capture)
	// ---------------------------------------------------------------------------
	static void
	h2_reject_stream(nghttp2_session *session, H2Stream &stream, std::int32_t stream_id, std::uint32_t error_code);
	[[nodiscard]] bool h2_prevalidate_client_frames(Conn &conn, std::string_view bytes);
	[[nodiscard]] static bool h2_valid_regular_header_name(std::string_view name) noexcept;
	[[nodiscard]] static bool h2_forbidden_connection_header(std::string_view name) noexcept;
	// nghttp2 wants to write bytes to the wire; accumulate into h2_pending_send.
	static ssize_t h2_send_cb(
		nghttp2_session * /*unused*/,
		std::uint8_t const *data,
		std::size_t length,
		int /*unused*/,
		void *user_data);
	// A new request stream is beginning; allocate its H2Stream entry.
	static int h2_on_begin_headers_cb(nghttp2_session * /*unused*/, nghttp2_frame const *frame, void *user_data);
	// Populate H2Stream fields from pseudo-headers and regular headers.
	static int h2_on_header_cb(
		nghttp2_session *session,
		nghttp2_frame const *frame,
		std::uint8_t const *name,
		std::size_t namelen,
		std::uint8_t const *header_value,
		std::size_t valuelen,
		std::uint8_t /*unused*/,
		void *user_data);
	// Accumulate DATA frame body bytes into stream.body.
	static int h2_on_data_chunk_cb(
		nghttp2_session *session,
		std::uint8_t /*unused*/,
		std::int32_t stream_id,
		std::uint8_t const *data,
		std::size_t len,
		void *user_data);
	// Data provider: feed response body bytes to nghttp2's framing layer.
	// Handles both static responses and SSE streaming.
	static ssize_t h2_read_cb(
		nghttp2_session *session,
		std::int32_t stream_id,
		std::uint8_t *buf,
		std::size_t length,
		std::uint32_t *data_flags,
		nghttp2_data_source *source,
		void * /*user_data*/);
	static void h2_submit_response(Conn &conn, std::int32_t stream_id, Response resp);
	// A frame is fully received.  On END_STREAM, dispatch to the router and
	// submit the HTTP/2 response via nghttp2_submit_response.
	static int h2_on_frame_recv_cb(nghttp2_session *session, nghttp2_frame const *frame, void *user_data);
	static int h2_on_invalid_frame_recv_cb(
		nghttp2_session *session,
		nghttp2_frame const *frame,
		int lib_error_code,
		void *user_data);
	// Stream fully closed — release its state.
	static int
	h2_on_stream_close_cb(nghttp2_session * /*unused*/, std::int32_t stream_id, std::uint32_t /*EC*/, void *user_data);
	// ---------------------------------------------------------------------------
	// H2 Ring methods
	// ---------------------------------------------------------------------------

	// SSL_write h2_pending_send into tls_send_buf, then queue a TLS send.
	// No-op if nothing pending or a send is already in flight (data accumulates
	// and will be flushed when handle_send_tls_complete's H2 branch runs next).
	void h2_flush_pending(Conn &conn);
	// Drive nghttp2 output (all queued frames) and flush to io_uring.
	void h2_do_send(Conn &conn);
	// Create nghttp2 server session and submit the server connection preface
	// (SETTINGS frame).  Does NOT flush — caller must call h2_do_send() after
	// running nghttp2_session_mem_recv() so that the SETTINGS and SETTINGS_ACK
	// are coalesced into a single TLS record.
	void h2_setup_conn(Conn &conn);
#endif // CONFLUX_HAS_HTTP2
	// Must be called from the std::thread that will run run_loop() (SINGLE_ISSUER).
	// `wq_fd`: when non-zero, sets IORING_SETUP_ATTACH_WQ so this ring shares
	// the parent ring's kernel io-wq. Pass ring[0].ring.ring_fd for rings 1..N.
	void init(
		std::uint16_t port,
		unsigned entries,
		std::uint32_t uring_flags,
		std::uint32_t wq_fd = 0,
		bool no_mmap = false);
	Conn &conn_for(int fd);
	void conn_erase(int fd, std::uint32_t gen);
	// Acquire a raw SQE without implicit submission. Returns null when the ring
	// is exhausted or fatal; callers handle that via defer_op() to avoid stalls.
	io_uring_sqe *get_sqe();
	// Defer an op whose SQE allocation failed. Replayed from run_loop once
	// the CQE reap frees ring capacity.
	void defer_op(conflux::detail::small_move_only_function<void()> op);
	void cancel_multishot_recv_or_defer(OsFd handle);
	void cancel_multishot_recv_or_defer(DirectFd handle);
	void drain_pending_ops();
	void defer_queue_send_if_current(int fd, std::uint32_t gen);
	void defer_handle_send_complete_if_current(int fd, std::uint32_t gen);
	void defer_start_streamed_body_if_current(int fd, std::uint32_t gen);
	void queue_multishot_accept();
	[[nodiscard]] RecvArmPolicy resolve_recv_arm_policy(Conn const &conn) const noexcept;
	void queue_multishot_recv(int fd);
	void queue_direct_accept_setup(int fd);
	// Submit WRITEV for a mapped-file response.
	// Adjusts iovecs to skip bytes already sent (conn.written).
	// When the body is large enough for SEND_ZC, keep the header send separate so
	// the body can use zero-copy after the header CQE drains.
	void queue_send_mapped(int fd);
	// file_io streaming path. Phase 1: send headers via prep_send. Phase 2:
	// once headers are fully delivered, kick off splice (plain) or read_fixed+
	// SSL_write (TLS). queue_send_streamed only handles phase 1; phase 2 is
	// triggered from handle_send once the header bytes are acked.
	void queue_send_streamed(int fd);
	// Acquire a pipe P and submit the splice chain via FileReader. Completion
	// calls back into handle_streamed_splice_done on the ring std::thread.
	void start_streamed_body(int fd);
#if CONFLUX_HAS_TLS
	// TLS streamed body: acquire a FixedBuffer, read_fixed a chunk of the file,
	// SSL_write it into wbio, flush and re-queue the TLS send. Pipelining depth
	// is effectively 1 per connection — suitable for unbuffered streaming.
	void start_streamed_tls_chunk(int fd);
	void on_streamed_tls_chunk_done(
		int fd,
		std::uint32_t conn_gen,
		FixedBuffer buf,
		std::size_t bytes,
		std::exception_ptr const &err);
	void write_mapped_tls_chunk(int fd, Conn &conn);
#endif
	void on_streamed_splice_done(int fd, std::uint32_t conn_gen, std::size_t delivered, std::exception_ptr const &err);
#if CONFLUX_HAS_TLS
	[[nodiscard]] bool tls_write_plaintext(int fd, Conn &conn, std::string_view bytes);
#endif
	void note_send_zc_tls_bypass_if_candidate(Conn &conn) noexcept;
	void queue_send(int fd);
	[[nodiscard]] static bool response_send_ready(Conn const &conn) noexcept;
	void start_response_send(int fd, Conn &conn);

	void invalidate_recv_if_armed(int fd);
	void close_listen_socket() noexcept;
	void cancel_accept_or_defer(int fd);
	void cancel_accept_or_defer();
	void submit_conn_close_or_defer(int fd, std::uint32_t gen);
	void submit_fd_shutdown_or_defer(int fd, std::uint32_t gen);
	void handle_fd_shutdown(int fd, int res, std::uint32_t gen);
	void queue_close(int fd);
	void queue_sse_wait(int fd);
	void queue_deferred_wait(int fd);
	void arm_shutdown_read();
	// Arm a one-shot periodic timer that fires every ~1 second for connection reaping.
	void arm_timer();
	void handle_timer();
	void handle_shutdown();
	void handle_accept(int res, conflux::uring::CqeFlags flg);
	void discard_recv_bufs(int res, conflux::uring::CqeFlags flags) noexcept;
	void discard_recv_bufs(RecvComp &rc) noexcept;
	void retire_incremental_partial(int fd, std::uint32_t gen, Conn &conn) noexcept;
	void reclaim_retired_incremental_recv(int fd, std::uint32_t gen) noexcept;
	void clear_retired_incremental_if_final(int fd, std::uint32_t gen, conflux::uring::CqeFlags flags) noexcept;
	void handle_recv_cqe(int fd, int res, conflux::uring::CqeFlags flg, std::uint32_t gen);
	bool handle_sse_send_complete(int fd, Conn &conn);
	void handle_http_response_send_complete(int fd, Conn &conn);
	[[nodiscard]] static bool make_blocking_fd(int fd);
	[[nodiscard]] WsHandoffState begin_ws_handoff(Conn &conn);
	void register_active_ws(int fd);
	void unregister_active_ws(int fd);
	std::uint64_t shutdown_active_ws_for_pressure();
	void launch_plain_ws_handler(WorkPool &pool, WsHandoffState state, int fd, std::string initial_buf);
	void finish_plain_ws_handoff(int fd, WsInstallEntry entry);
	void handoff_plain_ws(Conn &conn, int fd);
#if CONFLUX_HAS_TLS
	void launch_tls_ws_handler(WorkPool &pool, WsHandoffState state, int fd, SSL *ssl, std::string initial_buf);
	void handoff_tls_ws(Conn &conn, int fd);
#endif
	void queue_ws_cancel(int fd, WsInstallEntry entry);
#if CONFLUX_HAS_TLS
	void finish_tls_ws_handoff(int fd, WsInstallEntry entry);
#endif
	void handle_ws_cancel(int fd);
	void queue_ws_fixed_install(
		int slot_fd,
		WsHandoffState state,
		std::string initial_buf
#if CONFLUX_HAS_TLS
		,
		SSL *ssl = nullptr
#endif
	);
	void handle_fixed_fd_install(int slot_fd, int real_fd);
#if CONFLUX_HAS_TLS
	// Called when all bytes in tls_send_buf have been sent.  Drives the
	// post-send state machine for TLS connections.
	void handle_send_tls_complete(int fd, Conn &conn);
#endif // CONFLUX_HAS_TLS
	// Called once a response (or chunk) has been fully delivered.
	// Drives SSE/WS/normal post-send state machine.
	void handle_send_complete(int fd, Conn &conn);
	void finish_plain_send(int fd, Conn &conn);
	void finish_mapped_send(int fd, Conn &conn);
	void fail_send(int fd, Conn &conn);
	void handle_send(int fd, int res, std::uint32_t gen);
	void handle_send_zc(int fd, int res, conflux::uring::CqeFlags flags, std::uint32_t gen);

	void handle_sse_poll(int fd, int res, std::uint32_t gen);
	void handle_deferred_poll(int deferred_efd, int res, std::uint32_t gen);
	void handle_conn_close(int fd, int res, std::uint32_t gen);
	void handle_direct_slot_close(int fd, int res);
	void dispatch_cqe(Op op, int fd, int res, conflux::uring::CqeFlags flg, std::uint32_t gen);
	// Phase 1: copy recv data out of provided/pinned recv buffers, return
	// ownership immediately.  RecvPayload keeps the HTTP path independent of the
	// concrete buffer backend so a later RECV_ZC backend can preserve this flow.
	template<typename Buf>
	bool append_recv_buf_to(Buf &dst, RecvComp &rc);
	void phase1_copy_recv_bufs();
	void finish_ready_ws_handoffs();
#if CONFLUX_HAS_TLS
	// Per-connection TLS recv handler: feeds ciphertext into OpenSSL, drives the
	// handshake, and decrypts application data back into conn.partial.
	void phase1b_tls_one(Conn &conn, RecvComp &rc);
#endif // CONFLUX_HAS_TLS (phase1b_tls_one)
	// Phase 1b: run TLS recv processing.
	// Plain connections: no-op when TLS not compiled in.
	void phase1b_process();
	// Phase 2: build responses (serial — parallelism comes from multiple rings).
	void phase2_build_responses();
	// Phase 3: return unconsumed buffers + dispatch send/close.
	void phase3_dispatch();
	[[nodiscard]] bool ring_integrity_suspect() const noexcept;
	void note_recv_bundle_slices(RecvSlices const &slices) noexcept;
	void note_recv_payload(RecvPayload const &payload) noexcept;
	void note_cq_overflow() noexcept;
	[[nodiscard]] HttpServerMetrics metrics_snapshot() const noexcept;
	void try_grow_cq_after_overflow() noexcept;
	void enter_ring_fatal(ServerFatalReason reason) noexcept;
	void close_tracked_fds_sync() noexcept;
	void recycle_recv_buffer_direct(io_uring_cqe const *cqe) noexcept;
	void dispatch_cqe_fatal(io_uring_cqe const *cqe) noexcept;
	void emit_ring_diagnostics() noexcept;
	void flush_overflow_cqes_until_clear_or_limit() noexcept;
	conflux::http::RunStatus run_loop();
};
