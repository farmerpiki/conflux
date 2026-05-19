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

export enum class PasswordHashAlgorithm {
	argon2id,
	pbkdf2_sha256,
};

export struct PasswordHashOptions {
	PasswordHashAlgorithm algorithm{PasswordHashAlgorithm::argon2id};
	std::uint32_t memory_kib{64U * 1024U};
	std::uint32_t iterations{3U};
	std::uint32_t parallelism{1U};
	std::uint32_t salt_bytes{16U};
	std::uint32_t hash_bytes{32U};
};

export struct PasswordHashSecrets {
	std::string verifier_secret{};
};

export struct PasswordHashResourceLimits {
	// 0 means use the library default: min(max(hardware_concurrency / 2, 1), 4).
	std::uint32_t max_concurrent_hashes{0U};
	// Requests beyond active hashes wait up to this many queued callers; 0 means fail fast.
	std::uint32_t max_waiting_hashes{64U};
};

export struct PasswordVerifyResult {
	bool ok{false};
	bool needs_rehash{false};
};

namespace password_hash_detail {

constexpr std::uint32_t kArgon2Version = 0x13U;
constexpr std::uint32_t kPbkdf2DefaultIterations = 600'000U;
constexpr std::uint32_t kMaxPasswordBytes = static_cast<std::uint32_t>(1U << 30U);
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
	std::uint32_t const hw = max(1U, std::thread::hardware_concurrency());
	return min(4U, max(1U, hw / 2U));
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
		: gate(exchange(other.gate, nullptr)) {}
	HashPermit &operator =(HashPermit &&other) noexcept;
	~HashPermit();
};

struct PasswordHashGate {
	mutex mtx;
	std::condition_variable cv;
	std::uint32_t max_concurrent{default_hash_concurrency()};
	std::uint32_t max_waiting{64U};
	std::uint32_t active{};
	std::uint32_t waiting{};

