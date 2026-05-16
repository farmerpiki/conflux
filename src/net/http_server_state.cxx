module;
#include <cstddef> // must precede openssl/nghttp2: establishes C++ linkage for __is_constant_evaluated before extern "C" blocks re-include c++config.h

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdint>
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

import conflux.net.http.types;
import conflux.net.router;
import conflux.file_map;
import conflux.net.direct_slot_pool;
import conflux.net.vhost;
import conflux.net.config;
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

enum class Op : u8 {
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
	[[nodiscard]] SendZcMetrics snapshot() const noexcept {
		return static_cast<SendZcMetrics const &>(*this);
	}
};

enum class ServerFatalReason : u8 {
	none,
	cq_overflow,
	cq_overflow_no_nodrop,
	submit_wait_ebadr,
	internal_exception,

};

inline constexpr u32 OP_SHIFT = 56U;
inline constexpr u32 GEN_SHIFT = 24U;
inline constexpr u64 GEN_MASK = 0xFFFFFFFFULL;
inline constexpr u64 FD_MASK = 0x00FFFFFFULL;
constexpr u64 pack(
	Op op,
	u32 gen,
	int fd) noexcept {
	return (static_cast<u64>(static_cast<u8>(op)) << OP_SHIFT)
		 | ((static_cast<u64>(gen) & GEN_MASK) << GEN_SHIFT)
		 | (static_cast<u64>(static_cast<u32>(fd)) & FD_MASK);
}
constexpr Tup<Op, u32, int> unpack(
	u64 ud) noexcept {
	return {
		static_cast<Op>(ud >> OP_SHIFT),
		static_cast<u32>((ud >> GEN_SHIFT) & GEN_MASK),
		static_cast<int>(ud & FD_MASK)};
}

struct PartialBuf {
	S buf{};
	SZ pos{0};
	[[nodiscard]] inline bool empty() const noexcept { return pos >= buf.size(); }
	[[nodiscard]] inline SZ size() const noexcept { return buf.size() - pos; }
	[[nodiscard]] inline char const *data() const noexcept { return buf.data() + pos; }
	[[nodiscard]] inline char front() const noexcept { return buf[pos]; }
	[[nodiscard]] inline SV view() const noexcept { return {buf.data() + pos, buf.size() - pos}; }
	inline void append(
		char const *p,
		SZ n) {
		buf.append(p, n);
	}
	inline void consume(
		SZ n) noexcept {
		pos += n;
		if (pos >= buf.size()) {
			clear();
		}
	}
	inline void clear() noexcept {
		buf.clear();
		pos = 0;
	}
	[[nodiscard]] inline S take() {
		if (pos > 0) {
			buf.erase(0, pos);
		}
		pos = 0;
		return move(buf);
	}
};

