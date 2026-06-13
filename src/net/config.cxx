module;
#include <cerrno>
#include <cstdlib>

export module conflux.net.config;

import std;
import conflux.types;
import std.compat;
import conflux.file_io_sync;
import conflux.utils;

export namespace conflux::http {

constexpr std::uint16_t kConfigDefaultPort = 9090;
constexpr unsigned kConfigDefaultRingEntries = 1024;
constexpr std::size_t kConfigDefaultMaxBodySize = std::size_t{1024} * 1024;
constexpr std::uint32_t kConfigDefaultRequestTimeoutMs = 30000;
constexpr std::uint32_t kConfigDefaultTlsSniffTimeoutMs = 10000;
constexpr std::size_t kConfigDefaultMaxRequestLineSize = std::size_t{8} * 1024;
constexpr std::size_t kConfigDefaultMaxHeaderLineSize = std::size_t{8} * 1024;
constexpr std::size_t kConfigDefaultMaxHeaders = 100;
constexpr std::size_t kConfigDefaultMaxHeaderBlockSize = std::size_t{64} * 1024;
constexpr std::size_t kConfigDefaultMaxChunks = 100000;
struct ParserLimits {
	std::size_t max_request_line_size = kConfigDefaultMaxRequestLineSize;
	std::size_t max_header_line_size = kConfigDefaultMaxHeaderLineSize;
	std::size_t max_headers = kConfigDefaultMaxHeaders;
	std::size_t max_header_block_size = kConfigDefaultMaxHeaderBlockSize;
	std::size_t max_chunks = kConfigDefaultMaxChunks;
};
struct Http3Config {
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
struct StaticFileCacheConfig {
	bool enabled = false;
	std::size_t small_file_max_bytes = std::size_t{64} * 1024;
	std::size_t max_total_bytes = std::size_t{16} * 1024 * 1024;
};

enum class SecretSourceKind {
	unset,
	literal,
	environment,
	file,
};
struct SecretSource {
	SecretSourceKind kind{SecretSourceKind::unset};
	std::string value{};
};
struct SecretRotationConfig {
	SecretSource active;
	std::vector<SecretSource> previous;
	std::size_t min_secret_bytes;

	SecretRotationConfig()
		: active{}
		, previous{}
		, min_secret_bytes{16} {}
};
struct AuthSecretsConfig {
	SecretSource password_verifier_secret;
	std::size_t password_verifier_min_secret_bytes;
	SecretRotationConfig jwt;
	SecretRotationConfig cookie;
	SecretRotationConfig session;

