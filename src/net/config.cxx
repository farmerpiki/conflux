export module conflux.net.config;

import std;
import conflux.types;
import std.compat;
import conflux.utils;
using namespace std;

export constexpr u16 kConfigDefaultPort = 9090;
export constexpr unsigned kConfigDefaultRingEntries = 1024;
export constexpr size_t kConfigDefaultMaxBodySize = size_t{1024} * 1024;
export constexpr u32 kConfigDefaultRequestTimeoutMs = 30000;
export constexpr u32 kConfigDefaultTlsSniffTimeoutMs = 10000;
export constexpr size_t kConfigDefaultMaxRequestLineSize = size_t{8} * 1024;
export constexpr size_t kConfigDefaultMaxHeaderLineSize = size_t{8} * 1024;
export constexpr size_t kConfigDefaultMaxHeaders = 100;
export constexpr size_t kConfigDefaultMaxHeaderBlockSize = size_t{64} * 1024;
export constexpr size_t kConfigDefaultMaxChunks = 100'000;

export struct ParserLimits {
	size_t max_request_line_size = kConfigDefaultMaxRequestLineSize;
	size_t max_header_line_size = kConfigDefaultMaxHeaderLineSize;
	size_t max_headers = kConfigDefaultMaxHeaders;
	size_t max_header_block_size = kConfigDefaultMaxHeaderBlockSize;
	size_t max_chunks = kConfigDefaultMaxChunks;
};

export struct Http3Config {
	bool enabled = false;
	u32 idle_timeout_ms = 30'000;
	size_t max_streams_bidi = 100;
	size_t max_stream_data = size_t{1} * 1024 * 1024;
	size_t max_conn_data = size_t{10} * 1024 * 1024;
	// Alt-Svc max-age advertised on h1/h2 responses when h3 is enabled.
	u32 alt_svc_max_age_sec = 86400;
};

export struct StaticFileCacheConfig {
	bool enabled = false;
	size_t small_file_max_bytes = size_t{64} * 1024;
	size_t max_total_bytes = size_t{16} * 1024 * 1024;
};

// Per-hostname TLS credentials for SNI virtual hosting.
// When the client's TLS ClientHello SNI matches VirtualHost::hostname, the
// server switches to the certificate/key pair from this struct.
export struct VirtualHost {
	string hostname{}; // exact SNI hostname to match
	string cert_file{}; // PEM certificate chain for this host
	string key_file{}; // PEM private key for this host
	StaticFileCacheConfig static_file_cache{}; // optional per-host router default
};

export struct Config {
	u16 port = kConfigDefaultPort;
	unsigned rings = 0; // 0 = hardware_concurrency
	unsigned ring_entries = kConfigDefaultRingEntries; // SQ/CQ depth per ring
	size_t max_body_size = kConfigDefaultMaxBodySize; // max Content-Length before 413
	u32 request_timeout_ms = kConfigDefaultRequestTimeoutMs; // 0 = disabled
	u32 tls_sniff_timeout_ms = kConfigDefaultTlsSniffTimeoutMs; // 0 = disabled
	ParserLimits parser_limits{};
	bool startup_banner = true;

	// TLS (enabled when both cert_file and key_file are non-empty)
	string cert_file{}; // path to PEM certificate chain
	string key_file{}; // path to PEM private key
	// When true, plain HTTP connections receive a 301 redirect to the same URL on https://.
	// Only meaningful when TLS is configured (cert_file + key_file set).
	bool http_redirect_to_https = false;
	// Allowlist of Host header values accepted for the HTTPS redirect.
	// Must include every hostname or IP:port that clients may use (e.g. "example.com",
	// "example.com:8080", "127.0.0.1").  Requests whose Host header is not in this list
	// are rejected with 400 Bad Request instead of being redirected.
	// Required when http_redirect_to_https is true; an empty list rejects all redirects.
	vector<string> https_redirect_hosts{};
	// SNI virtual hosting: each entry provides an alternate cert/key for a hostname.
	// Matched by exact SNI hostname; the primary cert_file/key_file is the default.
	vector<VirtualHost> virtual_hosts{};
	// TLS 1.2 cipher list (OpenSSL SSL_CTX_set_cipher_list format); empty = built-in default.
	string tls_cipher_list{};
	// TLS 1.3 ciphersuites (OpenSSL SSL_CTX_set_ciphersuites format); empty = built-in default.
	string tls_ciphersuites{};

	// HTTP/3: disabled by default. Only meaningful when TLS is configured.
	Http3Config http3{};

	// Static file small-object cache defaults. Routers can copy this into
	// StaticOptions when registering static mounts.
	StaticFileCacheConfig static_file_cache{};

	// file_io pool sizing (per ring). Zero disables the corresponding feature
	// and callers fall back to non-zero-copy paths. Defaults kept small so the
	// common RLIMIT_MEMLOCK (8 MiB on many distros) survives several rings;
	// deployments with raised memlock can bump these for higher throughput.
	size_t fixed_buffer_slabs = 16; // IORING_OP_READ_FIXED slab count
	size_t fixed_buffer_bytes = size_t{16} * 1024; // bytes per slab
	size_t splice_pipe_pairs = 4; // pipe2(O_DIRECT) pairs for splice chains

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
	// Remove the SQ index indirection array (kernel 6.4+). Slightly reduces
	// per-ring memory. Incompatible with SQPOLL.
	bool no_sqarray = false; // IORING_SETUP_NO_SQARRAY
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
	// Enable kernel TLS (kTLS) offload via SSL_OP_ENABLE_KTLS (OpenSSL 3+).
	// When active after handshake, the kernel handles TLS encryption; static file
	// responses use splice_to_fd (zero-copy) instead of read_fixed+SSL_write.
	// Requires CONFIG_TLS=y in the running kernel.
	bool ktls = false;
};