struct RecvComp {
	int fd;
	int res;
	u32 gen;
	u32 flags;
};
inline constexpr SZ FD_TABLE_RESERVE = 4096;
inline constexpr unsigned DEFAULT_RING_ENTRIES = 1024U;
#if CONFLUX_HAS_HTTP2
struct Ring; // forward-declared so H2ConnCtx can hold Ring* while Conn precedes Ring
static constexpr SZ kH2PendingSendCap = SZ{64} * 1024;
struct H2Stream {
	S method{};
	S path{};
	S scheme{"https"};
	S authority{};
	HttpFields headers{};
	S body{};
	SZ expected_body_size{};
	bool body_reserved{};
	bool end_stream_seen{};
	bool rejected{};
	bool regular_header_seen{};
	bool seen_method{};
	bool seen_path{};
	bool seen_scheme{};
	bool seen_authority{};
	bool seen_content_length{};
	SZ header_count{};
	SZ header_list_size{};
	// Response state for the data provider callback:
	S response_body{};
	SZ response_off{};
	HttpFields response_trailers{};
	int deferred_efd{-1};
	// SSE streaming state (non-null → H2 SSE stream):
	SP<SseChannel> sse_channel{};
	S h2_sse_buf{}; // overflow: drained SSE data not yet framed
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
	SV raw,
	Ring const &ring,
	SZ max_body_size,
	bool http_redirect_to_https,
	V<S> const &https_redirect_hosts,
	ParserLimits const &limits);
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): field order mirrors connection state-machine phases.
struct alignas(
	64) Conn {
	int fd = -1;
	u32 gen = 0;
	bool recv_armed = false;
	u16 incremental_buf_id{};
	bool have_incremental_buf_id{};
	u32 last_recv_cqe_flags{};
	bool have_last_recv_cqe_flags{};
	bool send_queued = false;
	bool is_sse = false;
	bool sse_headers_sent = false;
	bool is_ws = false; // true → WebSocket upgrade; hand off fd to WS thread after send
	bool is_deferred = false;
	bool deferred_head_only = false; // HEAD on deferred route → strip body when ready
	bool closing = false; // close SQE already submitted for this generation
	bool close_after_send = false; // true → close instead of re-arming recv
	chrono::steady_clock::time_point close_after_send_deadline{}; // force-close grace deadline during shutdown
	int sse_efd = -1;
	SP<SseChannel> sse_channel{};
	int deferred_efd = -1;
	SP<DeferredResponse> deferred_response{};
	SP<WsUpgrade> ws_upgrade{}; // set when 101 pending; cleared after handoff
	SP<WorkPool> ws_work_pool{};
	HttpRequest saved_req{}; // copy of request saved for WS handler thread
	bool is_tls = false; // set after first-byte sniff; used by dispatch_request
#if CONFLUX_HAS_TLS
	// TLS state (null → plaintext connection)
	UniqueSsl ssl;
	S tls_rx_cipher{}; // encrypted bytes received from the socket, before SSL_read()
	S tls_send_pending{}; // encrypted bytes generated while no send can be submitted
	S tls_send_inflight{}; // stable borrowed storage for in-flight io_uring SEND
	SZ tls_send_off{}; // bytes of tls_send_inflight already sent
	bool tls_hs_done = false; // TLS handshake completed; also used as undecided sentinel
	bool tls_sending_response = false; // true → current tls_send_buf carries HTTP response data
	bool tls_shutdown_after_send = false; // true → send TLS close_notify after pending bytes drain
	bool tls_wait_peer_shutdown = false; // true → drain peer close_notify/FIN after our close_notify is sent
	bool ktls_send = false; // kTLS send offload active; splice_to_fd usable for TLS file body
	#endif
	bool has_response = false;
	S own_response{};
	PartialBuf partial{};
	SZ written = 0;
	FixedBuffer send_buf{};
	SZ send_buf_base_written{};
	SZ send_buf_len{};
	SZ request_bytes = 0; // bytes consumed by current dispatched request
	chrono::steady_clock::time_point last_activity; // updated on accept and recv
	chrono::steady_clock::time_point request_started{};
	bool request_in_progress = false;
	bool expect_continue_sent = false;
	S remote_addr{}; // peer IP, set on accept
	ChunkedDecodeState chunked_decode{};
	// mmap path: non-null when current response has a zero-copy file region
	SP<MappedBody> mapped_file{};
	SZ mapped_total{}; // own_response.size() + mapped_file->size
	u64 mapped_delivered{};
	A<iovec, 2> writev_iov{}; // iovecs rebuilt per-send in queue_send_mapped

	// file_io streaming path: non-null when current response streams via splice
	// (plain HTTP) or read_fixed+SSL_write (TLS). Phase tracks whether headers
	// have been flushed to the socket.
	SP<StreamedFile> streamed_file{};
	bool streamed_headers_sent = false;
	u64 streamed_delivered = 0;
	bool streamed_splice_in_flight = false;
	SendZcCqeState zc_state{};
	bool zc_tls_bypass_counted = false;
#if CONFLUX_HAS_HTTP2
	bool is_h2{};
	nghttp2_session *h2_session = nullptr;
	UP<H2ConnCtx> h2_ctx;
	M<i32, H2Stream> h2_streams{};
	S h2_pending_send{};
	i32 h2_sse_stream_id{-1}; // stream_id of active H2 SSE stream (-1 = none)
	bool h2_sse_pending_wait{}; // set by on_frame_recv_cb to trigger queue_sse_wait after h2_do_send
	#endif
};

#if CONFLUX_HTTP_TRACE
[[nodiscard]] inline char const *buffer_ring_mode_name(
	BufferRingMode mode) noexcept {
	switch (mode) {
	case BufferRingMode::classic_one_cqe_per_buffer: return "classic";
	case BufferRingMode::recv_bundle                      : return "recv_bundle";
	case BufferRingMode::incremental                      : return "incremental";
	}
	return "unknown";
}
#define HTTP_TRACE(MSG) eprintln(format("http_trace {}", (MSG)))
#else
#define HTTP_TRACE(MSG) ((void)0)
#endif

