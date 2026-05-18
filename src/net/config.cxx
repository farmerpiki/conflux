module;
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

export module conflux.net.config;

import std;
import conflux.types;
import std.compat;
import conflux.utils;

export constexpr std::uint16_t kConfigDefaultPort = 9090;
export constexpr unsigned kConfigDefaultRingEntries = 1024;
export constexpr std::size_t kConfigDefaultMaxBodySize = std::size_t{1024} * 1024;
export constexpr std::uint32_t kConfigDefaultRequestTimeoutMs = 30000;
export constexpr std::uint32_t kConfigDefaultTlsSniffTimeoutMs = 10000;
export constexpr std::size_t kConfigDefaultMaxRequestLineSize = std::size_t{8} * 1024;
export constexpr std::size_t kConfigDefaultMaxHeaderLineSize = std::size_t{8} * 1024;
export constexpr std::size_t kConfigDefaultMaxHeaders = 100;
export constexpr std::size_t kConfigDefaultMaxHeaderBlockSize = std::size_t{64} * 1024;
export constexpr std::size_t kConfigDefaultMaxChunks = 100000;
export struct ParserLimits {
	std::size_t max_request_line_size = kConfigDefaultMaxRequestLineSize;
	std::size_t max_header_line_size = kConfigDefaultMaxHeaderLineSize;
	std::size_t max_headers = kConfigDefaultMaxHeaders;
	std::size_t max_header_block_size = kConfigDefaultMaxHeaderBlockSize;
	std::size_t max_chunks = kConfigDefaultMaxChunks;
};
export struct Http3Config {
	bool enabled = false;
	std::uint32_t idle_timeout_ms = 30000;
	std::size_t max_streams_bidi = 100;
	std::size_t max_stream_data = std::size_t{1} * 1024 * 1024;
	std::size_t max_conn_data = std::size_t{10} * 1024 * 1024;
	// Alt-Svc max-age advertised on h1/h2 responses when h3 is enabled.
	std::uint32_t alt_svc_max_age_sec = 86400;
	// Per-request body cap; matches H1 max_body_size semantics.
	// Streams whose DATA exceeds this are reset with H3_REQUEST_REJECTED.
	size_t max_body_size = kConfigDefaultMaxBodySize;
};
export struct StaticFileCacheConfig {
	bool enabled = false;
	std::size_t small_file_max_bytes = std::size_t{64} * 1024;
	std::size_t max_total_bytes = std::size_t{16} * 1024 * 1024;
};