	[[nodiscard]] expected<HashPermit, std::string> acquire() {
		std::unique_lock lock{mtx};
		if (active < max_concurrent) {
			++active;
			return HashPermit{this};
		}
		if (waiting >= max_waiting) {
			return unexpected{"password hash: concurrency limit reached"};
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

	[[nodiscard]] expected<void, std::string> configure(
		PasswordHashResourceLimits limits) {
		std::uint32_t const concurrency =
			limits.max_concurrent_hashes == 0U ? default_hash_concurrency() : limits.max_concurrent_hashes;
		if (concurrency == 0U || concurrency > kMaxHashConcurrency) {
			return unexpected{"password hash: invalid max_concurrent_hashes"};
		}
		if (limits.max_waiting_hashes > kMaxHashWaiters) {
			return unexpected{"password hash: invalid max_waiting_hashes"};
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
		gate = exchange(other.gate, nullptr);
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
	auto [ptr, ec] = from_chars(begin, end, value);
	if (ec != errc{} || ptr != end) {
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
	return nullopt;
}

[[nodiscard]] expected<bool, std::string> parse_verifier_secret_flag(
	std::string_view params) {
	auto keyed = param_value(params, "k");
	if (!keyed) {
		return false;
	}
	std::uint32_t value{};
	if (!parse_u32(*keyed, value) || value > 1U) {
		return unexpected{"password hash: malformed verifier-secret parameter"};
	}
	return value == 1U;
}

[[nodiscard]] expected<std::string, std::string> decode_required_b64url(
	std::string_view encoded,
	std::string_view field) {
	if (encoded.empty()) {
		return unexpected{format("password hash: {} is empty", field)};
	}
	std::string decoded = base64url_decode(encoded);
	if (decoded.empty()) {
		return unexpected{format("password hash: {} is not valid base64url", field)};
	}
	return decoded;
}

[[nodiscard]] expected<ParsedHash, std::string> parse_hash(
	std::string_view encoded) {
	if (!encoded.starts_with('$')) {
		return unexpected{"password hash: expected modular crypt format"};
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
		return unexpected{"password hash: expected 5 fields"};
	}

	ParsedHash parsed{};
	if (parts[0] == "argon2id") {
		parsed.algorithm = PasswordHashAlgorithm::argon2id;
		auto version = param_value(parts[1], "v");
		if (!version || !parse_u32(*version, parsed.version) || parsed.version != kArgon2Version) {
			return unexpected{"password hash: unsupported argon2id version"};
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
			return unexpected{"password hash: malformed argon2id parameters"};
		}
		auto keyed = parse_verifier_secret_flag(parts[2]);
		if (!keyed) {
			return unexpected{keyed.error()};
		}
		parsed.uses_verifier_secret = *keyed;
	} else if (parts[0] == "pbkdf2-sha256") {
		parsed.algorithm = PasswordHashAlgorithm::pbkdf2_sha256;
		auto version = param_value(parts[1], "v");
		if (!version || !parse_u32(*version, parsed.version) || parsed.version != 1U) {
			return unexpected{"password hash: unsupported pbkdf2-sha256 version"};
		}
		auto iterations = param_value(parts[2], "i");
		auto hash_bytes = param_value(parts[2], "l");
		if (!iterations
			|| !hash_bytes
			|| !parse_u32(*iterations, parsed.iterations)
			|| !parse_u32(*hash_bytes, parsed.hash_bytes)) {
			return unexpected{"password hash: malformed pbkdf2-sha256 parameters"};
		}
		auto keyed = parse_verifier_secret_flag(parts[2]);
		if (!keyed) {
			return unexpected{keyed.error()};
		}
		parsed.uses_verifier_secret = *keyed;
	} else {
		return unexpected{"password hash: unsupported algorithm"};
	}

	auto salt = decode_required_b64url(parts[3], "salt");
	if (!salt) {
		return unexpected{salt.error()};
	}
	auto hash = decode_required_b64url(parts[4], "hash");
	if (!hash) {
		return unexpected{hash.error()};
	}
	if (salt->size() > kMaxSaltBytes || hash->size() > kMaxHashBytes) {
		return unexpected{"password hash: salt/hash too large"};
	}
	parsed.salt = move(*salt);
	parsed.hash = move(*hash);
	if (parsed.hash_bytes == 0U) {
		parsed.hash_bytes = static_cast<std::uint32_t>(parsed.hash.size());
	}
	if (parsed.hash_bytes != parsed.hash.size()) {
		return unexpected{"password hash: encoded hash length does not match parameters"};
	}
	return parsed;
}

[[nodiscard]] expected<void, std::string> validate_options(
	PasswordHashOptions const &opts) {
	if (opts.salt_bytes == 0U || opts.salt_bytes > kMaxSaltBytes) {
		return unexpected{"password hash: invalid salt byte count"};
	}
	if (opts.hash_bytes == 0U || opts.hash_bytes > kMaxHashBytes) {
		return unexpected{"password hash: invalid hash byte count"};
	}
	if (opts.algorithm == PasswordHashAlgorithm::argon2id) {
		if (opts.memory_kib == 0U || opts.memory_kib > kMaxArgon2MemoryKiB) {
			return unexpected{"password hash: invalid argon2id memory cost"};
		}
		if (opts.iterations == 0U || opts.iterations > kMaxArgon2Iterations) {
			return unexpected{"password hash: invalid argon2id iteration count"};
		}
		if (opts.parallelism == 0U || opts.parallelism > kMaxArgon2Parallelism) {
			return unexpected{"password hash: invalid argon2id parallelism"};
		}
	} else if (opts.algorithm == PasswordHashAlgorithm::pbkdf2_sha256) {
		if (opts.iterations == 0U || opts.iterations > kMaxPbkdf2Iterations) {
			return unexpected{"password hash: invalid pbkdf2-sha256 iteration count"};
		}
	} else {
		return unexpected{"password hash: unknown algorithm"};
	}
	return {};
}

[[nodiscard]] span<unsigned char const> bytes_view(
	std::string_view s) noexcept {
	return {reinterpret_cast<unsigned char const *>(s.data()), s.size()};
}

[[nodiscard]] std::string encode_b64url(
	span<unsigned char const> bytes) {
	return base64url_encode(bytes);
}

struct Sha256State {
	std::array<std::uint32_t, 8> h{};
	std::array<unsigned char, 64> pending{};
	std::uint64_t bytes{};
	std::size_t pending_size{};
};

[[nodiscard]] std::uint32_t rotr32(
	std::uint32_t v,
	std::uint32_t n) noexcept {
	return (v >> n) | (v << (32U - n));
}

void sha256_compress(
	std::array<std::uint32_t, 8> &h,
	unsigned char const *block) noexcept {
	static constexpr std::array<std::uint32_t, 64> K{
		0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
		0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
		0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
		0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
		0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
		0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
		0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
		0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
	};

	std::array<std::uint32_t, 64> w{};
	for (std::size_t i = 0; i < 16; ++i) {
		std::size_t const off = i * 4U;
		w[i] = (static_cast<std::uint32_t>(block[off]) << 24U)
			 | (static_cast<std::uint32_t>(block[off + 1U]) << 16U)
			 | (static_cast<std::uint32_t>(block[off + 2U]) << 8U)
			 | static_cast<std::uint32_t>(block[off + 3U]);
	}
	for (std::size_t i = 16; i < 64; ++i) {
		std::uint32_t const s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
		std::uint32_t const s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
		w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
	}

	auto [a, b, c, d, e, f, g, hh] = h;
	for (std::size_t i = 0; i < 64; ++i) {
		std::uint32_t const ch = (e & f) ^ (~e & g);
		std::uint32_t const maj = (a & b) ^ (a & c) ^ (b & c);
		std::uint32_t const s1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
		std::uint32_t const s0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
		std::uint32_t const t1 = hh + s1 + ch + K[i] + w[i];
		std::uint32_t const t2 = s0 + maj;
		hh = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += hh;
}

[[nodiscard]] Sha256State sha256_init() noexcept {
	return Sha256State{
		.h = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U}
    };
}

void sha256_update(
	Sha256State &state,
	span<unsigned char const> msg) noexcept {
	state.bytes += static_cast<std::uint64_t>(msg.size());
	if (state.pending_size != 0U) {
		std::size_t const n = min(msg.size(), state.pending.size() - state.pending_size);
		std::ranges::copy(msg.first(n), state.pending.begin() + static_cast<std::ptrdiff_t>(state.pending_size));
		state.pending_size += n;
		msg = msg.subspan(n);
		if (state.pending_size == state.pending.size()) {
			sha256_compress(state.h, state.pending.data());
			state.pending_size = 0U;
		}
	}
	while (msg.size() >= 64U) {
		sha256_compress(state.h, msg.data());
		msg = msg.subspan(64U);
	}
	if (!msg.empty()) {
		std::ranges::copy(msg, state.pending.begin());
		state.pending_size = msg.size();
	}
}

[[nodiscard]] std::array<unsigned char, 32> sha256_final(
	Sha256State state) noexcept {
	std::uint64_t const bit_len = state.bytes * 8ULL;
	state.pending[state.pending_size++] = 0x80U;
	if (state.pending_size > 56U) {
		for (std::size_t i = state.pending_size; i < 64U; ++i) {
			state.pending[i] = 0U;
		}
		sha256_compress(state.h, state.pending.data());
		state.pending_size = 0U;
	}
	for (std::size_t i = state.pending_size; i < 56U; ++i) {
		state.pending[i] = 0U;
	}
	for (std::size_t i = 0; i < 8U; ++i) {
		state.pending[56U + i] = static_cast<unsigned char>((bit_len >> (56U - (i * 8U))) & 0xFFU);
	}
	sha256_compress(state.h, state.pending.data());
	std::array<unsigned char, 32> out{};
	for (std::size_t i = 0; i < 32U; ++i) {
		out[i] = static_cast<unsigned char>((state.h[i / 4U] >> (24U - ((i % 4U) * 8U))) & 0xFFU);
	}
	return out;
}

[[nodiscard]] std::array<unsigned char, 32> sha256_noalloc(
	span<unsigned char const> msg) noexcept {
	auto state = sha256_init();
	sha256_update(state, msg);
	return sha256_final(state);
}

struct HmacSha256Key {
	std::array<unsigned char, 64> inner{};
	std::array<unsigned char, 64> outer{};
};

[[nodiscard]] HmacSha256Key hmac_sha256_key(
	span<unsigned char const> key) noexcept {
	std::array<unsigned char, 64> kpad{};
	if (key.size() > 64U) {
		auto hashed = sha256_noalloc(key);
		std::ranges::copy(hashed, kpad.begin());
	} else {
		std::ranges::copy(key, kpad.begin());
	}
	HmacSha256Key out{};
	for (std::size_t i = 0; i < kpad.size(); ++i) {
		out.inner[i] = static_cast<unsigned char>(kpad[i] ^ 0x36U);
		out.outer[i] = static_cast<unsigned char>(kpad[i] ^ 0x5CU);
	}
	return out;
}

[[nodiscard]] std::array<unsigned char, 32> hmac_sha256_noalloc(
	HmacSha256Key const &key,
	span<unsigned char const> a,
	span<unsigned char const> b = {}) noexcept {
	auto inner = sha256_init();
	sha256_update(inner, key.inner);
	sha256_update(inner, a);
	sha256_update(inner, b);
	auto inner_digest = sha256_final(inner);
	auto outer = sha256_init();
	sha256_update(outer, key.outer);
	sha256_update(outer, inner_digest);
	return sha256_final(outer);
}

[[nodiscard]] std::array<unsigned char, 32> pbkdf2_block(
	HmacSha256Key const &password_key,
	span<unsigned char const> salt,
	std::uint32_t iterations,
	std::uint32_t block_index) noexcept {
	std::array<unsigned char, 4> block_suffix{
		static_cast<unsigned char>((block_index >> 24U) & 0xFFU),
		static_cast<unsigned char>((block_index >> 16U) & 0xFFU),
		static_cast<unsigned char>((block_index >> 8U) & 0xFFU),
		static_cast<unsigned char>(block_index & 0xFFU),
	};
	auto u = hmac_sha256_noalloc(password_key, salt, block_suffix);
	std::array<unsigned char, 32> out = u;
	for (std::uint32_t i = 1; i < iterations; ++i) {
		u = hmac_sha256_noalloc(password_key, u);
		for (std::size_t j = 0; j < out.size(); ++j) {
			out[j] = static_cast<unsigned char>(out[j] ^ u[j]);
		}
	}
	return out;
}

[[nodiscard]] expected<std::string, std::string> pbkdf2_sha256(
	std::string_view password,
	std::string_view salt,
	std::uint32_t iterations,
	std::uint32_t hash_bytes) {
	if (iterations == 0U || iterations > kMaxPbkdf2Iterations) {
		return unexpected{"password hash: invalid pbkdf2-sha256 iteration count"};
	}
	if (hash_bytes == 0U || hash_bytes > kMaxHashBytes) {
		return unexpected{"password hash: invalid pbkdf2-sha256 hash byte count"};
	}
	std::vector<unsigned char> out(hash_bytes);
	span<unsigned char const> salt_bytes = bytes_view(salt);
	auto password_key = hmac_sha256_key(bytes_view(password));
	std::uint32_t block_index = 1U;
	std::size_t filled = 0;
	while (filled < out.size()) {
		auto block = pbkdf2_block(password_key, salt_bytes, iterations, block_index++);
		std::size_t const n = min(block.size(), out.size() - filled);
		std::ranges::copy(span{block}.first(n), out.begin() + static_cast<std::ptrdiff_t>(filled));
		filled += n;
	}
	return std::string{reinterpret_cast<char const *>(out.data()), out.size()};
}

[[nodiscard]] expected<std::string, std::string> apply_verifier_secret(
	std::string &&raw,
	std::string_view verifier_secret,
	std::uint32_t hash_bytes) {
	if (verifier_secret.empty()) {
		return move(raw);
	}
	if (hash_bytes == 0U || hash_bytes > kMaxHashBytes) {
		return unexpected{"password hash: invalid verifier-secret output byte count"};
	}
	auto key = hmac_sha256_key(bytes_view(verifier_secret));
	span<unsigned char const> raw_bytes = bytes_view(raw);
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
		auto block = hmac_sha256_noalloc(key, raw_bytes, suffix);
		std::size_t const n = min(block.size(), out.size() - filled);
		std::ranges::copy(span{block}.first(n), out.begin() + static_cast<std::ptrdiff_t>(filled));
		filled += n;
		++block_index;
	}
	return std::string{reinterpret_cast<char const *>(out.data()), out.size()};
}

[[nodiscard]] expected<std::string, std::string> argon2id_raw(
	std::string_view password,
	std::string_view salt,
	std::uint32_t memory_kib,
	std::uint32_t iterations,
	std::uint32_t parallelism,
	std::uint32_t hash_bytes) {
	Argon2Api const &api = argon2_api();
	if (api.hash_raw == nullptr) {
		return unexpected{"password hash: Argon2id unavailable (backend not configured or libargon2 unavailable)"};
	}
	if (password.size() > kMaxPasswordBytes) {
		return unexpected{"password hash: password too large"};
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
		return unexpected{format("password hash: {}", msg)};
	}
	return std::string{reinterpret_cast<char const *>(out.data()), out.size()};
}

[[nodiscard]] expected<std::string, std::string> derive_hash(
	std::string_view password,
	std::string_view salt,
	PasswordHashOptions const &opts,
	PasswordHashSecrets const &secrets) {
	if (auto valid = validate_options(opts); !valid) {
		return unexpected{valid.error()};
	}
	if (password.size() > kMaxPasswordBytes) {
		return unexpected{"password hash: password too large"};
	}
	expected<std::string, std::string> raw = unexpected{"password hash: unknown algorithm"};
	if (opts.algorithm == PasswordHashAlgorithm::argon2id) {
		raw = argon2id_raw(password, salt, opts.memory_kib, opts.iterations, opts.parallelism, opts.hash_bytes);
	} else {
		raw = pbkdf2_sha256(password, salt, opts.iterations, opts.hash_bytes);
	}
	if (!raw) {
		return unexpected{raw.error()};
	}
	if (secrets.verifier_secret.size() > kMaxVerifierSecretBytes) {
		return unexpected{"password hash: verifier secret too large"};
	}
	return apply_verifier_secret(move(*raw), secrets.verifier_secret, opts.hash_bytes);
}

[[nodiscard]] expected<std::string, std::string> derive_hash(
	std::string_view password,
	ParsedHash const &parsed,
	PasswordHashSecrets const &secrets) {
	if (parsed.uses_verifier_secret && secrets.verifier_secret.empty()) {
		return unexpected{"password hash: verifier secret required"};
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

} // namespace password_hash_detail

export [[nodiscard]] bool password_hash_argon2id_available() noexcept {
	return password_hash_detail::argon2_api().hash_raw != nullptr;
}

export [[nodiscard]] expected<void, std::string> password_hash_configure_resource_limits(
	PasswordHashResourceLimits limits) {
	return password_hash_detail::password_hash_gate().configure(limits);
}

export [[nodiscard]] PasswordHashResourceLimits password_hash_resource_limits() {
	return password_hash_detail::password_hash_gate().current();
}

export [[nodiscard]] PasswordHashOptions pbkdf2_sha256_password_hash_options(
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

export [[nodiscard]] expected<PasswordHashSecrets, std::string> password_hash_secrets_from_config(
	AuthSecretsConfig const &cfg,
	bool required = true) {
	auto secret = resolve_secret_source(cfg.password_verifier_secret, "password_verifier_secret", required);
	if (!secret) {
		return unexpected{secret.error()};
	}
	PasswordHashSecrets out{.verifier_secret = move(*secret)};
	if (!out.verifier_secret.empty()) {
		if (auto valid = validate_secret_bytes(
				out.verifier_secret,
				"password_verifier_secret",
				cfg.password_verifier_min_secret_bytes);
			!valid) {
			return unexpected{valid.error()};
		}
	}
	return out;
}

export [[nodiscard]] expected<PasswordHashSecrets, std::string> password_hash_secrets_from_config(
	Config const &cfg,
	bool required = true) {
	return password_hash_secrets_from_config(cfg.auth_secrets, required);
}

export [[nodiscard]] expected<std::string, std::string> password_hash_with_salt(
	std::string_view password,
	std::string_view salt,
	PasswordHashOptions const &opts = {},
	PasswordHashSecrets const &secrets = {}) {
	if (auto valid = password_hash_detail::validate_options(opts); !valid) {
		return unexpected{valid.error()};
	}
	if (salt.size() != opts.salt_bytes) {
		return unexpected{"password hash: supplied salt length does not match options"};
	}
	auto permit = password_hash_detail::password_hash_gate().acquire();
	if (!permit) {
		return unexpected{permit.error()};
	}
	auto raw = password_hash_detail::derive_hash(password, salt, opts, secrets);
	if (!raw) {
		return unexpected{raw.error()};
	}
	auto salt_b64 = password_hash_detail::encode_b64url(password_hash_detail::bytes_view(salt));
	auto hash_b64 = password_hash_detail::encode_b64url(password_hash_detail::bytes_view(*raw));
	std::string_view const key_flag = secrets.verifier_secret.empty() ? std::string_view{} : std::string_view{",k=1"};
	if (opts.algorithm == PasswordHashAlgorithm::argon2id) {
		return format(
			"$argon2id$v={}$m={},t={},p={}{}${}${}",
			password_hash_detail::kArgon2Version,
			opts.memory_kib,
			opts.iterations,
			opts.parallelism,
			key_flag,
			salt_b64,
			hash_b64);
	}
	return format(
		"$pbkdf2-sha256$v=1$i={},l={}{}${}${}",
		opts.iterations,
		opts.hash_bytes,
		key_flag,
		salt_b64,
		hash_b64);
}

export [[nodiscard]] expected<std::string, std::string> password_hash(
	std::string_view password,
	PasswordHashOptions const &opts = {},
	PasswordHashSecrets const &secrets = {}) {
	if (auto valid = password_hash_detail::validate_options(opts); !valid) {
		return unexpected{valid.error()};
	}
	std::vector<unsigned char> salt(opts.salt_bytes);
	crypto_random_bytes(salt);
	std::string_view const salt_view{reinterpret_cast<char const *>(salt.data()), salt.size()};
	return password_hash_with_salt(password, salt_view, opts, secrets);
}

export [[nodiscard]] expected<PasswordVerifyResult, std::string> password_verify(
	std::string_view password,
	std::string_view encoded,
	PasswordHashOptions const &current = {},
	PasswordHashSecrets const &secrets = {}) {
	auto parsed = password_hash_detail::parse_hash(encoded);
	if (!parsed) {
		return unexpected{parsed.error()};
	}
	auto permit = password_hash_detail::password_hash_gate().acquire();
	if (!permit) {
		return unexpected{permit.error()};
	}
	PasswordVerifyResult result;
	auto raw = password_hash_detail::derive_hash(password, *parsed, secrets);
	if (!raw) {
		return unexpected{raw.error()};
	}
	result.ok = constant_time_eq(parsed->hash, *raw);
	if (!result.ok
		&& parsed->algorithm == PasswordHashAlgorithm::pbkdf2_sha256
		&& !parsed->uses_verifier_secret
		&& !secrets.verifier_secret.empty()) {
		auto legacy = password_hash_detail::derive_hash(password, *parsed, PasswordHashSecrets{});
		if (!legacy) {
			return unexpected{legacy.error()};
		}
		result.ok = constant_time_eq(parsed->hash, *legacy);
	}
	result.needs_rehash = result.ok && !password_hash_detail::parameters_match(*parsed, current, secrets);
	return result;
}

export [[nodiscard]] expected<bool, std::string> password_needs_rehash(
	std::string_view encoded,
	PasswordHashOptions const &current = {},
	PasswordHashSecrets const &secrets = {}) {
	auto parsed = password_hash_detail::parse_hash(encoded);
	if (!parsed) {
		return unexpected{parsed.error()};
	}
	return !password_hash_detail::parameters_match(*parsed, current, secrets);
}