struct Ring;

conflux::work::root::Task<void>
do_streamed_splice(Ring *ring, int fd, u32 conn_gen, conflux::work::root::Task<SZ> splice_task);

conflux::work::root::Task<void> do_streamed_tls_chunk(
	Ring *ring,
	int fd,
	u32 conn_gen,
	SZ want,
	conflux::work::root::Task<FileReader::ReadFixedResult> read_task);
struct Ring {
	struct DeferredWait {
		int conn_fd{-1};
		i32 stream_id{-1};
		SP<DeferredResponse> response{};
	};
	struct WsHandoffState {
		SP<WsUpgrade> upgrade{};
		SP<WorkPool> pool{};
		HttpRequest request{};
	};
	struct WsInstallEntry {
		WsHandoffState state{};
		S initial_buf{};
#if CONFLUX_HAS_TLS
		UniqueSsl ssl{};
#endif
	};
	struct RetiredIncrementalBuf {
		u16 id{};
		bool present{};
	};
	static u64 pack_fd_gen(
		int fd,
		u32 gen) noexcept;
	static constexpr SZ BUF_SIZE = 8192;
	static constexpr u32 MAX_FILES = 65536;

	io_uring ring{};
	SocketRawRing raw_{conflux::uring::RingRef{ring}};
	mutable CompletionTable client_ct_{64};
	mutable Opt<SocketTaskRing> client_task_ring_{};
	// Backing memory for the ring when no_mmap = true. Freed on destroy.
	UPD<byte[], void (*)(void *)> ring_mem{nullptr, ::free};
	int listen_fd = -1;
	sockaddr_in6 client_addr{};
	socklen_t client_addr_len = sizeof(client_addr);

	V<Conn> fd_table{};
	V<RecvComp> recvs{};
	UM<int, DeferredWait> deferred_waits{};
	UM<u64, UP<u64>> in_flight_read_bufs{};
	UM<int, WsInstallEntry> ws_cancel_handoffs{};
	UM<int, WsInstallEntry> ws_installs{};
	UM<u64, RetiredIncrementalBuf> retired_incremental_recv{};
	V<int> ws_cancel_ready{};

	UP<BufferRing> buf_ring_;
	UP<DirectFdTable> direct_fds_;
	UP<DirectSlotPool> direct_slots_;
	int tcp_opt_one_ = 1; // stable optval for cmd_sock SETSOCKOPT

	conflux::uring::IoUringCaps caps{};
	u32 requested_setup_flags_ = 0; // flags requested before adaptive EINVAL fallback
	u32 active_setup_flags_ = 0; // flags used by the successful io_uring setup call
	u32 stripped_setup_flags_ = 0; // requested flags removed during adaptive EINVAL fallback
	bool listen_fixed = false;
	bool accepted_sockets_direct = false;
	bool direct_accept_enabled_ = true;
	bool cmd_sock_setsockopt_enabled_ = true;
	bool startup_banner = false;
	bool shutting_down = false;
	bool ring_fatal_{false};
	ServerFatalReason fatal_reason_{ServerFatalReason::none};
	u32 fatal_cq_overflow_count_{0};
	bool overflow_flush_limit_hit_{false};
	bool saw_overflow_since_last_resize_{false};
	bool cq_resize_unsupported_{false};
	bool use_recv_bundle = false; // IORING_RECVSEND_BUNDLE on multishot recv
	bool use_recv_incremental_buf = false; // IOU_PBUF_RING_INC on buffer ring
	bool auto_recv_arm_policy = false; // adaptive poll_first via IORING_CQE_F_SOCK_NONEMPTY
	int busy_poll_us_ = 0; // SO_BUSY_POLL optval; 0=disabled
	bool prefer_busy_poll_ = false; // SO_PREFER_BUSY_POLL
	int ring_core_ = -1; // sched_setaffinity core for this ring thread; -1=disabled
	int worker_core_ = -1; // IORING_REGISTER_IOWQ_AFF core for io-wq; -1=disabled
	bool send_zc_enabled_ = false;
	SZ send_zc_threshold_ = 16384;
	bool send_zc_report_usage_ = true;
	SendZcCounters zc_counters_{};
	u64 accepted_direct_failures_{};
	u64 recv_bundle_cqes_{};
	u64 recv_bundle_slices_{};
	u64 recv_bundle_bytes_{};
	SZ max_body_size = SZ{1024} * 1024; // set from Config before run_loop()
	u32 request_timeout_ms = 30000; // set from Config before run_loop(); 0 = disabled
	u32 tls_sniff_timeout_ms = 10000; // set from Config before run_loop(); 0 = disabled
	bool slow_handler_diagnostics = false; // set from Config before run_loop()
	u32 slow_handler_warn_ms = 25; // set from Config before run_loop()
	bool http_redirect_to_https = false; // set from Config before run_loop()
	V<S> https_redirect_hosts{}; // set from Config before run_loop()
	ParserLimits parser_limits{}; // set from Config before run_loop()
	S alt_svc_header{}; // "h3=\":443\"; ma=86400" when h3 enabled; empty otherwise

