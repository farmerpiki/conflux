module;
#include <cstddef>
#if defined(CONFLUX_PASSWORD_HASH_ARGON2_RUNTIME)
	#include <dlfcn.h>
#endif
#if defined(CONFLUX_PASSWORD_HASH_ARGON2_LINKED)
extern "C" {
int argon2id_hash_raw(
	unsigned int t_cost,
	unsigned int m_cost,
	unsigned int parallelism,
	void const *pwd,
	std::size_t pwdlen,
	void const *salt,
	std::size_t saltlen,
	void *hash,
	std::size_t hashlen);
char const *argon2_error_message(int error_code);
}
#endif

export module conflux.net.password_hash;
import std;
import conflux.types;
import conflux.crypto;
import conflux.utils;
import conflux.net.config;

export namespace conflux::http {

enum class PasswordHashAlgorithm {
	argon2id,
	pbkdf2_sha256,
};

struct PasswordHashOptions {
	PasswordHashAlgorithm algorithm{PasswordHashAlgorithm::argon2id};
	std::uint32_t memory_kib{64U * 1024U};
	std::uint32_t iterations{3U};
	std::uint32_t parallelism{1U};
	std::uint32_t salt_bytes{16U};
	std::uint32_t hash_bytes{32U};
};

struct PasswordHashSecrets {
	std::string verifier_secret{};
};

struct PasswordHashResourceLimits {
	// 0 means use the library default: std::min(std::max(hardware_concurrency / 2, 1), 4).
	std::uint32_t max_concurrent_hashes{0U};
	// Requests beyond active hashes wait up to this many queued callers; 0 means fail fast.
	std::uint32_t max_waiting_hashes{64U};
};

struct PasswordVerifyResult {
	bool ok{false};
	bool needs_rehash{false};
};

} // namespace conflux::http

namespace conflux::http::password_hash_detail {

constexpr std::uint32_t kArgon2Version = 0x13U;
constexpr std::uint32_t kPbkdf2DefaultIterations = 600'000U;
constexpr std::uint32_t kMaxPasswordBytes = 1U << 30U;
constexpr std::uint32_t kMaxSaltBytes = 1024U;
constexpr std::uint32_t kMaxHashBytes = 1024U;
constexpr std::uint32_t kMaxVerifierSecretBytes = 4096U;
constexpr std::uint32_t kMaxPbkdf2Iterations = 100'000'000U;
constexpr std::uint32_t kMaxArgon2Iterations = 1024U;
constexpr std::uint32_t kMaxArgon2MemoryKiB = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaxArgon2Parallelism = 1024U;
constexpr std::uint32_t kMaxHashConcurrency = 1024U;
constexpr std::uint32_t kMaxHashWaiters = 1U << 20U;

[[nodiscard]] std::uint32_t default_hash_concurrency() noexcept {
	std::uint32_t const hw = std::max(1U, std::thread::hardware_concurrency());
	return std::min(4U, std::max(1U, hw / 2U));
}

struct PasswordHashGate;

struct HashPermit {
	PasswordHashGate *gate{};
	HashPermit() noexcept = default;
	explicit HashPermit(
		PasswordHashGate *g) noexcept
		: gate(g) {}
	HashPermit(HashPermit const &) = delete;
	HashPermit &operator =(HashPermit const &) = delete;
	HashPermit(
		HashPermit &&other) noexcept
		: gate(std::exchange(other.gate, nullptr)) {}
	HashPermit &operator =(HashPermit &&other) noexcept;
	~HashPermit();
};

struct PasswordHashGate {
	std::mutex mtx;
	std::condition_variable cv;
	std::uint32_t max_concurrent{default_hash_concurrency()};
	std::uint32_t max_waiting{64U};
	std::uint32_t active{};
	std::uint32_t waiting{};

	[[nodiscard]] std::expected<HashPermit, std::string> acquire() {
		std::unique_lock lock{mtx};
		if (active < max_concurrent) {
			++active;
			return HashPermit{this};
		}
		if (waiting >= max_waiting) {
			return std::unexpected{"password std::hash: concurrency limit reached"};
		}
		++waiting;
		cv.wait(lock, [&] { return active < max_concurrent; });
		--waiting;
		++active;
		return HashPermit{this};
	}

