module;
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <unistd.h>
#if defined(CONFLUX_CRYPTO_USE_AESNI)
extern "C" {
void conflux_hex_encode_ssse3(unsigned char const *in, __SIZE_TYPE__ len, char *out);
}
#endif
#include "cpu_features.hxx"
#include "simd_backend.hxx"
export module conflux.utils;
import std;
import conflux.types;
namespace utils_detail {

inline constexpr std::uint64_t rotl(
	std::uint64_t x,
	int k) noexcept {
	return (x << k) | (x >> (64 - k));
}
// xoshiro256** PRNG. Seeded once per std::thread from getrandom(2).
class Xoshiro256ss {
	std::array<std::uint64_t, 4> s_{};

public:
	Xoshiro256ss() {
		// Seed from kernel CSPRNG. getrandom is non-blocking once initialized.
		auto *bytes = reinterpret_cast<unsigned char *>(s_.data());
		std::size_t total = 0;
		while (total < s_.size() * sizeof(std::uint64_t)) {
			auto const n = ::getrandom(bytes + total, s_.size() * sizeof(std::uint64_t) - total, 0);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				throw std::system_error{errno, std::system_category(), "random_bytes: getrandom"};
			}
			total += static_cast<std::size_t>(n);
		}
		if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0) {
			s_[0] = 0x9E3779B97F4A7C15ULL;
		}
	}
	std::uint64_t next() noexcept {
		std::uint64_t const result = rotl(s_[1] * 5, 7) * 9;
		std::uint64_t const t = s_[1] << 17;
		s_[2] ^= s_[0];
		s_[3] ^= s_[1];
		s_[1] ^= s_[2];
		s_[0] ^= s_[3];
		s_[2] ^= t;
		s_[3] = rotl(s_[3], 45);
		return result;
	}
};

} // namespace utils_detail
namespace {

std::mutex &stderr_mutex_() noexcept {
	static std::mutex mu;
	return mu;
}
void write_stderr_unlocked_(
	std::string_view message) noexcept {
	while (!message.empty()) {
		auto const n = ::write(STDERR_FILENO, message.data(), message.size());
		if (n > 0) {
			message.remove_prefix(static_cast<std::size_t>(n));
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		break;
	}
}

} // namespace
export void eprint(
	std::string_view message) noexcept {
	std::scoped_lock const lk{stderr_mutex_()};
	write_stderr_unlocked_(message);
}
export void eprintln(
	std::string_view message) noexcept {
	std::scoped_lock const lk{stderr_mutex_()};
	write_stderr_unlocked_(message);
	write_stderr_unlocked_("\n");
}

export void crypto_random_bytes(
	std::span<unsigned char> out) {
	std::size_t total = 0;
	while (total < out.size()) {
		auto n = ::getrandom(out.data() + total, out.size() - total, 0);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			throw std::system_error{errno, std::system_category(), "crypto_random_bytes: getrandom"};
		}
		total += static_cast<std::size_t>(n);
	}
}
export void random_bytes(
	std::span<unsigned char> out) {
	static thread_local utils_detail::Xoshiro256ss rng{};
	std::size_t i = 0;
	while (i + sizeof(std::uint64_t) <= out.size()) {
		std::uint64_t const v = rng.next();
		memcpy(out.data() + i, &v, sizeof(v));
		i += sizeof(std::uint64_t);
	}
	if (i < out.size()) {
		std::uint64_t const v = rng.next();
		memcpy(out.data() + i, &v, out.size() - i);
	}
}

// ---------------------------------------------------------------------------
// Generic hash combining
// ---------------------------------------------------------------------------

export template<typename T>
void hash_combine(
	std::size_t &seed,
	T const &value)
	noexcept(
		noexcept(std::hash<std::remove_cvref_t<T>>{}(value))) {
	seed ^= std::hash<std::remove_cvref_t<T>>{}(value)
		  + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL)
		  + (seed << 6U)
		  + (seed >> 2U);
}

