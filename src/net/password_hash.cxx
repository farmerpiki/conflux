module;
#include <dlfcn.h>

export module conflux.net.password_hash;
import std;
import conflux.types;
import conflux.crypto;
import conflux.utils;

export enum class PasswordHashAlgorithm {
	argon2id,
	pbkdf2_sha256,
};

export struct PasswordHashOptions {
	PasswordHashAlgorithm algorithm{PasswordHashAlgorithm::argon2id};
	u32 memory_kib{64U * 1024U};
	u32 iterations{3U};
	u32 parallelism{1U};
	u32 salt_bytes{16U};
	u32 hash_bytes{32U};
};

export struct PasswordVerifyResult {
	bool ok{false};
	bool needs_rehash{false};
};

namespace password_hash_detail {

constexpr u32 kArgon2Version = 0x13U;
constexpr u32 kPbkdf2DefaultIterations = 600'000U;
constexpr u32 kMaxPasswordBytes = static_cast<u32>(1U << 30U);
constexpr u32 kMaxSaltBytes = 1024U;
constexpr u32 kMaxHashBytes = 1024U;
constexpr u32 kMaxPbkdf2Iterations = 100'000'000U;
constexpr u32 kMaxArgon2Iterations = 1024U;
constexpr u32 kMaxArgon2MemoryKiB = 16U * 1024U * 1024U;
constexpr u32 kMaxArgon2Parallelism = 1024U;

struct ParsedHash {
	PasswordHashAlgorithm algorithm{};
	u32 version{};
	u32 memory_kib{};
	u32 iterations{};
	u32 parallelism{};
	u32 hash_bytes{};
	S salt{};
	S hash{};
};

using Argon2idHashRawFn = int (*)(
	u32 t_cost,
	u32 m_cost,
	u32 parallelism,
	void const *pwd,
	SZ pwdlen,
	void const *salt,
	SZ saltlen,
	void *hash,
	SZ hashlen);
using Argon2ErrorMessageFn = char const *(*)(int error_code);

struct Argon2Api {
	void *handle{};
	Argon2idHashRawFn hash_raw{};
	Argon2ErrorMessageFn error_message{};
};

[[nodiscard]] Argon2Api load_argon2() noexcept {
	static constexpr A<char const *, 2> kNames{"libargon2.so.1", "libargon2.so"};
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
}

[[nodiscard]] Argon2Api const &argon2_api() noexcept {
	static Argon2Api const api = load_argon2();
	return api;
}

[[nodiscard]] bool parse_u32(
	SV text,
	u32 &out) noexcept {
	if (text.empty()) {
		return false;
	}
	u32 value{};
	auto const *begin = text.data();
	auto const *end = text.data() + text.size();
	auto [ptr, ec] = from_chars(begin, end, value);
	if (ec != errc{} || ptr != end) {
		return false;
	}
	out = value;
	return true;
}

[[nodiscard]] Opt<SV> param_value(
	SV params,
	SV key) noexcept {
	SZ pos = 0;
	while (pos <= params.size()) {
		SZ const comma = params.find(',', pos);
		SV const part = comma == SV::npos ? params.substr(pos) : params.substr(pos, comma - pos);
		SZ const eq = part.find('=');
		if (eq != SV::npos && part.substr(0, eq) == key) {
			return part.substr(eq + 1);
		}
		if (comma == SV::npos) {
			break;
		}
		pos = comma + 1;
	}
	return nullopt;
}

[[nodiscard]] expected<S, S> decode_required_b64url(
	SV encoded,
	SV field) {
	if (encoded.empty()) {
		return unexpected{format("password hash: {} is empty", field)};
	}
	S decoded = base64url_decode(encoded);
	if (decoded.empty()) {
		return unexpected{format("password hash: {} is not valid base64url", field)};
	}
	return decoded;
}

[[nodiscard]] expected<ParsedHash, S> parse_hash(
	SV encoded) {
	if (!encoded.starts_with('$')) {
		return unexpected{"password hash: expected modular crypt format"};
	}
	V<SV> parts;
	SZ pos = 1;
	while (true) {
		SZ const next = encoded.find('$', pos);
		parts.push_back(next == SV::npos ? encoded.substr(pos) : encoded.substr(pos, next - pos));
		if (next == SV::npos) {
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
		if (!memory || !iterations || !parallelism
			|| !parse_u32(*memory, parsed.memory_kib)
			|| !parse_u32(*iterations, parsed.iterations)
			|| !parse_u32(*parallelism, parsed.parallelism)) {
			return unexpected{"password hash: malformed argon2id parameters"};
		}
	} else if (parts[0] == "pbkdf2-sha256") {
		parsed.algorithm = PasswordHashAlgorithm::pbkdf2_sha256;
		auto version = param_value(parts[1], "v");
		if (!version || !parse_u32(*version, parsed.version) || parsed.version != 1U) {
			return unexpected{"password hash: unsupported pbkdf2-sha256 version"};
		}
		auto iterations = param_value(parts[2], "i");
		auto hash_bytes = param_value(parts[2], "l");
		if (!iterations || !hash_bytes
			|| !parse_u32(*iterations, parsed.iterations)
			|| !parse_u32(*hash_bytes, parsed.hash_bytes)) {
			return unexpected{"password hash: malformed pbkdf2-sha256 parameters"};
		}
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
		parsed.hash_bytes = static_cast<u32>(parsed.hash.size());
	}
	if (parsed.hash_bytes != parsed.hash.size()) {
		return unexpected{"password hash: encoded hash length does not match parameters"};
	}
	return parsed;
}

[[nodiscard]] expected<void, S> validate_options(
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
	SV s) noexcept {
	return {reinterpret_cast<unsigned char const *>(s.data()), s.size()};
}

[[nodiscard]] S encode_b64url(
	span<unsigned char const> bytes) {
	return base64url_encode(bytes);
}

[[nodiscard]] A<unsigned char, 32> pbkdf2_block(
	SV password,
	span<unsigned char const> salt,
	u32 iterations,
	u32 block_index) {
	V<unsigned char> salt_block(salt.size() + 4U);
	ranges::copy(salt, salt_block.begin());
	salt_block[salt.size() + 0U] = static_cast<unsigned char>((block_index >> 24U) & 0xFFU);
	salt_block[salt.size() + 1U] = static_cast<unsigned char>((block_index >> 16U) & 0xFFU);
	salt_block[salt.size() + 2U] = static_cast<unsigned char>((block_index >> 8U) & 0xFFU);
	salt_block[salt.size() + 3U] = static_cast<unsigned char>(block_index & 0xFFU);

	auto u = hmac_sha256(bytes_view(password), salt_block);
	A<unsigned char, 32> out = u;
	for (u32 i = 1; i < iterations; ++i) {
		u = hmac_sha256(bytes_view(password), u);
		for (SZ j = 0; j < out.size(); ++j) {
			out[j] = static_cast<unsigned char>(out[j] ^ u[j]);
		}
	}
	return out;
}

[[nodiscard]] expected<S, S> pbkdf2_sha256(
	SV password,
	SV salt,
	u32 iterations,
	u32 hash_bytes) {
	if (iterations == 0U || iterations > kMaxPbkdf2Iterations) {
		return unexpected{"password hash: invalid pbkdf2-sha256 iteration count"};
	}
	if (hash_bytes == 0U || hash_bytes > kMaxHashBytes) {
		return unexpected{"password hash: invalid pbkdf2-sha256 hash byte count"};
	}
	V<unsigned char> out(hash_bytes);
	span<unsigned char const> salt_bytes = bytes_view(salt);
	u32 block_index = 1U;
	SZ filled = 0;
	while (filled < out.size()) {
		auto block = pbkdf2_block(password, salt_bytes, iterations, block_index++);
		SZ const n = min(block.size(), out.size() - filled);
		ranges::copy(span{block}.first(n), out.begin() + static_cast<std::ptrdiff_t>(filled));
		filled += n;
	}
	return S{reinterpret_cast<char const *>(out.data()), out.size()};
}

[[nodiscard]] expected<S, S> argon2id_raw(
	SV password,
	SV salt,
	u32 memory_kib,
	u32 iterations,
	u32 parallelism,
	u32 hash_bytes) {
	Argon2Api const &api = argon2_api();
	if (api.hash_raw == nullptr) {
		return unexpected{"password hash: Argon2id unavailable (libargon2 runtime library not found)"};
	}
	if (password.size() > kMaxPasswordBytes) {
		return unexpected{"password hash: password too large"};
	}
	V<unsigned char> out(hash_bytes);
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
		SV msg = "argon2id failed";
		if (api.error_message != nullptr) {
			msg = api.error_message(rc);
		}
		return unexpected{format("password hash: {}", msg)};
	}
	return S{reinterpret_cast<char const *>(out.data()), out.size()};
}

[[nodiscard]] expected<S, S> derive_hash(
	SV password,
	SV salt,
	PasswordHashOptions const &opts) {
	if (auto valid = validate_options(opts); !valid) {
		return unexpected{valid.error()};
	}
	if (opts.algorithm == PasswordHashAlgorithm::argon2id) {
		return argon2id_raw(password, salt, opts.memory_kib, opts.iterations, opts.parallelism, opts.hash_bytes);
	}
	return pbkdf2_sha256(password, salt, opts.iterations, opts.hash_bytes);
}

[[nodiscard]] expected<S, S> derive_hash(
	SV password,
	ParsedHash const &parsed) {
	PasswordHashOptions opts;
	opts.algorithm = parsed.algorithm;
	opts.memory_kib = parsed.memory_kib;
	opts.iterations = parsed.iterations;
	opts.parallelism = parsed.parallelism;
	opts.salt_bytes = static_cast<u32>(parsed.salt.size());
	opts.hash_bytes = static_cast<u32>(parsed.hash.size());
	return derive_hash(password, parsed.salt, opts);
}

[[nodiscard]] bool parameters_match(
	ParsedHash const &parsed,
	PasswordHashOptions const &opts) noexcept {
	if (parsed.algorithm != opts.algorithm || parsed.hash.size() != opts.hash_bytes) {
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

export [[nodiscard]] PasswordHashOptions pbkdf2_sha256_password_hash_options(
	u32 iterations = password_hash_detail::kPbkdf2DefaultIterations) noexcept {
	PasswordHashOptions opts;
	opts.algorithm = PasswordHashAlgorithm::pbkdf2_sha256;
	opts.memory_kib = 0U;
	opts.iterations = iterations;
	opts.parallelism = 1U;
	opts.salt_bytes = 16U;
	opts.hash_bytes = 32U;
	return opts;
}

export [[nodiscard]] expected<S, S> password_hash_with_salt(
	SV password,
	SV salt,
	PasswordHashOptions const &opts = {}) {
	if (auto valid = password_hash_detail::validate_options(opts); !valid) {
		return unexpected{valid.error()};
	}
	if (salt.size() != opts.salt_bytes) {
		return unexpected{"password hash: supplied salt length does not match options"};
	}
	auto raw = password_hash_detail::derive_hash(password, salt, opts);
	if (!raw) {
		return unexpected{raw.error()};
	}
	auto salt_b64 = password_hash_detail::encode_b64url(password_hash_detail::bytes_view(salt));
	auto hash_b64 = password_hash_detail::encode_b64url(password_hash_detail::bytes_view(*raw));
	if (opts.algorithm == PasswordHashAlgorithm::argon2id) {
		return format(
			"$argon2id$v={}$m={},t={},p={}${}${}",
			password_hash_detail::kArgon2Version,
			opts.memory_kib,
			opts.iterations,
			opts.parallelism,
			salt_b64,
			hash_b64);
	}
	return format(
		"$pbkdf2-sha256$v=1$i={},l={}${}${}",
		opts.iterations,
		opts.hash_bytes,
		salt_b64,
		hash_b64);
}

export [[nodiscard]] expected<S, S> password_hash(
	SV password,
	PasswordHashOptions const &opts = {}) {
	if (auto valid = password_hash_detail::validate_options(opts); !valid) {
		return unexpected{valid.error()};
	}
	V<unsigned char> salt(opts.salt_bytes);
	crypto_random_bytes(salt);
	SV const salt_view{reinterpret_cast<char const *>(salt.data()), salt.size()};
	return password_hash_with_salt(password, salt_view, opts);
}

export [[nodiscard]] expected<PasswordVerifyResult, S> password_verify(
	SV password,
	SV encoded,
	PasswordHashOptions const &current = {}) {
	auto parsed = password_hash_detail::parse_hash(encoded);
	if (!parsed) {
		return unexpected{parsed.error()};
	}
	auto raw = password_hash_detail::derive_hash(password, *parsed);
	if (!raw) {
		return unexpected{raw.error()};
	}
	PasswordVerifyResult result;
	result.ok = constant_time_eq(parsed->hash, *raw);
	result.needs_rehash = result.ok && !password_hash_detail::parameters_match(*parsed, current);
	return result;
}

export [[nodiscard]] expected<bool, S> password_needs_rehash(
	SV encoded,
	PasswordHashOptions const &current = {}) {
	auto parsed = password_hash_detail::parse_hash(encoded);
	if (!parsed) {
		return unexpected{parsed.error()};
	}
	return !password_hash_detail::parameters_match(*parsed, current);
}
