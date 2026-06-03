module;

#include <arpa/inet.h>
#include <cstring>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

export module conflux.net.dns;

import std;
import conflux.types;
import conflux.uring.completion;
import conflux.file_io;
import conflux.socket_io;
import conflux.work;

namespace conflux::net::dns {
namespace root = conflux::work::root;
using conflux::socket_io::SocketTaskRing;
using conflux::uring::CompletionTable;
using conflux::uring::UserDataFn;
using conflux::work::WorkPool;

// ─── address family / endpoint ──────────────────────────────────────────────

export enum class AddressFamily : std::uint8_t {
	v4,
	v6,
};
export struct Endpoint {
	::sockaddr_storage addr{};
	::socklen_t addr_len{};
	AddressFamily family{};
};
export struct ResolveResult {
	std::vector<Endpoint> endpoints;
	std::chrono::nanoseconds elapsed{};
	std::chrono::seconds suggested_ttl{0};
	bool from_cache{false};
	bool from_hosts_file{false};
	bool from_coalesced{false};
	bool is_negative{false}; // true = negative cache entry (NXDOMAIN)
};

// ─── errors ─────────────────────────────────────────────────────────────────

export enum class DnsErrorKind : std::uint8_t {
	timeout,
	nxdomain,
	servfail,
	refused,
	formerr,
	malformed,
	no_servers,
	network,
	truncated,
	cancelled,
	invalid_hostname,
	no_ring,
	cannot_block_on_owned_ring,
	not_implemented,
};
export struct DnsError final : std::runtime_error {
	DnsError(
		DnsErrorKind k,
		std::string const &msg,
		int err = 0,
		std::optional<std::uint8_t> r = {})
		: std::runtime_error{msg}
		, kind{k}
		, os_errno{err}
		, rcode{r} {}
	DnsErrorKind kind;
	int os_errno{0};
	std::optional<std::uint8_t> rcode{};
};
// ─── nameserver endpoint ────────────────────────────────────────────────────

export struct NameserverEndpoint {
	::sockaddr_storage addr{};
	::socklen_t addr_len{};
	std::uint16_t port{53};
};
// Parse "ip", "ip:port", "[ipv6]:port", "[ipv6]". Returns endpoint with
// AF_INET or AF_INET6 set in addr. On failure returns the unsupported
// literal so callers can surface a useful diagnostic.
export [[nodiscard]] std::expected<NameserverEndpoint, std::string> parse_nameserver(
	std::string_view literal) {
	if (literal.empty()) {
		return std::unexpected{std::string{"empty nameserver literal"}};
	}

	std::string host;
	std::uint16_t port = 53;

	if (literal.front() == '[') {
		auto const close = literal.find(']');
		if (close == std::string_view::npos) {
			return std::unexpected{std::string{"unterminated '[' in nameserver literal"}};
		}
		host.assign(literal.substr(1, close - 1));
		auto rest = literal.substr(close + 1);
		if (!rest.empty()) {
			if (rest.front() != ':') {
				return std::unexpected{std::string{"std::expected ':<port>' after ']' in nameserver literal"}};
			}
			rest.remove_prefix(1);
			if (rest.empty()) {
				return std::unexpected{std::string{"missing port after ':' in nameserver literal"}};
			}
			std::uint32_t parsed = 0;
			for (char const c: rest) {
				if (c < '0' || c > '9') {
					return std::unexpected{std::format("invalid port '{}' in nameserver literal", rest)};
				}
				parsed = parsed * 10 + static_cast<std::uint32_t>(c - '0');
				if (parsed > 0xFFFFU) {
					return std::unexpected{std::format("port out of range '{}' in nameserver literal", rest)};
				}
			}
			port = static_cast<std::uint16_t>(parsed);
		}
	} else {
		// Either bare IPv4 ("1.2.3.4"), bare IPv6 ("::1"), or "ipv4:port".
		// Distinguish: bare IPv6 contains ':', but so does "ipv4:port".
		// IPv6 always has ≥ 2 colons or contains "::"; IPv4 has at most 1.
		auto const colons = std::ranges::count(literal, ':');
		bool const is_ipv6 = colons >= 2 || literal.find("::") != std::string_view::npos;
		if (!is_ipv6 && colons == 1) {
			auto const colon = literal.find(':');
			host.assign(literal.substr(0, colon));
			auto rest = literal.substr(colon + 1);
			if (rest.empty()) {
				return std::unexpected{std::string{"missing port after ':' in nameserver literal"}};
			}
			std::uint32_t parsed = 0;
			for (char const c: rest) {
				if (c < '0' || c > '9') {
					return std::unexpected{std::format("invalid port '{}' in nameserver literal", rest)};
				}
				parsed = parsed * 10 + static_cast<std::uint32_t>(c - '0');
				if (parsed > 0xFFFFU) {
					return std::unexpected{std::format("port out of range '{}' in nameserver literal", rest)};
				}
			}
			port = static_cast<std::uint16_t>(parsed);
		} else {
			host.assign(literal);
		}
	}

	NameserverEndpoint ns{};
	::in_addr v4{};
	::in6_addr v6{};
	if (::inet_pton(AF_INET, host.c_str(), &v4) == 1) {
		auto *sin = reinterpret_cast<::sockaddr_in *>(&ns.addr);
		sin->sin_family = AF_INET;
		sin->sin_addr = v4;
		sin->sin_port = htons(port);
		ns.addr_len = sizeof(::sockaddr_in);
	} else if (::inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
		auto *sin6 = reinterpret_cast<::sockaddr_in6 *>(&ns.addr);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_addr = v6;
		sin6->sin6_port = htons(port);
		ns.addr_len = sizeof(::sockaddr_in6);
	} else {
		return std::unexpected{std::format("not an IP literal: '{}'", host)};
	}
	ns.port = port;
	return ns;
}
// ─── hostname validation (RFC 2181 §11) ─────────────────────────────────────

// Length-only checks. Allow underscores, digits, hyphens; reject NUL and
// dot-only / empty labels. Total length ≤ 253 bytes (excluding final '.'),
// per-label ≤ 63 bytes.
export [[nodiscard]] bool is_valid_hostname(
	std::string_view name) noexcept {
	if (name.empty()) {
		return false;
	}
	// Strip optional trailing root dot for length accounting.
	std::string_view trimmed = name;
	if (trimmed.back() == '.') {
		trimmed.remove_suffix(1);
	}
	if (trimmed.empty() || trimmed.size() > 253U) {
		return false;
	}
	size_t label_len = 0;
	for (char const c: trimmed) {
		if (c == '\0') {
			return false;
		}
		if (c == '.') {
			if (label_len == 0) {
				return false; // empty label / leading dot / consecutive dots
			}
			label_len = 0;
		} else {
			++label_len;
			if (label_len > 63U) {
				return false;
			}
		}
	}
	return label_len > 0; // last label must be non-empty
}
// ─── numeric literal short-circuit ──────────────────────────────────────────

// Returns an Endpoint if `host` is an IPv4 or IPv6 literal, else std::nullopt.
// `port` is written into the sockaddr in network order.
export [[nodiscard]] std::optional<Endpoint> try_parse_ip_literal(
	std::string_view host,
	std::uint16_t port) noexcept {
	// inet_pton needs a NUL-terminated string. Stack-buffer common case.
	std::array<char, 64> buf{};
	if (host.size() >= buf.size()) {
		return std::nullopt;
	}
	std::ranges::copy(host, buf.begin());
	buf[host.size()] = '\0';

	Endpoint ep{};
	::in_addr v4{};
	::in6_addr v6{};
	if (::inet_pton(AF_INET, buf.data(), &v4) == 1) {
		auto *sin = reinterpret_cast<::sockaddr_in *>(&ep.addr);
		sin->sin_family = AF_INET;
		sin->sin_addr = v4;
		sin->sin_port = htons(port);
		ep.addr_len = sizeof(::sockaddr_in);
		ep.family = AddressFamily::v4;
		return ep;
	}
	if (::inet_pton(AF_INET6, buf.data(), &v6) == 1) {
		auto *sin6 = reinterpret_cast<::sockaddr_in6 *>(&ep.addr);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_addr = v6;
		sin6->sin6_port = htons(port);
		ep.addr_len = sizeof(::sockaddr_in6);
		ep.family = AddressFamily::v6;
		return ep;
	}
	return std::nullopt;
}
// ─── DNS wire codec (RFC 1035 + EDNS0) ──────────────────────────────────────

namespace codec {

export enum class QType : std::uint16_t { // NOLINT(performance-enum-size) — DNS wire values need u16
	a = 1,
	ns = 2,
	cname = 5,
	soa = 6,
	ptr = 12,
	mx = 15,
	txt = 16,
	aaaa = 28,
	srv = 33,
	opt = 41,
};

export enum class QClass : std::uint16_t { // NOLINT(performance-enum-size) — DNS wire values need u16
	in = 1,
};

export enum class RCode : std::uint8_t {
	noerror = 0,
	formerr = 1,
	servfail = 2,
	nxdomain = 3,
	notimp = 4,
	refused = 5,
};

// Header flags layout (16-bit field after id, big-endian).
//   QR | OPCODE(4) | AA | TC | RD || RA | Z(3) | RCODE(4)
constexpr std::uint16_t kFlagQR = 0x8000U;
constexpr std::uint16_t kFlagAA = 0x0400U;
constexpr std::uint16_t kFlagTC = 0x0200U;
constexpr std::uint16_t kFlagRD = 0x0100U;
constexpr std::uint16_t kFlagRA = 0x0080U;
constexpr std::uint16_t kRCodeMask = 0x000FU;
export struct Header {
	std::uint16_t id{};
	std::uint16_t flags{};
	std::uint16_t qdcount{};
	std::uint16_t ancount{};
	std::uint16_t nscount{};
	std::uint16_t arcount{};
	[[nodiscard]] bool qr() const noexcept { return (flags & kFlagQR) != 0; }
	[[nodiscard]] bool tc() const noexcept { return (flags & kFlagTC) != 0; }
	[[nodiscard]] bool rd() const noexcept { return (flags & kFlagRD) != 0; }
	[[nodiscard]] bool ra() const noexcept { return (flags & kFlagRA) != 0; }
	[[nodiscard]] RCode rcode() const noexcept { return static_cast<RCode>(flags & kRCodeMask); }
};
export struct Question {
	std::string name; // canonical lowercase, no trailing root, dot-separated
	QType qtype{QType::a};
	QClass qclass{QClass::in};
};
export struct ResourceRecord {
	std::string name;
	QType type{QType::a};
	QClass rclass{QClass::in};
	std::uint32_t ttl{0};
	std::vector<std::uint8_t> rdata; // raw RDATA (uncompressed for OPT; for compressed names,
	// callers parse via decode helpers below)
};
export struct Message {
	Header header{};
	std::vector<Question> questions;
	std::vector<ResourceRecord> answers;
	std::vector<ResourceRecord> authority;
	std::vector<ResourceRecord> additional;
};
// EDNS0 OPT pseudo-RR.
export struct Edns0Options {
	std::uint16_t udp_size{4096};
	std::uint8_t ext_rcode{0};
	std::uint8_t version{0};
	std::uint16_t flags{0}; // DO bit etc; 0 for v1
};
// Encode a query (header + single question + optional EDNS0 OPT in additional).
// Returns the wire bytes. RD=1 set unconditionally.
export [[nodiscard]] std::vector<std::uint8_t> encode_query(
	std::uint16_t id,
	std::string_view qname,
	QType qtype,
	std::optional<Edns0Options> edns = Edns0Options{}) {
	std::vector<std::uint8_t> out;
	out.reserve(64);

	auto write_u16 = [&](std::uint16_t v) {
		out.push_back(static_cast<std::uint8_t>(v >> 8));
		out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
	};
	auto write_u32 = [&](std::uint32_t v) {
		out.push_back(static_cast<std::uint8_t>(v >> 24));
		out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFU));
		out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
		out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
	};