export template<typename... Ts>
[[nodiscard]] std::size_t hash_values(
	Ts const &...values)
	noexcept(
		(noexcept(hash_combine(std::declval<std::size_t &>(), values)) && ...)) {
	std::size_t seed = 0;
	(hash_combine(seed, values), ...);
	return seed;
}

// ---------------------------------------------------------------------------
// RFC 3986 percent-encoding
// ---------------------------------------------------------------------------

export std::string percent_encode(
	std::string_view in) {
	std::string out;
	out.reserve(in.size());
	static constexpr char kHex[] = "0123456789ABCDEF";
	for (char const ch: in) {
		auto const c = static_cast<unsigned char>(ch);
		if ((c >= 'A' && c <= 'Z')
			|| (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9')
			|| c == '-'
			|| c == '.'
			|| c == '_'
			|| c == '~') {
			out.push_back(ch);
		} else {
			out.push_back('%');
			out.push_back(kHex[c >> 4U]);
			out.push_back(kHex[c & 0x0FU]);
		}
	}
	return out;
}
// ---------------------------------------------------------------------------
// JSON string fallback escaping
// ---------------------------------------------------------------------------

namespace {

inline constexpr std::array<char, 16> kJsonHex_ =
	{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

void append_json_u_escape_(
	std::string &out,
	unsigned char c) {
	out += "\\u00";
	out += kJsonHex_[c >> 4U];
	out += kJsonHex_[c & 0x0FU];
}

[[nodiscard]] constexpr std::size_t json_string_content_escaped_size_(
	std::string_view value) noexcept {
	std::size_t out = 0;
	for (char const raw: value) {
		auto const c = static_cast<unsigned char>(raw);
		if (raw == '"' || raw == '\\' || raw == '\b' || raw == '\f' || raw == '\n' || raw == '\r' || raw == '\t') {
			out += 2;
		} else if (c < 0x20U) {
			out += 6;
		} else {
			++out;
		}
	}
	return out;
}

} // namespace

export void append_json_string_content_fallback(
	std::string &out,
	std::string_view value) {
	for (char const raw: value) {
		auto const c = static_cast<unsigned char>(raw);
		switch (raw) {
		case '"' : out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20U) {
				append_json_u_escape_(out, c);
			} else {
				out += raw;
			}
			break;
		}
	}
}

export [[nodiscard]] std::string json_string_content_fallback(
	std::string_view value) {
	std::string out;
	out.reserve(json_string_content_escaped_size_(value));
	append_json_string_content_fallback(out, value);
	return out;
}

export void append_json_string_fallback(
	std::string &out,
	std::string_view value) {
	out += '"';
	append_json_string_content_fallback(out, value);
	out += '"';
}

export [[nodiscard]] std::string json_string_fallback(
	std::string_view value) {
	std::string out;
	out.reserve(json_string_content_escaped_size_(value) + 2);
	append_json_string_fallback(out, value);
	return out;
}

// ---------------------------------------------------------------------------
// Hex encode / decode
// ---------------------------------------------------------------------------

export std::string hex_encode(
	std::span<unsigned char const> in) {
	std::string out;
	out.resize(in.size() * 2);
#if defined(CONFLUX_CRYPTO_USE_AESNI) && !CONFLUX_CPU_FEATURE_PROBES_RUNTIME
	conflux_hex_encode_ssse3(in.data(), in.size(), out.data());
	return out;
#endif
#if defined(CONFLUX_CRYPTO_USE_AESNI) && CONFLUX_CPU_FEATURE_PROBES_RUNTIME
	if (conflux_cpu_supports_aesni_pclmul_sse41()) {
		conflux_hex_encode_ssse3(in.data(), in.size(), out.data());
		return out;
	}
#endif
	static constexpr char kHex[] = "0123456789abcdef";
	for (std::size_t i = 0; i < in.size(); ++i) {
		out[i * 2] = kHex[in[i] >> 4U];
		out[i * 2 + 1] = kHex[in[i] & 0x0FU];
	}
	return out;
}
// ---------------------------------------------------------------------------
// Hex
// ---------------------------------------------------------------------------

// Returns the numeric value of a single hex digit [0-9a-fA-F], or -1 if invalid.
export constexpr int hex_char_to_int(
	char c) noexcept {
	constexpr int kLetterBase = 10;
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + kLetterBase;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + kLetterBase;
	}
	return -1;
}
export std::expected<std::vector<unsigned char>, std::string> hex_decode(
	std::string_view in) {
	if (in.size() % 2 != 0) {
		return std::unexpected(std::string{"hex_decode: odd-length input"});
	}
	std::vector<unsigned char> out;
	out.reserve(in.size() / 2);
	for (std::size_t i = 0; i < in.size(); i += 2) {
		int const hi = hex_char_to_int(in[i]);
		int const lo = hex_char_to_int(in[i + 1]);
		if (hi < 0 || lo < 0) {
			return std::unexpected(std::string{"hex_decode: invalid hex digit"});
		}
		out.push_back(static_cast<unsigned char>((hi << 4) | lo));
	}
	return out;
}
// ---------------------------------------------------------------------------
// HTTP status codes
// ---------------------------------------------------------------------------