	int shutdown_efd = -1; // set by HttpServer before run_loop(); not owned
	u64 shutdown_buf = 0; // read target for the shutdown eventfd SQE

	// file_io pools — constructed after io_uring_queue_init. Shared by
	// static-file serving and any other caller that grabs files.get().
	UP<CompletionTable> file_completions{};
	UP<RegisteredBufferTable> buf_table{};
	UP<FixedBufferPool> fixed_buffers{};
	UP<FixedBufferPool> send_buffers{};
	UP<PipePool> splice_pipes{};
	UP<FileReader> files{};
	bool send_fixed_buffers_supported{false};

	// pool sizing — set from Config before run_loop()
	SZ file_io_slabs = 64;
	SZ file_io_slab_bytes = SZ{64} * 1024;
	SZ file_io_pipe_pairs = 16;
	SZ send_buffer_slabs = 64;
	SZ send_buffer_bytes = SZ{4} * 1024;
	bool send_fixed_buffers_enabled = false;

	__kernel_timespec timer_ts{}; // reused for the periodic timeout SQE
	static constexpr chrono::milliseconds shutdown_close_after_send_timeout{5000};

	// Software fallback queue for ops that could not obtain an SQE even after
	// a flush. Drained at the top of each run_loop iteration (after CQE reap
	// frees SQ slots). Each thunk re-invokes the original queue_* path so
	// conn state (gen, buffers) is re-read at replay time.
	deque<conflux::work::root::detail::small_move_only_function<void()>> pending_ops{};

	Router const *router = nullptr; // set before init(); not owned
	VHostRouter const *vhost_router = nullptr; // set before init(); not owned

	u16 bound_port{}; // actual port after bind (handles port=0)
	Atom<u16> *port_signal = nullptr; // set for ring 0; signalled after listen()
#if CONFLUX_HAS_TLS
	SSL_CTX *ssl_ctx = nullptr; // non-owning; set by HttpServer before run(); null → plaintext
	// vhost_ctxs: hostname → SSL_CTX* for SNI switching (non-owning, owned by HttpServer::Impl)
	V<P<S, SSL_CTX *>> vhost_ctxs;
#endif

