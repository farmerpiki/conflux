// libFuzzer driver for cookie_signing.
// Invariants:
//   - sign -> verify round-trips with the same secret
//   - arbitrary signed values either verify to a sane value or fail without crashing

import std;
import conflux.types;
import conflux.net.cookie_signing;

using namespace std;

extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	if (size < 2 || size > 64U * 1024U) {
		return 0;
	}
	std::string_view input{reinterpret_cast<char const *>(data), size};
	std::string_view value;
	std::string_view secret;
	std::string_view arbitrary;
	auto const first = input.find('|');
	auto const second = first == std::string_view::npos ? std::string_view::npos : input.find('|', first + 1);
	if (first != std::string_view::npos && second != std::string_view::npos && second > first + 1) {
		value = input.substr(0, first);
		secret = input.substr(first + 1, second - first - 1);
		arbitrary = input.substr(second + 1);
	} else {
		auto const value_size = data[0] % 128U;
		auto const secret_size = 1U + (data[1] % 128U);
		if (size < 2U + value_size + secret_size) {
			return 0;
		}
		value = std::string_view{reinterpret_cast<char const *>(data + 2), value_size};
		secret = std::string_view{reinterpret_cast<char const *>(data + 2 + value_size), secret_size};
		arbitrary = std::string_view{
			reinterpret_cast<char const *>(data + 2 + value_size + secret_size),
			size - 2U - value_size - secret_size};
	}

	auto const signed_value = sign_cookie(value, secret);
	auto const verified = verify_cookie(signed_value, secret);
	if (!verified || *verified != value) {
		__builtin_trap();
	}
	auto const rejected = verify_cookie(signed_value, std::string_view{"different-secret"});
	if (rejected) {
		__builtin_trap();
	}
	auto const arbitrary_verified = verify_cookie(arbitrary, secret);
	if (arbitrary_verified && arbitrary.find('.') == std::string_view::npos) {
		__builtin_trap();
	}
	return 0;
}
