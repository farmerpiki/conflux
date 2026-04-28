module;

#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>

export module conflux.net.dns;

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.net.udp;

using std::exception_ptr;
using std::expected;
using std::optional;
using std::span;
using std::string;
using std::string_view;
using std::unexpected;
using std::unordered_map;
using std::vector;

namespace udp_ns = conflux::net::udp;

namespace conflux::net::dns {

// ─── address family / endpoint ──────────────────────────────────────────────

export enum class AddressFamily : u8 {
	v4,
	v6,
};

export struct Endpoint {
	::sockaddr_storage addr{};
	::socklen_t addr_len{};
	AddressFamily family{};
};

export struct ResolveResult {
	vector<Endpoint> endpoints;
	std::chrono::nanoseconds elapsed{};
	bool from_cache{false};
	bool from_hosts_file{false};
	bool from_coalesced{false};
};

// ─── errors ─────────────────────────────────────────────────────────────────

export enum class DnsErrorKind : u8 {
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
		string const &msg,
		int err = 0,
		optional<u8> r = {})
		: std::runtime_error{msg}
		, kind{k}
		, os_errno{err}
		, rcode{r} {}

	DnsErrorKind kind;
	int os_errno{0};
	optional<u8> rcode{};
};

// ─── nameserver endpoint ────────────────────────────────────────────────────

export struct NameserverEndpoint {
	::sockaddr_storage addr{};
	::socklen_t addr_len{};
	u16 port{53};
};

