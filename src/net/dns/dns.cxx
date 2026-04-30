module;

#include <arpa/inet.h>
#include <cstring>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

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

namespace detail {

template<typename T>
[[nodiscard]] auto flow_to_root_task(
	Flow<T> flow) -> conflux::work::root::Task<T> {
	using namespace conflux::work::root;
	auto [task, src] = make_task_source<T>(SubmitOptions{.enable_cancellation = false});
	auto shared = std::make_shared<TaskSource<T>>(std::move(src));
	spawn(
		std::move(flow)
		| then([shared](T value) mutable { (void)shared->commit_success(Success<T>{std::move(value)}); })
		| on_error([shared](std::exception_ptr const &ep) mutable { (void)shared->commit_failure(ep); })
		| on_cancel([shared]() mutable { (void)shared->commit_cancelled(CancelReason::requested); }));
	return std::move(task);
}

} // namespace detail

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
	std::chrono::seconds suggested_ttl{0};
	bool from_cache{false};
	bool from_hosts_file{false};
	bool from_coalesced{false};
	bool is_negative{false}; // true = negative cache entry (NXDOMAIN)
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

export enum class QType : u16 { // NOLINT(performance-enum-size) — DNS wire values need u16
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

export enum class QClass : u16 { // NOLINT(performance-enum-size) — DNS wire values need u16
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

	[[nodiscard]] conflux::work::root::Task<ResolveResult>
	resolve(string_view host, u16 port, ResolveOptions const &opts = {});

	[[nodiscard]] expected<ResolveResult, DnsError>
	resolve_blocking(string_view host, u16 port, ResolveOptions const &opts = {});

	void invalidate(string_view host);
	void clear_cache();
	void reload();

	[[nodiscard]] ResolverBackend backend() const noexcept;

	// Exposed for tests that pump the ring directly (e.g. coalescing tests).
	[[nodiscard]] FileReader *file_reader() const noexcept;

private:
	[[nodiscard]] Flow<ResolveResult> resolve_flow(string_view host, u16 port, ResolveOptions const &opts = {});

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

struct ResolvConfig {
	vector<NameserverEndpoint> nameservers;
	vector<string> search_domains;
	size_t ndots{1};
	std::chrono::milliseconds query_timeout{0};
	size_t attempts{1};
};

[[nodiscard]] string trim_ascii_copy(
	string_view sv) {
	while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
		sv.remove_prefix(1);
	}
	while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
		sv.remove_suffix(1);
	}
	return string{sv};
}

[[nodiscard]] optional<size_t> parse_decimal_size(
	string_view sv) noexcept {
	if (sv.empty()) {
		return std::nullopt;
	}
	size_t out = 0;
	for (char const c: sv) {
		if (c < '0' || c > '9') {
			return std::nullopt;
		}
		out = (out * 10U) + static_cast<size_t>(c - '0');
	}
	return out;
}

void parse_resolv_options(
	string_view rest,
	ResolvConfig &cfg) {
	while (!rest.empty()) {
		while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
			rest.remove_prefix(1);
		}
		auto const end = rest.find_first_of(" \t");
		string_view const token = end == string_view::npos ? rest : rest.substr(0, end);
		if (end == string_view::npos) {
			rest = {};
		} else {
			rest.remove_prefix(end + 1);
		}

		auto parse_after_colon = [](string_view value, string_view prefix) -> optional<size_t> {
			if (!value.starts_with(prefix)) {
				return std::nullopt;
			}
			return parse_decimal_size(value.substr(prefix.size()));
		};

		if (auto timeout = parse_after_colon(token, "timeout:"); timeout.has_value() && *timeout > 0) {
			cfg.query_timeout = std::chrono::seconds{*timeout};
		} else if (auto attempts = parse_after_colon(token, "attempts:"); attempts.has_value() && *attempts > 0) {
			cfg.attempts = *attempts;
		} else if (auto ndots = parse_after_colon(token, "ndots:"); ndots.has_value()) {
			cfg.ndots = *ndots;
		}
	}
}

[[nodiscard]] vector<string> parse_search_domains(
	string_view rest) {
	vector<string> out;
	while (!rest.empty()) {
		while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
			rest.remove_prefix(1);
		}
		auto const end = rest.find_first_of(" \t");
		string token = trim_ascii_copy(end == string_view::npos ? rest : rest.substr(0, end));
		if (end == string_view::npos) {
			rest = {};
		} else {
			rest.remove_prefix(end + 1);
		}
		if (token.empty()) {
			continue;
		}
		for (char &c: token) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c + ('a' - 'A'));
			}
		}
		if (!token.empty() && token.back() == '.') {
			token.pop_back();
		}
		if (!token.empty()) {
			out.push_back(std::move(token));
		}
	}
	return out;
}