	Ring() = default;
	~Ring();
	Ring(Ring const &) = delete;
	Ring &operator =(Ring const &) = delete;
	Ring(Ring &&) = delete;
	Ring &operator =(Ring &&) = delete;
	[[nodiscard]] HttpResponse dispatch(
		HttpRequestView const &req) const;
	[[nodiscard]] bool has_context_routes() const noexcept;
	[[nodiscard]] Opt<HttpResponse> try_dispatch_context(
		HttpRequestView const &req) const;
	[[nodiscard]] SP<WorkPool> resolve_ws_work_pool(
		HttpRequestView const &req) const;
	void clear_deferred_wait(
		int deferred_efd);
	void queue_deferred_wait(
		int conn_fd,
		int deferred_efd,
		SP<DeferredResponse> response,
		i32 stream_id = -1);
#if CONFLUX_HAS_TLS
	static void tls_flush_wbio(
		Conn &conn);
	static bool tls_feed_rbio(
		Conn &conn);
	// Submit an io_uring send for pending/in-flight TLS bytes.
	// Caller must NOT pre-set send_queued; this function owns the transition.
	void tls_queue_send(
		Conn &conn);
	void begin_tls_peer_shutdown_wait(
		int fd,
		Conn &conn);
	void queue_tls_shutdown(
		int fd,
		Conn &conn);
#endif // CONFLUX_HAS_TLS

#if CONFLUX_HAS_HTTP2
	// ---------------------------------------------------------------------------
	// nghttp2 static callbacks (passed as C function pointers; no capture)
	// ---------------------------------------------------------------------------
	static void h2_reject_stream(
		nghttp2_session *session,
		H2Stream &stream,
		i32 stream_id,
		u32 error_code);
	[[nodiscard]] static bool h2_valid_regular_header_name(
		SV name) noexcept;
	[[nodiscard]] static bool h2_forbidden_connection_header(
		SV name) noexcept;
	// nghttp2 wants to write bytes to the wire; accumulate into h2_pending_send.
	static ssize_t h2_send_cb(
		nghttp2_session * /*unused*/,
		u8 const *data,
		SZ length,
		int /*unused*/,
		void *user_data);
	// A new request stream is beginning; allocate its H2Stream entry.
	static int h2_on_begin_headers_cb(
		nghttp2_session * /*unused*/,
		nghttp2_frame const *frame,
		void *user_data);
	// Populate H2Stream fields from pseudo-headers and regular headers.
	static int h2_on_header_cb(
		nghttp2_session *session,
		nghttp2_frame const *frame,
		u8 const *name,
		SZ namelen,
		u8 const *header_value,
		SZ valuelen,
		u8 /*unused*/,
		void *user_data);
	// Accumulate DATA frame body bytes into stream.body.
	static int h2_on_data_chunk_cb(
		nghttp2_session *session,
		u8 /*unused*/,
		i32 stream_id,
		u8 const *data,
		SZ len,
		void *user_data);
	// Data provider: feed response body bytes to nghttp2's framing layer.
	// Handles both static responses and SSE streaming.
	static ssize_t h2_read_cb(
		nghttp2_session *session,
		i32 stream_id,
		u8 *buf,
		SZ length,
		u32 *data_flags,
		nghttp2_data_source *source,
		void * /*user_data*/);
	static void h2_submit_response(
		Conn &conn,
		i32 stream_id,
		HttpResponse resp);
	// A frame is fully received.  On END_STREAM, dispatch to the router and
	// submit the HTTP/2 response via nghttp2_submit_response.
	static int h2_on_frame_recv_cb(
		nghttp2_session *session,
		nghttp2_frame const *frame,
		void *user_data);
	// Stream fully closed — release its state.
	static int h2_on_stream_close_cb(
		nghttp2_session * /*unused*/,
		i32 stream_id,
		u32 /*EC*/,
		void *user_data);
	// ---------------------------------------------------------------------------
	// H2 Ring methods
	// ---------------------------------------------------------------------------