	// Header
	std::uint16_t const arcount = edns.has_value() ? 1U : 0U;
	write_u16(id);
	write_u16(kFlagRD);
	write_u16(1); // qdcount
	write_u16(0); // ancount
	write_u16(0); // nscount
	write_u16(arcount);

	// QNAME — labels with length prefix; root terminator 0.
	{
		size_t pos = 0;
		while (pos < qname.size()) {
			auto const dot = qname.find('.', pos);
			auto const len = (dot == std::string_view::npos ? qname.size() : dot) - pos;
			if (len == 0) {
				// empty label (consecutive dots) — caller should have validated
				break;
			}
			if (len > 63U) {
				// caller didn't validate; truncate label gracefully by clamping
				// no — refuse via assert-like std::exception. Use throw for codec contract.
				throw DnsError{
					DnsErrorKind::invalid_hostname,
					std::format("encode_query: label > 63 bytes in '{}'", qname)};
			}
			out.push_back(static_cast<std::uint8_t>(len));
			out.insert(
				out.end(),
				qname.begin() + static_cast<std::ptrdiff_t>(pos),
				qname.begin() + static_cast<std::ptrdiff_t>(pos + len));
			pos += len + (dot == std::string_view::npos ? 0 : 1);
		}
		out.push_back(0); // root
	}