export constexpr int kHttpOk = 200;
export constexpr int kHttpCreated = 201;
export constexpr int kHttpMovedPermanently = 301;
export constexpr int kHttpFound = 302;
export constexpr int kHttpNotModified = 304;
export constexpr int kHttpTemporaryRedirect = 307;
export constexpr int kHttpPermanentRedirect = 308;
export constexpr int kHttpBadRequest = 400;
export constexpr int kHttpUnauthorized = 401;
export constexpr int kHttpForbidden = 403;
export constexpr int kHttpNotFound = 404;
export constexpr int kHttpMethodNotAllowed = 405;
export constexpr int kHttpUnprocessableEntity = 422;
export constexpr int kHttpRequestEntityTooLarge = 413;
export constexpr int kHttpUriTooLong = 414;
export constexpr int kHttpRangeNotSatisfiable = 416;
export constexpr int kHttpRequestHeaderFieldsTooLarge = 431;
export constexpr int kHttpTooManyRequests = 429;
export constexpr int kHttpBadGateway = 502;
export constexpr int kHttpGatewayTimeout = 504;
export constexpr int kHttpNoContent = 204;
export constexpr int kHttpPartialContent = 206;
export constexpr int kHttpInternalServerError = 500;
// ---------------------------------------------------------------------------
// URL percent-encoding / decoding
// ---------------------------------------------------------------------------

export [[nodiscard]] constexpr bool is_url_unreserved(
	unsigned char c) noexcept {
	return (c >= 'A' && c <= 'Z')
		|| (c >= 'a' && c <= 'z')
		|| (c >= '0' && c <= '9')
		|| c == '-'
		|| c == '_'
		|| c == '.'
		|| c == '~';
}

export [[nodiscard]] constexpr bool url_needs_component_decode(
	std::string_view s) noexcept {
	return s.find_first_of(std::string_view{"%+"}) != std::string_view::npos;
}

export [[nodiscard]] constexpr bool url_needs_path_decode(
	std::string_view s) noexcept {
	return s.find('%') != std::string_view::npos;
}