	// SSL_write h2_pending_send into tls_send_buf, then queue a TLS send.
	// No-op if nothing pending or a send is already in flight (data accumulates
	// and will be flushed when handle_send_tls_complete's H2 branch runs next).
	void h2_flush_pending(
		Conn &conn);
	// Drive nghttp2 output (all queued frames) and flush to io_uring.
	void h2_do_send(
		Conn &conn);
	// Create nghttp2 server session and submit the server connection preface
	// (SETTINGS frame).  Does NOT flush — caller must call h2_do_send() after
	// running nghttp2_session_mem_recv() so that the SETTINGS and SETTINGS_ACK
	// are coalesced into a single TLS record.
	void h2_setup_conn(
		Conn &conn);
#endif // CONFLUX_HAS_HTTP2
	// Must be called from the thread that will run run_loop() (SINGLE_ISSUER).
	// `wq_fd`: when non-zero, sets IORING_SETUP_ATTACH_WQ so this ring shares
	// the parent ring's kernel io-wq. Pass ring[0].ring.ring_fd for rings 1..N.
	void init(
		u16 port,
		unsigned entries,
		u32 uring_flags,
		u32 wq_fd = 0,
		bool no_mmap = false);
	Conn &conn_for(
		int fd);
	void conn_erase(
		int fd,
		u32 gen);
	// Acquire a raw SQE without implicit submission. Returns null when the ring
	// is exhausted or fatal; callers handle that via defer_op() to avoid stalls.
	io_uring_sqe *get_sqe();
	// Defer an op whose SQE allocation failed. Replayed from run_loop once
	// the CQE reap frees ring capacity.
	void defer_op(
		conflux::work::root::detail::small_move_only_function<void()> op);
	void cancel_multishot_recv_or_defer(
		SocketHandle handle);
	void drain_pending_ops();
	void defer_queue_send_if_current(
		int fd,
		u32 gen);
	void defer_handle_send_complete_if_current(
		int fd,
		u32 gen);
	void defer_start_streamed_body_if_current(
		int fd,
		u32 gen);
	void queue_multishot_accept();
	[[nodiscard]] RecvArmPolicy resolve_recv_arm_policy(
		Conn const &conn) const noexcept;
	void queue_multishot_recv(
		int fd);
	void queue_direct_accept_setup(
		int fd);
	// Submit WRITEV for a mapped-file response.
	// Adjusts iovecs to skip bytes already sent (conn.written).
	// When the body is large enough for SEND_ZC, keep the header send separate so
	// the body can use zero-copy after the header CQE drains.
	void queue_send_mapped(
		int fd);
	// file_io streaming path. Phase 1: send headers via prep_send. Phase 2:
	// once headers are fully delivered, kick off splice (plain) or read_fixed+
	// SSL_write (TLS). queue_send_streamed only handles phase 1; phase 2 is
	// triggered from handle_send once the header bytes are acked.
	void queue_send_streamed(
		int fd);
	// Acquire a pipe P and submit the splice chain via FileReader. Completion
	// calls back into handle_streamed_splice_done on the ring thread.
	void start_streamed_body(
		int fd);
#if CONFLUX_HAS_TLS
	// TLS streamed body: acquire a FixedBuffer, read_fixed a chunk of the file,
	// SSL_write it into wbio, flush and re-queue the TLS send. Pipelining depth
	// is effectively 1 per connection — suitable for unbuffered streaming.
	void start_streamed_tls_chunk(
		int fd);
	void on_streamed_tls_chunk_done(
		int fd,
		u32 conn_gen,
		FixedBuffer buf,
		SZ bytes,
		EP const &err);
	void write_mapped_tls_chunk(
		int fd,
		Conn &conn);
#endif
	void on_streamed_splice_done(
		int fd,
		u32 conn_gen,
		SZ delivered,
		EP const &err);
#if CONFLUX_HAS_TLS
	[[nodiscard]] bool tls_write_plaintext(
		int fd,
		Conn &conn,
		SV bytes);
#endif
	void note_send_zc_tls_bypass_if_candidate(
		Conn &conn) noexcept;
	void queue_send(
		int fd);
	[[nodiscard]] static bool response_send_ready(
		Conn const &conn) noexcept;
	void start_response_send(
		int fd,
		Conn &conn);

