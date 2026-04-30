module;
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
export module conflux.utils;
import std;
import conflux.types;

namespace utils_detail {

inline constexpr u64 rotl(
	u64 x,
	int k) noexcept {
	return (x << k) | (x >> (64 - k));
}

// xoshiro256** PRNG. Seeded once per thread from getrandom(2).
class Xoshiro256ss {
	A<u64, 4> s_{};

public:
	Xoshiro256ss() {
		// Seed from kernel CSPRNG. getrandom is non-blocking once initialized.
		while (::getrandom(s_.data(), s_.size() * sizeof(u64), 0) != static_cast<ssize_t>(s_.size() * sizeof(u64))) {}
		if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0) {
			s_[0] = 0x9E3779B97F4A7C15ULL;
		}
	}

	u64 next() noexcept {
		u64 const result = rotl(s_[1] * 5, 7) * 9;
		u64 const t = s_[1] << 17;
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

// Fill `out` with pseudo-random bytes from a thread-local xoshiro256**
// seeded once from getrandom(2). Not cryptographically strong — use for
// trace IDs, request IDs, jitter, and similar non-security purposes.
export void random_bytes(
	span<unsigned char> out) {
	static thread_local utils_detail::Xoshiro256ss rng{};
	SZ i = 0;
	while (i + sizeof(u64) <= out.size()) {
		u64 const v = rng.next();
		memcpy(out.data() + i, &v, sizeof(v));
		i += sizeof(u64);
	}
	if (i < out.size()) {
		u64 const v = rng.next();
		memcpy(out.data() + i, &v, out.size() - i);
	}
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
export constexpr int kHttpGatewayTimeout = 504;
export constexpr int kHttpNoContent = 204;
export constexpr int kHttpPartialContent = 206;

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

// ---------------------------------------------------------------------------
// URL percent-decoding
// ---------------------------------------------------------------------------

// Decode a percent-encoded URL component ('+' → space, %XX → byte).
export S url_decode(
	SV s) {
	S out;
	auto const n = s.size();
	out.reserve(n);
	for (SZ i = 0; i < n; ++i) {
		if (s[i] == '+') {
			out += ' ';
		} else if (s[i] == '%' && i + 2 < n) {
			int const hi = hex_char_to_int(s[i + 1]);
			int const lo = hex_char_to_int(s[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out += static_cast<char>(static_cast<unsigned>(hi) << 4U | static_cast<unsigned>(lo));
				i += 2;
			} else {
				out += s[i];
			}
		} else {
			out += s[i];
		}
	}
	return out;
}

// Decode percent-encoded URL path segment (%XX → byte). '+' is NOT decoded to
// space (it is a literal '+' in path segments, not a form-encoding indicator).
export S url_decode_path(
	SV s) {
	S out;
	auto const n = s.size();
	out.reserve(n);
	for (SZ i = 0; i < n; ++i) {
		if (s[i] == '%' && i + 2 < n) {
			int const hi = hex_char_to_int(s[i + 1]);
			int const lo = hex_char_to_int(s[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out += static_cast<char>(static_cast<unsigned>(hi) << 4U | static_cast<unsigned>(lo));
				i += 2;
			} else {
				out += s[i];
			}
		} else {
			out += s[i];
		}
	}
	return out;
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
	u8 prefix{0};
};

namespace {

// POSIX inet_pton wants NUL-terminated C S; stage into a stack buffer.
template<typename T>
Opt<T> try_pton(
	int af,
	SV s) noexcept {
	A<char, INET6_ADDRSTRLEN> buf{};
	if (s.size() >= buf.size()) {
		return std::nullopt;
	}
	memcpy(buf.data(), s.data(), s.size());
	buf[s.size()] = '\0';
	T a{};
	return ::inet_pton(af, buf.data(), &a) == 1 ? Opt{a} : std::nullopt;
}

template<typename T>
S ntop(
	int af,
	T const &a) {
	A<char, INET6_ADDRSTRLEN> buf{};
	if (::inet_ntop(af, &a, buf.data(), buf.size()) != nullptr) {
		return S{buf.data()};
	}
	return {};
}

// Build an IPv4-mapped in6_addr from a network-byte-order in_addr.
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
		addr.s6_addr[full] &= static_cast<u8>(0xFFU << (8U - rem));
	}
	for (unsigned i = full + (rem > 0U ? 1U : 0U); i < 16U; ++i) {
		addr.s6_addr[i] = 0;
	}
}

// Strip zone ID (%...) and surrounding brackets from an address S.
SV strip_ip_decorators(
	SV s) noexcept {
	if (auto z = s.find('%'); z != SV::npos) {
		s.remove_suffix(s.size() - z);
	}
	if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
		s.remove_prefix(1);
		s.remove_suffix(1);
	}
	return s;
}

} // namespace

// Parse a dotted-decimal IPv4 address into host byte order.
export Opt<u32> parse_ipv4(
	SV s) noexcept {
	auto v4 = try_pton<in_addr>(AF_INET, s);
	return v4 ? Opt{ntohl(v4->s_addr)} : std::nullopt;
}

// Parse an IPv4 or IPv6 address. IPv4 → IPv4-mapped in6_addr.
// Strips zone IDs and surrounding brackets. Returns std::nullopt on failure.
export Opt<IpAddr> parse_ip(
	SV s) noexcept {
	s = strip_ip_decorators(s);
	if (auto v4 = try_pton<in_addr>(AF_INET, s)) {
		return ipv4_mapped(*v4);
	}
	return try_pton<in6_addr>(AF_INET6, s);
}

// Parse "addr/prefix" or bare "addr" (defaults to /32 for IPv4, /128 for IPv6).
// IPv4 CIDR prefix is stored as 96+N to match IPv4-mapped addresses uniformly.
// Returns std::nullopt if the address or prefix is invalid.
export Opt<IpCidr> parse_cidr(
	SV s) noexcept {
	auto slash = s.find('/');
	auto ip_stripped = strip_ip_decorators(slash == SV::npos ? s : s.substr(0, slash));
	auto v4 = try_pton<in_addr>(AF_INET, ip_stripped);
	auto v6 = v4 ? Opt<in6_addr>{} : try_pton<in6_addr>(AF_INET6, ip_stripped);
	if (!v4 && !v6) {
		return std::nullopt;
	}
	unsigned const max_prefix = v4 ? 32U : 128U;
	u32 prefix = max_prefix;
	if (slash != SV::npos) {
		SV const pfx_sv = s.substr(slash + 1);
		if (pfx_sv.empty()) {
			return std::nullopt;
		}
		auto const res = from_chars(pfx_sv.data(), pfx_sv.data() + pfx_sv.size(), prefix);
		if (res.ec != errc{} || res.ptr != pfx_sv.data() + pfx_sv.size()) {
			return std::nullopt;
		}
		if (prefix > max_prefix) {
			return std::nullopt;
		}
	}
	in6_addr net = v4 ? ipv4_mapped(*v4) : *v6;
	unsigned const final_prefix = v4 ? 96U + prefix : prefix;
	apply_prefix(net, final_prefix);
	return IpCidr{.network = net, .prefix = static_cast<u8>(final_prefix)};
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
		auto const mask = static_cast<u8>(0xFFU << (8U - rem));
		if ((ip.s6_addr[full] & mask) != cidr.network.s6_addr[full]) {
			return false;
		}
	}
	return true;
}

// Lowercase ASCII bytes A-Z in place. Leaves non-ASCII and already-lowercase
// bytes untouched; branch-free via the ASCII case bit.
export void ascii_lower_inplace(
	span<char> s) noexcept {
	for (auto &c: s) {
		unsigned char const u = static_cast<unsigned char>(c);
		c = static_cast<char>(u >= 'A' && u <= 'Z' ? u | 0x20 : u);
	}
}

// Allocate a lowercase copy of `s`.
export S ascii_lower(
	SV s) {
	S out{s};
	ascii_lower_inplace(out);
	return out;
}

// Trim leading/trailing ASCII whitespace (space, tab, CR, LF).
export SV trim(
	SV s) noexcept {
	auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while (!s.empty() && is_ws(s.front())) {
		s.remove_prefix(1);
	}
	while (!s.empty() && is_ws(s.back())) {
		s.remove_suffix(1);
	}
	return s;
}

// Format an IpAddr as a S. IPv4-mapped addresses render as bare IPv4.
export S ip_to_string(
	IpAddr const &ip) {
	if (IN6_IS_ADDR_V4MAPPED(&ip)) {
		in_addr v4{};
		memcpy(&v4.s_addr, &ip.s6_addr[12], sizeof(v4.s_addr));
		return ntop(AF_INET, v4);
	}
	return ntop(AF_INET6, ip);
}

// Parse a list of CIDR strings. Invalid entries emit a warning to stderr and are skipped.
export V<IpCidr> parse_cidr_list(
	V<S> const &cidr_strings) {
	V<IpCidr> result;
	result.reserve(cidr_strings.size());
	for (auto const &s: cidr_strings) {
		if (auto c = parse_cidr(s)) {
			result.push_back(*c);
		} else {
			println(stderr, "parse_cidr_list: invalid CIDR '{}' ignored", s);
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
