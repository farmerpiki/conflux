export module conflux.net.config;

import std;
import conflux.types;
import std.compat;
import conflux.utils;

export constexpr u16 kConfigDefaultPort = 9090;
export constexpr unsigned kConfigDefaultRingEntries = 1024;
export constexpr SZ kConfigDefaultMaxBodySize = SZ{1024} * 1024;
export constexpr u32 kConfigDefaultRequestTimeoutMs = 30000;
export constexpr u32 kConfigDefaultTlsSniffTimeoutMs = 10000;
export constexpr SZ kConfigDefaultMaxRequestLineSize = SZ{8} * 1024;
export constexpr SZ kConfigDefaultMaxHeaderLineSize = SZ{8} * 1024;
export constexpr SZ kConfigDefaultMaxHeaders = 100;
export constexpr SZ kConfigDefaultMaxHeaderBlockSize = SZ{64} * 1024;
export constexpr SZ kConfigDefaultMaxChunks = 100000;
export struct ParserLimits {
	SZ max_request_line_size = kConfigDefaultMaxRequestLineSize;
	SZ max_header_line_size = kConfigDefaultMaxHeaderLineSize;
	SZ max_headers = kConfigDefaultMaxHeaders;
	SZ max_header_block_size = kConfigDefaultMaxHeaderBlockSize;
	SZ max_chunks = kConfigDefaultMaxChunks;
};
export struct Http3Config {
	bool enabled = false;
	u32 idle_timeout_ms = 30000;
	SZ max_streams_bidi = 100;
	SZ max_stream_data = SZ{1} * 1024 * 1024;
	SZ max_conn_data = SZ{10} * 1024 * 1024;
	// Alt-Svc max-age advertised on h1/h2 responses when h3 is enabled.
	u32 alt_svc_max_age_sec = 86400;
	// Per-request body cap; matches H1 max_body_size semantics.
	// Streams whose DATA exceeds this are reset with H3_REQUEST_REJECTED.
	size_t max_body_size = kConfigDefaultMaxBodySize;
};
export struct StaticFileCacheConfig {
	bool enabled = false;
	SZ small_file_max_bytes = SZ{64} * 1024;
	SZ max_total_bytes = SZ{16} * 1024 * 1024;
};
// Per-hostname TLS credentials for SNI virtual hosting.
// When the client's TLS ClientHello SNI matches VirtualHost::hostname case-insensitively, the
// server switches to the certificate/key P from this struct.
export struct VirtualHost {
	S hostname{}; // SNI hostname to match
	S cert_file{}; // PEM certificate chain for this host
	S key_file{}; // PEM private key for this host
	StaticFileCacheConfig static_file_cache{}; // Opt per-host router default
};
export struct Config {
	[[nodiscard]] static Config low_latency() {
		Config cfg{};
		cfg.rings = 1;
		cfg.ring_entries = 256;
		cfg.single_issuer = true;
		cfg.defer_taskrun = true;
		cfg.coop_taskrun = true;
		cfg.taskrun_flag = true;
		return cfg;
	}
	[[nodiscard]] static Config test() {
		auto cfg = low_latency();
		cfg.port = 0;
		cfg.startup_banner = false;
		return cfg;
	}
	u16 port = kConfigDefaultPort;
	unsigned rings = 0; // 0 = hardware_concurrency
	unsigned ring_entries = kConfigDefaultRingEntries; // SQ/CQ depth per ring
	SZ max_body_size = kConfigDefaultMaxBodySize; // max Content-Length before 413
	u32 request_timeout_ms = kConfigDefaultRequestTimeoutMs; // 0 = disabled
	u32 tls_sniff_timeout_ms = kConfigDefaultTlsSniffTimeoutMs; // 0 = disabled
	// Emit a warning when a synchronous handler blocks on the ring thread past
	// slow_handler_warn_ms. Disabled by default to keep baseline overhead minimal.
	bool slow_handler_diagnostics = false;
	u32 slow_handler_warn_ms = 25;
	ParserLimits parser_limits{};
	bool startup_banner = true;