	// QTYPE / QCLASS
	write_u16(static_cast<std::uint16_t>(qtype));
	write_u16(static_cast<std::uint16_t>(QClass::in));

	// EDNS0 OPT — root name, type=41, class=udp_size, ttl=(extrcode<<24|ver<<16|flags),
	// rdlength=0.
	if (edns.has_value()) {
		out.push_back(0); // root name
		write_u16(static_cast<std::uint16_t>(QType::opt));
		write_u16(edns->udp_size);
		write_u32(
			(static_cast<std::uint32_t>(edns->ext_rcode) << 24)
			| (static_cast<std::uint32_t>(edns->version) << 16)
			| static_cast<std::uint32_t>(edns->flags));
		write_u16(0); // rdlength
	}

	return out;
}
// ─── decode helpers ─────────────────────────────────────────────────────────

// Decode a name starting at `offset` in `wire`. Follows compression pointers
// up to depth 16; pointer offsets must be strictly less than the offset they
// are reached from (no forward refs). Writes the decoded canonical
// lowercase name (dot-separated, no trailing root) into `out`. Returns the
// number of bytes consumed at the original offset (NOT following the
// pointer chain to its end).
export [[nodiscard]] size_t decode_name(
	std::span<std::uint8_t const> wire,
	size_t offset,
	std::string &out) {
	out.clear();
	constexpr size_t kMaxDepth = 16;
	size_t depth = 0;
	size_t cursor = offset;
	size_t consumed = 0;
	bool followed_pointer = false;

	while (true) {
		if (cursor >= wire.size()) {
			throw DnsError{DnsErrorKind::malformed, "decode_name: cursor out of range"};
		}
		std::uint8_t const len = wire[cursor];
		if (len == 0) {
			++cursor;
			if (!followed_pointer) {
				consumed = cursor - offset;
			}
			break;
		}
		if ((len & 0xC0U) == 0xC0U) {
			if (cursor + 1 >= wire.size()) {
				throw DnsError{DnsErrorKind::malformed, "decode_name: pointer truncated"};
			}
			size_t const next = (static_cast<size_t>(len & 0x3FU) << 8) | static_cast<size_t>(wire[cursor + 1]);
			if (!followed_pointer) {
				consumed = cursor + 2 - offset;
			}
			if (next >= cursor) {
				throw DnsError{
					DnsErrorKind::malformed,
					std::format("decode_name: forward pointer {} >= {}", next, cursor)};
			}
			++depth;
			if (depth > kMaxDepth) {
				throw DnsError{DnsErrorKind::malformed, "decode_name: pointer depth exceeded"};
			}
			cursor = next;
			followed_pointer = true;
			continue;
		}
		if ((len & 0xC0U) != 0) {
			throw DnsError{DnsErrorKind::malformed, std::format("decode_name: reserved label flags 0x{:02x}", len)};
		}
		if (cursor + 1 + len > wire.size()) {
			throw DnsError{DnsErrorKind::malformed, "decode_name: label runs past wire end"};
		}
		if (!out.empty()) {
			out.push_back('.');
		}
		for (size_t i = 0; i < len; ++i) {
			char const c = static_cast<char>(wire[cursor + 1 + i]);
			out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
		}
		if (out.size() > 253U) {
			throw DnsError{DnsErrorKind::malformed, "decode_name: total length exceeds 253"};
		}
		cursor += 1 + len;
	}
	return consumed;
}
[[nodiscard]] std::uint16_t read_u16(
	std::span<std::uint8_t const> wire,
	size_t offset) {
	if (offset + 2 > wire.size()) {
		throw DnsError{DnsErrorKind::malformed, "read_u16: short read"};
	}
	return static_cast<std::uint16_t>((static_cast<std::uint16_t>(wire[offset]) << 8) | wire[offset + 1]);
}
[[nodiscard]] std::uint32_t read_u32(
	std::span<std::uint8_t const> wire,
	size_t offset) {
	if (offset + 4 > wire.size()) {
		throw DnsError{DnsErrorKind::malformed, "read_u32: short read"};
	}
	return (static_cast<std::uint32_t>(wire[offset]) << 24)
		 | (static_cast<std::uint32_t>(wire[offset + 1]) << 16)
		 | (static_cast<std::uint32_t>(wire[offset + 2]) << 8)
		 | static_cast<std::uint32_t>(wire[offset + 3]);
}
// Parse a full message (header + sections). Throws DnsError on malformed input.
// RDATA is captured as raw bytes — callers project A/AAAA from it via the
// rdata_to_endpoint helpers below.
export [[nodiscard]] Message decode_message(
	std::span<std::uint8_t const> wire) {
	if (wire.size() < 12) {
		throw DnsError{DnsErrorKind::malformed, "decode_message: header < 12 bytes"};
	}
	Message m;
	m.header.id = read_u16(wire, 0);
	m.header.flags = read_u16(wire, 2);
	m.header.qdcount = read_u16(wire, 4);
	m.header.ancount = read_u16(wire, 6);
	m.header.nscount = read_u16(wire, 8);
	m.header.arcount = read_u16(wire, 10);

	size_t pos = 12;

	auto read_question = [&]() -> Question {
		Question q;
		size_t const consumed = decode_name(wire, pos, q.name);
		pos += consumed;
		q.qtype = static_cast<QType>(read_u16(wire, pos));
		q.qclass = static_cast<QClass>(read_u16(wire, pos + 2));
		pos += 4;
		return q;
	};

	auto read_rr = [&]() -> ResourceRecord {
		ResourceRecord rr;
		size_t const consumed = decode_name(wire, pos, rr.name);
		pos += consumed;
		rr.type = static_cast<QType>(read_u16(wire, pos));
		rr.rclass = static_cast<QClass>(read_u16(wire, pos + 2));
		rr.ttl = read_u32(wire, pos + 4);
		std::uint16_t const rdlen = read_u16(wire, pos + 8);
		pos += 10;
		if (pos + rdlen > wire.size()) {
			throw DnsError{DnsErrorKind::malformed, std::format("decode_message: RDLENGTH {} overruns wire", rdlen)};
		}
		rr.rdata.assign(
			wire.begin() + static_cast<std::ptrdiff_t>(pos),
			wire.begin() + static_cast<std::ptrdiff_t>(pos + rdlen));
		pos += rdlen;
		return rr;
	};
	auto append_generated = [](auto &out, std::uint16_t count, auto &&read_fn) {
		out.reserve(count);
		std::generate_n(std::back_inserter(out), count, std::forward<decltype(read_fn)>(read_fn));
	};

	append_generated(m.questions, m.header.qdcount, read_question);
	append_generated(m.answers, m.header.ancount, read_rr);
	append_generated(m.authority, m.header.nscount, read_rr);
	append_generated(m.additional, m.header.arcount, read_rr);
	return m;
}
// Convert an A or AAAA RR into an Endpoint. Returns std::nullopt for other types.
export [[nodiscard]] std::optional<Endpoint> rdata_to_endpoint(
	ResourceRecord const &rr,
	std::uint16_t port) {
	Endpoint ep{};
	if (rr.type == QType::a && rr.rdata.size() == 4) {
		auto *sin = reinterpret_cast<::sockaddr_in *>(&ep.addr);
		sin->sin_family = AF_INET;
		sin->sin_port = htons(port);
		std::memcpy(&sin->sin_addr, rr.rdata.data(), 4);
		ep.addr_len = sizeof(::sockaddr_in);
		ep.family = AddressFamily::v4;
		return ep;
	}
	if (rr.type == QType::aaaa && rr.rdata.size() == 16) {
		auto *sin6 = reinterpret_cast<::sockaddr_in6 *>(&ep.addr);
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port = htons(port);
		std::memcpy(&sin6->sin6_addr, rr.rdata.data(), 16);
		ep.addr_len = sizeof(::sockaddr_in6);
		ep.family = AddressFamily::v6;
		return ep;
	}
	return std::nullopt;
}
// Map RCODE → DnsErrorKind. RCODE 0 = noerror returns std::nullopt.
export [[nodiscard]] std::optional<DnsErrorKind> rcode_to_error(
	RCode r) noexcept {
	switch (r) {
	case RCode::noerror : return std::nullopt;
	case RCode::formerr : return DnsErrorKind::formerr;
	case RCode::servfail: return DnsErrorKind::servfail;
	case RCode::nxdomain: return DnsErrorKind::nxdomain;
	case RCode::notimp  : return DnsErrorKind::servfail;
	case RCode::refused : return DnsErrorKind::refused;
	}
	return DnsErrorKind::servfail;
}

} // namespace codec