// Parse "ip", "ip:port", "[ipv6]:port", "[ipv6]". Returns endpoint with
// AF_INET or AF_INET6 set in addr. On failure returns the unsupported
// literal so callers can surface a useful diagnostic.
export [[nodiscard]] expected<NameserverEndpoint, string> parse_nameserver(
	string_view literal) {
	if (literal.empty()) {
		return unexpected{string{"empty nameserver literal"}};
	}

	string host;
	u16 port = 53;

	if (literal.front() == '[') {
		auto const close = literal.find(']');
		if (close == string_view::npos) {
			return unexpected{string{"unterminated '[' in nameserver literal"}};
		}
		host.assign(literal.substr(1, close - 1));
		auto rest = literal.substr(close + 1);
		if (!rest.empty()) {
			if (rest.front() != ':') {
				return unexpected{string{"expected ':<port>' after ']' in nameserver literal"}};
			}
			rest.remove_prefix(1);
			if (rest.empty()) {
				return unexpected{string{"missing port after ':' in nameserver literal"}};
			}
			u32 parsed = 0;
			for (char const c: rest) {
				if (c < '0' || c > '9') {
					return unexpected{std::format("invalid port '{}' in nameserver literal", rest)};
				}
				parsed = parsed * 10 + static_cast<u32>(c - '0');
				if (parsed > 0xFFFFU) {
					return unexpected{std::format("port out of range '{}' in nameserver literal", rest)};
				}
			}
			port = static_cast<u16>(parsed);
		}
	} else {
		// Either bare IPv4 ("1.2.3.4"), bare IPv6 ("::1"), or "ipv4:port".
		// Distinguish: bare IPv6 contains ':', but so does "ipv4:port".
		// IPv6 always has ≥ 2 colons or contains "::"; IPv4 has at most 1.
		auto const colons = std::ranges::count(literal, ':');
		bool const is_ipv6 = colons >= 2 || literal.find("::") != string_view::npos;
		if (!is_ipv6 && colons == 1) {
			auto const colon = literal.find(':');
			host.assign(literal.substr(0, colon));
			auto rest = literal.substr(colon + 1);
			if (rest.empty()) {
				return unexpected{string{"missing port after ':' in nameserver literal"}};
			}
			u32 parsed = 0;
			for (char const c: rest) {
				if (c < '0' || c > '9') {
					return unexpected{std::format("invalid port '{}' in nameserver literal", rest)};
				}
				parsed = parsed * 10 + static_cast<u32>(c - '0');
				if (parsed > 0xFFFFU) {
					return unexpected{std::format("port out of range '{}' in nameserver literal", rest)};
				}
			}
			port = static_cast<u16>(parsed);
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
		return unexpected{std::format("not an IP literal: '{}'", host)};
	}
	ns.port = port;
	return ns;
}

// ─── hostname validation (RFC 2181 §11) ─────────────────────────────────────

// Length-only checks. Allow underscores, digits, hyphens; reject NUL and
// dot-only / empty labels. Total length ≤ 253 bytes (excluding final '.'),
// per-label ≤ 63 bytes.
export [[nodiscard]] bool is_valid_hostname(
	string_view name) noexcept {
	if (name.empty()) {
		return false;
	}
	// Strip optional trailing root dot for length accounting.
	string_view trimmed = name;
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

// Returns an Endpoint if `host` is an IPv4 or IPv6 literal, else nullopt.
// `port` is written into the sockaddr in network order.
export [[nodiscard]] optional<Endpoint> try_parse_ip_literal(
	string_view host,
	u16 port) noexcept {
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

export enum class QType : u16 {
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

export enum class QClass : u16 {
	in = 1,
};

export enum class RCode : u8 {
	noerror = 0,
	formerr = 1,
	servfail = 2,
	nxdomain = 3,
	notimp = 4,
	refused = 5,
};

// Header flags layout (16-bit field after id, big-endian).
//   QR | OPCODE(4) | AA | TC | RD || RA | Z(3) | RCODE(4)
constexpr u16 kFlagQR = 0x8000U;
constexpr u16 kFlagAA = 0x0400U;
constexpr u16 kFlagTC = 0x0200U;
constexpr u16 kFlagRD = 0x0100U;
constexpr u16 kFlagRA = 0x0080U;
constexpr u16 kRCodeMask = 0x000FU;

export struct Header {
	u16 id{};
	u16 flags{};
	u16 qdcount{};
	u16 ancount{};
	u16 nscount{};
	u16 arcount{};

	[[nodiscard]] bool qr() const noexcept { return (flags & kFlagQR) != 0; }
	[[nodiscard]] bool tc() const noexcept { return (flags & kFlagTC) != 0; }
	[[nodiscard]] bool rd() const noexcept { return (flags & kFlagRD) != 0; }
	[[nodiscard]] bool ra() const noexcept { return (flags & kFlagRA) != 0; }
	[[nodiscard]] RCode rcode() const noexcept { return static_cast<RCode>(flags & kRCodeMask); }
};

export struct Question {
	string name; // canonical lowercase, no trailing root, dot-separated
	QType qtype{QType::a};
	QClass qclass{QClass::in};
};

export struct ResourceRecord {
	string name;
	QType type{QType::a};
	QClass rclass{QClass::in};
	u32 ttl{0};
	vector<u8> rdata; // raw RDATA (uncompressed for OPT; for compressed names,
					  // callers parse via decode helpers below)
};

export struct Message {
	Header header{};
	vector<Question> questions;
	vector<ResourceRecord> answers;
	vector<ResourceRecord> authority;
	vector<ResourceRecord> additional;
};

// EDNS0 OPT pseudo-RR.
export struct Edns0Options {
	u16 udp_size{4096};
	u8 ext_rcode{0};
	u8 version{0};
	u16 flags{0}; // DO bit etc; 0 for v1
};

// Encode a query (header + single question + optional EDNS0 OPT in additional).
// Returns the wire bytes. RD=1 set unconditionally.
export [[nodiscard]] vector<u8> encode_query(
	u16 id,
	string_view qname,
	QType qtype,
	optional<Edns0Options> edns = Edns0Options{}) {
	vector<u8> out;
	out.reserve(64);

	auto write_u16 = [&](u16 v) {
		out.push_back(static_cast<u8>(v >> 8));
		out.push_back(static_cast<u8>(v & 0xFFU));
	};
	auto write_u32 = [&](u32 v) {
		out.push_back(static_cast<u8>(v >> 24));
		out.push_back(static_cast<u8>((v >> 16) & 0xFFU));
		out.push_back(static_cast<u8>((v >> 8) & 0xFFU));
		out.push_back(static_cast<u8>(v & 0xFFU));
	};

	// Header
	u16 const arcount = edns.has_value() ? 1U : 0U;
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
			auto const len = (dot == string_view::npos ? qname.size() : dot) - pos;
			if (len == 0) {
				// empty label (consecutive dots) — caller should have validated
				break;
			}
			if (len > 63U) {
				// caller didn't validate; truncate label gracefully by clamping
				// no — refuse via assert-like exception. Use throw for codec contract.
				throw DnsError{
					DnsErrorKind::invalid_hostname,
					std::format("encode_query: label > 63 bytes in '{}'", qname)};
			}
			out.push_back(static_cast<u8>(len));
			out.insert(
				out.end(),
				qname.begin() + static_cast<std::ptrdiff_t>(pos),
				qname.begin() + static_cast<std::ptrdiff_t>(pos + len));
			pos += len + (dot == string_view::npos ? 0 : 1);
		}
		out.push_back(0); // root
	}

	// QTYPE / QCLASS
	write_u16(static_cast<u16>(qtype));
	write_u16(static_cast<u16>(QClass::in));

	// EDNS0 OPT — root name, type=41, class=udp_size, ttl=(extrcode<<24|ver<<16|flags),
	// rdlength=0.
	if (edns.has_value()) {
		out.push_back(0); // root name
		write_u16(static_cast<u16>(QType::opt));
		write_u16(edns->udp_size);
		write_u32(
			(static_cast<u32>(edns->ext_rcode) << 24)
			| (static_cast<u32>(edns->version) << 16)
			| static_cast<u32>(edns->flags));
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
	span<u8 const> wire,
	size_t offset,
	string &out) {
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
		u8 const len = wire[cursor];
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

[[nodiscard]] u16 read_u16(
	span<u8 const> wire,
	size_t offset) {
	if (offset + 2 > wire.size()) {
		throw DnsError{DnsErrorKind::malformed, "read_u16: short read"};
	}
	return static_cast<u16>((static_cast<u16>(wire[offset]) << 8) | wire[offset + 1]);
}

[[nodiscard]] u32 read_u32(
	span<u8 const> wire,
	size_t offset) {
	if (offset + 4 > wire.size()) {
		throw DnsError{DnsErrorKind::malformed, "read_u32: short read"};
	}
	return (static_cast<u32>(wire[offset]) << 24)
		 | (static_cast<u32>(wire[offset + 1]) << 16)
		 | (static_cast<u32>(wire[offset + 2]) << 8)
		 | static_cast<u32>(wire[offset + 3]);
}

// Parse a full message (header + sections). Throws DnsError on malformed input.
// RDATA is captured as raw bytes — callers project A/AAAA from it via the
// rdata_to_endpoint helpers below.
export [[nodiscard]] Message decode_message(
	span<u8 const> wire) {
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
		u16 const rdlen = read_u16(wire, pos + 8);
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

	m.questions.reserve(m.header.qdcount);
	for (u16 i = 0; i < m.header.qdcount; ++i) {
		m.questions.push_back(read_question());
	}
	m.answers.reserve(m.header.ancount);
	for (u16 i = 0; i < m.header.ancount; ++i) {
		m.answers.push_back(read_rr());
	}
	m.authority.reserve(m.header.nscount);
	for (u16 i = 0; i < m.header.nscount; ++i) {
		m.authority.push_back(read_rr());
	}
	m.additional.reserve(m.header.arcount);
	for (u16 i = 0; i < m.header.arcount; ++i) {
		m.additional.push_back(read_rr());
	}
	return m;
}

// Convert an A or AAAA RR into an Endpoint. Returns nullopt for other types.
export [[nodiscard]] optional<Endpoint> rdata_to_endpoint(
	ResourceRecord const &rr,
	u16 port) {
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

// Map RCODE → DnsErrorKind. RCODE 0 = noerror returns nullopt.
export [[nodiscard]] optional<DnsErrorKind> rcode_to_error(
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

export enum class ResolverBackend : u8 {
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
	vector<NameserverEndpoint> override_nameservers{};
};

export struct ResolverOptions {
	size_t cache_capacity{1024};
	std::chrono::seconds cache_max_ttl{300};
	std::chrono::seconds cache_negative_ttl{30};
	std::filesystem::path resolv_conf{"/etc/resolv.conf"};
	std::filesystem::path hosts_file{"/etc/hosts"};
	bool enable_etc_hosts{true};
	u16 edns0_udp_size{4096};
	size_t max_in_flight_queries{4096};
	vector<NameserverEndpoint> override_nameservers{};
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

	[[nodiscard]] Flow<ResolveResult> resolve(string_view host, u16 port, ResolveOptions const &opts = {});

	[[nodiscard]] expected<ResolveResult, DnsError>
	resolve_blocking(string_view host, u16 port, ResolveOptions const &opts = {});

	void invalidate(string_view host);
	void clear_cache();

	[[nodiscard]] ResolverBackend backend() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// ─── thread-local current resolver ──────────────────────────────────────────

namespace {

thread_local Resolver *tls_current_resolver{nullptr};

} // namespace

export [[nodiscard]] Resolver *current_resolver() noexcept {
	return tls_current_resolver;
}

export class CurrentResolverScope {
	Resolver *prev_;

public:
	explicit CurrentResolverScope(
		Resolver *next) noexcept
		: prev_{tls_current_resolver} {
		tls_current_resolver = next;
	}
	~CurrentResolverScope() { tls_current_resolver = prev_; }
	CurrentResolverScope(CurrentResolverScope const &) = delete;
	CurrentResolverScope &operator =(CurrentResolverScope const &) = delete;
	CurrentResolverScope(CurrentResolverScope &&) = delete;
	CurrentResolverScope &operator =(CurrentResolverScope &&) = delete;
};

// ─── file-local helpers ──────────────────────────────────────────────────────

[[nodiscard]] vector<NameserverEndpoint> parse_resolv_conf(
	std::filesystem::path const &path) noexcept {
	vector<NameserverEndpoint> out;
	try {
		std::ifstream f{path};
		if (!f.is_open()) {
			return out;
		}
		string line;
		while (std::getline(f, line)) {
			if (line.empty() || line.front() == '#' || line.front() == ';') {
				continue;
			}
			constexpr std::string_view kToken = "nameserver";
			if (line.rfind(kToken, 0) != 0) {
				continue;
			}
			size_t pos = kToken.size();
			while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
				++pos;
			}
			string_view sv{line.data() + pos, line.size() - pos};
			while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
				sv.remove_suffix(1);
			}
			if (auto ns = parse_nameserver(sv); ns.has_value()) {
				out.push_back(*ns);
			}
		}
	} catch (...) {} // NOLINT(bugprone-empty-catch)
	return out;
}

[[nodiscard]] unordered_map<string, vector<Endpoint>> parse_hosts_file(
	std::filesystem::path const &path) noexcept {
	unordered_map<string, vector<Endpoint>> out;
	try {
		std::ifstream f{path};
		if (!f.is_open()) {
			return out;
		}
		string line;
		while (std::getline(f, line)) {
			if (auto hash = line.find('#'); hash != string::npos) {
				line.resize(hash);
			}
			size_t pos = 0;
			auto skip_ws = [&] {
				while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
					++pos;
				}
			};
			auto next_token = [&]() -> string_view {
				skip_ws();
				if (pos == line.size()) {
					return {};
				}
				size_t const start = pos;
				while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') {
					++pos;
				}
				return {line.data() + start, pos - start};
			};

			auto ip_sv = next_token();
			if (ip_sv.empty()) {
				continue;
			}

			Endpoint ep{};
			::in_addr v4{};
			::in6_addr v6{};
			string const ip_str{ip_sv};
			if (::inet_pton(AF_INET, ip_str.c_str(), &v4) == 1) {
				auto *sin = reinterpret_cast<::sockaddr_in *>(&ep.addr);
				sin->sin_family = AF_INET;
				sin->sin_addr = v4;
				sin->sin_port = 0;
				ep.addr_len = sizeof(::sockaddr_in);
				ep.family = AddressFamily::v4;
			} else if (::inet_pton(AF_INET6, ip_str.c_str(), &v6) == 1) {
				auto *sin6 = reinterpret_cast<::sockaddr_in6 *>(&ep.addr);
				sin6->sin6_family = AF_INET6;
				sin6->sin6_addr = v6;
				sin6->sin6_port = 0;
				ep.addr_len = sizeof(::sockaddr_in6);
				ep.family = AddressFamily::v6;
			} else {
				continue;
			}

			while (true) {
				auto name_sv = next_token();
				if (name_sv.empty()) {
					break;
				}
				string name{name_sv};
				for (char &c: name) {
					if (c >= 'A' && c <= 'Z') {
						c += 'a' - 'A';
					}
				}
				out[name].push_back(ep);
			}
		}
	} catch (...) {} // NOLINT(bugprone-empty-catch)
	return out;
}

// Single UDP DNS query: open ephemeral socket, send wire bytes to ns,
// receive response with timeout, decode and validate. Maps UdpError →
// DnsError so callers only see DnsError.
[[nodiscard]] Flow<codec::Message> udp_single_query(
	FileReader &reader,
	NameserverEndpoint ns,
	vector<u8> wire,
	std::chrono::milliseconds timeout) {
	constexpr size_t kRxSize = 4096;

	std::shared_ptr<udp_ns::UdpSocket> sock;
	try {
		sock =
			std::make_shared<udp_ns::UdpSocket>(udp_ns::UdpSocket::open_ephemeral(static_cast<int>(ns.addr.ss_family)));
	} catch (...) {
		FlowSource<codec::Message> const src;
		auto flow = src.flow();
		src.reject(std::current_exception());
		return flow;
	}

	auto wire_buf = std::make_shared<vector<u8>>(std::move(wire));
	auto rx_buf = std::make_shared<std::array<u8, kRxSize>>();

	return udp_ns::sendto(
			   reader,
			   *sock,
			   span<u8 const>{wire_buf->data(), wire_buf->size()},
			   reinterpret_cast<::sockaddr const *>(&ns.addr),
			   ns.addr_len)
		 | flat_then([reader_ptr = &reader, sock, rx_buf, timeout](size_t) mutable -> Flow<udp_ns::UdpRecvResult> {
			   return udp_ns::recvfrom_with_timeout(
				   *reader_ptr,
				   *sock,
				   span<u8>{rx_buf->data(), rx_buf->size()},
				   timeout);
		   })
		 | then([sock, wire_buf, rx_buf](udp_ns::UdpRecvResult result) -> codec::Message {
			   auto msg = codec::decode_message(span<u8 const>{rx_buf->data(), result.bytes});
			   if (!msg.header.qr()) {
				   throw DnsError{DnsErrorKind::malformed, "dns: response QR=0"};
			   }
			   if (auto k = codec::rcode_to_error(msg.header.rcode()); k.has_value()) {
				   throw DnsError{
					   *k,
					   std::format("dns: RCODE {}", static_cast<u8>(msg.header.rcode())),
					   0,
					   optional<u8>{static_cast<u8>(msg.header.rcode())}};
			   }
			   if (msg.header.tc()) {
				   throw DnsError{DnsErrorKind::truncated, "dns: response TC=1"};
			   }
			   return msg;
		   })
		 | on_error([](exception_ptr const &ep) -> codec::Message {
			   try {
				   std::rethrow_exception(ep);
			   } catch (DnsError const &) { throw; } catch (udp_ns::UdpError const &e) {
				   if (e.code().value() == ETIMEDOUT) {
					   throw DnsError{DnsErrorKind::timeout, "dns: query timed out"};
				   }
				   throw DnsError{DnsErrorKind::network, std::format("dns: udp error: {}", e.what()), e.code().value()};
			   } catch (...) { throw; }
			   return codec::Message{}; // unreachable
		   });
}

// ─── Resolver::Impl ─────────────────────────────────────────────────────────

struct Resolver::Impl {
	ResolverBackend backend{};
	std::unique_ptr<FileReader> reader{};
	WorkPool *pool{nullptr};
	ResolverOptions opts;
	vector<NameserverEndpoint> nameservers;
	unordered_map<string, vector<Endpoint>> hosts_cache;
};

Resolver::Resolver(
	::io_uring *ring,
	CompletionTable *completions,
	UserDataFn encode_ud,
	ResolverOptions opts)
	: impl_{std::make_unique<Impl>()} {
	impl_->backend = ResolverBackend::native_udp;
	impl_->reader = std::make_unique<FileReader>(ring, completions, std::move(encode_ud));
	impl_->opts = std::move(opts);
	impl_->nameservers = impl_->opts.override_nameservers.empty() ? parse_resolv_conf(impl_->opts.resolv_conf) :
																	impl_->opts.override_nameservers;
	if (impl_->opts.enable_etc_hosts) {
		impl_->hosts_cache = parse_hosts_file(impl_->opts.hosts_file);
	}
}

Resolver::Resolver(
	WorkPool &pool,
	ResolverOptions opts)
	: impl_{std::make_unique<Impl>()} {
	impl_->backend = ResolverBackend::nss_thread;
	impl_->pool = &pool;
	impl_->opts = std::move(opts);
	if (impl_->opts.enable_etc_hosts) {
		impl_->hosts_cache = parse_hosts_file(impl_->opts.hosts_file);
	}
}

Resolver::~Resolver() = default;

Flow<ResolveResult> Resolver::resolve(
	string_view host,
	u16 port,
	ResolveOptions const &per_opts) {
	if (auto ep = try_parse_ip_literal(host, port); ep.has_value()) {
		FlowSource<ResolveResult> const src;
		auto flow = src.flow();
		ResolveResult r;
		r.endpoints.push_back(*ep);
		src.resolve(std::move(r));
		return flow;
	}

	if (!is_valid_hostname(host)) {
		FlowSource<ResolveResult> const src;
		auto flow = src.flow();
		src.reject(
			std::make_exception_ptr(
				DnsError{DnsErrorKind::invalid_hostname, std::format("invalid hostname '{}'", host)}));
		return flow;
	}

	// /etc/hosts lookup
	if (impl_->opts.enable_etc_hosts && !per_opts.bypass_cache) {
		string key{host};
		for (char &c: key) {
			if (c >= 'A' && c <= 'Z') {
				c += 'a' - 'A';
			}
		}
		if (!key.empty() && key.back() == '.') {
			key.pop_back();
		}
		auto it = impl_->hosts_cache.find(key);
		if (it != impl_->hosts_cache.end()) {
			vector<Endpoint> eps;
			for (auto const &ep: it->second) {
				if (ep.family == AddressFamily::v4 && per_opts.allow_v4) {
					auto e = ep;
					reinterpret_cast<::sockaddr_in *>(&e.addr)->sin_port = htons(port);
					eps.push_back(e);
				} else if (ep.family == AddressFamily::v6 && per_opts.allow_v6) {
					auto e = ep;
					reinterpret_cast<::sockaddr_in6 *>(&e.addr)->sin6_port = htons(port);
					eps.push_back(e);
				}
			}
			if (!eps.empty()) {
				FlowSource<ResolveResult> const src;
				auto flow = src.flow();
				ResolveResult r;
				r.endpoints = std::move(eps);
				r.from_hosts_file = true;
				src.resolve(std::move(r));
				return flow;
			}
		}
	}

	if (impl_->backend == ResolverBackend::nss_thread) {
		FlowSource<ResolveResult> const src;
		auto flow = src.flow();
		src.reject(
			std::make_exception_ptr(
				DnsError{DnsErrorKind::not_implemented, "conflux.net.dns: nss_thread backend not yet implemented"}));
		return flow;
	}

	auto const &ns_list = per_opts.override_nameservers.empty() ? impl_->nameservers : per_opts.override_nameservers;

	if (ns_list.empty()) {
		FlowSource<ResolveResult> const src;
		auto flow = src.flow();
		src.reject(std::make_exception_ptr(DnsError{DnsErrorKind::no_servers, "resolve: no nameservers configured"}));
		return flow;
	}

	NameserverEndpoint ns = ns_list.front();
	FileReader *reader = impl_->reader.get();
	auto const timeout = per_opts.query_timeout;
	codec::Edns0Options edns{.udp_size = impl_->opts.edns0_udp_size};
	bool const do_v4 = per_opts.allow_v4;
	bool const do_v6 = per_opts.allow_v6;

	string hostname{host};
	if (!hostname.empty() && hostname.back() == '.') {
		hostname.pop_back();
	}

	u16 const qid_a = static_cast<u16>(std::random_device{}() & 0xFFFFU);
	u16 const qid_aaaa = static_cast<u16>((static_cast<u32>(qid_a) + 1U) & 0xFFFFU);

	// A query — timeout is swallowed (returns empty list); other errors propagate.
	Flow<vector<Endpoint>> a_flow = [&]() -> Flow<vector<Endpoint>> {
		if (!do_v4) {
			FlowSource<vector<Endpoint>> const src;
			auto f = src.flow();
			src.resolve({});
			return f;
		}
		auto wire = codec::encode_query(qid_a, hostname, codec::QType::a, edns);
		return udp_single_query(*reader, ns, std::move(wire), timeout)
			 | then([port](codec::Message const &msg) -> vector<Endpoint> {
				   vector<Endpoint> eps;
				   for (auto const &rr: msg.answers) {
					   if (auto ep = codec::rdata_to_endpoint(rr, port);
						   ep.has_value() && ep->family == AddressFamily::v4) {
						   eps.push_back(*ep);
					   }
				   }
				   return eps;
			   })
			 | on_error([](exception_ptr const &ep) -> vector<Endpoint> {
				   try {
					   std::rethrow_exception(ep);
				   } catch (DnsError const &de) {
					   if (de.kind == DnsErrorKind::timeout) {
						   return {};
					   }
					   throw;
				   }
				   return {};
			   });
	}();

	if (!do_v6) {
		return std::move(a_flow) | then([](vector<Endpoint> eps) -> ResolveResult {
				   ResolveResult r;
				   r.endpoints = std::move(eps);
				   return r;
			   });
	}

	// Chain AAAA after A; v4 results shared across then/on_error branches.
	return std::move(a_flow)
		 | flat_then(
			   [reader, ns, hostname = std::move(hostname), qid_aaaa, timeout, edns, port](
				   vector<Endpoint> v4eps) mutable -> Flow<ResolveResult> {
				   auto v4_ptr = std::make_shared<vector<Endpoint>>(std::move(v4eps));
				   auto wire_aaaa = codec::encode_query(qid_aaaa, hostname, codec::QType::aaaa, edns);
				   return udp_single_query(*reader, ns, std::move(wire_aaaa), timeout)
						| then([v4_ptr, port](codec::Message const &msg) -> ResolveResult {
							  vector<Endpoint> all = *v4_ptr;
							  for (auto const &rr: msg.answers) {
								  if (auto ep = codec::rdata_to_endpoint(rr, port);
									  ep.has_value() && ep->family == AddressFamily::v6) {
									  all.push_back(*ep);
								  }
							  }
							  ResolveResult r;
							  r.endpoints = std::move(all);
							  return r;
						  })
						| on_error([v4_ptr](exception_ptr const &ep) -> ResolveResult {
							  try {
								  std::rethrow_exception(ep);
							  } catch (DnsError const &de) {
								  if (de.kind == DnsErrorKind::timeout) {
									  ResolveResult r;
									  r.endpoints = *v4_ptr;
									  return r;
								  }
								  throw;
							  }
							  return {};
						  });
			   });
}

expected<ResolveResult, DnsError> Resolver::resolve_blocking(
	string_view host,
	u16 port,
	ResolveOptions const &opts) {
	if (current_resolver() == this && impl_->backend == ResolverBackend::native_udp) {
		return unexpected{
			DnsError{
					 DnsErrorKind::cannot_block_on_owned_ring,
					 "resolve_blocking: caller owns this resolver's ring; would deadlock"}
        };
	}

	if (auto ep = try_parse_ip_literal(host, port); ep.has_value()) {
		ResolveResult r;
		r.endpoints.push_back(*ep);
		return r;
	}

	if (!is_valid_hostname(host)) {
		return unexpected{
			DnsError{DnsErrorKind::invalid_hostname, std::format("invalid hostname '{}'", host)}
        };
	}

	if (impl_->opts.enable_etc_hosts && !opts.bypass_cache) {
		string key{host};
		for (char &c: key) {
			if (c >= 'A' && c <= 'Z') {
				c += 'a' - 'A';
			}
		}
		if (!key.empty() && key.back() == '.') {
			key.pop_back();
		}
		auto it = impl_->hosts_cache.find(key);
		if (it != impl_->hosts_cache.end()) {
			vector<Endpoint> eps;
			for (auto const &ep: it->second) {
				if (ep.family == AddressFamily::v4 && opts.allow_v4) {
					auto e = ep;
					reinterpret_cast<::sockaddr_in *>(&e.addr)->sin_port = htons(port);
					eps.push_back(e);
				} else if (ep.family == AddressFamily::v6 && opts.allow_v6) {
					auto e = ep;
					reinterpret_cast<::sockaddr_in6 *>(&e.addr)->sin6_port = htons(port);
					eps.push_back(e);
				}
			}
			if (!eps.empty()) {
				ResolveResult r;
				r.endpoints = std::move(eps);
				r.from_hosts_file = true;
				return r;
			}
		}
	}

	return unexpected{
		DnsError{
				 DnsErrorKind::not_implemented,
				 "conflux.net.dns: resolve_blocking not yet implemented for this backend"}
    };
}

void Resolver::invalidate(
	string_view host) {
	(void)host;
}

void Resolver::clear_cache() {}

ResolverBackend Resolver::backend() const noexcept {
	return impl_->backend;
}

} // namespace conflux::net::dns