export enum class SecretSourceKind {
	unset,
	literal,
	environment,
	file,
};
export struct SecretSource {
	SecretSourceKind kind{SecretSourceKind::unset};
	std::string value{};
};
export struct SecretRotationConfig {
	SecretSource active{};
	std::vector<SecretSource> previous{};
	std::size_t min_secret_bytes{16};
};
export struct AuthSecretsConfig {
	SecretSource password_verifier_secret{};
	std::size_t password_verifier_min_secret_bytes{16};
	SecretRotationConfig jwt{};
	SecretRotationConfig cookie{};
	SecretRotationConfig session{};
};
export struct ResolvedSecretRotation {
	std::string active{};
	std::vector<std::string> previous{};
	std::size_t min_secret_bytes{16};
};
// Per-hostname TLS credentials for SNI virtual hosting.
// When the client's TLS ClientHello SNI matches VirtualHost::hostname case-insensitively, the
// server switches to the certificate/key P from this struct.
export struct VirtualHost {
	std::string hostname{}; // SNI hostname to match
	std::string cert_file{}; // PEM certificate chain for this host
	std::string key_file{}; // PEM private key for this host
	StaticFileCacheConfig static_file_cache{}; // Opt per-host router default
};
export struct Config {
	[[nodiscard]] static Config low_latency() {
		Config cfg{};
		cfg.rings = 4;
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
	std::uint16_t port = kConfigDefaultPort;
	unsigned rings = 0; // 0 = hardware_concurrency
	unsigned ring_entries = kConfigDefaultRingEntries; // SQ/CQ depth per ring
	std::size_t max_body_size = kConfigDefaultMaxBodySize; // max Content-Length before 413
	std::uint32_t request_timeout_ms = kConfigDefaultRequestTimeoutMs; // 0 = disabled
	std::uint32_t tls_sniff_timeout_ms = kConfigDefaultTlsSniffTimeoutMs; // 0 = disabled
	// Emit a warning when a synchronous handler blocks on the ring thread past
	// slow_handler_warn_ms. Disabled by default to keep baseline overhead minimal.
	bool slow_handler_diagnostics = false;
	std::uint32_t slow_handler_warn_ms = 25;
	ParserLimits parser_limits{};
	bool startup_banner = true;

	// TLS (enabled when both cert_file and key_file are non-empty)
	std::string cert_file{}; // path to PEM certificate chain
	std::string key_file{}; // path to PEM private key
	// When true, plain HTTP connections receive a 301 redirect to the same URL on https://.
	// Only meaningful when TLS is configured (cert_file + key_file set).
	bool http_redirect_to_https = false;
	// Allowlist of Host header values accepted for the HTTPS redirect.
	// Must include every hostname or IP:port that clients may use (e.g. "example.com",
	// "example.com:8080", "127.0.0.1").  Requests whose Host header is not in this list
	// are rejected with 400 Bad Request instead of being redirected.
	// Required when http_redirect_to_https is true; an empty list rejects all redirects.
	std::vector<std::string> https_redirect_hosts{};
	// SNI virtual hosting: each entry provides an alternate cert/key for a hostname.
	// Matched by case-insensitive SNI hostname; the primary cert_file/key_file is the default.
	std::vector<VirtualHost> virtual_hosts{};
	// TLS 1.2 cipher list (OpenSSL SSL_CTX_set_cipher_list format); empty = built-in default.
	std::string tls_cipher_list{};
	// TLS 1.3 ciphersuites (OpenSSL SSL_CTX_set_ciphersuites format); empty = built-in default.
	std::string tls_ciphersuites{};

	// HTTP/3: disabled by default. Only meaningful when TLS is configured.
	Http3Config http3{};

	// Static file small-object cache defaults. Routers can copy this into
	// StaticOptions when registering static mounts.
	StaticFileCacheConfig static_file_cache{};

	// Authentication secret sources. Empty by default: production auth helpers
	// return explicit missing-secret errors until deployments configure env/file/literal sources.
	AuthSecretsConfig auth_secrets{};

	// file_io pool sizing (per ring). Zero disables the corresponding feature
	// and callers fall back to non-zero-copy paths. Defaults kept small so the
	// common RLIMIT_MEMLOCK (8 MiB on many distros) survives several rings;
	// deployments with raised memlock can bump these for higher throughput.
	std::size_t fixed_buffer_slabs = 16; // IORING_OP_READ_FIXED slab count
	std::size_t fixed_buffer_bytes = std::size_t{16} * 1024; // bytes per slab
	std::size_t splice_pipe_pairs = 4; // pipe2(O_DIRECT) pairs for splice chains
	std::size_t send_buffer_slabs = 64; // registered send-buffer slab count
	std::size_t send_buffer_bytes = std::size_t{4} * 1024; // bytes per send slab
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
	// Diagnostic/perf isolation knobs. Defaults preserve the current fast path.
	// They make external compatibility failures bisectable without rebuilding.
	bool direct_accept = true;
	bool cmd_sock_setsockopt = true;
	// Enable kernel TLS (kTLS) offload via SSL_OP_ENABLE_KTLS (OpenSSL 3+).
	// When active after handshake, the kernel handles TLS encryption; static file
	// responses use splice_to_fd (zero-copy) instead of read_fixed+SSL_write.
	// Requires CONFIG_TLS=y in the running kernel.
	bool ktls = false;
	// Busy-poll per accepted socket. 0 = disabled. Both require kernel 4.5+.
	std::uint32_t busy_poll_us = 0; // SO_BUSY_POLL microseconds per socket
	bool prefer_busy_poll = false; // SO_PREFER_BUSY_POLL
	// Thread / io-wq core pinning. -1 = disabled (default).
	// ring_core: base CPU for sched_setaffinity on ring threads (ring i → ring_core+i).
	// worker_core_base: base CPU for IORING_REGISTER_IOWQ_AFF (ring i → worker_core_base+i).
	int ring_core = -1;
	int worker_core_base = -1;

	// SEND_ZC: zero-copy send for HTTP responses (kernel 6.0+)
	// off = never; auto = use if caps.send_zc, disable on repeated copies;
	// on = require at startup, fail if unsupported.
	std::string send_zc{"auto"};
	std::size_t send_zc_threshold = 16384;
	bool send_zc_report_usage = true;
};

export [[nodiscard]] ResolvedSecretRotation single_secret_rotation(
	std::string_view active,
	std::size_t min_secret_bytes = 16) {
	return {.active = std::string{active}, .previous = {}, .min_secret_bytes = min_secret_bytes};
}

export [[nodiscard]] bool secret_source_configured(
	SecretSource const &src) noexcept {
	return src.kind != SecretSourceKind::unset;
}

namespace {

expected<std::string, int>
read_text_file_local(std::string_view path, std::size_t max_bytes = std::size_t{16} * 1024 * 1024);

} // namespace

export [[nodiscard]] expected<std::string, std::string> resolve_secret_source(
	SecretSource const &src,
	std::string_view name,
	bool required = true) {
	if (src.kind == SecretSourceKind::unset) {
		if (required) {
			return unexpected{format("auth secret '{}': missing required source", name)};
		}
		return std::string{};
	}
	if (src.value.empty()) {
		return unexpected{format("auth secret '{}': empty source value", name)};
	}
	if (src.kind == SecretSourceKind::literal) {
		return src.value;
	}
	if (src.kind == SecretSourceKind::environment) {
		auto const *value = std::getenv(src.value.c_str());
		if (value == nullptr || value[0] == '\0') {
			return unexpected{format("auth secret '{}': environment variable '{}' is unset or empty", name, src.value)};
		}
		return std::string{value};
	}
	if (src.kind == SecretSourceKind::file) {
		auto bytes = read_text_file_local(src.value, std::size_t{1024} * 1024);
		if (!bytes) {
			return unexpected{format("auth secret '{}': cannot open secret file '{}'", name, src.value)};
		}
		std::string value{move(*bytes)};
		while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
			value.pop_back();
		}
		if (value.empty()) {
			return unexpected{format("auth secret '{}': secret file '{}' is empty", name, src.value)};
		}
		return value;
	}
	return unexpected{format("auth secret '{}': unsupported source kind", name)};
}