// ─── options ────────────────────────────────────────────────────────────────

export enum class ResolverBackend : std::uint8_t {
	native_udp,
	nss_thread,
};
export struct ResolveOptions {
	AddressFamily prefer{AddressFamily::v6};
	bool allow_v4{true};
	bool allow_v6{true};
	std::chrono::milliseconds query_timeout{2000};
	std::chrono::milliseconds total_timeout{5000};
	bool bypass_cache{false};
	std::vector<NameserverEndpoint> override_nameservers{};
};
export struct ResolverOptions {
	size_t cache_capacity{1024};
	std::chrono::seconds cache_max_ttl{300};
	std::chrono::seconds cache_negative_ttl{30};
	std::filesystem::path resolv_conf{"/etc/resolv.conf"};
	std::filesystem::path hosts_file{"/etc/hosts"};
	bool enable_etc_hosts{true};
	std::uint16_t edns0_udp_size{4096};
	size_t max_in_flight_queries{4096};
	std::vector<NameserverEndpoint> override_nameservers{};
};
// ─── Resolver ───────────────────────────────────────────────────────────────

export class Resolver {
public:
	Resolver(::io_uring *ring, CompletionTable *completions, UserDataFn encode_ud, ResolverOptions opts = {});

	explicit Resolver(WorkPool &pool, ResolverOptions opts = {});