namespace {

inline constexpr std::array<char, 16> kUrlHex_ =
	{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

void append_url_encoded_impl_(
	std::string &out,
	std::string_view s,
	bool space_as_plus) {
	for (auto const raw_c: s) {
		unsigned char const c = static_cast<unsigned char>(raw_c);
		if (is_url_unreserved(c)) {
			out += static_cast<char>(c);
		} else if (space_as_plus && c == ' ') {
			out += '+';
		} else {
			out += '%';
			out += kUrlHex_[c >> 4U];
			out += kUrlHex_[c & 0x0FU];
		}
	}
}

} // namespace

export [[nodiscard]] constexpr std::size_t url_percent_encoded_size(
	std::string_view s) noexcept {
	std::size_t out = 0;
	for (auto const raw_c: s) {
		out += is_url_unreserved(static_cast<unsigned char>(raw_c)) ? std::size_t{1} : std::size_t{3};
	}
	return out;
}

export void append_url_percent_encoded(
	std::string &out,
	std::string_view s) {
	append_url_encoded_impl_(out, s, false);
}

export [[nodiscard]] std::string url_percent_encode(
	std::string_view s) {
	std::string out;
	out.reserve(url_percent_encoded_size(s));
	append_url_percent_encoded(out, s);
	return out;
}

export [[nodiscard]] constexpr std::size_t url_form_encoded_size(
	std::string_view s) noexcept {
	std::size_t out = 0;
	for (auto const raw_c: s) {
		auto const c = static_cast<unsigned char>(raw_c);
		out += (is_url_unreserved(c) || c == ' ') ? std::size_t{1} : std::size_t{3};
	}
	return out;
}

export void append_url_form_encoded(
	std::string &out,
	std::string_view s) {
	append_url_encoded_impl_(out, s, true);
}

export [[nodiscard]] std::string url_form_encode(
	std::string_view s) {
	std::string out;
	out.reserve(url_form_encoded_size(s));
	append_url_form_encoded(out, s);
	return out;
}

namespace {

std::size_t scan_url_plain_run_(
	char const *p,
	std::size_t n,
	bool plus_is_special) noexcept {
#if defined(CONFLUX_STDSIMD) && CONFLUX_SIMD_SELECTION_DIRECT
	constexpr std::size_t kStdsimdThreshold = 24;
	if (n >= kStdsimdThreshold) {
		return conflux_url_scan_plain_run_stdsimd(p, n, plus_is_special ? 1 : 0);
	}
#elif defined(CONFLUX_STDSIMD) && CONFLUX_SIMD_SELECTION_RUNTIME
	constexpr std::size_t kStdsimdThreshold = 24;
	if (n >= kStdsimdThreshold && conflux_cpu_supports_avx2()) {
		return conflux_url_scan_plain_run_stdsimd(p, n, plus_is_special ? 1 : 0);
	}
#endif
	for (std::size_t i = 0; i < n; ++i) {
		if (p[i] == '%' || (plus_is_special && p[i] == '+')) {
			return i;
		}
	}
	return n;
}

[[nodiscard]] std::string url_decode_impl_(
	std::string_view s,
	bool plus_is_space) {
	std::string out;
	out.reserve(s.size());
	std::size_t i = 0;
	while (i < s.size()) {
		std::size_t const run = scan_url_plain_run_(s.data() + i, s.size() - i, plus_is_space);
		out.append(s.data() + i, run);
		i += run;
		if (i >= s.size()) {
			break;
		}
		if (plus_is_space && s[i] == '+') {
			out += ' ';
			++i;
		} else if (s[i] == '%' && i + 2 < s.size()) {
			int const hi = hex_char_to_int(s[i + 1]);
			int const lo = hex_char_to_int(s[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out += static_cast<char>(static_cast<unsigned>(hi) << 4U | static_cast<unsigned>(lo));
				i += 3;
			} else {
				out += s[i++];
			}
		} else {
			out += s[i++];
		}
	}
	return out;
}

} // namespace

// Decode a percent-encoded URL component ('+' -> space, %XX -> byte).
export std::string url_decode(
	std::string_view s) {
	return url_decode_impl_(s, true);
}

// Decode a path component (%XX -> byte, '+' is literal).
export std::string url_decode_path(
	std::string_view s) {
	return url_decode_impl_(s, false);
}
// ---------------------------------------------------------------------------
// IP address / CIDR utilities (IPv4 + IPv6)
// ---------------------------------------------------------------------------

// IP addresses are always stored as in6_addr.
// IPv4 is represented as IPv4-mapped IPv6 (::ffff:a.b.c.d, RFC 4291 §2.5.5.2).
// IPv4 CIDR prefix N → stored prefix 96+N so comparisons are uniform 128-bit.
export using IpAddr = in6_addr;
export struct IpCidr {
	in6_addr network{};
	std::uint8_t prefix{0};
};
namespace {

// POSIX inet_pton wants NUL-terminated C S; stage into a stack buffer.
template<typename T>
std::optional<T> try_pton(
	int af,
	std::string_view s) noexcept {
	std::array<char, INET6_ADDRSTRLEN> buf{};
	if (s.size() >= buf.size()) {
		return std::nullopt;
	}
	memcpy(buf.data(), s.data(), s.size());
	buf[s.size()] = '\0';
	T a{};
	return ::inet_pton(af, buf.data(), &a) == 1 ? std::optional{a} : std::nullopt;
}
template<typename T>
std::string ntop(
	int af,
	T const &a) {
	std::array<char, INET6_ADDRSTRLEN> buf{};
	if (::inet_ntop(af, &a, buf.data(), buf.size()) != nullptr) {
		return std::string{buf.data()};
	}
	return {};
}
// Build an IPv4-mapped in6_addr from a network-std::byte-order in_addr.
in6_addr ipv4_mapped(
	in_addr const &v4) noexcept {
	in6_addr out{};
	out.s6_addr[10] = 0xFF;
	out.s6_addr[11] = 0xFF;
	memcpy(&out.s6_addr[12], &v4.s_addr, sizeof(v4.s_addr));
	return out;
}
// Mask an in6_addr to the given prefix length (0–128) in place.
void apply_prefix(
	in6_addr &addr,
	unsigned prefix) noexcept {
	unsigned const full = prefix / 8U;
	unsigned const rem = prefix % 8U;
	if (rem > 0U) {
		addr.s6_addr[full] &= static_cast<std::uint8_t>(0xFFU << (8U - rem));
	}
	for (unsigned i = full + (rem > 0U ? 1U : 0U); i < 16U; ++i) {
		addr.s6_addr[i] = 0;
	}
}
// Strip zone ID (%...) and surrounding brackets from an address S.
std::string_view strip_ip_decorators(
	std::string_view s) noexcept {
	if (auto z = s.find('%'); z != std::string_view::npos) {
		s.remove_suffix(s.size() - z);
	}
	if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
		s.remove_prefix(1);
		s.remove_suffix(1);
	}
	return s;
}

} // namespace
// Parse a dotted-decimal IPv4 address into host std::byte order.
export std::optional<std::uint32_t> parse_ipv4(
	std::string_view s) noexcept {
	auto v4 = try_pton<in_addr>(AF_INET, s);
	return v4 ? std::optional{ntohl(v4->s_addr)} : std::nullopt;
}
// Parse an IPv4 or IPv6 address. IPv4 → IPv4-mapped in6_addr.
// Strips zone IDs and surrounding brackets. Returns std::nullopt on failure.
export std::optional<IpAddr> parse_ip(
	std::string_view s) noexcept {
	s = strip_ip_decorators(s);
	if (auto v4 = try_pton<in_addr>(AF_INET, s)) {
		return ipv4_mapped(*v4);
	}
	return try_pton<in6_addr>(AF_INET6, s);
}
// Parse "addr/prefix" or bare "addr" (defaults to /32 for IPv4, /128 for IPv6).
// IPv4 CIDR prefix is stored as 96+N to match IPv4-mapped addresses uniformly.
// Returns std::nullopt if the address or prefix is invalid.
export std::optional<IpCidr> parse_cidr(
	std::string_view s) noexcept {
	auto slash = s.find('/');
	auto ip_stripped = strip_ip_decorators(slash == std::string_view::npos ? s : s.substr(0, slash));
	auto v4 = try_pton<in_addr>(AF_INET, ip_stripped);
	auto v6 = v4 ? std::optional<in6_addr>{} : try_pton<in6_addr>(AF_INET6, ip_stripped);
	if (!v4 && !v6) {
		return std::nullopt;
	}
	unsigned const max_prefix = v4 ? 32U : 128U;
	std::uint32_t prefix = max_prefix;
	if (slash != std::string_view::npos) {
		std::string_view const pfx_sv = s.substr(slash + 1);
		if (pfx_sv.empty()) {
			return std::nullopt;
		}
		auto const res = std::from_chars(pfx_sv.data(), pfx_sv.data() + pfx_sv.size(), prefix);
		if (res.ec != std::errc{} || res.ptr != pfx_sv.data() + pfx_sv.size()) {
			return std::nullopt;
		}
		if (prefix > max_prefix) {
			return std::nullopt;
		}
	}
	in6_addr net = v4 ? ipv4_mapped(*v4) : *v6;
	unsigned const final_prefix = v4 ? 96U + prefix : prefix;
	apply_prefix(net, final_prefix);
	return IpCidr{.network = net, .prefix = static_cast<std::uint8_t>(final_prefix)};
}
// Match ip against cidr. Both IPv4 and IPv6 use 128-bit prefix comparison.
export bool cidr_match(
	IpCidr const &cidr,
	IpAddr const &ip) noexcept {
	unsigned const full = cidr.prefix / 8U;
	unsigned const rem = cidr.prefix % 8U;
	for (unsigned i = 0U; i < full; ++i) {
		if (ip.s6_addr[i] != cidr.network.s6_addr[i]) {
			return false;
		}
	}
	if (rem > 0U) {
		auto const mask = static_cast<std::uint8_t>(0xFFU << (8U - rem));
		if ((ip.s6_addr[full] & mask) != cidr.network.s6_addr[full]) {
			return false;
		}
	}
	return true;
}
// Lowercase ASCII bytes A-Z in place. Leaves non-ASCII and already-lowercase
// bytes untouched; branch-free via the ASCII case bit.
export void ascii_lower_inplace(
	std::span<char> s) noexcept {
#if defined(CONFLUX_STDSIMD) && CONFLUX_SIMD_SELECTION_DIRECT
	constexpr std::size_t kStdsimdThreshold = 32;
	if (s.size() >= kStdsimdThreshold) {
		conflux_ascii_lower_inplace_stdsimd(s.data(), s.size());
		return;
	}
#elif defined(CONFLUX_STDSIMD) && CONFLUX_SIMD_SELECTION_RUNTIME
	constexpr std::size_t kStdsimdThreshold = 32;
	if (s.size() >= kStdsimdThreshold && conflux_cpu_supports_avx2()) {
		conflux_ascii_lower_inplace_stdsimd(s.data(), s.size());
		return;
	}
#endif
	for (auto &c: s) {
		unsigned char const u = static_cast<unsigned char>(c);
		c = static_cast<char>(u >= 'A' && u <= 'Z' ? u | 0x20 : u);
	}
}
// Allocate a lowercase copy of `s`.
export std::string ascii_lower(
	std::string_view s) {
	std::string out{s};
	ascii_lower_inplace(out);
	return out;
}
export void ascii_upper_inplace(
	std::span<char> s) noexcept {
	for (auto &c: s) {
		unsigned char const u = static_cast<unsigned char>(c);
		c = static_cast<char>(u >= 'a' && u <= 'z' ? u & ~0x20U : u);
	}
}
export std::string ascii_upper(
	std::string_view s) {
	std::string out{s};
	ascii_upper_inplace(out);
	return out;
}
// Trim leading/trailing ASCII whitespace (space, tab, CR, LF).
export std::string_view trim(
	std::string_view s) noexcept {
	auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while (!s.empty() && is_ws(s.front())) {
		s.remove_prefix(1);
	}
	while (!s.empty() && is_ws(s.back())) {
		s.remove_suffix(1);
	}
	return s;
}

export std::string_view strip_cr(
	std::string_view s) noexcept {
	if (!s.empty() && s.back() == '\r') {
		s.remove_suffix(1);
	}
	return s;
}
export std::optional<std::pair<std::string_view, std::string_view>> split_once(
	std::string_view s,
	char delim) noexcept {
	auto const pos = s.find(delim);
	if (pos == std::string_view::npos) {
		return std::nullopt;
	}
	return std::pair<std::string_view, std::string_view>{s.substr(0, pos), s.substr(pos + 1)};
}
export struct LineView {
	std::string_view text;
	std::size_t line_no{};
};
export class LineRange {
	std::string_view text_{};

public:
	constexpr explicit LineRange(
		std::string_view text) noexcept
		: text_{text} {}
	class iterator {
		std::string_view rest_{};
		LineView current_{};
		std::size_t next_line_no_{1};
		bool done_{true};

