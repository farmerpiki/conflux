// Crypto example: hash, HMAC, base64url, constant-time compare, AES-GCM.
//
// This is intentionally small but end-to-end: derive printable fingerprints,
// seal authenticated bytes, open them again, then show tamper rejection.
import conflux.crypto;
import conflux.types;
import std;

using std::println;

template<SZ N>
static span<unsigned char const> bytes(
	A<unsigned char, N> const &value) noexcept {
	return {value.data(), value.size()};
}

static span<unsigned char const> bytes(
	V<unsigned char> const &value) noexcept {
	return {value.data(), value.size()};
}

static S hex(
	span<unsigned char const> in) {
	S out;
	out.reserve(in.size() * 2);
	for (auto b: in) {
		out += format("{:02x}", static_cast<unsigned>(b));
	}
	return out;
}

static A<unsigned char, 32> fixed_key() {
	A<unsigned char, 32> key{};
	constexpr SV seed = "0123456789abcdef0123456789abcdef";
	for (SZ i = 0; i < key.size(); ++i) {
		key[i] = static_cast<unsigned char>(seed[i]);
	}
	return key;
}

static A<unsigned char, 12> fixed_iv() {
	A<unsigned char, 12> iv{};
	constexpr SV seed = "conflux-iv12";
	for (SZ i = 0; i < iv.size(); ++i) {
		iv[i] = static_cast<unsigned char>(seed[i]);
	}
	return iv;
}

int main() {
	constexpr SV payload = "conflux crypto example payload";
	constexpr SV aad = "route=/internal/seal;v=1";

	auto digest = sha256(to_unsigned_span(payload));
	auto mac = hmac_sha256(to_unsigned_span("example signing key"), to_unsigned_span(payload));

	println("sha256     {}", hex(bytes(digest)));
	println("hmac       {}", hex(bytes(mac)));
	println("b64url(mac) {}", base64url_encode(bytes(mac)));
	println("constant-time self check: {}", constant_time_eq(hex(bytes(mac)), hex(bytes(mac))));

	auto key = fixed_key();
	auto iv = fixed_iv();
	auto sealed = aes_gcm_encrypt(bytes(key), bytes(iv), to_unsigned_span(payload), to_unsigned_span(aad));
	if (!sealed) {
		println(cerr, "seal failed: {}", sealed.error());
		return 1;
	}
	println("sealed bytes: {}", sealed->size());

	auto opened = aes_gcm_decrypt(bytes(key), bytes(iv), bytes(*sealed), to_unsigned_span(aad));
	if (!opened) {
		println(cerr, "open failed: {}", opened.error());
		return 1;
	}
	println("opened: {}", SV{reinterpret_cast<char const *>(opened->data()), opened->size()});

	auto tampered = *sealed;
	tampered[0] = static_cast<unsigned char>(tampered[0] ^ 0x01U);
	auto rejected = aes_gcm_decrypt(bytes(key), bytes(iv), bytes(tampered), to_unsigned_span(aad));
	if (!rejected) {
		println("tamper rejected: {}", rejected.error());
	}
}