	void release() noexcept {
		{
			std::lock_guard lock{mtx};
			if (active > 0U) {
				--active;
			}
		}
		cv.notify_one();
	}

	[[nodiscard]] std::expected<void, std::string> configure(
		PasswordHashResourceLimits limits) {
		std::uint32_t const concurrency =
			limits.max_concurrent_hashes == 0U ? default_hash_concurrency() : limits.max_concurrent_hashes;
		if (concurrency == 0U || concurrency > kMaxHashConcurrency) {
			return std::unexpected{"password std::hash: invalid max_concurrent_hashes"};
		}
		if (limits.max_waiting_hashes > kMaxHashWaiters) {
			return std::unexpected{"password std::hash: invalid max_waiting_hashes"};
		}
		{
			std::lock_guard lock{mtx};
			max_concurrent = concurrency;
			max_waiting = limits.max_waiting_hashes;
		}
		cv.notify_all();
		return {};
	}

	[[nodiscard]] PasswordHashResourceLimits current() {
		std::lock_guard lock{mtx};
		return {.max_concurrent_hashes = max_concurrent, .max_waiting_hashes = max_waiting};
	}
};

HashPermit &HashPermit::operator =(
	HashPermit &&other) noexcept {
	if (this != &other) {
		if (gate != nullptr) {
			gate->release();
		}
		gate = std::exchange(other.gate, nullptr);
	}
	return *this;
}

HashPermit::~HashPermit() {
	if (gate != nullptr) {
		gate->release();
	}
}

[[nodiscard]] PasswordHashGate &password_hash_gate() noexcept {
	static PasswordHashGate gate;
	return gate;
}

struct ParsedHash {
	PasswordHashAlgorithm algorithm{};
	std::uint32_t version{};
	std::uint32_t memory_kib{};
	std::uint32_t iterations{};
	std::uint32_t parallelism{};
	std::uint32_t hash_bytes{};
	bool uses_verifier_secret{false};
	std::string salt{};
	std::string hash{};
};

using Argon2idHashRawFn = int (*)(
	std::uint32_t t_cost,
	std::uint32_t m_cost,
	std::uint32_t parallelism,
	void const *pwd,
	std::size_t pwdlen,
	void const *salt,
	std::size_t saltlen,
	void *hash,
	std::size_t hashlen);
using Argon2ErrorMessageFn = char const *(*)(int error_code);

struct Argon2Api {
	void *handle{};
	Argon2idHashRawFn hash_raw{};
	Argon2ErrorMessageFn error_message{};
};

[[nodiscard]] Argon2Api load_argon2() noexcept {
#if defined(CONFLUX_PASSWORD_HASH_ARGON2_LINKED)
	return Argon2Api{nullptr, &::argon2id_hash_raw, &::argon2_error_message};
#elif defined(CONFLUX_PASSWORD_HASH_ARGON2_RUNTIME)
	static constexpr std::array<char const *, 2> kNames{"libargon2.so.1", "libargon2.so"};
	for (char const *name: kNames) {
		void *handle = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
		if (handle == nullptr) {
			continue;
		}
		auto *hash_raw = reinterpret_cast<Argon2idHashRawFn>(::dlsym(handle, "argon2id_hash_raw"));
		auto *error_message = reinterpret_cast<Argon2ErrorMessageFn>(::dlsym(handle, "argon2_error_message"));
		if (hash_raw != nullptr) {
			return Argon2Api{handle, hash_raw, error_message};
		}
		::dlclose(handle);
	}
	return {};
#else
	return {};
#endif
}

[[nodiscard]] Argon2Api const &argon2_api() noexcept {
	static Argon2Api const api = load_argon2();
	return api;
}

[[nodiscard]] bool parse_u32(
	std::string_view text,
	std::uint32_t &out) noexcept {
	if (text.empty()) {
		return false;
	}
	std::uint32_t value{};
	auto const *begin = text.data();
	auto const *end = text.data() + text.size();
	auto [ptr, ec] = std::from_chars(begin, end, value);
	if (ec != std::errc{} || ptr != end) {
		return false;
	}
	out = value;
	return true;
}

[[nodiscard]] std::optional<std::string_view> param_value(
	std::string_view params,
	std::string_view key) noexcept {
	std::size_t pos = 0;
	while (pos <= params.size()) {
		std::size_t const comma = params.find(',', pos);
		std::string_view const part =
			comma == std::string_view::npos ? params.substr(pos) : params.substr(pos, comma - pos);
		std::size_t const eq = part.find('=');
		if (eq != std::string_view::npos && part.substr(0, eq) == key) {
			return part.substr(eq + 1);
		}
		if (comma == std::string_view::npos) {
			break;
		}
		pos = comma + 1;
	}
	return std::nullopt;
}

[[nodiscard]] std::expected<bool, std::string> parse_verifier_secret_flag(
	std::string_view params) {
	auto keyed = param_value(params, "k");
	if (!keyed) {
		return false;
	}
	std::uint32_t value{};
	if (!parse_u32(*keyed, value) || value > 1U) {
		return std::unexpected{"password std::hash: malformed verifier-secret parameter"};
	}
	return value == 1U;
}

[[nodiscard]] std::expected<std::string, std::string> decode_required_b64url(
	std::string_view encoded,
	std::string_view field) {
	if (encoded.empty()) {
		return std::unexpected{std::format("password std::hash: {} is empty", field)};
	}
	std::string decoded = conflux::crypto::base64url_decode(encoded);
	if (decoded.empty()) {
		return std::unexpected{std::format("password std::hash: {} is not valid base64url", field)};
	}
	return decoded;
}

[[nodiscard]] std::expected<ParsedHash, std::string> parse_hash(
	std::string_view encoded) {
	if (!encoded.starts_with('$')) {
		return std::unexpected{"password std::hash: std::expected modular crypt std::format"};
	}
	std::vector<std::string_view> parts;
	std::size_t pos = 1;
	while (true) {
		std::size_t const next = encoded.find('$', pos);
		parts.push_back(next == std::string_view::npos ? encoded.substr(pos) : encoded.substr(pos, next - pos));
		if (next == std::string_view::npos) {
			break;
		}
		pos = next + 1;
	}

	if (parts.size() != 5) {
		return std::unexpected{"password std::hash: std::expected 5 fields"};
	}

	ParsedHash parsed{};
	if (parts[0] == "argon2id") {
		parsed.algorithm = PasswordHashAlgorithm::argon2id;
		auto version = param_value(parts[1], "v");
		if (!version || !parse_u32(*version, parsed.version) || parsed.version != kArgon2Version) {
			return std::unexpected{"password std::hash: unsupported argon2id version"};
		}
		auto memory = param_value(parts[2], "m");
		auto iterations = param_value(parts[2], "t");
		auto parallelism = param_value(parts[2], "p");
		if (!memory
			|| !iterations
			|| !parallelism
			|| !parse_u32(*memory, parsed.memory_kib)
			|| !parse_u32(*iterations, parsed.iterations)
			|| !parse_u32(*parallelism, parsed.parallelism)) {
			return std::unexpected{"password std::hash: malformed argon2id parameters"};
		}
		auto keyed = parse_verifier_secret_flag(parts[2]);
		if (!keyed) {
			return std::unexpected{keyed.error()};
		}
		parsed.uses_verifier_secret = *keyed;
	} else if (parts[0] == "pbkdf2-sha256") {
		parsed.algorithm = PasswordHashAlgorithm::pbkdf2_sha256;
		auto version = param_value(parts[1], "v");
		if (!version || !parse_u32(*version, parsed.version) || parsed.version != 1U) {
			return std::unexpected{"password std::hash: unsupported pbkdf2-sha256 version"};
		}
		auto iterations = param_value(parts[2], "i");
		auto hash_bytes = param_value(parts[2], "l");
		if (!iterations
			|| !hash_bytes
			|| !parse_u32(*iterations, parsed.iterations)
			|| !parse_u32(*hash_bytes, parsed.hash_bytes)) {
			return std::unexpected{"password std::hash: malformed pbkdf2-sha256 parameters"};
		}
		auto keyed = parse_verifier_secret_flag(parts[2]);
		if (!keyed) {
			return std::unexpected{keyed.error()};
		}
		parsed.uses_verifier_secret = *keyed;
	} else {
		return std::unexpected{"password std::hash: unsupported algorithm"};
	}

	auto salt = decode_required_b64url(parts[3], "salt");
	if (!salt) {
		return std::unexpected{salt.error()};
	}
	auto hash = decode_required_b64url(parts[4], "hash");
	if (!hash) {
		return std::unexpected{hash.error()};
	}
	if (salt->size() > kMaxSaltBytes || hash->size() > kMaxHashBytes) {
		return std::unexpected{"password std::hash: salt/std::hash too large"};
	}
	parsed.salt = std::move(*salt);
	parsed.hash = std::move(*hash);
	if (parsed.hash_bytes == 0U) {
		parsed.hash_bytes = static_cast<std::uint32_t>(parsed.hash.size());
	}
	if (parsed.hash_bytes != parsed.hash.size()) {
		return std::unexpected{"password std::hash: encoded std::hash length does not match parameters"};
	}
	return parsed;
}

[[nodiscard]] std::expected<void, std::string> validate_options(
	PasswordHashOptions const &opts) {
	if (opts.salt_bytes == 0U || opts.salt_bytes > kMaxSaltBytes) {
		return std::unexpected{"password std::hash: invalid salt std::byte count"};
	}
	if (opts.hash_bytes == 0U || opts.hash_bytes > kMaxHashBytes) {
		return std::unexpected{"password std::hash: invalid std::hash std::byte count"};
	}
	if (opts.algorithm == PasswordHashAlgorithm::argon2id) {
		if (opts.memory_kib == 0U || opts.memory_kib > kMaxArgon2MemoryKiB) {
			return std::unexpected{"password std::hash: invalid argon2id memory cost"};
		}
		if (opts.iterations == 0U || opts.iterations > kMaxArgon2Iterations) {
			return std::unexpected{"password std::hash: invalid argon2id iteration count"};
		}
		if (opts.parallelism == 0U || opts.parallelism > kMaxArgon2Parallelism) {
			return std::unexpected{"password std::hash: invalid argon2id parallelism"};
		}
	} else if (opts.algorithm == PasswordHashAlgorithm::pbkdf2_sha256) {
		if (opts.iterations == 0U || opts.iterations > kMaxPbkdf2Iterations) {
			return std::unexpected{"password std::hash: invalid pbkdf2-sha256 iteration count"};
		}
	} else {
		return std::unexpected{"password std::hash: unknown algorithm"};
	}
	return {};
}

[[nodiscard]] std::span<unsigned char const> bytes_view(
	std::string_view s) noexcept {
	return {reinterpret_cast<unsigned char const *>(s.data()), s.size()};
}

[[nodiscard]] std::string encode_b64url(
	std::span<unsigned char const> bytes) {
	return conflux::crypto::base64url_encode(bytes);
}

[[nodiscard]] std::array<unsigned char, 32> pbkdf2_block(
	conflux::crypto::HmacSha256Key const &password_key,
	std::span<unsigned char const> salt,
	std::uint32_t iterations,
	std::uint32_t block_index) noexcept {
	std::array<unsigned char, 4> block_suffix{
		static_cast<unsigned char>((block_index >> 24U) & 0xFFU),
		static_cast<unsigned char>((block_index >> 16U) & 0xFFU),
		static_cast<unsigned char>((block_index >> 8U) & 0xFFU),
		static_cast<unsigned char>(block_index & 0xFFU),
	};
	auto u = conflux::crypto::hmac_sha256_precomputed(password_key, salt, block_suffix);
	std::array<unsigned char, 32> out = u;
	for (std::uint32_t i = 1; i < iterations; ++i) {
		u = conflux::crypto::hmac_sha256_precomputed(password_key, u);
		for (std::size_t j = 0; j < out.size(); ++j) {
			out[j] = static_cast<unsigned char>(out[j] ^ u[j]);
		}
	}
	return out;
}

[[nodiscard]] std::expected<std::string, std::string> pbkdf2_sha256(
	std::string_view password,
	std::string_view salt,
	std::uint32_t iterations,
	std::uint32_t hash_bytes) {
	if (iterations == 0U || iterations > kMaxPbkdf2Iterations) {
		return std::unexpected{"password std::hash: invalid pbkdf2-sha256 iteration count"};
	}
	if (hash_bytes == 0U || hash_bytes > kMaxHashBytes) {
		return std::unexpected{"password std::hash: invalid pbkdf2-sha256 std::hash std::byte count"};
	}
	std::vector<unsigned char> out(hash_bytes);
	std::span<unsigned char const> salt_bytes = bytes_view(salt);
	auto password_key = conflux::crypto::hmac_sha256_key(bytes_view(password));
	std::uint32_t block_index = 1U;
	std::size_t filled = 0;
	while (filled < out.size()) {
		auto block = pbkdf2_block(password_key, salt_bytes, iterations, block_index++);
		std::size_t const n = std::min(block.size(), out.size() - filled);
		std::ranges::copy(std::span{block}.first(n), out.begin() + static_cast<std::ptrdiff_t>(filled));
		filled += n;
	}
	return std::string{reinterpret_cast<char const *>(out.data()), out.size()};
}

[[nodiscard]] std::expected<std::string, std::string> apply_verifier_secret(
	std::string &&raw,
	std::string_view verifier_secret,
	std::uint32_t hash_bytes) {
	if (verifier_secret.empty()) {
		return std::move(raw);
	}
	if (hash_bytes == 0U || hash_bytes > kMaxHashBytes) {
		return std::unexpected{"password std::hash: invalid verifier-secret output std::byte count"};
	}
	auto key = conflux::crypto::hmac_sha256_key(bytes_view(verifier_secret));
	std::span<unsigned char const> raw_bytes = bytes_view(raw);
	std::vector<unsigned char> out(hash_bytes);
	std::uint32_t block_index = 1U;
	std::size_t filled = 0;
	while (filled < out.size()) {
		std::array<unsigned char, 4> suffix{
			static_cast<unsigned char>((block_index >> 24U) & 0xFFU),
			static_cast<unsigned char>((block_index >> 16U) & 0xFFU),
			static_cast<unsigned char>((block_index >> 8U) & 0xFFU),
			static_cast<unsigned char>(block_index & 0xFFU),
		};
		auto block = conflux::crypto::hmac_sha256_precomputed(key, raw_bytes, suffix);
		std::size_t const n = std::min(block.size(), out.size() - filled);
		std::ranges::copy(std::span{block}.first(n), out.begin() + static_cast<std::ptrdiff_t>(filled));
		filled += n;
		++block_index;
	}
	return std::string{reinterpret_cast<char const *>(out.data()), out.size()};
}

[[nodiscard]] std::expected<std::string, std::string> argon2id_raw(
	std::string_view password,
	std::string_view salt,
	std::uint32_t memory_kib,
	std::uint32_t iterations,
	std::uint32_t parallelism,
	std::uint32_t hash_bytes) {
	Argon2Api const &api = argon2_api();
	if (api.hash_raw == nullptr) {
		return std::unexpected{
			"password std::hash: Argon2id unavailable (backend not configured or libargon2 unavailable)"};
	}
	if (password.size() > kMaxPasswordBytes) {
		return std::unexpected{"password std::hash: password too large"};
	}
	std::vector<unsigned char> out(hash_bytes);
	int const rc = api.hash_raw(
		iterations,
		memory_kib,
		parallelism,
		password.data(),
		password.size(),
		salt.data(),
		salt.size(),
		out.data(),
		out.size());
	if (rc != 0) {
		std::string_view msg = "argon2id failed";
		if (api.error_message != nullptr) {
			msg = api.error_message(rc);
		}
		return std::unexpected{std::format("password std::hash: {}", msg)};
	}
	return std::string{reinterpret_cast<char const *>(out.data()), out.size()};
}

[[nodiscard]] std::expected<std::string, std::string> derive_hash(
	std::string_view password,
	std::string_view salt,
	PasswordHashOptions const &opts,
	PasswordHashSecrets const &secrets) {
	if (auto valid = validate_options(opts); !valid) {
		return std::unexpected{valid.error()};
	}
	if (password.size() > kMaxPasswordBytes) {
		return std::unexpected{"password std::hash: password too large"};
	}
	std::expected<std::string, std::string> raw = std::unexpected{"password std::hash: unknown algorithm"};
	if (opts.algorithm == PasswordHashAlgorithm::argon2id) {
		raw = argon2id_raw(password, salt, opts.memory_kib, opts.iterations, opts.parallelism, opts.hash_bytes);
	} else {
		raw = pbkdf2_sha256(password, salt, opts.iterations, opts.hash_bytes);
	}
	if (!raw) {
		return std::unexpected{raw.error()};
	}
	if (secrets.verifier_secret.size() > kMaxVerifierSecretBytes) {
		return std::unexpected{"password std::hash: verifier secret too large"};
	}
	return apply_verifier_secret(std::move(*raw), secrets.verifier_secret, opts.hash_bytes);
}

[[nodiscard]] std::expected<std::string, std::string> derive_hash(
	std::string_view password,
	ParsedHash const &parsed,
	PasswordHashSecrets const &secrets) {
	if (parsed.uses_verifier_secret && secrets.verifier_secret.empty()) {
		return std::unexpected{"password std::hash: verifier secret required"};
	}
	PasswordHashOptions opts;
	opts.algorithm = parsed.algorithm;
	opts.memory_kib = parsed.memory_kib;
	opts.iterations = parsed.iterations;
	opts.parallelism = parsed.parallelism;
	opts.salt_bytes = static_cast<std::uint32_t>(parsed.salt.size());
	opts.hash_bytes = static_cast<std::uint32_t>(parsed.hash.size());
	return derive_hash(password, parsed.salt, opts, secrets);
}

[[nodiscard]] bool parameters_match(
	ParsedHash const &parsed,
	PasswordHashOptions const &opts,
	PasswordHashSecrets const &secrets) noexcept {
	if (parsed.algorithm != opts.algorithm
		|| parsed.hash.size() != opts.hash_bytes
		|| parsed.uses_verifier_secret != !secrets.verifier_secret.empty()) {
		return false;
	}
	if (parsed.algorithm == PasswordHashAlgorithm::argon2id) {
		return parsed.version == kArgon2Version
			&& parsed.memory_kib == opts.memory_kib
			&& parsed.iterations == opts.iterations
			&& parsed.parallelism == opts.parallelism;
	}
	return parsed.version == 1U && parsed.iterations == opts.iterations;
}

} // namespace conflux::http::password_hash_detail