		void advance_() noexcept {
			if (rest_.empty()) {
				done_ = true;
				current_ = {};
				return;
			}
			auto const eol = rest_.find('\n');
			std::string_view line = eol == std::string_view::npos ? rest_ : rest_.substr(0, eol);
			current_ = LineView{.text = strip_cr(line), .line_no = next_line_no_++};
			if (eol == std::string_view::npos) {
				rest_ = {};
			} else {
				rest_.remove_prefix(eol + 1);
			}
			done_ = false;
		}

	public:
		using value_type = LineView;
		using difference_type = std::ptrdiff_t;
		using iterator_concept = std::input_iterator_tag;
		using iterator_category = std::input_iterator_tag;
		iterator() noexcept = default;
		explicit iterator(
			std::string_view text) noexcept
			: rest_{text}
			, done_{false} {
			advance_();
		}
		[[nodiscard]] LineView const &operator *() const noexcept { return current_; }
		[[nodiscard]] LineView const *operator ->() const noexcept { return &current_; }
		iterator &operator ++() noexcept {
			advance_();
			return *this;
		}
		void operator ++(
			int) noexcept {
			advance_();
		}
		[[nodiscard]] bool operator ==(
			std::default_sentinel_t) const noexcept {
			return done_;
		}
	};
	[[nodiscard]] iterator begin() const noexcept { return iterator{text_}; }
	[[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }
};

template<>
inline constexpr bool std::ranges::enable_borrowed_range<LineRange> = true;

static_assert(std::ranges::borrowed_range<LineRange>);
// Format an IpAddr as a string. IPv4-mapped addresses render as bare IPv4.
export std::string ip_to_string(
	IpAddr const &ip) {
	if (IN6_IS_ADDR_V4MAPPED(&ip)) {
		in_addr v4{};
		memcpy(&v4.s_addr, &ip.s6_addr[12], sizeof(v4.s_addr));
		return ntop(AF_INET, v4);
	}
	return ntop(AF_INET6, ip);
}
// Parse a list of CIDR strings. Invalid entries emit a warning to stderr and are skipped.
export std::vector<IpCidr> parse_cidr_list(
	std::vector<std::string> const &cidr_strings) {
	std::vector<IpCidr> result;
	result.reserve(cidr_strings.size());
	for (auto const &s: cidr_strings) {
		if (auto c = parse_cidr(s)) {
			result.push_back(*c);
		} else {
			eprintln(std::format("parse_cidr_list: invalid CIDR '{}' ignored", s));
		}
	}
	return result;
}
// Wait for fd to become ready for events (POLLIN, POLLOUT, …).
// Returns false on timeout or error; true when events are set.
export bool wait_fd(
	int fd,
	short events,
	int timeout_sec) {
	pollfd pfd{.fd = fd, .events = events, .revents = 0};
	int const rc = ::poll(&pfd, 1, timeout_sec < 0 ? -1 : timeout_sec * 1000);
	return rc > 0 && (pfd.revents & (events | POLLERR | POLLHUP)) != 0;
}
