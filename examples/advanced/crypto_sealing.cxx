// Crypto example: hash, HMAC, base64url, constant-time compare, AES-GCM.
//
// This is intentionally small but end-to-end: derive printable fingerprints,
// seal authenticated bytes, open them again, then show tamper rejection.
import conflux.crypto;
import conflux.types;
import std;

template<std::size_t N>
static std::span<unsigned char const> bytes(
	std::array<unsigned char, N> const &value) noexcept {
	return {value.data(), value.size()};
}

static std::span<unsigned char const> bytes(
	std::vector<unsigned char> const &value) noexcept {
	return {value.data(), value.size()};
}

static std::string hex(
	std::span<unsigned char const> in) {
	std::string out;
	out.reserve(in.size() * 2);
	for (auto b: in) {
		out += std::format("{:02x}", static_cast<unsigned>(b));
	}
	return out;
}

static std::array<unsigned char, 32> fixed_key() {
	std::array<unsigned char, 32> key{};
	constexpr std::string_view seed = "0123456789abcdef0123456789abcdef";
	for (std::size_t i = 0; i < key.size(); ++i) {
		key[i] = static_cast<unsigned char>(seed[i]);
	}
	return key;
}

static std::array<unsigned char, 12> fixed_iv() {
	std::array<unsigned char, 12> iv{};
	constexpr std::string_view seed = "conflux-iv12";
	for (std::size_t i = 0; i < iv.size(); ++i) {
		iv[i] = static_cast<unsigned char>(seed[i]);
	}
	return iv;
}

int main() {
	constexpr std::string_view payload = "conflux crypto example payload";
	constexpr std::string_view aad = "route=/internal/seal;v=1";

	auto digest = conflux::crypto::sha256(conflux::crypto::to_unsigned_span(payload));
	auto mac = conflux::crypto::hmac_sha256(
		conflux::crypto::to_unsigned_span("example signing key"),
		conflux::crypto::to_unsigned_span(payload));

	std::println("sha256     {}", hex(bytes(digest)));
	std::println("hmac       {}", hex(bytes(mac)));
	std::println("b64url(mac) {}", conflux::crypto::base64url_encode(bytes(mac)));
	std::println("constant-time self check: {}", conflux::crypto::constant_time_eq(hex(bytes(mac)), hex(bytes(mac))));

	auto key = fixed_key();
	auto iv = fixed_iv();
	auto sealed = conflux::crypto::aes_gcm_encrypt(
		bytes(key),
		bytes(iv),
		conflux::crypto::to_unsigned_span(payload),
		conflux::crypto::to_unsigned_span(aad));
	if (!sealed) {
		std::println(std::cerr, "seal failed: {}", sealed.error());
		return 1;
	}
	std::println("sealed bytes: {}", sealed->size());

	auto opened =
		conflux::crypto::aes_gcm_decrypt(bytes(key), bytes(iv), bytes(*sealed), conflux::crypto::to_unsigned_span(aad));
	if (!opened) {
		std::println(std::cerr, "open failed: {}", opened.error());
		return 1;
	}
	std::println("opened: {}", std::string_view{reinterpret_cast<char const *>(opened->data()), opened->size()});

	auto tampered = *sealed;
	if (tampered.empty()) {
		std::println(std::cerr, "seal produced no bytes");
		return 1;
	}
	tampered.front() = static_cast<unsigned char>(tampered.front() ^ 0x01U);
	auto rejected = conflux::crypto::aes_gcm_decrypt(
		bytes(key),
		bytes(iv),
		bytes(tampered),
		conflux::crypto::to_unsigned_span(aad));
	if (!rejected) {
		std::println("tamper rejected: {}", rejected.error());
	}
}