	~Resolver();
	Resolver(Resolver const &) = delete;
	Resolver &operator =(Resolver const &) = delete;
	Resolver(Resolver &&) = delete;
	Resolver &operator =(Resolver &&) = delete;

	[[nodiscard]] conflux::work::root::Task<ResolveResult>
	resolve(std::string_view host, std::uint16_t port, ResolveOptions const &opts = {});

	// ring must outlive the returned Task and any coalesced waiters sharing that ring
	[[nodiscard]] conflux::work::root::Task<ResolveResult>
	resolve(SocketTaskRing &ring, std::string_view host, std::uint16_t port, ResolveOptions const &opts = {});

	[[nodiscard]] std::expected<ResolveResult, DnsError>
	resolve_blocking(std::string_view host, std::uint16_t port, ResolveOptions const &opts = {});

	void invalidate(std::string_view host);
	void clear_cache();
	void reload();

	[[nodiscard]] ResolverBackend backend() const noexcept;

	// Exposed for tests that pump the ring directly (e.g. coalescing tests).
	[[nodiscard]] conflux::file_io::FileReader *file_reader() const noexcept;

private:
	[[nodiscard]] root::Task<ResolveResult> resolve_flow(
		SocketTaskRing *external_ring,
		std::string_view host,
		std::uint16_t port,
		ResolveOptions const &opts = {});
	[[nodiscard]] root::Task<ResolveResult> resolve_nss_thread(
		SocketTaskRing *external_ring,
		std::string_view host,
		std::uint16_t port,
		ResolveOptions const &opts,
		std::string const &cache_key);
	[[nodiscard]] root::Task<ResolveResult> resolve_native_udp(
		SocketTaskRing *external_ring,
		SocketTaskRing *task_ring,
		std::string_view host,
		std::uint16_t port,
		ResolveOptions const &opts,
		std::string const &cache_key);

	struct Impl;
	std::shared_ptr<Impl> impl_;
};
// ─── std::thread-local current resolver ──────────────────────────────────────────

export [[nodiscard]] Resolver *current_resolver() noexcept;

export class CurrentResolverScope {
	Resolver *prev_{};

public:
	explicit CurrentResolverScope(Resolver *next) noexcept;
	~CurrentResolverScope();
	CurrentResolverScope(CurrentResolverScope const &) = delete;
	CurrentResolverScope &operator =(CurrentResolverScope const &) = delete;
	CurrentResolverScope(CurrentResolverScope &&) = delete;
	CurrentResolverScope &operator =(CurrentResolverScope &&) = delete;
};

} // namespace conflux::net::dns