	AuthSecretsConfig()
		: password_verifier_secret{}
		, password_verifier_min_secret_bytes{16}
		, jwt{}
		, cookie{}
		, session{} {}
};
struct ResolvedSecretRotation {
	std::string active{};
	std::vector<std::string> previous{};
	std::size_t min_secret_bytes{16};
};
enum class ConfigIssueCode {
	unknown_section,
	unknown_key,
	invalid_value,
	missing_required_value,
	incompatible_options,
	unsafe_option,
	secret_would_be_logged,
};
struct ConfigIssue {
	ConfigIssueCode code{ConfigIssueCode::invalid_value};
	std::string file{};
	std::size_t line{};
	std::string section{};
	std::string key{};
	std::string value{};
	std::string message{};
	std::string hint{};
};
// Per-hostname TLS credentials for SNI virtual hosting.
// When the client's TLS ClientHello SNI matches VirtualHost::hostname case-insensitively, the
// server switches to the certificate/key P from this struct.
struct VirtualHost {
	std::string hostname{}; // SNI hostname to match
	std::string cert_file{}; // PEM certificate chain for this host
	std::string key_file{}; // PEM private key for this host
	std::string cert_pem{}; // PEM certificate chain bytes for generated/small credentials
	std::string key_pem{}; // PEM private key bytes for generated/small credentials
	StaticFileCacheConfig static_file_cache{}; // Opt per-host router default
};
struct Config {
	[[nodiscard]] static Config public_server() {
		Config cfg{};
		cfg.strict_config = true;
		cfg.slow_handler_diagnostics = true;
		return cfg;
	}
	[[nodiscard]] static Config development() {
		Config cfg{};
		cfg.slow_handler_diagnostics = true;
		cfg.startup_banner = true;
		return cfg;
	}
	[[nodiscard]] static Config low_latency() {
		Config cfg{};
		cfg.rings = 4;
		cfg.ring_entries = 256;
		cfg.single_issuer = true;
		cfg.defer_taskrun = true;
		cfg.coop_taskrun = true;
		cfg.taskrun_flag = true;
		cfg.feature_fallback = conflux::runtime::FeatureFallback::fail_fast;
		return cfg;
	}
	[[nodiscard]] static Config benchmark() {
		auto cfg = low_latency();
		cfg.startup_banner = false;
		cfg.request_timeout_ms = 0;
		cfg.tls_sniff_timeout_ms = 0;
		return cfg;
	}
	[[nodiscard]] static Config unsafe_max_speed() {
		auto cfg = benchmark();
		cfg.parser_limits.max_headers = 1000;
		cfg.parser_limits.max_header_block_size = std::size_t{256} * 1024;
		cfg.max_body_size = std::size_t{16} * 1024 * 1024;
		cfg.send_fixed_buffers = true;
		cfg.send_zc = "on";
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
	std::size_t max_body_size = kConfigDefaultMaxBodySize; // std::max Content-Length before 413
	std::uint32_t request_timeout_ms = kConfigDefaultRequestTimeoutMs; // 0 = disabled
	std::uint32_t tls_sniff_timeout_ms = kConfigDefaultTlsSniffTimeoutMs; // 0 = disabled
	// Emit a warning when a synchronous handler blocks on the ring std::thread past
	// slow_handler_warn_ms. Disabled by default to keep baseline overhead minimal.
	bool slow_handler_diagnostics = false;
	std::uint32_t slow_handler_warn_ms = 25;
	ParserLimits parser_limits{};
	bool startup_banner = true;
	conflux::runtime::FeatureFallback feature_fallback = conflux::runtime::FeatureFallback::warn_and_fallback;
	bool strict_config = false;
	bool dump_effective_config = false;

	// TLS (enabled when either file or in-memory credential pair is complete)
	std::string cert_file{}; // path to PEM certificate chain; for deployment-sized credentials
	std::string key_file{}; // path to PEM private key; for deployment-sized credentials
	std::string cert_pem{}; // PEM certificate chain bytes; for generated/small credentials
	std::string key_pem{}; // PEM private key bytes; for generated/small credentials
	// When true, plain HTTP connections receive a 301 redirect to the same URL on https://.
	// Only meaningful when TLS is configured.
	bool http_redirect_to_https = false;
	// Allowlist of Host header values accepted for the HTTPS redirect.
	// Must include every hostname or IP:port that clients may use (e.g. "example.com",
	// "example.com:8080", "127.0.0.1").  Requests whose Host header is not in this list
	// are rejected with 400 Bad Request instead of being redirected.
	// Required when http_redirect_to_https is true; an empty list rejects all redirects.
	std::vector<std::string> https_redirect_hosts{};
	// SNI virtual hosting: each entry provides an alternate cert/key for a hostname.
	// Matched by case-insensitive SNI hostname; the primary TLS credentials are the default.
	std::vector<VirtualHost> virtual_hosts{};
	// TLS 1.2 cipher list (OpenSSL SSL_CTX_set_cipher_list std::format); empty = built-in default.
	std::string tls_cipher_list{};
	// TLS 1.3 ciphersuites (OpenSSL SSL_CTX_set_ciphersuites std::format); empty = built-in default.
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
	// IORING_SETUP_ATTACH_WQ. Reduces kernel std::thread overhead on high-ring-count setups.
	bool attach_wq = false; // IORING_SETUP_ATTACH_WQ
	// Remove the SQ index indirection A (kernel 6.4+). Slightly reduces
	// per-ring memory. Incompatible with SQPOLL.
	bool no_sqarray = true; // IORING_SETUP_NO_SQARRAY
	// Allow kernel to mix 16-std::byte and 32-std::byte CQEs on the same ring (kernel 6.5+).
	// CQEs that carry extra payload set IORING_CQE_F_32 in flags.
	bool cqe_mixed = false; // IORING_SETUP_CQE_MIXED
	// Application allocates ring memory; avoids kernel mmap overhead (kernel 6.5+).
	// Ring std::thread allocates a page-aligned buffer sized by io_uring_mlock_size;
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

	[[nodiscard]] std::string summary_redacted() const;
	[[nodiscard]] std::string to_json_redacted() const;
};

[[nodiscard]] ResolvedSecretRotation single_secret_rotation(
	std::string_view active,
	std::size_t min_secret_bytes = 16) {
	return {.active = std::string{active}, .previous = {}, .min_secret_bytes = min_secret_bytes};
}

[[nodiscard]] bool secret_source_configured(
	SecretSource const &src) noexcept {
	return src.kind != SecretSourceKind::unset;
}

} // namespace conflux::http

namespace conflux::http { namespace {

std::expected<std::string, int>
read_text_file_local(std::string_view path, std::size_t max_bytes = std::size_t{16} * 1024 * 1024);

}} // namespace conflux::http

export namespace conflux::http {

[[nodiscard]] std::expected<std::string, std::string> resolve_secret_source(
	SecretSource const &src,
	std::string_view name,
	bool required = true) {
	if (src.kind == SecretSourceKind::unset) {
		if (required) {
			return std::unexpected{std::format("auth secret '{}': missing required source", name)};
		}
		return std::string{};
	}
	if (src.value.empty()) {
		return std::unexpected{std::format("auth secret '{}': empty source value", name)};
	}
	if (src.kind == SecretSourceKind::literal) {
		return src.value;
	}
	if (src.kind == SecretSourceKind::environment) {
		auto const *value = std::getenv(src.value.c_str());
		if (value == nullptr || value[0] == '\0') {
			return std::unexpected{
				std::format("auth secret '{}': environment variable '{}' is unset or empty", name, src.value)};
		}
		return std::string{value};
	}
	if (src.kind == SecretSourceKind::file) {
		auto bytes = read_text_file_local(src.value, std::size_t{1024} * 1024);
		if (!bytes) {
			return std::unexpected{std::format("auth secret '{}': cannot open secret file '{}'", name, src.value)};
		}
		std::string value{std::move(*bytes)};
		while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
			value.pop_back();
		}
		if (value.empty()) {
			return std::unexpected{std::format("auth secret '{}': secret file '{}' is empty", name, src.value)};
		}
		return value;
	}
	return std::unexpected{std::format("auth secret '{}': unsupported source kind", name)};
}

[[nodiscard]] std::expected<void, std::string> validate_secret_bytes(
	std::string_view secret,
	std::string_view name,
	std::size_t min_bytes) {
	if (secret.empty()) {
		return std::unexpected{std::format("auth secret '{}': resolved secret is empty", name)};
	}
	if (secret.size() < min_bytes) {
		return std::unexpected{
			std::format("auth secret '{}': resolved secret must be at least {} bytes", name, min_bytes)};
	}
	return {};
}

[[nodiscard]] std::expected<void, std::string> validate_secret_rotation(
	ResolvedSecretRotation const &secrets,
	std::string_view name) {
	if (auto valid = validate_secret_bytes(secrets.active, name, secrets.min_secret_bytes); !valid) {
		return valid;
	}
	for (std::size_t i = 0; i < secrets.previous.size(); ++i) {
		auto const previous_name = std::format("{}.previous[{}]", name, i);
		if (auto valid = validate_secret_bytes(secrets.previous[i], previous_name, secrets.min_secret_bytes); !valid) {
			return valid;
		}
	}
	return {};
}

[[nodiscard]] std::expected<ResolvedSecretRotation, std::string> resolve_secret_rotation(
	SecretRotationConfig const &cfg,
	std::string_view name,
	bool required = true) {
	ResolvedSecretRotation out{.min_secret_bytes = cfg.min_secret_bytes};
	auto active = resolve_secret_source(cfg.active, name, required);
	if (!active) {
		return std::unexpected{active.error()};
	}
	out.active = std::move(*active);
	if (!out.active.empty()) {
		if (auto valid = validate_secret_bytes(out.active, name, cfg.min_secret_bytes); !valid) {
			return std::unexpected{valid.error()};
		}
	}
	for (std::size_t i = 0; i < cfg.previous.size(); ++i) {
		auto previous = resolve_secret_source(cfg.previous[i], std::format("{}.previous[{}]", name, i), true);
		if (!previous) {
			return std::unexpected{previous.error()};
		}
		if (auto valid =
				validate_secret_bytes(*previous, std::format("{}.previous[{}]", name, i), cfg.min_secret_bytes);
			!valid) {
			return std::unexpected{valid.error()};
		}
		out.previous.push_back(std::move(*previous));
	}
	return out;
}

} // namespace conflux::http

namespace conflux::http { namespace {

std::expected<std::string, int> read_text_file_local(
	std::string_view path,
	std::size_t max_bytes) {
	auto bytes = conflux::file_io_sync::blocking_read_text_file(path, max_bytes);
	if (!bytes) {
		return std::unexpected{bytes.error().code().value()};
	}
	return std::move(*bytes);
}

std::string_view strip_inline_comment(
	std::string_view s) {
	for (std::size_t i = 1; i < s.size(); ++i) {
		if ((s[i] == '#' || s[i] == ';') && (s[i - 1] == ' ' || s[i - 1] == '\t')) {
			return conflux::utils::trim(s.substr(0, i));
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
	throw std::runtime_error{std::format("invalid boolean for '{}': '{}'", key, v)};
}
template<typename T>
T parse_uint(
	std::string_view v,
	std::string_view key) {
	T result{};
	auto const *end = std::ranges::next(v.data(), ssize(v));
	auto [ptr, ec] = std::from_chars(v.data(), end, result);
	if (ec != std::errc{} || ptr != end) {
		throw std::runtime_error{std::format("invalid integer for '{}': '{}'", key, v)};
	}
	return result;
}
int parse_int(
	std::string_view v,
	std::string_view key) {
	int result{};
	auto const *end = std::ranges::next(v.data(), ssize(v));
	auto [ptr, ec] = std::from_chars(v.data(), end, result);
	if (ec != std::errc{} || ptr != end) {
		throw std::runtime_error{std::format("invalid integer for '{}': '{}'", key, v)};
	}
	return result;
}

template<class T, std::size_t N, class Parser>
bool apply_config_member_table(
	Config &cfg,
	std::string_view key,
	std::string_view val,
	std::array<std::pair<std::string_view, T Config::*>, N> const &table,
	Parser &&parser) {
	auto const it = std::ranges::find(table, key, &std::pair<std::string_view, T Config::*>::first);
	if (it == table.end()) {
		return false;
	}
	cfg.*(it->second) = parser(val, key);
	return true;
}

bool apply_server_key(
	Config &cfg,
	std::string_view key,
	std::string_view val) {
	static constexpr std::array<std::pair<std::string_view, unsigned Config::*>, 2> kUnsignedKeys{
		{
         {"rings", &Config::rings},
         {"ring_entries", &Config::ring_entries},
		 }
    };
	if (apply_config_member_table(cfg, key, val, kUnsignedKeys, parse_uint<unsigned>)) {
		return true;
	}
	static constexpr std::array<std::pair<std::string_view, std::uint32_t Config::*>, 4> kUint32Keys{
		{
         {"request_timeout_ms", &Config::request_timeout_ms},
         {"tls_sniff_timeout_ms", &Config::tls_sniff_timeout_ms},
         {"slow_handler_warn_ms", &Config::slow_handler_warn_ms},
         {"busy_poll_us", &Config::busy_poll_us},
		 }
    };
	if (apply_config_member_table(cfg, key, val, kUint32Keys, parse_uint<std::uint32_t>)) {
		return true;
	}
	static constexpr std::array<std::pair<std::string_view, std::size_t Config::*>, 6> kSizeKeys{
		{
         {"max_body_size", &Config::max_body_size},
         {"fixed_buffer_slabs", &Config::fixed_buffer_slabs},
         {"fixed_buffer_bytes", &Config::fixed_buffer_bytes},
         {"splice_pipe_pairs", &Config::splice_pipe_pairs},
         {"send_buffer_slabs", &Config::send_buffer_slabs},
         {"send_buffer_bytes", &Config::send_buffer_bytes},
		 }
    };
	if (apply_config_member_table(cfg, key, val, kSizeKeys, parse_uint<std::size_t>)) {
		return true;
	}
	static constexpr std::array<std::pair<std::string_view, std::string Config::*>, 4> kStringKeys{
		{
         {"cert_file", &Config::cert_file},
         {"key_file", &Config::key_file},
         {"cert_pem", &Config::cert_pem},
         {"key_pem", &Config::key_pem},
		 }
    };
	if (apply_config_member_table(cfg, key, val, kStringKeys, [](std::string_view v, std::string_view) {
			return std::string{v};
		})) {
		return true;
	}
	if (key == "port") {
		cfg.port = parse_uint<std::uint16_t>(val, key);
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
	} else if (key == "send_fixed_buffers") {
		cfg.send_fixed_buffers = parse_bool(val, key);
	} else if (key == "ring_core") {
		cfg.ring_core = parse_int(val, key);
	} else if (key == "worker_core_base") {
		cfg.worker_core_base = parse_int(val, key);
	} else if (key == "startup_banner") {
		cfg.startup_banner = parse_bool(val, key);
	} else if (key == "http_redirect_to_https") {
		cfg.http_redirect_to_https = parse_bool(val, key);
	} else if (key == "feature_fallback") {
		if (val == "fail_fast") {
			cfg.feature_fallback = conflux::runtime::FeatureFallback::fail_fast;
		} else if (val == "warn_and_fallback") {
			cfg.feature_fallback = conflux::runtime::FeatureFallback::warn_and_fallback;
		} else if (val == "silent_fallback") {
			cfg.feature_fallback = conflux::runtime::FeatureFallback::silent_fallback;
		} else {
			throw std::runtime_error{std::format("invalid feature_fallback: '{}'", val)};
		}
	} else if (key == "strict_config") {
		cfg.strict_config = parse_bool(val, key);
	} else if (key == "dump_effective_config") {
		cfg.dump_effective_config = parse_bool(val, key);
	} else {
		return false;
	}
	return true;
}
bool apply_http3_key(
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
	} else {
		return false;
	}
	return true;
}
bool apply_static_cache_key(
	Config &cfg,
	std::string_view key,
	std::string_view val) {
	if (key == "enabled") {
		cfg.static_file_cache.enabled = parse_bool(val, key);
	} else if (key == "small_file_max_bytes") {
		cfg.static_file_cache.small_file_max_bytes = parse_uint<std::size_t>(val, key);
	} else if (key == "max_total_bytes") {
		cfg.static_file_cache.max_total_bytes = parse_uint<std::size_t>(val, key);
	} else {
		return false;
	}
	return true;
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
bool apply_secret_rotation_key(
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
	} else {
		return false;
	}
	return true;
}
bool apply_auth_key(
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
		return apply_secret_rotation_key(cfg.auth_secrets.jwt, "jwt", key, val)
			|| apply_secret_rotation_key(cfg.auth_secrets.cookie, "cookie", key, val)
			|| apply_secret_rotation_key(cfg.auth_secrets.session, "session", key, val);
	}
	return true;
}
bool apply_tls_key(
	Config &cfg,
	std::string_view key,
	std::string_view val) {
	static constexpr std::array<std::pair<std::string_view, std::string Config::*>, 2> kStringKeys{
		{
         {"cipher_list", &Config::tls_cipher_list},
         {"ciphersuites", &Config::tls_ciphersuites},
		 }
    };
	return apply_config_member_table(cfg, key, val, kStringKeys, [](std::string_view v, std::string_view) {
		return std::string{v};
	});
}
bool apply_iouring_key(
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
	if (apply_config_member_table(cfg, key, val, kBoolKeys, parse_bool)) {
		return true;
	}
	if (key == "send_zc") {
		if (val == "off" || val == "auto" || val == "on") {
			cfg.send_zc = val;
		} else {
			throw std::runtime_error{std::format("invalid send_zc: '{}'", val)};
		}
		return true;
	}
	if (key == "send_zc_threshold") {
		cfg.send_zc_threshold = parse_uint<std::size_t>(val, key);
		return true;
	}
	if (key == "send_zc_report_usage") {
		cfg.send_zc_report_usage = parse_bool(val, key);
		return true;
	}
	return false;
}

[[nodiscard]] std::string hint_for_key(
	std::string_view key) {
	static constexpr std::array<std::string_view, 5> known{
		"max_body_size",
		"request_timeout_ms",
		"tls_sniff_timeout_ms",
		"send_zc_threshold",
		"startup_banner",
	};
	for (auto known_key: known) {
		if (!key.empty() && known_key.starts_with(key.substr(0, std::min<std::size_t>(key.size(), 7)))) {
			return std::format("did you mean {}?", known_key);
		}
	}
	if (key == "max_bdy_size") {
		return "did you mean max_body_size?";
	}
	return {};
}

struct ParseResult {
	Config cfg;
	std::vector<ConfigIssue> issues;
};

ParseResult parse_ini_contents(
	std::string_view contents,
	std::string_view file,
	bool strict) {
	Config cfg{};
	std::vector<ConfigIssue> issues;
	std::string section;

	std::size_t line_no = 0;
	for (auto const line: conflux::utils::LineRange{contents}) {
		++line_no;
		auto s = conflux::utils::trim(line.text);
		if (s.empty() || s[0] == '#' || s[0] == ';') {
			continue;
		}

		if (s[0] == '[') {
			auto close = s.find(']', 1);
			if (close != std::string_view::npos) {
				section = std::string{conflux::utils::trim(s.substr(1, close - 1))};
			}
			continue;
		}

		auto eq = s.find('=');
		if (eq == std::string_view::npos) {
			continue;
		}

		auto key = conflux::utils::trim(s.substr(0, eq));
		auto val = conflux::utils::trim(strip_inline_comment(conflux::utils::trim(s.substr(eq + 1))));

		try {
			bool applied = false;
			bool known_section = true;
			if (section == "server") {
				applied = apply_server_key(cfg, key, val);
			} else if (section == "io_uring") {
				applied = apply_iouring_key(cfg, key, val);
			} else if (section == "tls") {
				applied = apply_tls_key(cfg, key, val);
			} else if (section == "http3") {
				applied = apply_http3_key(cfg, key, val);
			} else if (section == "static_cache") {
				applied = apply_static_cache_key(cfg, key, val);
			} else if (section == "auth") {
				applied = apply_auth_key(cfg, key, val);
			} else {
				known_section = false;
			}
			if (!known_section && strict) {
				issues.push_back(
					ConfigIssue{
						.code = ConfigIssueCode::unknown_section,
						.file = std::string{file},
						.line = line_no,
						.section = section,
						.key = std::string{key},
						.value = std::string{val},
						.message = "unknown config section"});
			}
			if (known_section && !applied && strict && !section.empty()) {
				issues.push_back(
					ConfigIssue{
						.code = ConfigIssueCode::unknown_key,
						.file = std::string{file},
						.line = line_no,
						.section = section,
						.key = std::string{key},
						.value = std::string{val},
						.message = "unknown config key",
						.hint = hint_for_key(key)});
			}
		} catch (std::exception const &ex) {
			issues.push_back(
				ConfigIssue{
					.code = ConfigIssueCode::invalid_value,
					.file = std::string{file},
					.line = line_no,
					.section = section,
					.key = std::string{key},
					.value = std::string{val},
					.message = ex.what()});
		}
		// unknown sections/keys silently ignored — forward-compatible
	}

	return ParseResult{.cfg = std::move(cfg), .issues = std::move(issues)};
}

[[nodiscard]] bool contains_sensitive_config_token(
	std::string_view text) {
	std::string lower;
	lower.reserve(text.size());
	for (char const ch: text) {
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
	}
	constexpr std::array<std::string_view, 7> tokens{"secret", "token", "key", "password", "jwt", "cookie", "session"};
	return std::ranges::any_of(tokens, [&](std::string_view token) { return lower.find(token) != std::string::npos; });
}

[[nodiscard]] bool should_redact_config_issue_value(
	ConfigIssue const &issue) {
	return contains_sensitive_config_token(issue.section) || contains_sensitive_config_token(issue.key);
}

[[nodiscard]] bool has_tls_file_credentials(
	Config const &cfg) noexcept {
	return !cfg.cert_file.empty() && !cfg.key_file.empty();
}

[[nodiscard]] bool has_tls_pem_credentials(
	Config const &cfg) noexcept {
	return !cfg.cert_pem.empty() && !cfg.key_pem.empty();
}

[[nodiscard]] bool has_any_tls_file_field(
	Config const &cfg) noexcept {
	return !cfg.cert_file.empty() || !cfg.key_file.empty();
}

[[nodiscard]] bool has_any_tls_pem_field(
	Config const &cfg) noexcept {
	return !cfg.cert_pem.empty() || !cfg.key_pem.empty();
}

[[nodiscard]] bool has_tls_credentials(
	Config const &cfg) noexcept {
	return has_tls_file_credentials(cfg) || has_tls_pem_credentials(cfg);
}

}} // namespace conflux::http

export namespace conflux::http {

[[nodiscard]] std::string config_issue_code_string(
	ConfigIssueCode code) {
	switch (code) {
	case ConfigIssueCode::unknown_section       : return "config.unknown_section";
	case ConfigIssueCode::unknown_key           : return "config.unknown_key";
	case ConfigIssueCode::invalid_value         : return "config.invalid_value";
	case ConfigIssueCode::missing_required_value: return "config.missing_required_value";
	case ConfigIssueCode::incompatible_options  : return "config.incompatible_options";
	case ConfigIssueCode::unsafe_option         : return "config.unsafe_option";
	case ConfigIssueCode::secret_would_be_logged: return "config.secret_would_be_logged";
	}
	return "config.invalid_value";
}

[[nodiscard]] std::string config_issue_summary(
	ConfigIssue const &issue) {
	auto out = std::format(
		"{} file={} line={} section={} key={}",
		config_issue_code_string(issue.code),
		issue.file,
		issue.line,
		issue.section,
		issue.key);
	if (!issue.value.empty()) {
		out += std::format(" value={}", should_redact_config_issue_value(issue) ? "<redacted>" : issue.value);
	}
	if (!issue.message.empty()) {
		out += std::format(" message={}", issue.message);
	}
	if (!issue.hint.empty()) {
		out += std::format(" hint={}", issue.hint);
	}
	return out;
}

[[nodiscard]] std::vector<ConfigIssue> validate_config(
	Config const &cfg,
	std::string_view file = {}) {
	std::vector<ConfigIssue> issues;
	auto add = [&](ConfigIssueCode code, std::string_view section, std::string_view key, std::string message) {
		issues.push_back(
			ConfigIssue{
				.code = code,
				.file = std::string{file},
				.section = std::string{section},
				.key = std::string{key},
				.message = std::move(message)});
	};
	if (cfg.parser_limits.max_request_line_size == 0
		|| cfg.parser_limits.max_header_line_size == 0
		|| cfg.parser_limits.max_headers == 0
		|| cfg.parser_limits.max_header_block_size == 0) {
		add(ConfigIssueCode::invalid_value, "server", "parser_limits", "HTTP parser limits must be greater than zero");
	}
	if (cfg.request_timeout_ms > 24U * 60U * 60U * 1000U) {
		add(ConfigIssueCode::invalid_value, "server", "request_timeout_ms", "request timeout exceeds one day");
	}
	if (cfg.http3.enabled) {
#if !CONFLUX_HAS_HTTP3
		add(ConfigIssueCode::incompatible_options, "http3", "enabled", "HTTP/3 support was disabled at build time");
#endif
		if (!has_tls_credentials(cfg)) {
			add(ConfigIssueCode::incompatible_options, "http3", "enabled", "HTTP/3 requires TLS credentials");
		}
	}
	if (has_any_tls_file_field(cfg) && !has_tls_file_credentials(cfg)) {
		add(ConfigIssueCode::invalid_value, "tls", "cert_file", "cert_file and key_file must be set together");
	}
	if (has_any_tls_pem_field(cfg) && !has_tls_pem_credentials(cfg)) {
		add(ConfigIssueCode::invalid_value, "tls", "cert_pem", "cert_pem and key_pem must be set together");
	}
	if (has_any_tls_file_field(cfg) && has_any_tls_pem_field(cfg)) {
		add(ConfigIssueCode::incompatible_options,
			"tls",
			"credentials",
			"choose either cert_file/key_file or cert_pem/key_pem, not both");
	}
	for (std::size_t i = 0; i < cfg.virtual_hosts.size(); ++i) {
		auto const &host = cfg.virtual_hosts[i];
		bool const host_file_any = !host.cert_file.empty() || !host.key_file.empty();
		bool const host_pem_any = !host.cert_pem.empty() || !host.key_pem.empty();
		bool const host_file_complete = !host.cert_file.empty() && !host.key_file.empty();
		bool const host_pem_complete = !host.cert_pem.empty() && !host.key_pem.empty();
		if (host_file_any && !host_file_complete) {
			add(ConfigIssueCode::invalid_value,
				"tls",
				std::format("virtual_hosts[{}].cert_file", i),
				"cert_file and key_file must be set together");
		}
		if (host_pem_any && !host_pem_complete) {
			add(ConfigIssueCode::invalid_value,
				"tls",
				std::format("virtual_hosts[{}].cert_pem", i),
				"cert_pem and key_pem must be set together");
		}
		if (host_file_any && host_pem_any) {
			add(ConfigIssueCode::incompatible_options,
				"tls",
				std::format("virtual_hosts[{}].credentials", i),
				"choose either cert_file/key_file or cert_pem/key_pem, not both");
		}
	}
	if (cfg.http_redirect_to_https && !has_tls_credentials(cfg)) {
		add(ConfigIssueCode::incompatible_options,
			"server",
			"http_redirect_to_https",
			"HTTPS redirect requires TLS credentials");
	}
	if (cfg.http_redirect_to_https && cfg.https_redirect_hosts.empty()) {
		add(ConfigIssueCode::incompatible_options,
			"server",
			"https_redirect_hosts",
			"HTTPS redirect requires an explicit allowed-host list");
	}
#if !CONFLUX_HAS_TLS
	if (has_any_tls_file_field(cfg) || has_any_tls_pem_field(cfg) || cfg.http_redirect_to_https || cfg.ktls) {
		add(ConfigIssueCode::incompatible_options, "tls", "enabled", "TLS support was disabled at build time");
	}
#endif
	if (cfg.send_zc == "on") {
#if !CONFLUX_ENABLE_SEND_ZC
		add(ConfigIssueCode::incompatible_options, "io_uring", "send_zc", "SEND_ZC support was disabled at build time");
#endif
	}
	if (cfg.feature_fallback == conflux::runtime::FeatureFallback::silent_fallback
		&& cfg.request_timeout_ms == 0
		&& cfg.tls_sniff_timeout_ms == 0) {
		add(ConfigIssueCode::incompatible_options,
			"server",
			"feature_fallback",
			"benchmark-style configs reject silent fallback");
	}
	return issues;
}

[[nodiscard]] std::vector<ConfigIssue> unsafe_config_issues(
	Config const &cfg,
	std::string_view file = {}) {
	std::vector<ConfigIssue> issues;
	auto add = [&](std::string_view section, std::string_view key, std::string message) {
		issues.push_back(
			ConfigIssue{
				.code = ConfigIssueCode::unsafe_option,
				.file = std::string{file},
				.section = std::string{section},
				.key = std::string{key},
				.message = std::move(message)});
	};
	if (cfg.request_timeout_ms == 0) {
		add("server", "request_timeout_ms", "request timeout is disabled");
	}
	if (cfg.tls_sniff_timeout_ms == 0) {
		add("server", "tls_sniff_timeout_ms", "TLS sniff timeout is disabled");
	}
	if (cfg.max_body_size > kConfigDefaultMaxBodySize) {
		add("server", "max_body_size", "request body limit exceeds public default");
	}
	if (cfg.parser_limits.max_headers > kConfigDefaultMaxHeaders) {
		add("server", "max_headers", "header count limit exceeds public default");
	}
	if (cfg.parser_limits.max_header_block_size > kConfigDefaultMaxHeaderBlockSize) {
		add("server", "max_header_block_size", "header block limit exceeds public default");
	}
	if (cfg.send_fixed_buffers) {
		add("server", "send_fixed_buffers", "registered send buffers are explicitly enabled");
	}
	if (cfg.send_zc == "on") {
		add("io_uring", "send_zc", "SEND_ZC is required instead of opportunistic");
	}
	return issues;
}

[[nodiscard]] std::vector<conflux::runtime::CapabilityIssue> validate_config_capabilities(
	Config const &cfg,
	conflux::runtime::RuntimeCapabilities const &caps) {
	std::vector<conflux::runtime::CapabilityIssue> issues;
	auto add = [&](conflux::runtime::CapabilityIssueCode code,
				   std::string feature,
				   std::string message,
				   std::string hint = {}) {
		issues.push_back(
			conflux::runtime::CapabilityIssue{
				.code = code,
				.feature = std::move(feature),
				.message = std::move(message),
				.hint = std::move(hint)});
	};
	if (cfg.sqpoll && !caps.sqpoll) {
		add(conflux::runtime::CapabilityIssueCode::unavailable,
			"sqpoll",
			"SQPOLL was requested but is not active",
			"disable sqpoll or use fail_fast to reject fallback");
	}
	if (cfg.send_fixed_buffers && !caps.fixed_buffers) {
		add(conflux::runtime::CapabilityIssueCode::unavailable,
			"fixed_buffers",
			"registered send buffers were requested but fixed buffers are unavailable",
			"raise RLIMIT_MEMLOCK or disable send_fixed_buffers");
	}
	if (cfg.fixed_buffer_slabs != 0 && cfg.fixed_buffer_bytes != 0 && caps.memlock_soft != 0) {
		auto const requested =
			static_cast<std::uint64_t>(cfg.fixed_buffer_slabs) * static_cast<std::uint64_t>(cfg.fixed_buffer_bytes);
		if (requested > caps.memlock_soft) {
			add(conflux::runtime::CapabilityIssueCode::insufficient_memlock,
				"fixed_buffers",
				"configured fixed-buffer memory exceeds RLIMIT_MEMLOCK soft limit",
				"reduce fixed_buffer_slabs/fixed_buffer_bytes or raise memlock");
		}
	}
	if (cfg.send_zc == "on" && !caps.send_zc) {
		add(conflux::runtime::CapabilityIssueCode::unavailable,
			"send_zc",
			"SEND_ZC was required but is unavailable",
			"use send_zc=auto/off or run on a kernel that supports IORING_OP_SEND_ZC");
	}
	if (cfg.recv_incremental_buf && !caps.incremental_buffers) {
		add(conflux::runtime::CapabilityIssueCode::unavailable,
			"incremental_buffers",
			"incremental provided buffers were requested but are unavailable",
			"disable recv_incremental_buf");
	}
	return issues;
}

[[nodiscard]] std::string feature_fallback_string(
	conflux::runtime::FeatureFallback policy) {
	switch (policy) {
	case conflux::runtime::FeatureFallback::fail_fast        : return "fail_fast";
	case conflux::runtime::FeatureFallback::warn_and_fallback: return "warn_and_fallback";
	case conflux::runtime::FeatureFallback::silent_fallback  : return "silent_fallback";
	}
	return "warn_and_fallback";
}

std::string Config::summary_redacted() const {
	return std::format(
		"server.host=0.0.0.0 server.port={} server.max_body_size={} server.request_timeout_ms={} "
		"server.feature_fallback={} server.strict_config={} auth.jwt_secret=<redacted> auth.cookie_secret=<redacted>",
		port,
		max_body_size,
		request_timeout_ms,
		feature_fallback_string(feature_fallback),
		strict_config ? "true" : "false");
}

std::string Config::to_json_redacted() const {
	return std::format(
		"{{\n"
		"  \"server\": {{\n"
		"    \"host\": \"0.0.0.0\",\n"
		"    \"port\": {},\n"
		"    \"max_body_size\": {},\n"
		"    \"request_timeout_ms\": {},\n"
		"    \"feature_fallback\": \"{}\",\n"
		"    \"strict_config\": {}\n"
		"  }},\n"
		"  \"security\": {{\n"
		"    \"jwt_secret\": \"<redacted>\",\n"
		"    \"cookie_secret\": \"<redacted>\"\n"
		"  }}\n"
		"}}",
		port,
		max_body_size,
		request_timeout_ms,
		feature_fallback_string(feature_fallback),
		strict_config ? "true" : "false");
}

[[nodiscard]] std::expected<Config, std::string> config_from_ini_checked(
	char const *path) {
	auto contents = read_text_file_local(std::string_view{path});
	if (!contents) {
		return std::unexpected{std::format("cannot open config: {}", path)};
	}
	try {
		auto parsed = parse_ini_contents(*contents, path, false);
		if (!parsed.issues.empty()) {
			return std::unexpected{config_issue_summary(parsed.issues.front())};
		}
		auto validation = validate_config(parsed.cfg, path);
		if (!validation.empty()) {
			return std::unexpected{config_issue_summary(validation.front())};
		}
		return std::move(parsed.cfg);
	} catch (std::exception const &ex) { return std::unexpected{std::string{ex.what()}}; } catch (...) {
		return std::unexpected{std::string{"unknown config parse error"}};
	}
}

[[nodiscard]] std::expected<Config, std::vector<ConfigIssue>> config_from_ini_checked(
	char const *path,
	bool strict) {
	auto contents = read_text_file_local(std::string_view{path});
	if (!contents) {
		return std::unexpected{std::vector<ConfigIssue>{ConfigIssue{
			.code = ConfigIssueCode::missing_required_value,
			.file = std::string{path},
			.message = "cannot open config"}}};
	}
	auto parsed = parse_ini_contents(*contents, path, strict);
	auto validation = validate_config(parsed.cfg, path);
	parsed.issues.insert(
		parsed.issues.end(),
		std::make_move_iterator(validation.begin()),
		std::make_move_iterator(validation.end()));
	if (!parsed.issues.empty()) {
		return std::unexpected{std::move(parsed.issues)};
	}
	return std::move(parsed.cfg);
}

[[nodiscard]] std::expected<Config, std::string> try_config_from_ini(
	char const *path) {
	return config_from_ini_checked(path);
}

// Throws std::runtime_error on parse / IO failure.
Config config_from_ini(
	char const *path) {
	auto cfg = config_from_ini_checked(path);
	if (!cfg) {
		throw std::runtime_error{cfg.error()};
	}
	return std::move(*cfg);
}

} // namespace conflux::http