[[nodiscard]] ResolvConfig parse_resolv_conf(
	std::filesystem::path const &path) noexcept {
	ResolvConfig out;
	try {
		std::ifstream f{path};
		if (!f.is_open()) {
			return out;
		}
		string line;
		while (std::getline(f, line)) {
			if (auto comment = line.find_first_of("#;"); comment != string::npos) {
				line.resize(comment);
			}
			auto trimmed = trim_ascii_copy(line);
			if (trimmed.empty()) {
				continue;
			}
			auto const split = trimmed.find_first_of(" \t");
			string_view const key{trimmed.data(), split == string::npos ? trimmed.size() : split};
			string_view rest{};
			if (split != string::npos) {
				rest = string_view{trimmed.data() + split + 1, trimmed.size() - split - 1};
			}
			if (key == "nameserver") {
				auto const sv = trim_ascii_copy(rest);
				if (auto ns = parse_nameserver(sv); ns.has_value()) {
					out.nameservers.push_back(*ns);
				}
				continue;
			}
			if (key == "options") {
				parse_resolv_options(rest, out);
				continue;
			}
			if (key == "search") {
				out.search_domains = parse_search_domains(rest);
				continue;
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

[[nodiscard]] string lowercase_ascii(
	string_view value) {
	string out{value};
	for (char &c: out) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c + ('a' - 'A'));
		}
	}
	return out;
}

[[nodiscard]] bool same_dns_peer(
	::sockaddr_storage const &actual,
	::socklen_t actual_len,
	NameserverEndpoint const &expected) noexcept {
	if (actual.ss_family != expected.addr.ss_family || actual_len == 0) {
		return false;
	}
	if (actual.ss_family == AF_INET) {
		if (actual_len < static_cast<::socklen_t>(sizeof(::sockaddr_in))
			|| expected.addr_len < static_cast<::socklen_t>(sizeof(::sockaddr_in))) {
			return false;
		}
		auto const *a = reinterpret_cast<::sockaddr_in const *>(&actual);
		auto const *e = reinterpret_cast<::sockaddr_in const *>(&expected.addr);
		return a->sin_port == e->sin_port && a->sin_addr.s_addr == e->sin_addr.s_addr;
	}
	if (actual.ss_family == AF_INET6) {
		if (actual_len < static_cast<::socklen_t>(sizeof(::sockaddr_in6))
			|| expected.addr_len < static_cast<::socklen_t>(sizeof(::sockaddr_in6))) {
			return false;
		}
		auto const *a = reinterpret_cast<::sockaddr_in6 const *>(&actual);
		auto const *e = reinterpret_cast<::sockaddr_in6 const *>(&expected.addr);
		return a->sin6_port == e->sin6_port
			&& std::memcmp(&a->sin6_addr, &e->sin6_addr, sizeof(::in6_addr)) == 0
			&& a->sin6_scope_id == e->sin6_scope_id;
	}
	return false;
}

[[nodiscard]] bool has_expected_question(
	codec::Message const &msg,
	u16 expected_id,
	string_view expected_qname,
	codec::QType expected_qtype) noexcept {
	if (msg.header.id != expected_id || msg.questions.empty()) {
		return false;
	}
	auto const &q = msg.questions.front();
	return q.name == expected_qname && q.qtype == expected_qtype && q.qclass == codec::QClass::in;
}

void validate_accepted_response_status(
	codec::Message const &msg) {
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
}

[[nodiscard]] Flow<codec::Message> recv_valid_udp_response(
	FileReader &reader,
	std::shared_ptr<udp_ns::UdpSocket> const &sock,
	std::shared_ptr<std::array<u8, 4096>> const &rx_buf,
	NameserverEndpoint ns,
	u16 expected_id,
	string expected_qname,
	codec::QType expected_qtype,
	std::chrono::steady_clock::time_point deadline) {
	auto const now = std::chrono::steady_clock::now();
	if (now >= deadline) {
		FlowSource<codec::Message> const src;
		auto flow = src.flow();
		src.reject(std::make_exception_ptr(DnsError{DnsErrorKind::timeout, "dns: query timed out"}));
		return flow;
	}
	auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
	return task_as_flow(
			   udp_ns::recvfrom_with_timeout(reader, *sock, span<u8>{rx_buf->data(), rx_buf->size()}, remaining))
		 | flat_then(
			   [reader_ptr = &reader,
				sock,
				rx_buf,
				ns,
				expected_id,
				expected_qname = std::move(expected_qname),
				expected_qtype,
				deadline](udp_ns::UdpRecvResult result) mutable -> Flow<codec::Message> {
				   auto msg = codec::decode_message(span<u8 const>{rx_buf->data(), result.bytes});
				   bool const source_ok = same_dns_peer(result.from, result.from_len, ns);
				   bool const question_ok = has_expected_question(msg, expected_id, expected_qname, expected_qtype);
				   if (!source_ok || !question_ok) {
					   return recv_valid_udp_response(
						   *reader_ptr,
						   sock,
						   rx_buf,
						   ns,
						   expected_id,
						   std::move(expected_qname),
						   expected_qtype,
						   deadline);
				   }
				   validate_accepted_response_status(msg);
				   FlowSource<codec::Message> const src;
				   auto flow = src.flow();
				   src.resolve(std::move(msg));
				   return flow;
			   });
}

// Single UDP DNS query: open ephemeral socket, send wire bytes to ns,
// receive response with timeout, decode and validate. Maps UdpError →
// DnsError so callers only see DnsError.
[[nodiscard]] Flow<codec::Message> udp_single_query(
	FileReader &reader,
	NameserverEndpoint ns,
	vector<u8> wire,
	u16 expected_id,
	string expected_qname,
	codec::QType expected_qtype,
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

	return task_as_flow(
			   udp_ns::sendto(
				   reader,
				   *sock,
				   span<u8 const>{wire_buf->data(), wire_buf->size()},
				   reinterpret_cast<::sockaddr const *>(&ns.addr),
				   ns.addr_len))
		 | flat_then(
			   [reader_ptr = &reader,
				sock,
				wire_buf,
				rx_buf,
				ns,
				expected_id,
				expected_qname = std::move(expected_qname),
				expected_qtype,
				timeout](size_t) mutable -> Flow<codec::Message> {
				   (void)wire_buf;
				   return recv_valid_udp_response(
					   *reader_ptr,
					   sock,
					   rx_buf,
					   ns,
					   expected_id,
					   std::move(expected_qname),
					   expected_qtype,
					   std::chrono::steady_clock::now() + timeout);
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

// TCP DNS query per RFC 1035 §4.2.2: 2-byte big-endian length prefix framing.
[[nodiscard]] Flow<codec::Message> tcp_single_query(
	FileReader &reader,
	NameserverEndpoint ns,
	vector<u8> wire,
	u16 expected_id,
	string const &expected_qname,
	codec::QType expected_qtype,
	std::chrono::milliseconds timeout) {
	auto framed = std::make_shared<vector<u8>>();
	framed->reserve(2 + wire.size());
	auto const wlen = static_cast<u16>(wire.size());
	framed->push_back(static_cast<u8>(wlen >> 8U));
	framed->push_back(static_cast<u8>(wlen & 0xFFU));
	framed->insert(framed->end(), wire.begin(), wire.end());
	int const family = static_cast<int>(ns.addr.ss_family);
	return task_as_flow(reader.socket_async(family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP))
		 | flat_then(
			   [reader_ptr = &reader, ns, framed, expected_id, expected_qname, expected_qtype, timeout](
				   FileHandle raw_fh) mutable -> Flow<codec::Message> {
				   auto fh = std::make_shared<FileHandle>(std::move(raw_fh));
				   if (timeout.count() > 0) {
					   auto const sec = std::chrono::duration_cast<std::chrono::seconds>(timeout);
					   auto const usec = std::chrono::duration_cast<std::chrono::microseconds>(timeout - sec).count();
					   ::timeval tv{};
					   tv.tv_sec = sec.count();
					   tv.tv_usec = static_cast<suseconds_t>(usec);
					   int const raw_fd = fh->raw_fd();
					   if (raw_fd >= 0) {
						   (void)::setsockopt(raw_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
						   (void)::setsockopt(raw_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
					   }
				   }
				   return task_as_flow(reader_ptr->connect_async(*fh, ns.addr, ns.addr_len))
						| flat_then([reader_ptr, fh, framed]() mutable -> Flow<size_t> {
							  return task_as_flow(
								  reader_ptr->send_async(*fh, framed->data(), framed->size(), MSG_NOSIGNAL));
						  })
						| flat_then(
							  [reader_ptr, fh, framed, expected_id, expected_qname, expected_qtype](
								  size_t sent) mutable -> Flow<codec::Message> {
								  if (sent != framed->size()) {
									  throw DnsError{DnsErrorKind::network, "dns: tcp short send"};
								  }
								  auto len_buf = std::make_shared<std::array<u8, 2>>();
								  return task_as_flow(reader_ptr->recv_async(*fh, len_buf->data(), 2, MSG_WAITALL))
									   | flat_then(
											 [reader_ptr, fh, len_buf, expected_id, expected_qname, expected_qtype](
												 size_t n) mutable -> Flow<codec::Message> {
												 if (n != 2) {
													 throw DnsError{
														 DnsErrorKind::network,
														 "dns: tcp short length prefix"};
												 }
												 u16 const resp_len = static_cast<u16>(
													 (static_cast<u16>((*len_buf)[0]) << 8U)
													 | static_cast<u16>((*len_buf)[1]));
												 if (resp_len == 0) {
													 throw DnsError{
														 DnsErrorKind::malformed,
														 "dns: tcp zero-length response"};
												 }
												 auto resp_buf = std::make_shared<vector<u8>>(resp_len);
												 return task_as_flow(reader_ptr->recv_async(
															*fh,
															resp_buf->data(),
															resp_len,
															MSG_WAITALL))
													  | then(
															[fh, resp_buf, expected_id, expected_qname, expected_qtype](
																size_t resp_n) -> codec::Message {
																if (resp_n != resp_buf->size()) {
																	throw DnsError{
																		DnsErrorKind::network,
																		"dns: tcp short response"};
																}
																auto msg = codec::decode_message(
																	span<u8 const>{resp_buf->data(), resp_n});
																if (!has_expected_question(
																		msg,
																		expected_id,
																		expected_qname,
																		expected_qtype)) {
																	throw DnsError{
																		DnsErrorKind::malformed,
																		"dns: tcp response mismatch"};
																}
																validate_accepted_response_status(msg);
																return msg;
															});
											 });
							  });
			   })
		 | on_error([](exception_ptr const &ep) -> codec::Message {
			   try {
				   std::rethrow_exception(ep);
			   } catch (DnsError const &) { throw; } catch (...) {
				   throw DnsError{DnsErrorKind::network, "dns: tcp query failed"};
			   }
			   return codec::Message{}; // unreachable
		   });
}

// Minimum TTL across all answer RRs of the given family (UINT32_MAX if none).
[[nodiscard]] u32 min_answer_ttl(
	codec::Message const &msg,
	AddressFamily family) noexcept {
	u32 min_ttl = std::numeric_limits<u32>::max();
	for (auto const &rr: msg.answers) {
		auto const is_match = (family == AddressFamily::v4 && rr.type == codec::QType::a)
						   || (family == AddressFamily::v6 && rr.type == codec::QType::aaaa);
		if (is_match) {
			min_ttl = std::min(min_ttl, rr.ttl);
		}
	}
	return min_ttl;
}

// Ordered by severity so max(a,b) gives the dominant failure reason.
enum class BatchFailReason : u8 {
	none = 0,
	timeout = 1,
	network = 2,
	nxdomain = 3,
	truncated = 4,
};

struct EndpointBatch {
	vector<Endpoint> eps;
	u32 min_ttl{std::numeric_limits<u32>::max()};
	BatchFailReason fail_reason{BatchFailReason::none};
	bool was_queried{false};
};

// ─── UDP flow builder (shared by resolve() and resolve_blocking()) ──────────

// Immediately-resolved fail batch for a queried family.
[[nodiscard]] Flow<EndpointBatch> resolve_to_fail_flow(
	BatchFailReason reason) {
	FlowSource<EndpointBatch> const src;
	auto f = src.flow();
	EndpointBatch b;
	b.fail_reason = reason;
	b.was_queried = true;
	src.resolve(std::move(b));
	return f;
}

// Build a Flow<EndpointBatch> for a single address family. Errors are absorbed
// into a BatchFailReason field so the parallel join (join_all) always completes.
// Cancellation is the only exception that still propagates.
[[nodiscard]] Flow<EndpointBatch> build_family_flow(
	FileReader &reader,
	NameserverEndpoint ns,
	SV hostname,
	u16 port,
	u16 qid,
	codec::QType qtype,
	AddressFamily fam,
	std::chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	auto wire = codec::encode_query(qid, hostname, qtype, edns);
	auto wire_ptr = make_shared<V<u8>>(wire); // copy for TCP fallback
	auto expected_qname = lowercase_ascii(hostname);
	return udp_single_query(reader, ns, std::move(wire), qid, expected_qname, qtype, timeout)
		 | then([fam, port](codec::Message const &msg) -> EndpointBatch {
			   EndpointBatch batch;
			   batch.was_queried = true;
			   batch.min_ttl = min_answer_ttl(msg, fam);
			   for (auto const &rr: msg.answers) {
				   if (auto ep = codec::rdata_to_endpoint(rr, port); ep.has_value() && ep->family == fam) {
					   batch.eps.push_back(*ep);
				   }
			   }
			   return batch;
		   })
		 | on_error(
			   [reader_ptr = &reader, ns, wire_ptr, timeout, fam, port, qid, hostname = string{hostname}, qtype](
				   EP const &eptr) -> Flow<EndpointBatch> {
				   try {
					   std::rethrow_exception(eptr);
				   } catch (DnsError const &de) {
					   if (de.kind == DnsErrorKind::cancelled) {
						   throw;
					   }
					   if (de.kind == DnsErrorKind::truncated) {
						   return tcp_single_query(
									  *reader_ptr,
									  ns,
									  *wire_ptr,
									  qid,
									  lowercase_ascii(hostname),
									  qtype,
									  timeout)
								| then([fam, port](codec::Message const &msg) -> EndpointBatch {
									  EndpointBatch b;
									  b.was_queried = true;
									  b.min_ttl = min_answer_ttl(msg, fam);
									  for (auto const &rr: msg.answers) {
										  if (auto ep = codec::rdata_to_endpoint(rr, port);
											  ep.has_value() && ep->family == fam) {
											  b.eps.push_back(*ep);
										  }
									  }
									  return b;
								  })
								| on_error([](EP const &) -> EndpointBatch {
									  EndpointBatch b;
									  b.fail_reason = BatchFailReason::truncated;
									  b.was_queried = true;
									  return b;
								  });
					   }
					   auto const r = (de.kind == DnsErrorKind::nxdomain) ? BatchFailReason::nxdomain :
									  (de.kind == DnsErrorKind::timeout)  ? BatchFailReason::timeout :
																			BatchFailReason::network;
					   return resolve_to_fail_flow(r);
				   }
				   return resolve_to_fail_flow(BatchFailReason::network);
			   });
}

// Immediate empty batch for a disabled address family (was_queried=false).
[[nodiscard]] Flow<EndpointBatch> make_empty_batch_flow() {
	FlowSource<EndpointBatch> const src;
	auto f = src.flow();
	src.resolve({});
	return f;
}

// Fire A and AAAA queries in parallel (RFC 8305 §3). Connection-attempt
// staggering belongs in the caller's connect loop, not here.
[[nodiscard]] Flow<ResolveResult> build_native_udp_flow(
	FileReader &reader,
	NameserverEndpoint ns,
	string const &hostname,
	u16 port,
	bool do_v4,
	bool do_v6,
	AddressFamily prefer,
	std::chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	u16 const qid_a = static_cast<u16>(std::random_device{}() & 0xFFFFU);
	u16 const qid_aaaa = static_cast<u16>((static_cast<u32>(qid_a) + 1U) & 0xFFFFU);

	auto a_flow =
		do_v4 ?
			build_family_flow(reader, ns, hostname, port, qid_a, codec::QType::a, AddressFamily::v4, timeout, edns) :
			make_empty_batch_flow();
	auto aaaa_flow = do_v6 ? build_family_flow(
								 reader,
								 ns,
								 hostname,
								 port,
								 qid_aaaa,
								 codec::QType::aaaa,
								 AddressFamily::v6,
								 timeout,
								 edns) :
							 make_empty_batch_flow();

	return join_all(std::move(a_flow), std::move(aaaa_flow))
		 | then([prefer](Tup<EndpointBatch, EndpointBatch> batches) -> ResolveResult {
			   auto [v4, v6] = std::move(batches);
			   if (v4.eps.empty() && v6.eps.empty()) {
				   // Both families have no results. Propagate the dominant failure.
				   auto const w = (static_cast<u8>(v4.fail_reason) >= static_cast<u8>(v6.fail_reason)) ?
									  v4.fail_reason :
									  v6.fail_reason;
				   if (w == BatchFailReason::truncated) {
					   throw DnsError{DnsErrorKind::truncated, "dns: udp truncated and tcp fallback failed"};
				   }
				   if (w == BatchFailReason::nxdomain) {
					   throw DnsError{DnsErrorKind::nxdomain, "dns: name not found"};
				   }
			   }
			   vector<Endpoint> all;
			   all.reserve(v6.eps.size() + v4.eps.size());
			   auto append_all = [&all](vector<Endpoint> const &eps) {
				   for (auto const &ep: eps) {
					   all.push_back(ep);
				   }
			   };
			   if (prefer == AddressFamily::v4) {
				   append_all(v4.eps);
				   append_all(v6.eps);
			   } else {
				   append_all(v6.eps);
				   append_all(v4.eps);
			   }
			   u32 const min_ttl = std::min(v4.min_ttl, v6.min_ttl);
			   ResolveResult r;
			   r.endpoints = std::move(all);
			   if (min_ttl != std::numeric_limits<u32>::max()) {
				   r.suggested_ttl = std::chrono::seconds{min_ttl};
			   }
			   return r;
		   });
}

[[nodiscard]] Flow<ResolveResult> build_native_udp_flow_with_nameservers(
	FileReader &reader,
	vector<NameserverEndpoint> nameservers,
	size_t index,
	string hostname,
	u16 port,
	bool do_v4,
	bool do_v6,
	AddressFamily prefer,
	std::chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	if (index >= nameservers.size()) {
		FlowSource<ResolveResult> const src;
		auto flow = src.flow();
		src.reject(std::make_exception_ptr(DnsError{DnsErrorKind::no_servers, "dns: no nameservers configured"}));
		return flow;
	}
	auto const ns = nameservers[index];
	auto query_host = hostname;
	return build_native_udp_flow(reader, ns, query_host, port, do_v4, do_v6, prefer, timeout, edns)
		 | flat_then(
			   [reader_ptr = &reader,
				nameservers = std::move(nameservers),
				index,
				hostname = std::move(hostname),
				port,
				do_v4,
				do_v6,
				prefer,
				timeout,
				edns](ResolveResult result) mutable -> Flow<ResolveResult> {
				   if (!result.endpoints.empty() || index + 1 >= nameservers.size()) {
					   FlowSource<ResolveResult> const src;
					   auto flow = src.flow();
					   src.resolve(std::move(result));
					   return flow;
				   }
				   return build_native_udp_flow_with_nameservers(
					   *reader_ptr,
					   std::move(nameservers),
					   index + 1,
					   std::move(hostname),
					   port,
					   do_v4,
					   do_v6,
					   prefer,
					   timeout,
					   edns);
			   });
}

[[nodiscard]] Flow<ResolveResult> build_native_udp_flow_with_candidates(
	FileReader &reader,
	vector<NameserverEndpoint> nameservers,
	vector<string> candidates,
	size_t index,
	u16 port,
	bool do_v4,
	bool do_v6,
	AddressFamily prefer,
	std::chrono::milliseconds timeout,
	codec::Edns0Options edns) {
	if (index >= candidates.size()) {
		FlowSource<ResolveResult> const src;
		auto flow = src.flow();
		src.reject(std::make_exception_ptr(DnsError{DnsErrorKind::nxdomain, "dns: name not found"}));
		return flow;
	}
	auto candidate = candidates[index];
	auto query_nameservers = nameservers;
	return build_native_udp_flow_with_nameservers(
			   reader,
			   std::move(query_nameservers),
			   0,
			   candidate,
			   port,
			   do_v4,
			   do_v6,
			   prefer,
			   timeout,
			   edns)
		 | on_error(
			   [reader_ptr = &reader,
				nameservers = std::move(nameservers),
				candidates = std::move(candidates),
				index,
				port,
				do_v4,
				do_v6,
				prefer,
				timeout,
				edns](exception_ptr const &ep) mutable -> Flow<ResolveResult> {
				   try {
					   std::rethrow_exception(ep);
				   } catch (DnsError const &de) {
					   if (de.kind != DnsErrorKind::nxdomain || index + 1 >= candidates.size()) {
						   throw;
					   }
					   return build_native_udp_flow_with_candidates(
						   *reader_ptr,
						   std::move(nameservers),
						   std::move(candidates),
						   index + 1,
						   port,
						   do_v4,
						   do_v6,
						   prefer,
						   timeout,
						   edns);
				   }
				   throw;
			   });
}

// ─── LRU TTL cache ──────────────────────────────────────────────────────────

struct DnsCacheEntry {
	ResolveResult result;
	std::chrono::steady_clock::time_point expires;
};

class LruDnsCache {
	using List = std::list<std::pair<string, DnsCacheEntry>>;
	size_t capacity_;
	List order_;
	unordered_map<string, List::iterator> index_;
	mutable std::mutex mtx_;

public:
	explicit LruDnsCache(
		size_t cap)
		: capacity_{cap} {}

	[[nodiscard]] std::optional<ResolveResult> get(
		string const &key) {
		std::scoped_lock const lk{mtx_};
		auto it = index_.find(key);
		if (it == index_.end()) {
			return std::nullopt;
		}
		if (std::chrono::steady_clock::now() >= it->second->second.expires) {
			order_.erase(it->second);
			index_.erase(it);
			return std::nullopt;
		}
		order_.splice(order_.begin(), order_, it->second);
		return it->second->second.result;
	}

	void put(
		string const &key,
		ResolveResult result,
		std::chrono::seconds ttl) {
		auto const expires = std::chrono::steady_clock::now() + ttl;
		std::scoped_lock const lk{mtx_};
		auto it = index_.find(key);
		if (it != index_.end()) {
			it->second->second = {std::move(result), expires};
			order_.splice(order_.begin(), order_, it->second);
			return;
		}
		if (order_.size() >= capacity_) {
			auto lru = std::prev(order_.end());
			index_.erase(lru->first);
			order_.erase(lru);
		}
		order_.push_front({
			key,
			{std::move(result), expires}
        });
		index_[key] = order_.begin();
	}

	void invalidate_by_host(
		string_view host) {
		string const prefix = std::format("{}:", host);
		std::scoped_lock const lk{mtx_};
		for (auto it = order_.begin(); it != order_.end();) {
			if (it->first.starts_with(prefix)) {
				index_.erase(it->first);
				it = order_.erase(it);
			} else {
				++it;
			}
		}
	}

	void clear() {
		std::scoped_lock const lk{mtx_};
		order_.clear();
		index_.clear();
	}
};

[[nodiscard]] string make_cache_key(
	string_view host,
	u16 port,
	AddressFamily prefer,
	bool v4,
	bool v6) {
	return std::format(
		"{}:{}:{}{}{}",
		host,
		port,
		prefer == AddressFamily::v4 ? '4' : '6',
		v4 ? '4' : '-',
		v6 ? '6' : '-');
}

[[nodiscard]] std::chrono::milliseconds effective_native_timeout(
	ResolveOptions const &opts) noexcept {
	if (opts.query_timeout.count() <= 0) {
		return opts.total_timeout;
	}
	if (opts.total_timeout.count() <= 0) {
		return opts.query_timeout;
	}
	return std::min(opts.query_timeout, opts.total_timeout);
}

[[nodiscard]] ResolveOptions apply_resolv_defaults(
	ResolveOptions opts,
	std::chrono::milliseconds resolv_query_timeout) noexcept {
	ResolveOptions const defaults;
	if (opts.query_timeout == defaults.query_timeout && resolv_query_timeout.count() > 0) {
		opts.query_timeout = resolv_query_timeout;
	}
	return opts;
}

[[nodiscard]] vector<NameserverEndpoint> nameservers_with_attempts(
	vector<NameserverEndpoint> const &base,
	size_t attempts) {
	vector<NameserverEndpoint> out;
	if (base.empty()) {
		return out;
	}
	attempts = std::max<size_t>(attempts, 1);
	out.reserve(base.size() * attempts);
	for (size_t attempt = 0; attempt < attempts; ++attempt) {
		out.insert(out.end(), base.begin(), base.end());
	}
	return out;
}

[[nodiscard]] vector<string> resolve_candidates(
	string_view host,
	vector<string> const &search_domains,
	size_t ndots) {
	string normalized{host};
	if (!normalized.empty() && normalized.back() == '.') {
		normalized.pop_back();
		return {std::move(normalized)};
	}
	auto const dot_count = static_cast<size_t>(std::ranges::count(normalized, '.'));
	if (search_domains.empty() || dot_count >= ndots) {
		return {std::move(normalized)};
	}
	vector<string> out;
	out.reserve(search_domains.size() + 1);
	for (auto const &domain: search_domains) {
		out.push_back(std::format("{}.{}", normalized, domain));
	}
	out.push_back(std::move(normalized));
	return out;
}

// ─── Resolver::Impl ─────────────────────────────────────────────────────────

struct CoalescedBroadcast {
	vector<FlowSource<ResolveResult>> waiters;
};

struct Resolver::Impl {
	ResolverBackend backend{};
	std::unique_ptr<FileReader> reader{};
	WorkPool *pool{nullptr};
	ResolverOptions opts;
	vector<NameserverEndpoint> nameservers;
	vector<string> search_domains;
	size_t ndots{1};
	std::chrono::milliseconds resolv_query_timeout{0};
	size_t attempts{1};
	unordered_map<string, vector<Endpoint>> hosts_cache;
	std::shared_ptr<LruDnsCache> cache{};
	unordered_map<string, CoalescedBroadcast> in_flight;
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
	auto const resolv = parse_resolv_conf(impl_->opts.resolv_conf);
	impl_->nameservers =
		impl_->opts.override_nameservers.empty() ? resolv.nameservers : impl_->opts.override_nameservers;
	impl_->search_domains = resolv.search_domains;
	impl_->ndots = resolv.ndots;
	impl_->resolv_query_timeout = resolv.query_timeout;
	impl_->attempts = resolv.attempts;
	if (impl_->opts.enable_etc_hosts) {
		impl_->hosts_cache = parse_hosts_file(impl_->opts.hosts_file);
	}
	if (impl_->opts.cache_capacity > 0) {
		impl_->cache = std::make_shared<LruDnsCache>(impl_->opts.cache_capacity);
	}
}

Resolver::Resolver(
	WorkPool &pool,
	ResolverOptions opts)
	: impl_{std::make_unique<Impl>()} {
	impl_->backend = ResolverBackend::nss_thread;
	impl_->pool = &pool;
	impl_->opts = std::move(opts);
	auto const resolv = parse_resolv_conf(impl_->opts.resolv_conf);
	impl_->nameservers =
		impl_->opts.override_nameservers.empty() ? resolv.nameservers : impl_->opts.override_nameservers;
	impl_->search_domains = resolv.search_domains;
	impl_->ndots = resolv.ndots;
	impl_->resolv_query_timeout = resolv.query_timeout;
	impl_->attempts = resolv.attempts;
	if (impl_->opts.enable_etc_hosts) {
		impl_->hosts_cache = parse_hosts_file(impl_->opts.hosts_file);
	}
	if (impl_->opts.cache_capacity > 0) {
		impl_->cache = std::make_shared<LruDnsCache>(impl_->opts.cache_capacity);
	}
}

Resolver::~Resolver() = default;

Flow<ResolveResult> Resolver::resolve_flow(
	string_view host,
	u16 port,
	ResolveOptions const &per_opts) {
	auto const effective_opts = apply_resolv_defaults(per_opts, impl_->resolv_query_timeout);
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
	if (impl_->opts.enable_etc_hosts && !effective_opts.bypass_cache) {
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
				if (ep.family == AddressFamily::v4 && effective_opts.allow_v4) {
					auto e = ep;
					reinterpret_cast<::sockaddr_in *>(&e.addr)->sin_port = htons(port);
					eps.push_back(e);
				} else if (ep.family == AddressFamily::v6 && effective_opts.allow_v6) {
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

	// Unified key for LRU cache and in-flight coalescing (empty when bypass_cache)
	string const coalesce_key =
		effective_opts.bypass_cache ?
			string{} :
			make_cache_key(host, port, effective_opts.prefer, effective_opts.allow_v4, effective_opts.allow_v6);

	// LRU cache lookup
	if (impl_->cache && !coalesce_key.empty()) {
		if (auto hit = impl_->cache->get(coalesce_key); hit.has_value()) {
			FlowSource<ResolveResult> const src;
			auto flow = src.flow();
			if (hit->is_negative) {
				src.reject(std::make_exception_ptr(DnsError{DnsErrorKind::nxdomain, "dns: nxdomain (cached)"}));
				return flow;
			}
			hit->from_cache = true;
			src.resolve(std::move(*hit));
			return flow;
		}
	}

	auto cache_insert = [cache = impl_->cache, cache_key = coalesce_key, max_ttl = impl_->opts.cache_max_ttl](
							ResolveResult r) -> ResolveResult { // NOLINT(bugprone-exception-escape)
		try {
			if (cache && !cache_key.empty() && !r.endpoints.empty()) {
				auto const ttl = (r.suggested_ttl.count() > 0) ? std::min(r.suggested_ttl, max_ttl) : max_ttl;
				cache->put(cache_key, r, ttl);
			}
		} catch (...) {} // NOLINT(bugprone-empty-catch)
		return r;
	};

	if (impl_->backend == ResolverBackend::nss_thread) {
		FlowSource<ResolveResult> const src;
		auto flow = src.flow();
		bool const ok = impl_->pool->enqueue([src, // NOLINT(bugprone-exception-escape)
											  h = string{host},
											  port,
											  allow_v4 = effective_opts.allow_v4,
											  allow_v6 = effective_opts.allow_v6,
											  cache = impl_->cache,
											  cache_key = coalesce_key,
											  ttl = impl_->opts.cache_max_ttl]() mutable {
			try {
				addrinfo hints{};
				hints.ai_family = AF_UNSPEC;
				hints.ai_socktype = SOCK_STREAM;
				hints.ai_flags = AI_ADDRCONFIG;
				addrinfo *res = nullptr;
				string const p = std::to_string(port);
				int const gai = ::getaddrinfo(h.c_str(), p.c_str(), &hints, &res);
				if (gai != 0 || res == nullptr) {
					src.reject(
						std::make_exception_ptr(
							DnsError{DnsErrorKind::nxdomain, std::format("getaddrinfo: {}", ::gai_strerror(gai))}));
					return;
				}
				ResolveResult result;
				for (auto *rp = res; rp != nullptr; rp = rp->ai_next) {
					if (rp->ai_family == AF_INET && allow_v4) {
						Endpoint ep{};
						ep.addr_len = static_cast<::socklen_t>(rp->ai_addrlen);
						ep.family = AddressFamily::v4;
						std::memcpy(&ep.addr, rp->ai_addr, ep.addr_len);
						result.endpoints.push_back(ep);
					} else if (rp->ai_family == AF_INET6 && allow_v6) {
						Endpoint ep{};
						ep.addr_len = static_cast<::socklen_t>(rp->ai_addrlen);
						ep.family = AddressFamily::v6;
						std::memcpy(&ep.addr, rp->ai_addr, ep.addr_len);
						result.endpoints.push_back(ep);
					}
				}
				::freeaddrinfo(res);
				if (result.endpoints.empty()) {
					src.reject(
						std::make_exception_ptr(
							DnsError{DnsErrorKind::nxdomain, std::format("no usable addresses for '{}'", h)}));
					return;
				}
				if (cache && !cache_key.empty() && !result.endpoints.empty()) {
					cache->put(cache_key, result, ttl);
				}
				src.resolve(std::move(result));
			} catch (...) { src.reject(std::current_exception()); } // NOLINT(bugprone-empty-catch)
		});
		if (!ok) {
			src.reject(
				std::make_exception_ptr(DnsError{DnsErrorKind::cancelled, "nss_thread: work pool not accepting jobs"}));
		}
		return flow;
	}

	auto const base_ns =
		effective_opts.override_nameservers.empty() ? impl_->nameservers : effective_opts.override_nameservers;
	auto const ns_list = nameservers_with_attempts(base_ns, impl_->attempts);

	if (ns_list.empty()) {
		FlowSource<ResolveResult> const src;
		auto flow = src.flow();
		src.reject(std::make_exception_ptr(DnsError{DnsErrorKind::no_servers, "resolve: no nameservers configured"}));
		return flow;
	}

	// In-flight coalescing: second+ caller for same query attaches a waiter FlowSource
	// that the primary query's completion step will resolve/reject.
	if (!coalesce_key.empty()) {
		if (auto it = impl_->in_flight.find(coalesce_key); it != impl_->in_flight.end()) {
			FlowSource<ResolveResult> const waiter;
			auto flow = waiter.flow();
			it->second.waiters.push_back(waiter);
			return std::move(flow)
				 | then([](ResolveResult r) {
					   r.from_coalesced = true;
					   return r;
				   })
				 | on_cancel([]() -> ResolveResult {
					   throw DnsError{DnsErrorKind::cancelled, "dns: query cancelled"};
					   return {};
				   });
		}
		impl_->in_flight.emplace(coalesce_key, CoalescedBroadcast{});
	}

	FileReader *reader = impl_->reader.get();
	auto const timeout = effective_native_timeout(effective_opts);
	codec::Edns0Options const edns{.udp_size = impl_->opts.edns0_udp_size};
	bool const do_v4 = effective_opts.allow_v4;
	bool const do_v6 = effective_opts.allow_v6;
	auto const candidates = resolve_candidates(host, impl_->search_domains, impl_->ndots);

	auto fanout_success = // NOLINT(bugprone-exception-escape)
		[impl = impl_.get(), coalesce_key](ResolveResult r) -> ResolveResult {
		if (!coalesce_key.empty()) {
			if (auto it = impl->in_flight.find(coalesce_key); it != impl->in_flight.end()) {
				for (auto const &w: it->second.waiters) {
					auto copy = r;
					copy.from_coalesced = true;
					w.resolve(std::move(copy));
				}
				impl->in_flight.erase(it);
			}
		}
		return r;
	};

	auto fanout_error = // NOLINT(bugprone-exception-escape)
		[impl = impl_.get(), coalesce_key](exception_ptr const &ep) -> ResolveResult {
		if (!coalesce_key.empty()) {
			if (auto it = impl->in_flight.find(coalesce_key); it != impl->in_flight.end()) {
				for (auto const &w: it->second.waiters) {
					w.reject(ep);
				}
				impl->in_flight.erase(it);
			}
		}
		// Negative caching: store NXDOMAIN entries so repeat lookups skip the wire.
		try {
			std::rethrow_exception(ep);
		} catch (DnsError const &de) {
			if (de.kind == DnsErrorKind::nxdomain && impl->cache && !coalesce_key.empty()) {
				ResolveResult neg;
				neg.is_negative = true;
				try {
					impl->cache->put(coalesce_key, neg, impl->opts.cache_negative_ttl);
				} catch (...) {} // NOLINT(bugprone-empty-catch)
			}
			throw;
		}
		return {};
	};

	return build_native_udp_flow_with_candidates(
			   *reader,
			   ns_list,
			   candidates,
			   0,
			   port,
			   do_v4,
			   do_v6,
			   effective_opts.prefer,
			   timeout,
			   edns)
		 | then(std::move(cache_insert))
		 | then(std::move(fanout_success))
		 | on_error(std::move(fanout_error))
		 | on_cancel([impl = impl_.get(), coalesce_key]() -> ResolveResult {
			   if (!coalesce_key.empty()) {
				   if (auto it = impl->in_flight.find(coalesce_key); it != impl->in_flight.end()) {
					   auto cancelled =
						   std::make_exception_ptr(DnsError{DnsErrorKind::cancelled, "dns: query cancelled"});
					   for (auto const &w: it->second.waiters) {
						   w.reject(cancelled);
					   }
					   impl->in_flight.erase(it);
				   }
			   }
			   throw DnsError{DnsErrorKind::cancelled, "dns: query cancelled"};
			   return {};
		   });
}

conflux::work::root::Task<ResolveResult> Resolver::resolve(
	string_view host,
	u16 port,
	ResolveOptions const &opts) {
	return detail::flow_to_root_task(resolve_flow(host, port, opts));
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
	auto const effective_opts = apply_resolv_defaults(opts, impl_->resolv_query_timeout);

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

	if (impl_->opts.enable_etc_hosts && !effective_opts.bypass_cache) {
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
				if (ep.family == AddressFamily::v4 && effective_opts.allow_v4) {
					auto e = ep;
					reinterpret_cast<::sockaddr_in *>(&e.addr)->sin_port = htons(port);
					eps.push_back(e);
				} else if (ep.family == AddressFamily::v6 && effective_opts.allow_v6) {
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

	if (impl_->backend == ResolverBackend::nss_thread) {
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_ADDRCONFIG;
		addrinfo *res = nullptr;
		string const h{host};
		string const p = std::to_string(port);
		auto const t0 = std::chrono::steady_clock::now();
		int const gai = ::getaddrinfo(h.c_str(), p.c_str(), &hints, &res);
		auto const elapsed = std::chrono::steady_clock::now() - t0;
		if (gai != 0 || res == nullptr) {
			return unexpected{
				DnsError{DnsErrorKind::nxdomain, std::format("getaddrinfo: {}", ::gai_strerror(gai))}
            };
		}
		ResolveResult result;
		result.elapsed = elapsed;
		for (auto *rp = res; rp != nullptr; rp = rp->ai_next) {
			if (rp->ai_family == AF_INET && effective_opts.allow_v4) {
				Endpoint ep{};
				ep.addr_len = static_cast<::socklen_t>(rp->ai_addrlen);
				ep.family = AddressFamily::v4;
				std::memcpy(&ep.addr, rp->ai_addr, ep.addr_len);
				result.endpoints.push_back(ep);
			} else if (rp->ai_family == AF_INET6 && effective_opts.allow_v6) {
				Endpoint ep{};
				ep.addr_len = static_cast<::socklen_t>(rp->ai_addrlen);
				ep.family = AddressFamily::v6;
				std::memcpy(&ep.addr, rp->ai_addr, ep.addr_len);
				result.endpoints.push_back(ep);
			}
		}
		::freeaddrinfo(res);
		if (result.endpoints.empty()) {
			return unexpected{
				DnsError{DnsErrorKind::nxdomain, std::format("no usable addresses for '{}'", host)}
            };
		}
		return result;
	}

	// native_udp: spin a temporary ring for this synchronous call
	{
		auto const base_ns =
			effective_opts.override_nameservers.empty() ? impl_->nameservers : effective_opts.override_nameservers;
		auto const ns_list = nameservers_with_attempts(base_ns, impl_->attempts);
		if (ns_list.empty()) {
			return unexpected{
				DnsError{DnsErrorKind::no_servers, "resolve_blocking: no nameservers configured"}
            };
		}

		optional<DnsError> last_nxdomain;
		for (auto const &candidate: resolve_candidates(host, impl_->search_domains, impl_->ndots)) {
			string const cache_key = impl_->cache && !effective_opts.bypass_cache ? make_cache_key(
																						candidate,
																						port,
																						effective_opts.prefer,
																						effective_opts.allow_v4,
																						effective_opts.allow_v6) :
																					string{};
			if (impl_->cache && !effective_opts.bypass_cache) {
				if (auto hit = impl_->cache->get(cache_key); hit.has_value()) {
					if (hit->is_negative) {
						last_nxdomain = DnsError{DnsErrorKind::nxdomain, "dns: nxdomain (cached)"};
						continue;
					}
					hit->from_cache = true;
					return std::move(*hit);
				}
			}

			::io_uring tmp_ring{};
			if (::io_uring_queue_init(32, &tmp_ring, 0) < 0) {
				return unexpected{
					DnsError{DnsErrorKind::no_ring, "resolve_blocking: io_uring_queue_init failed"}
                };
			}
			struct RingGuard {
				::io_uring *r;
				~RingGuard() { ::io_uring_queue_exit(r); }
			} const guard{&tmp_ring};
			CompletionTable tmp_ct;
			FileReader tmp_reader{&tmp_ring, &tmp_ct, [](u32 slot, u32 gen) noexcept -> u64 {
									  return (static_cast<u64>(gen) << 32U) | slot;
								  }};
			codec::Edns0Options const edns{.udp_size = impl_->opts.edns0_udp_size};
			auto flow = build_native_udp_flow_with_nameservers(
				tmp_reader,
				ns_list,
				0,
				candidate,
				port,
				effective_opts.allow_v4,
				effective_opts.allow_v6,
				effective_opts.prefer,
				effective_native_timeout(effective_opts),
				edns);
			try {
				auto budget = effective_native_timeout(effective_opts) + std::chrono::milliseconds{500};
				auto result = block_on<ResolveResult>(tmp_reader, std::move(flow), budget);
				if (result.endpoints.empty()) {
					return result;
				}
				if (impl_->cache && !cache_key.empty() && !result.endpoints.empty()) {
					auto const max_ttl = impl_->opts.cache_max_ttl;
					auto const ttl =
						(result.suggested_ttl.count() > 0) ? std::min(result.suggested_ttl, max_ttl) : max_ttl;
					try {
						impl_->cache->put(cache_key, result, ttl);
					} catch (...) {} // NOLINT(bugprone-empty-catch)
				}
				return result;
			} catch (PumpTimeout const &) {
				return unexpected{
					DnsError{DnsErrorKind::timeout, "resolve_blocking: pump timeout"}
                };
			} catch (DnsError const &e) {
				if (e.kind == DnsErrorKind::nxdomain) {
					last_nxdomain = e;
					if (impl_->cache && !cache_key.empty()) {
						ResolveResult neg;
						neg.is_negative = true;
						try {
							impl_->cache->put(cache_key, neg, impl_->opts.cache_negative_ttl);
						} catch (...) {} // NOLINT(bugprone-empty-catch)
					}
					continue;
				}
				return unexpected{e};
			} catch (std::exception const &e) {
				return unexpected{
					DnsError{DnsErrorKind::network, std::format("resolve_blocking: {}", e.what())}
                };
			}
		}
		return unexpected{last_nxdomain.value_or(DnsError{DnsErrorKind::nxdomain, "dns: name not found"})};
	}
}

void Resolver::invalidate(
	string_view host) {
	if (impl_->cache) {
		impl_->cache->invalidate_by_host(host);
	}
}

void Resolver::clear_cache() {
	if (impl_->cache) {
		impl_->cache->clear();
	}
}

void Resolver::reload() {
	auto const resolv = parse_resolv_conf(impl_->opts.resolv_conf);
	if (impl_->opts.override_nameservers.empty()) {
		impl_->nameservers = resolv.nameservers;
	}
	impl_->search_domains = resolv.search_domains;
	impl_->ndots = resolv.ndots;
	impl_->resolv_query_timeout = resolv.query_timeout;
	impl_->attempts = resolv.attempts;
	if (impl_->opts.enable_etc_hosts) {
		impl_->hosts_cache = parse_hosts_file(impl_->opts.hosts_file);
	}
	clear_cache();
}

ResolverBackend Resolver::backend() const noexcept {
	return impl_->backend;
}

FileReader *Resolver::file_reader() const noexcept {
	return impl_->reader.get();
}

} // namespace conflux::net::dns
