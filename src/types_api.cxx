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
#ifndef CONFLUX_BUILD_API_SURFACE
	#define CONFLUX_BUILD_API_SURFACE "curated"
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
#ifndef CONFLUX_HAS_JSON_REFLECT
	#define CONFLUX_HAS_JSON_REFLECT 0
#endif

export module conflux.types:api;

import std;

export namespace conflux {

struct IoError final : std::system_error {
	IoError(
		int err,
		std::string const &what)
		: std::system_error{err, std::generic_category(), what} {}

	[[nodiscard]] int errnum() const noexcept { return code().value(); }
};

template<typename T>
[[nodiscard]] int errnum(
	std::expected<T, IoError> const &result) noexcept {
	return result.error().errnum();
}

} // namespace conflux

export namespace conflux::support {

[[nodiscard]] constexpr std::uint64_t fnv1a64(
	std::string_view value) noexcept {
	std::uint64_t hash = 14695981039346656037ULL;
	for (char const raw: value) {
		hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(raw));
		hash *= 1099511628211ULL;
	}
	return hash;
}

[[nodiscard]] constexpr std::uint64_t fnv1a64_ascii_fold(
	std::string_view value) noexcept {
	std::uint64_t hash = 14695981039346656037ULL;
	for (char const raw: value) {
		auto const c = static_cast<unsigned char>(raw);
		hash ^= static_cast<std::uint64_t>(c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c);
		hash *= 1099511628211ULL;
	}
	return hash;
}

struct TransparentStringHash {
	using is_transparent = void;

	[[nodiscard]] std::size_t operator ()(
		std::string_view value) const noexcept {
		return std::hash<std::string_view>{}(value);
	}

	[[nodiscard]] std::size_t operator ()(
		std::string const &value) const noexcept {
		return operator ()(std::string_view{value});
	}
};

struct TransparentStringEqual {
	using is_transparent = void;

	[[nodiscard]] bool operator ()(
		std::string_view lhs,
		std::string_view rhs) const noexcept {
		return lhs == rhs;
	}

	[[nodiscard]] bool operator ()(
		std::string const &lhs,
		std::string_view rhs) const noexcept {
		return std::string_view{lhs} == rhs;
	}

	[[nodiscard]] bool operator ()(
		std::string_view lhs,
		std::string const &rhs) const noexcept {
		return lhs == std::string_view{rhs};
	}

	[[nodiscard]] bool operator ()(
		std::string const &lhs,
		std::string const &rhs) const noexcept {
		return lhs == rhs;
	}
};

template<class Value>
using TransparentStringMap = std::unordered_map<std::string, Value, TransparentStringHash, TransparentStringEqual>;

template<class Value>
class StringLruMap {
	struct Entry {
		Value value;
		std::list<std::string>::iterator order_it;
	};

public:
	struct TouchResult {
		Value *value{};
		bool inserted{};
		bool evicted{};
	};

	explicit StringLruMap(
		std::size_t max_entries)
		: max_{std::max<std::size_t>(max_entries, 1)} {}

	[[nodiscard]] Value *find(
		std::string_view key) noexcept {
		auto it = map_.find(key);
		if (it == map_.end()) {
			return nullptr;
		}
		order_.splice(order_.end(), order_, it->second.order_it);
		return &it->second.value;
	}

	template<class Factory>
	[[nodiscard]] TouchResult get_or_create(
		std::string_view key,
		Factory &&make_value) {
		if (auto *value = find(key); value != nullptr) {
			return {.value = value};
		}

		bool evicted = false;
		if (map_.size() >= max_) {
			map_.erase(order_.front());
			order_.pop_front();
			evicted = true;
		}

		auto owned = std::string{key};
		order_.push_back(owned);
		auto [it, _] = map_.emplace(
			std::move(owned),
			Entry{
				.value = std::invoke(std::forward<Factory>(make_value)),
				.order_it = std::prev(order_.end()),
			});
		return {.value = &it->second.value, .inserted = true, .evicted = evicted};
	}