	// TLS (enabled when both cert_file and key_file are non-empty)
	S cert_file{}; // path to PEM certificate chain
	S key_file{}; // path to PEM private key
	// When true, plain HTTP connections receive a 301 redirect to the same URL on https://.
	// Only meaningful when TLS is configured (cert_file + key_file set).
	bool http_redirect_to_https = false;
	// Allowlist of Host header values accepted for the HTTPS redirect.
	// Must include every hostname or IP:port that clients may use (e.g. "example.com",
	// "example.com:8080", "127.0.0.1").  Requests whose Host header is not in this list
	// are rejected with 400 Bad Request instead of being redirected.
	// Required when http_redirect_to_https is true; an empty list rejects all redirects.
	V<S> https_redirect_hosts{};
	// SNI virtual hosting: each entry provides an alternate cert/key for a hostname.
	// Matched by case-insensitive SNI hostname; the primary cert_file/key_file is the default.
	V<VirtualHost> virtual_hosts{};
	// TLS 1.2 cipher list (OpenSSL SSL_CTX_set_cipher_list format); empty = built-in default.
	S tls_cipher_list{};
	// TLS 1.3 ciphersuites (OpenSSL SSL_CTX_set_ciphersuites format); empty = built-in default.
	S tls_ciphersuites{};

	// HTTP/3: disabled by default. Only meaningful when TLS is configured.
	Http3Config http3{};

	// Static file small-object cache defaults. Routers can copy this into
	// StaticOptions when registering static mounts.
	StaticFileCacheConfig static_file_cache{};

	// file_io pool sizing (per ring). Zero disables the corresponding feature
	// and callers fall back to non-zero-copy paths. Defaults kept small so the
	// common RLIMIT_MEMLOCK (8 MiB on many distros) survives several rings;
	// deployments with raised memlock can bump these for higher throughput.
	SZ fixed_buffer_slabs = 16; // IORING_OP_READ_FIXED slab count
	SZ fixed_buffer_bytes = SZ{16} * 1024; // bytes per slab
	SZ splice_pipe_pairs = 4; // pipe2(O_DIRECT) pairs for splice chains
	SZ send_buffer_slabs = 64; // registered send-buffer slab count
	SZ send_buffer_bytes = SZ{4} * 1024; // bytes per send slab
	bool send_fixed_buffers = false; // enable registered send buffers

	// io_uring setup flags
	bool single_issuer = true; // IORING_SETUP_SINGLE_ISSUER
	bool defer_taskrun = true; // IORING_SETUP_DEFER_TASKRUN
	bool sqpoll = false; // IORING_SETUP_SQPOLL
	bool coop_taskrun = true; // IORING_SETUP_COOP_TASKRUN
	bool taskrun_flag = true; // IORING_SETUP_TASKRUN_FLAG
	bool submit_all = false; // IORING_SETUP_SUBMIT_ALL
	// When true and rings > 1, ring[1..N] attach to ring[0]'s kernel io-wq via
	// IORING_SETUP_ATTACH_WQ. Reduces kernel thread overhead on high-ring-count setups.
	bool attach_wq = false; // IORING_SETUP_ATTACH_WQ
	// Remove the SQ index indirection A (kernel 6.4+). Slightly reduces
	// per-ring memory. Incompatible with SQPOLL.
	bool no_sqarray = true; // IORING_SETUP_NO_SQARRAY
	// Allow kernel to mix 16-byte and 32-byte CQEs on the same ring (kernel 6.5+).
	// CQEs that carry extra payload set IORING_CQE_F_32 in flags.
	bool cqe_mixed = false; // IORING_SETUP_CQE_MIXED
	// Application allocates ring memory; avoids kernel mmap overhead (kernel 6.5+).
	// Ring thread allocates a page-aligned buffer sized by io_uring_mlock_size;
	// uses io_uring_queue_init_mem instead of io_uring_queue_init_params.
	bool no_mmap = false; // IORING_SETUP_NO_MMAP
	// Multishot recv grabs multiple provided buffers per CQE (kernel 6.0+).
	// Only activated when IORING_FEAT_RECVSEND_BUNDLE is present in ring features.
	// Reduces per-CQE overhead for large recv bursts.
	bool recv_bundle = false; // IORING_RECVSEND_BUNDLE
	bool recv_incremental_buf = false; // IOU_PBUF_RING_INC (kernel 6.10+)
	bool auto_recv_arm_policy = false; // adaptive poll_first via IORING_CQE_F_SOCK_NONEMPTY hint
	// Enable kernel TLS (kTLS) offload via SSL_OP_ENABLE_KTLS (OpenSSL 3+).
	// When active after handshake, the kernel handles TLS encryption; static file
	// responses use splice_to_fd (zero-copy) instead of read_fixed+SSL_write.
	// Requires CONFIG_TLS=y in the running kernel.
	bool ktls = false;
	// Busy-poll per accepted socket. 0 = disabled. Both require kernel 4.5+.
	u32 busy_poll_us = 0; // SO_BUSY_POLL microseconds per socket
	bool prefer_busy_poll = false; // SO_PREFER_BUSY_POLL
	// Thread / io-wq core pinning. -1 = disabled (default).
	// ring_core: base CPU for sched_setaffinity on ring threads (ring i → ring_core+i).
	// worker_core_base: base CPU for IORING_REGISTER_IOWQ_AFF (ring i → worker_core_base+i).
	int ring_core = -1;
	int worker_core_base = -1;