namespace {

string_view strip_inline_comment(
	string_view s) {
	for (size_t i = 1; i < s.size(); ++i) {
		if ((s[i] == '#' || s[i] == ';') && (s[i - 1] == ' ' || s[i - 1] == '\t')) {
			return trim(s.substr(0, i));
		}
	}
	return s;
}

bool parse_bool(
	string_view v,
	string_view key) {
	if (v == "true" || v == "1" || v == "yes") {
		return true;
	}
	if (v == "false" || v == "0" || v == "no") {
		return false;
	}
	throw runtime_error{format("invalid boolean for '{}': '{}'", key, v)};
}

template<typename T>
T parse_uint(
	string_view v,
	string_view key) {
	T result{};
	auto const *end = ranges::next(v.data(), ssize(v));
	auto [ptr, ec] = from_chars(v.data(), end, result);
	if (ec != errc{} || ptr != end) {
		throw runtime_error{format("invalid integer for '{}': '{}'", key, v)};
	}
	return result;
}

void apply_server_key(
	Config &cfg,
	string_view key,
	string_view val) {
	static constexpr array<pair<string_view, unsigned Config::*>, 2> kUnsignedKeys{
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
	static constexpr array<pair<string_view, string Config::*>, 2> kStringKeys{
		{
         {"cert_file", &Config::cert_file},
         {"key_file", &Config::key_file},
		 }
    };
	for (auto const &[k, member]: kStringKeys) {
		if (key == k) {
			cfg.*member = string{val};
			return;
		}
	}
	if (key == "port") {
		cfg.port = parse_uint<u16>(val, key);
	} else if (key == "max_body_size") {
		cfg.max_body_size = parse_uint<size_t>(val, key);
	} else if (key == "request_timeout_ms") {
		cfg.request_timeout_ms = parse_uint<u32>(val, key);
	} else if (key == "tls_sniff_timeout_ms") {
		cfg.tls_sniff_timeout_ms = parse_uint<u32>(val, key);
	} else if (key == "fixed_buffer_slabs") {
		cfg.fixed_buffer_slabs = parse_uint<size_t>(val, key);
	} else if (key == "fixed_buffer_bytes") {
		cfg.fixed_buffer_bytes = parse_uint<size_t>(val, key);
	} else if (key == "splice_pipe_pairs") {
		cfg.splice_pipe_pairs = parse_uint<size_t>(val, key);
	} else if (key == "startup_banner") {
		cfg.startup_banner = parse_bool(val, key);
	} else if (key == "http_redirect_to_https") {
		cfg.http_redirect_to_https = parse_bool(val, key);
	}
}

void apply_http3_key(
	Config &cfg,
	string_view key,
	string_view val) {
	if (key == "enabled") {
		cfg.http3.enabled = parse_bool(val, key);
	} else if (key == "idle_timeout_ms") {
		cfg.http3.idle_timeout_ms = parse_uint<u32>(val, key);
	} else if (key == "max_streams_bidi") {
		cfg.http3.max_streams_bidi = parse_uint<size_t>(val, key);
	} else if (key == "max_stream_data") {
		cfg.http3.max_stream_data = parse_uint<size_t>(val, key);
	} else if (key == "max_conn_data") {
		cfg.http3.max_conn_data = parse_uint<size_t>(val, key);
	} else if (key == "alt_svc_max_age_sec") {
		cfg.http3.alt_svc_max_age_sec = parse_uint<u32>(val, key);
	}
}

void apply_static_cache_key(
	Config &cfg,
	string_view key,
	string_view val) {
	if (key == "enabled") {
		cfg.static_file_cache.enabled = parse_bool(val, key);
	} else if (key == "small_file_max_bytes") {
		cfg.static_file_cache.small_file_max_bytes = parse_uint<size_t>(val, key);
	} else if (key == "max_total_bytes") {
		cfg.static_file_cache.max_total_bytes = parse_uint<size_t>(val, key);
	}
}

void apply_tls_key(
	Config &cfg,
	string_view key,
	string_view val) {
	static constexpr array<pair<string_view, string Config::*>, 2> kStringKeys{
		{
         {"cipher_list", &Config::tls_cipher_list},
         {"ciphersuites", &Config::tls_ciphersuites},
		 }
    };
	for (auto const &[k, member]: kStringKeys) {
		if (key == k) {
			cfg.*member = string{val};
			return;
		}
	}
}

void apply_iouring_key(
	Config &cfg,
	string_view key,
	string_view val) {
	static constexpr array<pair<string_view, bool Config::*>, 12> kBoolKeys{
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
         {"ktls", &Config::ktls},
		 }
    };
	for (auto const &[k, member]: kBoolKeys) {
		if (key == k) {
			cfg.*member = parse_bool(val, key);
			return;
		}
	}
}

} // namespace

// Throws runtime_error on parse / IO failure.
export Config config_from_ini(
	char const *path) {
	ifstream file{path};
	if (!file) {
		throw runtime_error{format("cannot open config: {}", path)};
	}

	Config cfg{};
	string section;
	string line;

	while (getline(file, line)) {
		auto s = trim(string_view{line});
		if (s.empty() || s[0] == '#' || s[0] == ';') {
			continue;
		}

		if (s[0] == '[') {
			auto close = s.find(']', 1);
			if (close != string_view::npos) {
				section = string{trim(s.substr(1, close - 1))};
			}
			continue;
		}

		auto eq = s.find('=');
		if (eq == string_view::npos) {
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