	bool erase(
		std::string_view key) noexcept {
		auto it = map_.find(key);
		if (it == map_.end()) {
			return false;
		}
		order_.erase(it->second.order_it);
		map_.erase(it);
		return true;
	}

	void clear() noexcept {
		order_.clear();
		map_.clear();
	}

	[[nodiscard]] Value *insert_or_assign(
		std::string_view key,
		Value value) {
		auto it = map_.find(key);
		if (it != map_.end()) {
			it->second.value = std::move(value);
			order_.splice(order_.end(), order_, it->second.order_it);
			return &it->second.value;
		}

		if (map_.size() >= max_) {
			(void)evict_lru();
		}

		auto owned = std::string{key};
		order_.push_back(owned);
		auto [inserted, _] = map_.emplace(
			std::move(owned),
			Entry{
				.value = std::move(value),
				.order_it = std::prev(order_.end()),
			});
		return &inserted->second.value;
	}

	bool evict_lru() {
		return evict_lru([](std::string_view, Value &) {});
	}

	template<class OnEvict>
	bool evict_lru(
		OnEvict &&on_evict) {
		if (order_.empty()) {
			return false;
		}
		auto it = map_.find(order_.front());
		if (it == map_.end()) {
			order_.pop_front();
			return true;
		}
		std::invoke(std::forward<OnEvict>(on_evict), std::string_view{it->first}, it->second.value);
		order_.erase(it->second.order_it);
		map_.erase(it);
		return true;
	}

	template<class Predicate>
	std::size_t erase_if(
		Predicate &&pred) {
		std::size_t erased = 0;
		for (auto it = order_.begin(); it != order_.end();) {
			auto map_it = map_.find(*it);
			if (map_it == map_.end()) {
				it = order_.erase(it);
				continue;
			}
			if (!std::invoke(pred, std::string_view{map_it->first}, map_it->second.value)) {
				++it;
				continue;
			}
			it = order_.erase(it);
			map_.erase(map_it);
			++erased;
		}
		return erased;
	}

	[[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
	[[nodiscard]] bool empty() const noexcept { return map_.empty(); }

private:
	std::size_t max_;
	std::list<std::string> order_;
	TransparentStringMap<Entry> map_;
};

} // namespace conflux::support

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
	std::string_view api_surface;

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
		.api_surface = CONFLUX_BUILD_API_SURFACE,
		.tls = CONFLUX_HAS_TLS != 0,
		.http2 = CONFLUX_HAS_HTTP2 != 0,
		.http3 = CONFLUX_HAS_HTTP3 != 0,
		.db = CONFLUX_HAS_DB != 0,
		.json_reflect = CONFLUX_HAS_JSON_REFLECT != 0,
		.modules = std::string_view{CONFLUX_BUILD_INTERFACE_MODE} == "MODULE_INTERFACE",
		.header_interface = std::string_view{CONFLUX_BUILD_INTERFACE_MODE} == "HEADER_INTERFACE",
	};
}

[[nodiscard]] std::string build_info_summary() {
	auto const info = build_info();
	return std::format(
		"conflux {} git={} compiler={} stdlib={} interface={} features={} api_surface={} "
		"tls={} http2={} http3={} db={}",
		info.version,
		info.git_commit,
		info.compiler,
		info.stdlib_name,
		info.interface_mode,
		info.feature_set,
		info.api_surface,
		info.tls ? "on" : "off",
		info.http2 ? "on" : "off",
		info.http3 ? "on" : "off",
		info.db ? "on" : "off");
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

#if defined(CONFLUX_INTERFACE_HEADER)
[[nodiscard]] std::expected<RuntimeCapabilities, CapabilityIssue> detect_capabilities();
#endif

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
	case CapabilityIssueCode::unknown             : return "capability.unknown";
	}
	return "capability.unknown";
}

} // namespace conflux::runtime