	void invalidate_recv_if_armed(
		int fd);
	void cancel_accept_or_defer();
	void submit_conn_close_or_defer(
		int fd,
		u32 gen);
	void submit_fd_shutdown_or_defer(
		int fd,
		u32 gen);
	void handle_fd_shutdown(
		int fd,
		int res,
		u32 gen);
	void queue_close(
		int fd);
	void queue_sse_wait(
		int fd);
	void queue_deferred_wait(
		int fd);
	void arm_shutdown_read();
	// Arm a one-shot periodic timer that fires every ~1 second for connection reaping.
	void arm_timer();
	void handle_timer();
	void handle_shutdown();
	void handle_accept(
		int res,
		u32 flg);
	void discard_recv_bufs(
		int res,
		u32 flags) noexcept;
	void discard_recv_bufs(
		RecvComp &rc) noexcept;
	void retire_incremental_partial(
		int fd,
		u32 gen,
		Conn &conn) noexcept;
	void reclaim_retired_incremental_recv(
		int fd,
		u32 gen) noexcept;
	void clear_retired_incremental_if_final(
		int fd,
		u32 gen,
		u32 flags) noexcept;
	void handle_recv_cqe(
		int fd,
		int res,
		u32 flg,
		u32 gen);
	bool handle_sse_send_complete(
		int fd,
		Conn &conn);
	void handle_http_response_send_complete(
		int fd,
		Conn &conn);
	[[nodiscard]] static bool make_blocking_fd(
		int fd);
	[[nodiscard]] WsHandoffState begin_ws_handoff(
		Conn &conn);
	void launch_plain_ws_handler(
		WorkPool &pool,
		WsHandoffState state,
		int fd,
		S initial_buf);
	void finish_plain_ws_handoff(
		int fd,
		WsInstallEntry entry);
	void handoff_plain_ws(
		Conn &conn,
		int fd);
#if CONFLUX_HAS_TLS
	void launch_tls_ws_handler(
		WorkPool &pool,
		WsHandoffState state,
		int fd,
		SSL *ssl,
		S initial_buf);
	void handoff_tls_ws(
		Conn &conn,
		int fd);
#endif
	void queue_ws_cancel(
		int fd,
		WsInstallEntry entry);
#if CONFLUX_HAS_TLS
	void finish_tls_ws_handoff(
		int fd,
		WsInstallEntry entry);
#endif
	void handle_ws_cancel(
		int fd);
	void queue_ws_fixed_install(
		int slot_fd,
		WsHandoffState state,
		S initial_buf
#if CONFLUX_HAS_TLS
		,
		SSL *ssl = nullptr
#endif
	);
	void handle_fixed_fd_install(
		int slot_fd,
		int real_fd);
#if CONFLUX_HAS_TLS
	// Called when all bytes in tls_send_buf have been sent.  Drives the
	// post-send state machine for TLS connections.
	void handle_send_tls_complete(
		int fd,
		Conn &conn);
#endif // CONFLUX_HAS_TLS
	// Called once a response (or chunk) has been fully delivered.
	// Drives SSE/WS/normal post-send state machine.
	void handle_send_complete(
		int fd,
		Conn &conn);
	void finish_plain_send(
		int fd,
		Conn &conn);
	void finish_mapped_send(
		int fd,
		Conn &conn);
	void fail_send(
		int fd,
		Conn &conn);
	void handle_send(
		int fd,
		int res,
		u32 gen);
	void handle_send_zc(
		int fd,
		int res,
		u32 flags,
		u32 gen);

	void handle_sse_poll(
		int fd,
		int res,
		u32 gen);
	void handle_deferred_poll(
		int deferred_efd,
		int res,
		u32 gen);
	void handle_conn_close(
		int fd,
		int res,
		u32 gen);
	void handle_direct_slot_close(
		int fd,
		int res);
	void dispatch_cqe(
		Op op,
		int fd,
		int res,
		u32 flg,
		u32 gen);
	// Phase 1: copy recv data out of provided/pinned recv buffers, return
	// ownership immediately.  RecvPayload keeps the HTTP path independent of the
	// concrete buffer backend so a later RECV_ZC backend can preserve this flow.
	template<typename Buf>
	bool append_recv_buf_to(
		Buf &dst,
		RecvComp &rc);
	void phase1_copy_recv_bufs();
	void finish_ready_ws_handoffs();
#if CONFLUX_HAS_TLS
	// Per-connection TLS recv handler: feeds ciphertext into OpenSSL, drives the
	// handshake, and decrypts application data back into conn.partial.
	void phase1b_tls_one(
		Conn &conn,
		RecvComp &rc);
#endif // CONFLUX_HAS_TLS (phase1b_tls_one)
	// Phase 1b: run TLS recv processing.
	// Plain connections: no-op when TLS not compiled in.
	void phase1b_process();
	// Phase 2: build responses (serial — parallelism comes from multiple rings).
	void phase2_build_responses();
	// Phase 3: return unconsumed buffers + dispatch send/close.
	void phase3_dispatch();
	[[nodiscard]] bool ring_integrity_suspect() const noexcept;
	void note_recv_bundle_slices(
		RecvSlices const &slices) noexcept;
	void note_recv_payload(
		RecvPayload const &payload) noexcept;
	void note_cq_overflow() noexcept;
	[[nodiscard]] HttpServerMetrics metrics_snapshot() const noexcept;
	void try_grow_cq_after_overflow() noexcept;
	void enter_ring_fatal(
		ServerFatalReason reason) noexcept;
	void close_tracked_fds_sync() noexcept;
	void recycle_recv_buffer_direct(
		io_uring_cqe const *cqe) noexcept;
	void dispatch_cqe_fatal(
		io_uring_cqe const *cqe) noexcept;
	void emit_ring_diagnostics() noexcept;
	void flush_overflow_cqes_until_clear_or_limit() noexcept;
	RunStatus run_loop();
};