export [[nodiscard]] expected<void, std::string> validate_secret_bytes(
	std::string_view secret,
	std::string_view name,
	std::size_t min_bytes) {
	if (secret.empty()) {
		return unexpected{format("auth secret '{}': resolved secret is empty", name)};
	}
	if (secret.size() < min_bytes) {
		return unexpected{format("auth secret '{}': resolved secret must be at least {} bytes", name, min_bytes)};
	}
	return {};
}

export [[nodiscard]] expected<ResolvedSecretRotation, std::string> resolve_secret_rotation(
	SecretRotationConfig const &cfg,
	std::string_view name,
	bool required = true) {
	ResolvedSecretRotation out{.min_secret_bytes = cfg.min_secret_bytes};
	auto active = resolve_secret_source(cfg.active, name, required);
	if (!active) {
		return unexpected{active.error()};
	}
	out.active = move(*active);
	if (!out.active.empty()) {
		if (auto valid = validate_secret_bytes(out.active, name, cfg.min_secret_bytes); !valid) {
			return unexpected{valid.error()};
		}
	}
	for (std::size_t i = 0; i < cfg.previous.size(); ++i) {
		auto previous = resolve_secret_source(cfg.previous[i], format("{}.previous[{}]", name, i), true);
		if (!previous) {
			return unexpected{previous.error()};
		}
		if (auto valid = validate_secret_bytes(*previous, format("{}.previous[{}]", name, i), cfg.min_secret_bytes);
			!valid) {
			return unexpected{valid.error()};
		}
		out.previous.push_back(move(*previous));
	}
	return out;
}