	// SEND_ZC: zero-copy send for HTTP responses (kernel 6.0+)
	// off = never; auto = use if caps.send_zc, disable on repeated copies;
	// on = require at startup, fail if unsupported.
	S send_zc{"auto"};
	SZ send_zc_threshold = 16384;
	bool send_zc_report_usage = true;
};
namespace {

SV strip_inline_comment(
	SV s) {
	for (SZ i = 1; i < s.size(); ++i) {
		if ((s[i] == '#' || s[i] == ';') && (s[i - 1] == ' ' || s[i - 1] == '\t')) {
			return trim(s.substr(0, i));
		}
	}
	return s;
}
bool parse_bool(
	SV v,
	SV key) {
	if (v == "true" || v == "1" || v == "yes") {
		return true;
	}
	if (v == "false" || v == "0" || v == "no") {
		return false;
	}
	throw RE{format("invalid boolean for '{}': '{}'", key, v)};
}
template<typename T>
T parse_uint(
	SV v,
	SV key) {
	T result{};
	auto const *end = ranges::next(v.data(), ssize(v));
	auto [ptr, ec] = from_chars(v.data(), end, result);
	if (ec != errc{} || ptr != end) {
		throw RE{format("invalid integer for '{}': '{}'", key, v)};
	}
	return result;
}
int parse_int(
	SV v,
	SV key) {
	int result{};
	auto const *end = ranges::next(v.data(), ssize(v));
	auto [ptr, ec] = from_chars(v.data(), end, result);
	if (ec != errc{} || ptr != end) {
		throw RE{format("invalid integer for '{}': '{}'", key, v)};
	}
	return result;
}
void apply_server_key(
	Config &cfg,
	SV key,
	SV val) {
	static constexpr A<P<SV, unsigned Config::*>, 2> kUnsignedKeys{
		{
         {"rings", &Config::rings},
         {"ring_entries", &Config::ring_entries},
		 }
    };
	for (auto const &[k, member]: kUnsignedKeys) {
		if (key == k) {
			cfg.*member = parse_uint<unsigned>(val, key);
			return;
		}
	}
	static constexpr A<P<SV, S Config::*>, 2> kStringKeys{
		{
         {"cert_file", &Config::cert_file},
         {"key_file", &Config::key_file},
		 }
    };
	for (auto const &[k, member]: kStringKeys) {
		if (key == k) {
			cfg.*member = S{val};
			return;
		}
	}
	if (key == "port") {
		cfg.port = parse_uint<u16>(val, key);
	} else if (key == "max_body_size") {
		cfg.max_body_size = parse_uint<SZ>(val, key);
	} else if (key == "request_timeout_ms") {
		cfg.request_timeout_ms = parse_uint<u32>(val, key);
	} else if (key == "tls_sniff_timeout_ms") {
		cfg.tls_sniff_timeout_ms = parse_uint<u32>(val, key);
	} else if (key == "slow_handler_diagnostics") {
		cfg.slow_handler_diagnostics = parse_bool(val, key);
	} else if (key == "slow_handler_warn_ms") {
		cfg.slow_handler_warn_ms = parse_uint<u32>(val, key);
	} else if (key == "fixed_buffer_slabs") {
		cfg.fixed_buffer_slabs = parse_uint<SZ>(val, key);
	} else if (key == "fixed_buffer_bytes") {
		cfg.fixed_buffer_bytes = parse_uint<SZ>(val, key);
	} else if (key == "splice_pipe_pairs") {
		cfg.splice_pipe_pairs = parse_uint<SZ>(val, key);
	} else if (key == "send_buffer_slabs") {
		cfg.send_buffer_slabs = parse_uint<SZ>(val, key);
	} else if (key == "send_buffer_bytes") {
		cfg.send_buffer_bytes = parse_uint<SZ>(val, key);
	} else if (key == "send_fixed_buffers") {
		cfg.send_fixed_buffers = parse_bool(val, key);
	} else if (key == "busy_poll_us") {
		cfg.busy_poll_us = parse_uint<u32>(val, key);
	} else if (key == "ring_core") {
		cfg.ring_core = parse_int(val, key);
	} else if (key == "worker_core_base") {
		cfg.worker_core_base = parse_int(val, key);
	} else if (key == "startup_banner") {
		cfg.startup_banner = parse_bool(val, key);
	} else if (key == "http_redirect_to_https") {
		cfg.http_redirect_to_https = parse_bool(val, key);
	}
}
void apply_http3_key(
	Config &cfg,
	SV key,
	SV val) {
	if (key == "enabled") {
		cfg.http3.enabled = parse_bool(val, key);
	} else if (key == "idle_timeout_ms") {
		cfg.http3.idle_timeout_ms = parse_uint<u32>(val, key);
	} else if (key == "max_streams_bidi") {
		cfg.http3.max_streams_bidi = parse_uint<SZ>(val, key);
	} else if (key == "max_stream_data") {
		cfg.http3.max_stream_data = parse_uint<SZ>(val, key);
	} else if (key == "max_conn_data") {
		cfg.http3.max_conn_data = parse_uint<SZ>(val, key);
	} else if (key == "alt_svc_max_age_sec") {
		cfg.http3.alt_svc_max_age_sec = parse_uint<u32>(val, key);
	}
}
void apply_static_cache_key(
	Config &cfg,
	SV key,
	SV val) {
	if (key == "enabled") {
		cfg.static_file_cache.enabled = parse_bool(val, key);
	} else if (key == "small_file_max_bytes") {
		cfg.static_file_cache.small_file_max_bytes = parse_uint<SZ>(val, key);
	} else if (key == "max_total_bytes") {
		cfg.static_file_cache.max_total_bytes = parse_uint<SZ>(val, key);
	}
}
void apply_tls_key(
	Config &cfg,
	SV key,
	SV val) {
	static constexpr A<P<SV, S Config::*>, 2> kStringKeys{
		{
         {"cipher_list", &Config::tls_cipher_list},
         {"ciphersuites", &Config::tls_ciphersuites},
		 }
    };
	for (auto const &[k, member]: kStringKeys) {
		if (key == k) {
			cfg.*member = S{val};
			return;
		}
	}
}
void apply_iouring_key(
	Config &cfg,
	SV key,
	SV val) {
	static constexpr A<P<SV, bool Config::*>, 15> kBoolKeys{
		{
         {"single_issuer", &Config::single_issuer},
         {"defer_taskrun", &Config::defer_taskrun},
         {"sqpoll", &Config::sqpoll},
         {"coop_taskrun", &Config::coop_taskrun},
         {"taskrun_flag", &Config::taskrun_flag},
         {"submit_all", &Config::submit_all},
         {"attach_wq", &Config::attach_wq},
         {"no_sqarray", &Config::no_sqarray},
         {"cqe_mixed", &Config::cqe_mixed},
         {"no_mmap", &Config::no_mmap},
         {"recv_bundle", &Config::recv_bundle},
         {"recv_incremental_buf", &Config::recv_incremental_buf},
         {"ktls", &Config::ktls},
         {"auto_recv_arm_policy", &Config::auto_recv_arm_policy},
         {"prefer_busy_poll", &Config::prefer_busy_poll},
		 }
    };
	for (auto const &[k, member]: kBoolKeys) {
		if (key == k) {
			cfg.*member = parse_bool(val, key);
			return;
		}
	}
	if (key == "send_zc") {
		if (val == "off" || val == "auto" || val == "on") {
			cfg.send_zc = val;
		}
		return;
	}
	if (key == "send_zc_threshold") {
		cfg.send_zc_threshold = parse_uint<SZ>(val, key);
		return;
	}
	if (key == "send_zc_report_usage") {
		cfg.send_zc_report_usage = parse_bool(val, key);
		return;
	}
}

} // namespace
// Throws RE on parse / IO failure.
export Config config_from_ini(
	char const *path) {
	std::ifstream file{path};
	if (!file) {
		throw RE{format("cannot open config: {}", path)};
	}

	Config cfg{};
	S section;
	S line;

	while (std::getline(file, line)) {
		auto s = trim(SV{line});
		if (s.empty() || s[0] == '#' || s[0] == ';') {
			continue;
		}

		if (s[0] == '[') {
			auto close = s.find(']', 1);
			if (close != SV::npos) {
				section = S{trim(s.substr(1, close - 1))};
			}
			continue;
		}

		auto eq = s.find('=');
		if (eq == SV::npos) {
			continue;
		}

		auto key = trim(s.substr(0, eq));
		auto val = trim(strip_inline_comment(trim(s.substr(eq + 1))));

		if (section == "server") {
			apply_server_key(cfg, key, val);
		} else if (section == "io_uring") {
			apply_iouring_key(cfg, key, val);
		} else if (section == "tls") {
			apply_tls_key(cfg, key, val);
		} else if (section == "http3") {
			apply_http3_key(cfg, key, val);
		} else if (section == "static_cache") {
			apply_static_cache_key(cfg, key, val);
		}
		// unknown sections/keys silently ignored — forward-compatible
	}

	return cfg;
}