export namespace conflux::http {

[[nodiscard]] bool password_hash_argon2id_available() noexcept {
	return password_hash_detail::argon2_api().hash_raw != nullptr;
}

[[nodiscard]] std::expected<void, std::string> password_hash_configure_resource_limits(
	PasswordHashResourceLimits limits) {
	return password_hash_detail::password_hash_gate().configure(limits);
}

[[nodiscard]] PasswordHashResourceLimits password_hash_resource_limits() {
	return password_hash_detail::password_hash_gate().current();
}

[[nodiscard]] PasswordHashOptions pbkdf2_sha256_password_hash_options(
	std::uint32_t iterations = password_hash_detail::kPbkdf2DefaultIterations) noexcept {
	PasswordHashOptions opts;
	opts.algorithm = PasswordHashAlgorithm::pbkdf2_sha256;
	opts.memory_kib = 0U;
	opts.iterations = iterations;
	opts.parallelism = 1U;
	opts.salt_bytes = 16U;
	opts.hash_bytes = 32U;
	return opts;
}

[[nodiscard]] std::expected<PasswordHashSecrets, std::string> password_hash_secrets_from_config(
	conflux::http::AuthSecretsConfig const &cfg,
	bool required = true) {
	auto secret =
		conflux::http::resolve_secret_source(cfg.password_verifier_secret, "password_verifier_secret", required);
	if (!secret) {
		return std::unexpected{secret.error()};
	}
	PasswordHashSecrets out{.verifier_secret = std::move(*secret)};
	if (!out.verifier_secret.empty()) {
		if (auto valid = conflux::http::validate_secret_bytes(
				out.verifier_secret,
				"password_verifier_secret",
				cfg.password_verifier_min_secret_bytes);
			!valid) {
			return std::unexpected{valid.error()};
		}
	}
	return out;
}

[[nodiscard]] std::expected<PasswordHashSecrets, std::string> password_hash_secrets_from_config(
	conflux::http::Config const &cfg,
	bool required = true) {
	return password_hash_secrets_from_config(cfg.auth_secrets, required);
}

[[nodiscard]] std::expected<std::string, std::string> password_hash_with_salt(
	std::string_view password,
	std::string_view salt,
	PasswordHashOptions const &opts = {},
	PasswordHashSecrets const &secrets = {}) {
	if (auto valid = password_hash_detail::validate_options(opts); !valid) {
		return std::unexpected{valid.error()};
	}
	if (salt.size() != opts.salt_bytes) {
		return std::unexpected{"password std::hash: supplied salt length does not match options"};
	}
	auto permit = password_hash_detail::password_hash_gate().acquire();
	if (!permit) {
		return std::unexpected{permit.error()};
	}
	auto raw = password_hash_detail::derive_hash(password, salt, opts, secrets);
	if (!raw) {
		return std::unexpected{raw.error()};
	}
	auto salt_b64 = password_hash_detail::encode_b64url(password_hash_detail::bytes_view(salt));
	auto hash_b64 = password_hash_detail::encode_b64url(password_hash_detail::bytes_view(*raw));
	std::string_view const key_flag = secrets.verifier_secret.empty() ? std::string_view{} : std::string_view{",k=1"};
	if (opts.algorithm == PasswordHashAlgorithm::argon2id) {
		return std::format(
			"$argon2id$v={}$m={},t={},p={}{}${}${}",
			password_hash_detail::kArgon2Version,
			opts.memory_kib,
			opts.iterations,
			opts.parallelism,
			key_flag,
			salt_b64,
			hash_b64);
	}
	return std::format(
		"$pbkdf2-sha256$v=1$i={},l={}{}${}${}",
		opts.iterations,
		opts.hash_bytes,
		key_flag,
		salt_b64,
		hash_b64);
}

[[nodiscard]] std::expected<std::string, std::string> password_hash(
	std::string_view password,
	PasswordHashOptions const &opts = {},
	PasswordHashSecrets const &secrets = {}) {
	if (auto valid = password_hash_detail::validate_options(opts); !valid) {
		return std::unexpected{valid.error()};
	}
	std::vector<unsigned char> salt(opts.salt_bytes);
	crypto_random_bytes(salt);
	std::string_view const salt_view{reinterpret_cast<char const *>(salt.data()), salt.size()};
	return password_hash_with_salt(password, salt_view, opts, secrets);
}

[[nodiscard]] std::expected<PasswordVerifyResult, std::string> password_verify(
	std::string_view password,
	std::string_view encoded,
	PasswordHashOptions const &current = {},
	PasswordHashSecrets const &secrets = {}) {
	auto parsed = password_hash_detail::parse_hash(encoded);
	if (!parsed) {
		return std::unexpected{parsed.error()};
	}
	auto permit = password_hash_detail::password_hash_gate().acquire();
	if (!permit) {
		return std::unexpected{permit.error()};
	}
	PasswordVerifyResult result;
	auto raw = password_hash_detail::derive_hash(password, *parsed, secrets);
	if (!raw) {
		return std::unexpected{raw.error()};
	}
	result.ok = conflux::crypto::constant_time_eq(parsed->hash, *raw);
	if (!result.ok
		&& parsed->algorithm == PasswordHashAlgorithm::pbkdf2_sha256
		&& !parsed->uses_verifier_secret
		&& !secrets.verifier_secret.empty()) {
		auto legacy = password_hash_detail::derive_hash(password, *parsed, PasswordHashSecrets{});
		if (!legacy) {
			return std::unexpected{legacy.error()};
		}
		result.ok = conflux::crypto::constant_time_eq(parsed->hash, *legacy);
	}
	result.needs_rehash = result.ok && !password_hash_detail::parameters_match(*parsed, current, secrets);
	return result;
}

[[nodiscard]] std::expected<bool, std::string> password_needs_rehash(
	std::string_view encoded,
	PasswordHashOptions const &current = {},
	PasswordHashSecrets const &secrets = {}) {
	auto parsed = password_hash_detail::parse_hash(encoded);
	if (!parsed) {
		return std::unexpected{parsed.error()};
	}
	return !password_hash_detail::parameters_match(*parsed, current, secrets);
}

} // namespace conflux::http
