module;

#ifndef CONFLUX_BUILD_VERSION
	#define CONFLUX_BUILD_VERSION "unknown"
#endif
#ifndef CONFLUX_BUILD_GIT_COMMIT
	#define CONFLUX_BUILD_GIT_COMMIT "unknown"
#endif
#ifndef CONFLUX_BUILD_COMPILER
	#define CONFLUX_BUILD_COMPILER "unknown"
#endif
#ifndef CONFLUX_BUILD_COMPILER_VERSION
	#define CONFLUX_BUILD_COMPILER_VERSION "unknown"
#endif
#ifndef CONFLUX_BUILD_STDLIB
	#define CONFLUX_BUILD_STDLIB "unknown"
#endif
#ifndef CONFLUX_BUILD_STDLIB_VERSION
	#define CONFLUX_BUILD_STDLIB_VERSION "unknown"
#endif
#ifndef CONFLUX_BUILD_TYPE_VALUE
	#define CONFLUX_BUILD_TYPE_VALUE "unknown"
#endif
#ifndef CONFLUX_BUILD_INTERFACE_MODE
	#define CONFLUX_BUILD_INTERFACE_MODE "MODULE_INTERFACE"
#endif
#ifndef CONFLUX_BUILD_FEATURE_SET
	#define CONFLUX_BUILD_FEATURE_SET "full"
#endif
#ifndef CONFLUX_USE_MOCK_LIBURING
	#define CONFLUX_USE_MOCK_LIBURING 0
#endif
#ifndef CONFLUX_HAS_TLS
	#define CONFLUX_HAS_TLS 0
#endif
#ifndef CONFLUX_HAS_HTTP2
	#define CONFLUX_HAS_HTTP2 0
#endif
#ifndef CONFLUX_HAS_HTTP3
	#define CONFLUX_HAS_HTTP3 0
#endif
#ifndef CONFLUX_HAS_DB
	#define CONFLUX_HAS_DB 0
#endif
#ifndef CONFLUX_HAS_JSON
	#define CONFLUX_HAS_JSON 0
#endif

export module conflux.types;

import std;

export struct IoError final : std::system_error {
	IoError(
		int err,
		std::string const &what)
		: std::system_error{err, std::generic_category(), what} {}
};

export namespace conflux {

struct BuildInfo {
	std::string_view version;
	std::string_view git_commit;
	std::string_view compiler;
	std::string_view compiler_version;
	std::string_view stdlib_name;
	std::string_view stdlib_version;
	std::string_view build_type;
	std::string_view interface_mode;
	std::string_view feature_set;

	bool mock_liburing;
	bool tls;
	bool http2;
	bool http3;
	bool db;
	bool json_reflect;
	bool modules;
	bool header_interface;
};

[[nodiscard]] BuildInfo build_info() noexcept {
	return BuildInfo{
		.version = CONFLUX_BUILD_VERSION,
		.git_commit = CONFLUX_BUILD_GIT_COMMIT,
		.compiler = CONFLUX_BUILD_COMPILER,
		.compiler_version = CONFLUX_BUILD_COMPILER_VERSION,
		.stdlib_name = CONFLUX_BUILD_STDLIB,
		.stdlib_version = CONFLUX_BUILD_STDLIB_VERSION,
		.build_type = CONFLUX_BUILD_TYPE_VALUE,
		.interface_mode = CONFLUX_BUILD_INTERFACE_MODE,
		.feature_set = CONFLUX_BUILD_FEATURE_SET,
		.mock_liburing = CONFLUX_USE_MOCK_LIBURING != 0,
		.tls = CONFLUX_HAS_TLS != 0,
		.http2 = CONFLUX_HAS_HTTP2 != 0,
		.http3 = CONFLUX_HAS_HTTP3 != 0,
		.db = CONFLUX_HAS_DB != 0,
		.json_reflect = CONFLUX_HAS_JSON != 0,
		.modules = std::string_view{CONFLUX_BUILD_INTERFACE_MODE} == "MODULE_INTERFACE",
		.header_interface = std::string_view{CONFLUX_BUILD_INTERFACE_MODE} == "HEADER_INTERFACE",
	};
}

[[nodiscard]] std::string build_info_summary() {
	auto const info = build_info();
	return std::format(
		"conflux {} git={} compiler={} stdlib={} interface={} features={} tls={} http2={} http3={} db={} "
		"mock_liburing={}",
		info.version,
		info.git_commit,
		info.compiler,
		info.stdlib_name,
		info.interface_mode,
		info.feature_set,
		info.tls ? "on" : "off",
		info.http2 ? "on" : "off",
		info.http3 ? "on" : "off",
		info.db ? "on" : "off",
		info.mock_liburing ? "on" : "off");
}

} // namespace conflux

export namespace conflux::runtime {

enum class FeatureFallback {
	fail_fast,
	warn_and_fallback,
	silent_fallback,
};

enum class CapabilityIssueCode {
	unavailable,
	unsupported_kernel,
	disabled_at_build,
	disabled_by_config,
	blocked_by_seccomp,
	insufficient_memlock,
	incompatible_option,
	mock_backend,
	unknown,
};

struct CapabilityIssue {
	CapabilityIssueCode code{CapabilityIssueCode::unknown};
	std::string feature;
	std::string message;
	std::string hint;
};

struct RuntimeCapabilities {
	bool io_uring{};
	bool mock_backend{};
	bool sqpoll{};
	bool iopoll{};
	bool single_issuer{};
	bool defer_taskrun{};
	bool coop_taskrun{};
	bool taskrun_flag{};
	bool multishot_accept{};
	bool multishot_recv{};
	bool provided_buffers{};
	bool incremental_buffers{};
	bool registered_files{};
	bool fixed_buffers{};
	bool send_zc{};
	bool recv_zc{};
	bool openat2{};
	bool likely_seccomp_restricted{};
	std::uint64_t memlock_soft{};
	std::uint64_t memlock_hard{};
	std::vector<CapabilityIssue> issues;
};

std::string capability_issue_code_string(
	CapabilityIssueCode code) {
	switch (code) {
	case CapabilityIssueCode::unavailable         : return "capability.unavailable";
	case CapabilityIssueCode::unsupported_kernel  : return "capability.unsupported_kernel";
	case CapabilityIssueCode::disabled_at_build   : return "capability.disabled_at_build";
	case CapabilityIssueCode::disabled_by_config  : return "capability.disabled_by_config";
	case CapabilityIssueCode::blocked_by_seccomp  : return "capability.blocked_by_seccomp";
	case CapabilityIssueCode::insufficient_memlock: return "capability.insufficient_memlock";
	case CapabilityIssueCode::incompatible_option : return "capability.incompatible_option";
	case CapabilityIssueCode::mock_backend        : return "capability.mock_backend";
	case CapabilityIssueCode::unknown             : return "capability.unknown";
	}
	return "capability.unknown";
}

} // namespace conflux::runtime