namespace {

struct LocalFd {
	int fd{-1};
	LocalFd() noexcept = default;
	explicit LocalFd(
		int f) noexcept
		: fd{f} {}
	LocalFd(LocalFd const &) = delete;
	LocalFd &operator =(LocalFd const &) = delete;
	LocalFd(
		LocalFd &&o) noexcept
		: fd{exchange(o.fd, -1)} {}
	LocalFd &operator =(LocalFd &&) = delete;
	~LocalFd() {
		if (fd >= 0) {
			::close(fd);
		}
	}
};
expected<std::string, int> read_text_file_local(
	std::string_view path,
	std::size_t max_bytes) {
	std::string native{path};
	LocalFd file{::open(native.c_str(), O_RDONLY | O_CLOEXEC)};
	if (file.fd < 0) {
		return unexpected{errno};
	}
	std::string out;
	std::array<char, 16 * 1024> buf{};
	for (;;) {
		auto const n = ::read(file.fd, buf.data(), buf.size());
		if (n == 0) {
			return out;
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return unexpected{errno};
		}
		auto const count = static_cast<std::size_t>(n);
		if (count > max_bytes - out.size()) {
			return unexpected{EFBIG};
		}
		out.append(buf.data(), count);
	}
}

std::string_view strip_inline_comment(
	std::string_view s) {
	for (std::size_t i = 1; i < s.size(); ++i) {
		if ((s[i] == '#' || s[i] == ';') && (s[i - 1] == ' ' || s[i - 1] == '\t')) {
			return trim(s.substr(0, i));
		}
	}
	return s;
}
bool parse_bool(
	std::string_view v,
	std::string_view key) {
	if (v == "true" || v == "1" || v == "yes") {
		return true;
	}
	if (v == "false" || v == "0" || v == "no") {
		return false;
	}
	throw std::runtime_error{format("invalid boolean for '{}': '{}'", key, v)};
}
template<typename T>
T parse_uint(
	std::string_view v,
	std::string_view key) {
	T result{};
	auto const *end = ranges::next(v.data(), ssize(v));
	auto [ptr, ec] = from_chars(v.data(), end, result);
	if (ec != errc{} || ptr != end) {
		throw std::runtime_error{format("invalid integer for '{}': '{}'", key, v)};
	}
	return result;
}
int parse_int(
	std::string_view v,
	std::string_view key) {
	int result{};
	auto const *end = ranges::next(v.data(), ssize(v));
	auto [ptr, ec] = from_chars(v.data(), end, result);
	if (ec != errc{} || ptr != end) {
		throw std::runtime_error{format("invalid integer for '{}': '{}'", key, v)};
	}
	return result;
}
void apply_server_key(
	Config &cfg,
	std::string_view key,
	std::string_view val) {
	static constexpr std::array<std::pair<std::string_view, unsigned Config::*>, 2> kUnsignedKeys{
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
	static constexpr std::array<std::pair<std::string_view, std::string Config::*>, 2> kStringKeys{
		{
         {"cert_file", &Config::cert_file},
         {"key_file", &Config::key_file},
		 }
    };
	for (auto const &[k, member]: kStringKeys) {
		if (key == k) {
			cfg.*member = std::string{val};
			return;
		}
	}
	if (key == "port") {
		cfg.port = parse_uint<std::uint16_t>(val, key);
	} else if (key == "max_body_size") {
		cfg.max_body_size = parse_uint<std::size_t>(val, key);
	} else if (key == "request_timeout_ms") {
		cfg.request_timeout_ms = parse_uint<std::uint32_t>(val, key);
	} else if (key == "tls_sniff_timeout_ms") {
		cfg.tls_sniff_timeout_ms = parse_uint<std::uint32_t>(val, key);
	} else if (key == "max_request_line_size") {
		cfg.parser_limits.max_request_line_size = parse_uint<std::size_t>(val, key);
	} else if (key == "max_header_line_size") {
		cfg.parser_limits.max_header_line_size = parse_uint<std::size_t>(val, key);
	} else if (key == "max_headers") {
		cfg.parser_limits.max_headers = parse_uint<std::size_t>(val, key);
	} else if (key == "max_header_block_size") {
		cfg.parser_limits.max_header_block_size = parse_uint<std::size_t>(val, key);
	} else if (key == "max_chunks") {
		cfg.parser_limits.max_chunks = parse_uint<std::size_t>(val, key);
	} else if (key == "slow_handler_diagnostics") {
		cfg.slow_handler_diagnostics = parse_bool(val, key);
	} else if (key == "slow_handler_warn_ms") {
		cfg.slow_handler_warn_ms = parse_uint<std::uint32_t>(val, key);
	} else if (key == "fixed_buffer_slabs") {
		cfg.fixed_buffer_slabs = parse_uint<std::size_t>(val, key);
	} else if (key == "fixed_buffer_bytes") {
		cfg.fixed_buffer_bytes = parse_uint<std::size_t>(val, key);
	} else if (key == "splice_pipe_pairs") {
		cfg.splice_pipe_pairs = parse_uint<std::size_t>(val, key);
	} else if (key == "send_buffer_slabs") {
		cfg.send_buffer_slabs = parse_uint<std::size_t>(val, key);
	} else if (key == "send_buffer_bytes") {
		cfg.send_buffer_bytes = parse_uint<std::size_t>(val, key);
	} else if (key == "send_fixed_buffers") {
		cfg.send_fixed_buffers = parse_bool(val, key);
	} else if (key == "busy_poll_us") {
		cfg.busy_poll_us = parse_uint<std::uint32_t>(val, key);
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
	std::string_view key,
	std::string_view val) {
	if (key == "enabled") {
		cfg.http3.enabled = parse_bool(val, key);
	} else if (key == "idle_timeout_ms") {
		cfg.http3.idle_timeout_ms = parse_uint<std::uint32_t>(val, key);
	} else if (key == "max_streams_bidi") {
		cfg.http3.max_streams_bidi = parse_uint<std::size_t>(val, key);
	} else if (key == "max_stream_data") {
		cfg.http3.max_stream_data = parse_uint<std::size_t>(val, key);
	} else if (key == "max_conn_data") {
		cfg.http3.max_conn_data = parse_uint<std::size_t>(val, key);
	} else if (key == "alt_svc_max_age_sec") {
		cfg.http3.alt_svc_max_age_sec = parse_uint<std::uint32_t>(val, key);
	} else if (key == "max_body_size") {
		cfg.http3.max_body_size = parse_uint<std::size_t>(val, key);
	}
}
void apply_static_cache_key(
	Config &cfg,
	std::string_view key,
	std::string_view val) {
	if (key == "enabled") {
		cfg.static_file_cache.enabled = parse_bool(val, key);
	} else if (key == "small_file_max_bytes") {
		cfg.static_file_cache.small_file_max_bytes = parse_uint<std::size_t>(val, key);
	} else if (key == "max_total_bytes") {
		cfg.static_file_cache.max_total_bytes = parse_uint<std::size_t>(val, key);
	}
}

[[nodiscard]] SecretSource literal_secret_source(
	std::string_view val) {
	return {.kind = SecretSourceKind::literal, .value = std::string{val}};
}
[[nodiscard]] SecretSource env_secret_source(
	std::string_view val) {
	return {.kind = SecretSourceKind::environment, .value = std::string{val}};
}
[[nodiscard]] SecretSource file_secret_source(
	std::string_view val) {
	return {.kind = SecretSourceKind::file, .value = std::string{val}};
}
void apply_secret_rotation_key(
	SecretRotationConfig &cfg,
	std::string_view prefix,
	std::string_view key,
	std::string_view val) {
	auto matches = [&](std::string_view suffix) noexcept {
		return key.size() == prefix.size() + suffix.size()
			&& key.substr(0, prefix.size()) == prefix
			&& key.substr(prefix.size()) == suffix;
	};
	if (matches("_secret")) {
		cfg.active = literal_secret_source(val);
	} else if (matches("_secret_env")) {
		cfg.active = env_secret_source(val);
	} else if (matches("_secret_file")) {
		cfg.active = file_secret_source(val);
	} else if (matches("_previous_secret")) {
		cfg.previous.push_back(literal_secret_source(val));
	} else if (matches("_previous_secret_env")) {
		cfg.previous.push_back(env_secret_source(val));
	} else if (matches("_previous_secret_file")) {
		cfg.previous.push_back(file_secret_source(val));
	} else if (matches("_min_secret_bytes")) {
		cfg.min_secret_bytes = parse_uint<std::size_t>(val, key);
	}
}
void apply_auth_key(
	Config &cfg,
	std::string_view key,
	std::string_view val) {
	if (key == "password_verifier_secret") {
		cfg.auth_secrets.password_verifier_secret = literal_secret_source(val);
	} else if (key == "password_verifier_secret_env") {
		cfg.auth_secrets.password_verifier_secret = env_secret_source(val);
	} else if (key == "password_verifier_secret_file") {
		cfg.auth_secrets.password_verifier_secret = file_secret_source(val);
	} else if (key == "password_verifier_min_secret_bytes") {
		cfg.auth_secrets.password_verifier_min_secret_bytes = parse_uint<std::size_t>(val, key);
	} else {
		apply_secret_rotation_key(cfg.auth_secrets.jwt, "jwt", key, val);
		apply_secret_rotation_key(cfg.auth_secrets.cookie, "cookie", key, val);
		apply_secret_rotation_key(cfg.auth_secrets.session, "session", key, val);
	}
}
void apply_tls_key(
	Config &cfg,
	std::string_view key,
	std::string_view val) {
	static constexpr std::array<std::pair<std::string_view, std::string Config::*>, 2> kStringKeys{
		{
         {"cipher_list", &Config::tls_cipher_list},
         {"ciphersuites", &Config::tls_ciphersuites},
		 }
    };
	for (auto const &[k, member]: kStringKeys) {
		if (key == k) {
			cfg.*member = std::string{val};
			return;
		}
	}
}
void apply_iouring_key(
	Config &cfg,
	std::string_view key,
	std::string_view val) {
	static constexpr std::array<std::pair<std::string_view, bool Config::*>, 17> kBoolKeys{
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
         {"direct_accept", &Config::direct_accept},
         {"cmd_sock_setsockopt", &Config::cmd_sock_setsockopt},
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
		cfg.send_zc_threshold = parse_uint<std::size_t>(val, key);
		return;
	}
	if (key == "send_zc_report_usage") {
		cfg.send_zc_report_usage = parse_bool(val, key);
		return;
	}
}

Config parse_ini_contents(
	std::string_view contents) {
	Config cfg{};
	std::string section;

	std::size_t line_no = 0;
	for (auto const line: LineRange{contents}) {
		++line_no;
		auto s = trim(line.text);
		if (s.empty() || s[0] == '#' || s[0] == ';') {
			continue;
		}

		if (s[0] == '[') {
			auto close = s.find(']', 1);
			if (close != std::string_view::npos) {
				section = std::string{trim(s.substr(1, close - 1))};
			}
			continue;
		}

		auto eq = s.find('=');
		if (eq == std::string_view::npos) {
			continue;
		}

		auto key = trim(s.substr(0, eq));
		auto val = trim(strip_inline_comment(trim(s.substr(eq + 1))));

		try {
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
			} else if (section == "auth") {
				apply_auth_key(cfg, key, val);
			}
		} catch (exception const &ex) {
			throw std::runtime_error{format("config line {} [{}].{}: {}", line_no, section, key, ex.what())};
		}
		// unknown sections/keys silently ignored — forward-compatible
	}

	return cfg;
}

} // namespace

export [[nodiscard]] expected<Config, std::string> config_from_ini_checked(
	char const *path) {
	auto contents = read_text_file_local(std::string_view{path});
	if (!contents) {
		return unexpected{format("cannot open config: {}", path)};
	}
	try {
		return parse_ini_contents(*contents);
	} catch (exception const &ex) { return unexpected{std::string{ex.what()}}; } catch (...) {
		return unexpected{std::string{"unknown config parse error"}};
	}
}

export [[nodiscard]] expected<Config, std::string> try_config_from_ini(
	char const *path) {
	return config_from_ini_checked(path);
}

// Throws std::runtime_error on parse / IO failure.
export Config config_from_ini(
	char const *path) {
	auto cfg = config_from_ini_checked(path);
	if (!cfg) {
		throw std::runtime_error{cfg.error()};
	}
	return move(*cfg);
}
