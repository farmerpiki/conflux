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
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

module conflux.net.http_server;

import std;
import conflux.types;
import std.compat;

import conflux.net.http.types;
import conflux.net.router;
import conflux.file_map;
import conflux.net.detail.direct_slot_pool;
import conflux.net.vhost;
import conflux.net.config;
import conflux.net.http1_parser;
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
import :loop;
using namespace conflux::socket_io;

namespace conflux::http {

void add_metrics(
	conflux::http::HttpServerMetrics &dst,
	conflux::http::HttpServerMetrics const &src) noexcept {
	dst.sq_dropped += src.sq_dropped;
	dst.cq_overflow += src.cq_overflow;
	dst.accepted_direct_failures += src.accepted_direct_failures;
	dst.zc_notifications_pending += src.zc_notifications_pending;
	dst.zc_capable_rings += src.zc_capable_rings;
	dst.zc_enabled_rings += src.zc_enabled_rings;
	dst.recv_bundle_cqes += src.recv_bundle_cqes;
	dst.recv_bundle_slices += src.recv_bundle_slices;
	dst.recv_bundle_bytes += src.recv_bundle_bytes;
	dst.send_zc.attempts += src.send_zc.attempts;
	dst.send_zc.plain_attempts += src.send_zc.plain_attempts;
	dst.send_zc.mapped_attempts += src.send_zc.mapped_attempts;
	dst.send_zc.bytes_requested += src.send_zc.bytes_requested;
	dst.send_zc.bytes_sent += src.send_zc.bytes_sent;
	dst.send_zc.notifications += src.send_zc.notifications;
	dst.send_zc.copied_notifications += src.send_zc.copied_notifications;
	dst.send_zc.sends_without_notification += src.send_zc.sends_without_notification;
	dst.send_zc.errors_enomem += src.send_zc.errors_enomem;
	dst.send_zc.errors_other += src.send_zc.errors_other;
	dst.send_zc.fallback_regular_send += src.send_zc.fallback_regular_send;
	dst.send_zc.tls_bypass += src.send_zc.tls_bypass;
	dst.send_zc.tls_bypass_bytes += src.send_zc.tls_bypass_bytes;
	dst.send_zc.adaptive_disable_count += src.send_zc.adaptive_disable_count;
	dst.rejections.malformed_request += src.rejections.malformed_request;
	dst.rejections.request_line_too_large += src.rejections.request_line_too_large;
	dst.rejections.header_line_too_large += src.rejections.header_line_too_large;
	dst.rejections.header_block_too_large += src.rejections.header_block_too_large;
	dst.rejections.too_many_headers += src.rejections.too_many_headers;
	dst.rejections.missing_host += src.rejections.missing_host;
	dst.rejections.duplicate_host += src.rejections.duplicate_host;
	dst.rejections.malformed_content_length += src.rejections.malformed_content_length;
	dst.rejections.duplicate_content_length += src.rejections.duplicate_content_length;
	dst.rejections.content_length_with_transfer_encoding += src.rejections.content_length_with_transfer_encoding;
	dst.rejections.unsupported_transfer_encoding += src.rejections.unsupported_transfer_encoding;
	dst.rejections.invalid_transfer_encoding += src.rejections.invalid_transfer_encoding;
	dst.rejections.invalid_chunk += src.rejections.invalid_chunk;
	dst.rejections.body_too_large += src.rejections.body_too_large;
	dst.rejections.expectation_failed += src.rejections.expectation_failed;
	dst.rejections.header_timeout += src.rejections.header_timeout;
	dst.rejections.body_timeout += src.rejections.body_timeout;
	dst.static_files.mapped_responses += src.static_files.mapped_responses;
	dst.static_files.streamed_responses += src.static_files.streamed_responses;
	dst.static_files.splice_submits += src.static_files.splice_submits;
	dst.static_files.tls_read_fixed_submits += src.static_files.tls_read_fixed_submits;
	dst.static_files.tls_mapped_plaintext_chunks += src.static_files.tls_mapped_plaintext_chunks;
	dst.pressure.accept_rejected += src.pressure.accept_rejected;
	dst.pressure.connections_closed_for_pressure += src.pressure.connections_closed_for_pressure;
	dst.pressure.response_backpressure_events += src.pressure.response_backpressure_events;
	dst.pressure.sse_dropped_newest += src.pressure.sse_dropped_newest;
	dst.pressure.sse_dropped_oldest += src.pressure.sse_dropped_oldest;
	dst.pressure.sse_disconnected_for_pressure += src.pressure.sse_disconnected_for_pressure;
	dst.pressure.websocket_closed_for_pressure += src.pressure.websocket_closed_for_pressure;
	dst.pressure.drain_started += src.pressure.drain_started;
	dst.pressure.drain_deadline_hit += src.pressure.drain_deadline_hit;
	dst.pressure.drain_forced_close += src.pressure.drain_forced_close;
}

[[nodiscard]] std::optional<std::string> startup_config_error(
	conflux::http::Config const &cfg) {
	auto config_issues = conflux::http::validate_config(cfg);
	if (!config_issues.empty()) {
		return conflux::http::config_issue_summary(config_issues.front());
	}
	if (auto caps = conflux::runtime::detect_capabilities()) {
		auto capability_issues = conflux::http::validate_config_capabilities(cfg, *caps);
		if (cfg.feature_fallback == conflux::runtime::FeatureFallback::fail_fast && !capability_issues.empty()) {
			auto const &issue = capability_issues.front();
			return std::format("capability.{}: {}", issue.feature, issue.message);
		}
	} else if (cfg.feature_fallback == conflux::runtime::FeatureFallback::fail_fast) {
		return std::format("capability.{}: {}", caps.error().feature, caps.error().message);
	}
	return std::nullopt;
}

struct HttpServer::Impl {
	conflux::http::Config cfg{};
	unsigned rings{};
	std::uint32_t uring_flags{};
	conflux::http::Router router;
	conflux::http::VHostRouter vhost_router;
	bool use_vhost = false;
	std::vector<std::unique_ptr<Ring>> ring_vec;
	std::vector<int> shutdown_efds;
	conflux::http::HttpServerObservabilityHooks observability_hooks{};
	std::atomic<std::uint16_t> bound_port_;
	std::mutex startup_error_mu;
	std::exception_ptr startup_error{};
	std::atomic_bool startup_failed{false};
	std::atomic<std::uint8_t> run_status_{static_cast<std::uint8_t>(conflux::http::RunStatus::stopped_normally)};
	Ring::DrainControl drain_control{};
	std::atomic_bool running_{false};
	// Signalled by ring[0] after init when attach_wq=true. Ring[1..N] wait
	// here for the wq_fd before calling io_uring_queue_init_params. -2 = unset.
	std::atomic<int> wq_ring_fd_{-2};
#if CONFLUX_HAS_TLS
	std::optional<conflux::net_tls::TlsServerContext> tls_ctx; // owned; shared (read-only) across rings
#endif
#if CONFLUX_HAS_HTTP3
	std::optional<conflux::net_tls::TlsServerContext> http3_tls_ctx;
	std::mutex http3_mu;
	std::unique_ptr<conflux::http::detail::Http3Listener> http3_listener;
#endif
};

void HttpServer::initialize(
	conflux::http::Config const &cfg) {
	impl_->cfg = cfg;
	impl_->rings = cfg.rings == 0 ? std::thread::hardware_concurrency() : cfg.rings;
	impl_->uring_flags = conflux::http::detail::build_uring_flags(cfg);

#if CONFLUX_HAS_TLS
	// TLS setup: create SSL_CTX if cert and key are provided.
	if (!cfg.cert_file.empty() && !cfg.key_file.empty()) {
		conflux::net_tls::init_openssl_once();
		conflux::net_tls::TlsServerOptions const primary_opts{
			.cert_file = cfg.cert_file,
			.key_file = cfg.key_file,
			.cipher_list = cfg.tls_cipher_list,
			.ciphersuites = cfg.tls_ciphersuites,
			.ktls = cfg.ktls,
		};
		impl_->tls_ctx.emplace(primary_opts);
		SSL_CTX *const ctx = impl_->tls_ctx->native_handle();
	#if CONFLUX_HAS_HTTP2
		conflux::http::detail::http2_configure_alpn(ctx); // prefer h2, fall back to http/1.1
	#endif
	#if CONFLUX_HAS_HTTP3
		if (cfg.http3.enabled) {
			impl_->http3_tls_ctx.emplace(primary_opts);
			conflux::http::detail::http3_configure_alpn(impl_->http3_tls_ctx->native_handle());
		}
	#endif

		// Load per-hostname SSL_CTX for SNI virtual hosts.
		for (auto const &vh: cfg.virtual_hosts) {
			impl_->tls_ctx->add_vhost(
				vh.hostname,
				conflux::net_tls::TlsServerOptions{
					.cert_file = vh.cert_file,
					.key_file = vh.key_file,
					.cipher_list = cfg.tls_cipher_list,
					.ciphersuites = cfg.tls_ciphersuites,
					.ktls = cfg.ktls,
				});
		}
		impl_->tls_ctx->install_sni();
	}
#endif // CONFLUX_HAS_TLS

	impl_->ring_vec.reserve(impl_->rings);
	impl_->shutdown_efds.reserve(impl_->rings);
	for (unsigned i = 0; i < impl_->rings; ++i) {
		impl_->ring_vec.emplace_back(std::make_unique<Ring>());
		int efd = ::eventfd(0, EFD_CLOEXEC);
		if (efd < 0) {
			throw std::system_error{errno, std::system_category(), "eventfd (shutdown)"};
		}
		if (efd <= 2) {
			int const dup = ::fcntl(efd, F_DUPFD_CLOEXEC, 3);
			::close(efd);
			if (dup < 0) {
				throw std::system_error{errno, std::system_category(), "eventfd dup above stdio"};
			}
			efd = dup;
		}
		impl_->shutdown_efds.push_back(efd);
	}
}

HttpServer::HttpServer(
	conflux::http::Config const &cfg,
	conflux::http::Router &&router)
	: impl_(new Impl{}) {
	impl_->router = std::move(router);
	initialize(cfg);
}

HttpServer::HttpServer(
	conflux::http::Config const &cfg,
	conflux::http::VHostRouter &&vhost_router)
	: impl_(new Impl{}) {
	impl_->use_vhost = true;
	impl_->vhost_router = std::move(vhost_router);
	initialize(cfg);
}

HttpServer::~HttpServer() {
	if (impl_) {
		for (int const efd: impl_->shutdown_efds) {
			::close(efd);
		}
		delete impl_;
	}
}

std::expected<std::unique_ptr<HttpServer>, std::string> HttpServer::try_create(
	conflux::http::Config const &cfg,
	conflux::http::Router &&router) {
	if (auto error = startup_config_error(cfg)) {
		return std::unexpected{std::move(*error)};
	}
	try {
		return std::make_unique<HttpServer>(cfg, std::move(router));
	} catch (std::exception const &ex) { return std::unexpected{std::string{ex.what()}}; } catch (...) {
		return std::unexpected{std::string{"unknown HttpServer construction error"}};
	}
}

std::expected<std::unique_ptr<HttpServer>, std::string> HttpServer::try_create(
	conflux::http::Config const &cfg,
	conflux::http::VHostRouter &&vhost_router) {
	if (auto error = startup_config_error(cfg)) {
		return std::unexpected{std::move(*error)};
	}
	try {
		return std::make_unique<HttpServer>(cfg, std::move(vhost_router));
	} catch (std::exception const &ex) { return std::unexpected{std::string{ex.what()}}; } catch (...) {
		return std::unexpected{std::string{"unknown HttpServer construction error"}};
	}
}

void HttpServer::request_shutdown() noexcept {
	std::uint64_t const v = 1;
	for (int const efd: impl_->shutdown_efds) {
		(void)::write(efd, &v, sizeof(v));
	}
}

void HttpServer::shutdown() {
	request_shutdown();
#if CONFLUX_HAS_HTTP3
	std::unique_ptr<conflux::http::detail::Http3Listener> to_stop;
	{
		std::scoped_lock const lk{impl_->http3_mu};
		to_stop = std::move(impl_->http3_listener);
	}
	if (to_stop) {
		to_stop->stop();
	}
#endif
}

[[nodiscard]] conflux::http::DrainReport HttpServer::drain(
	conflux::http::DrainOptions options) {
	if (impl_ == nullptr) {
		return {};
	}
	if (options.deadline < std::chrono::milliseconds{0}) {
		options.deadline = std::chrono::milliseconds{0};
	}
	auto &control = impl_->drain_control;
	control.options = options;
	control.deadline = std::chrono::steady_clock::now() + options.deadline;
	control.deadline_hit.store(false, std::memory_order_release);
	control.accepted_before_stop.store(0, std::memory_order_release);
	control.idle_closed.store(0, std::memory_order_release);
	control.requests_finished.store(0, std::memory_order_release);
	control.streams_closed.store(0, std::memory_order_release);
	control.forced_closed.store(0, std::memory_order_release);
	control.active.store(true, std::memory_order_release);
	request_shutdown();
	while (impl_->running_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < control.deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
	}
	if (impl_->running_.load(std::memory_order_acquire)) {
		control.deadline_hit.store(true, std::memory_order_release);
	}
	return conflux::http::DrainReport{
		.accepted_before_stop = control.accepted_before_stop.load(std::memory_order_acquire),
		.idle_closed = control.idle_closed.load(std::memory_order_acquire),
		.requests_finished = control.requests_finished.load(std::memory_order_acquire),
		.streams_closed = control.streams_closed.load(std::memory_order_acquire),
		.forced_closed = control.forced_closed.load(std::memory_order_acquire),
		.deadline_hit = control.deadline_hit.load(std::memory_order_acquire),
	};
}

[[nodiscard]] conflux::http::RunStatus HttpServer::run() noexcept {
	try {
		(void)::signal(SIGPIPE, SIG_IGN);
		impl_->running_.store(true, std::memory_order_release);
		unsigned const entries = impl_->cfg.ring_entries == 0 ? DEFAULT_RING_ENTRIES : impl_->cfg.ring_entries;

		std::vector<std::thread> threads;
		threads.reserve(impl_->rings);

		for (unsigned i = 0; i < impl_->rings; ++i) {
			threads.emplace_back([this, i, entries] {
				try {
					auto &r = *impl_->ring_vec[i];
					r.router = impl_->use_vhost ? nullptr : &impl_->router;
					r.vhost_router = impl_->use_vhost ? &impl_->vhost_router : nullptr;
					r.observability_hooks_ = impl_->observability_hooks;
					r.shutdown_efd = impl_->shutdown_efds[i];
					r.drain_control = &impl_->drain_control;
					r.max_body_size = impl_->cfg.max_body_size;
					r.request_timeout_ms = impl_->cfg.request_timeout_ms;
					r.tls_sniff_timeout_ms = impl_->cfg.tls_sniff_timeout_ms;
					r.slow_handler_diagnostics = impl_->cfg.slow_handler_diagnostics;
					r.slow_handler_warn_ms = impl_->cfg.slow_handler_warn_ms;
					r.http_redirect_to_https = impl_->cfg.http_redirect_to_https;
					r.https_redirect_hosts = impl_->cfg.https_redirect_hosts;
					r.parser_limits = impl_->cfg.parser_limits;
					r.file_io_slabs = impl_->cfg.fixed_buffer_slabs;
					r.file_io_slab_bytes = impl_->cfg.fixed_buffer_bytes;
					r.file_io_pipe_pairs = impl_->cfg.splice_pipe_pairs;
					r.send_buffer_slabs = impl_->cfg.send_buffer_slabs;
					r.send_buffer_bytes = impl_->cfg.send_buffer_bytes;
					r.send_fixed_buffers_enabled = impl_->cfg.send_fixed_buffers;
					r.direct_accept_enabled_ = impl_->cfg.direct_accept;
					r.cmd_sock_setsockopt_enabled_ = impl_->cfg.cmd_sock_setsockopt;
					r.startup_banner = impl_->cfg.startup_banner;
#if CONFLUX_HAS_TLS
					r.ssl_ctx = impl_->tls_ctx ? impl_->tls_ctx->native_handle() : nullptr;
// vhost_ctxs on Ring is informational only; SNI callback is already
// registered on the primary SSL_CTX in the constructor.
#endif
					if (i == 0)
						r.port_signal = &impl_->bound_port_;
					int parent = -1;
					if (impl_->cfg.attach_wq && i > 0) {
						impl_->wq_ring_fd_.wait(-2, std::memory_order_acquire);
						parent = impl_->wq_ring_fd_.load(std::memory_order_acquire);
					}
					std::uint32_t const wq_fd = conflux::http::detail::wq_fd_for_ring(impl_->cfg, i, parent);
					r.use_recv_incremental_buf = impl_->cfg.recv_incremental_buf && CONFLUX_ENABLE_RECV_INCREMENTAL_BUF;
					r.use_recv_bundle =
						!r.use_recv_incremental_buf && impl_->cfg.recv_bundle && CONFLUX_ENABLE_RECV_BUNDLE;
					r.init(impl_->cfg.port, entries, impl_->uring_flags, wq_fd, impl_->cfg.no_mmap);
					r.auto_recv_arm_policy = impl_->cfg.auto_recv_arm_policy;
					r.busy_poll_us_ = static_cast<int>(impl_->cfg.busy_poll_us);
					r.prefer_busy_poll_ = impl_->cfg.prefer_busy_poll;
					r.ring_core_ = impl_->cfg.ring_core >= 0 ? impl_->cfg.ring_core + static_cast<int>(i) : -1;
					r.worker_core_ =
						impl_->cfg.worker_core_base >= 0 ? impl_->cfg.worker_core_base + static_cast<int>(i) : -1;
					r.send_zc_threshold_ = impl_->cfg.send_zc_threshold;
					r.send_zc_report_usage_ = impl_->cfg.send_zc_report_usage;
					if (impl_->cfg.send_zc == "on") {
#if !CONFLUX_ENABLE_SEND_ZC
						throw std::runtime_error{"send_zc = on but experimental SEND_ZC is disabled at build time"};
#else
						if (!r.caps.send_zc)
							throw std::runtime_error{"send_zc = on but kernel does not support IORING_OP_SEND_ZC"};
						{
							std::scoped_lock lk{r.metrics_mu_};
							r.send_zc_enabled_ = true;
						}
#endif
					} else if (impl_->cfg.send_zc == "auto") {
						std::scoped_lock lk{r.metrics_mu_};
						r.send_zc_enabled_ = CONFLUX_ENABLE_SEND_ZC && r.caps.send_zc;
					}
					if (impl_->cfg.attach_wq && i == 0) {
						impl_->wq_ring_fd_.store(r.ring.ring_fd, std::memory_order_release);
						impl_->wq_ring_fd_.notify_all();
					}
#if CONFLUX_HAS_HTTP3
					if (impl_->cfg.http3.enabled && !impl_->use_vhost && impl_->http3_tls_ctx)
						r.alt_svc_header = conflux::http::detail::http3_alt_svc_value(
							r.bound_port,
							impl_->cfg.http3.alt_svc_max_age_sec);
#endif

					if (i == 0 && impl_->cfg.startup_banner) {
						auto const feat_s = caps_to_log_string(r.caps);
						eprintln(std::format("uring_features={}", feat_s.empty() ? "none" : feat_s));
						eprintln(
							std::format(
								"uring_setup_flags_requested={}",
								conflux::http::detail::setup_flags_str(r.requested_setup_flags_)));
						eprintln(
							std::format(
								"uring_setup_flags_active={}",
								conflux::http::detail::setup_flags_str(r.active_setup_flags_)));
						eprintln(
							std::format(
								"uring_setup_flags_stripped={}",
								conflux::http::detail::setup_flags_str(r.stripped_setup_flags_)));
					}
					if (i == 0 && impl_->cfg.startup_banner)
						eprintln(
							std::format(
								"listening on {}://0.0.0.0:{}  "
								"(rings={}, entries={}, flags={}, listen_fixed={}, accepted_sockets_direct={}, "
								"buf_ring=true)",
#if CONFLUX_HAS_TLS
								impl_->tls_ctx ? "http/https" : "http",
#else
								"http",
#endif
								r.bound_port,
								impl_->rings,
								entries,
								conflux::http::detail::flags_str(impl_->cfg),
								r.listen_fixed,
								r.accepted_sockets_direct));

					auto const status = r.run_loop();
					if (status != conflux::http::RunStatus::stopped_normally) {
						std::uint8_t expected = static_cast<std::uint8_t>(conflux::http::RunStatus::stopped_normally);
						impl_->run_status_.compare_exchange_strong(
							expected,
							static_cast<std::uint8_t>(status),
							std::memory_order_release,
							std::memory_order_relaxed);
						shutdown();
					}
				} catch (...) {
					{
						std::scoped_lock const lk{impl_->startup_error_mu};
						if (!impl_->startup_error)
							impl_->startup_error = std::current_exception();
					}
					impl_->startup_failed.store(true, std::memory_order_release);
					{
						std::uint8_t expected = static_cast<std::uint8_t>(conflux::http::RunStatus::stopped_normally);
						impl_->run_status_.compare_exchange_strong(
							expected,
							static_cast<std::uint8_t>(conflux::http::RunStatus::fatal_internal_exception),
							std::memory_order_release,
							std::memory_order_relaxed);
					}
					impl_->bound_port_.store(std::numeric_limits<std::uint16_t>::max(), std::memory_order_release);
					impl_->bound_port_.notify_all();
					if (impl_->cfg.attach_wq && i == 0) {
						impl_->wq_ring_fd_.store(-1, std::memory_order_release);
						impl_->wq_ring_fd_.notify_all();
					}
					shutdown();
				}
			});
		}

#if CONFLUX_HAS_HTTP3
		if (impl_->cfg.http3.enabled && impl_->http3_tls_ctx && !impl_->use_vhost) {
			std::uint16_t const h3_port = port();
			auto listener = std::make_unique<conflux::http::detail::Http3Listener>(
				impl_->use_vhost ? nullptr : &impl_->router,
				impl_->cfg.http3,
				h3_port,
				impl_->http3_tls_ctx->native_handle());
			listener->start();
			{
				std::scoped_lock const lk{impl_->http3_mu};
				impl_->http3_listener = std::move(listener);
			}
		}
#endif

		for (auto &t: threads) {
			t.join();
		}
		impl_->running_.store(false, std::memory_order_release);
		impl_->running_.notify_all();
#if CONFLUX_HAS_HTTP3
		std::unique_ptr<conflux::http::detail::Http3Listener> to_reset;
		{
			std::scoped_lock const lk{impl_->http3_mu};
			to_reset = std::move(impl_->http3_listener);
		}
		if (to_reset) {
			to_reset->stop();
		}
#endif
		return static_cast<conflux::http::RunStatus>(impl_->run_status_.load(std::memory_order_acquire));
	} catch (...) {
		if (impl_ != nullptr) {
			impl_->running_.store(false, std::memory_order_release);
			impl_->running_.notify_all();
		}
		return conflux::http::RunStatus::fatal_internal_exception;
	}
}

[[nodiscard]] conflux::http::HttpServerMetrics HttpServer::metrics() const noexcept {
	conflux::http::HttpServerMetrics out{};
	if (impl_ == nullptr) {
		return out;
	}
	for (auto const &ring: impl_->ring_vec) {
		if (ring) {
			add_metrics(out, ring->metrics_snapshot());
		}
	}
	return out;
}

[[nodiscard]] std::string HttpServer::startup_report() const {
	std::string out;
	out += "Build:\n  ";
	out += conflux::build_info_summary();
	out += "\n\nCapabilities:\n";
	if (auto caps = conflux::runtime::detect_capabilities()) {
		auto report = conflux::runtime::capability_report(*caps);
		for (auto const line: LineRange{report}) {
			out += "  ";
			out += line.text;
			out += '\n';
		}
		auto issues = conflux::http::validate_config_capabilities(impl_->cfg, *caps);
		out += "\nFallbacks:\n";
		out += std::format("  policy={}\n", conflux::http::feature_fallback_string(impl_->cfg.feature_fallback));
		if (impl_->cfg.feature_fallback == conflux::runtime::FeatureFallback::silent_fallback && !issues.empty()) {
			out += "  suppressed\n";
		} else if (issues.empty()) {
			out += "  none\n";
		} else {
			for (auto const &issue: issues) {
				out += std::format(
					"  {} {}: {}",
					conflux::runtime::capability_issue_code_string(issue.code),
					issue.feature,
					issue.message);
				if (!issue.hint.empty()) {
					out += std::format(" ({})", issue.hint);
				}
				out += '\n';
			}
		}
	} else {
		out += std::format("  {}: {}\n", caps.error().feature, caps.error().message);
	}
	out += "\nConfig:\n  ";
	out += impl_->cfg.summary_redacted();
	out += '\n';
	if (impl_->cfg.dump_effective_config) {
		out += "  ";
		out += impl_->cfg.to_json_redacted();
		out += '\n';
	}
	return out;
}

void HttpServer::set_observability_hooks(
	conflux::http::HttpServerObservabilityHooks hooks) {
	impl_->observability_hooks = std::move(hooks);
	for (auto const &ring: impl_->ring_vec) {
		if (ring) {
			ring->observability_hooks_ = impl_->observability_hooks;
		}
	}
}

[[nodiscard]] std::uint16_t HttpServer::port() const {
	std::uint16_t p = 0;
	while ((p = impl_->bound_port_.load(std::memory_order_acquire)) == 0) {
		if (impl_->startup_failed.load(std::memory_order_acquire)) {
			std::scoped_lock const lk{impl_->startup_error_mu};
			if (impl_->startup_error) {
				std::rethrow_exception(impl_->startup_error);
			}
			throw std::runtime_error{"HttpServer startup failed"};
		}
		impl_->bound_port_.wait(0, std::memory_order_acquire);
	}
	if (impl_->startup_failed.load(std::memory_order_acquire)) {
		std::scoped_lock const lk{impl_->startup_error_mu};
		if (impl_->startup_error) {
			std::rethrow_exception(impl_->startup_error);
		}
		throw std::runtime_error{"HttpServer startup failed"};
	}
	return p;
}

} // namespace conflux::http
