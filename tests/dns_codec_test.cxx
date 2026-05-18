// Plain TU — not a module unit.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.work;
import conflux.net.dns;

using namespace std;
using namespace conflux::net::dns;
using namespace conflux::net::dns::codec;
// ---------------------------------------------------------------------------
// is_valid_hostname — RFC 2181 §11 length-only rules
// ---------------------------------------------------------------------------

TEST_CASE(
	"dns: hostname accepts ordinary names",
	"[dns][validator]") {
	CHECK(is_valid_hostname("example.com"));
	CHECK(is_valid_hostname("a.b.c"));
	CHECK(is_valid_hostname("api-v2.service.internal"));
	CHECK(is_valid_hostname("a"));
}
TEST_CASE(
	"dns: hostname accepts trailing root dot",
	"[dns][validator]") {
	CHECK(is_valid_hostname("example.com."));
}
TEST_CASE(
	"dns: hostname rejects empty / leading / trailing dots",
	"[dns][validator]") {
	CHECK_FALSE(is_valid_hostname(""));
	CHECK_FALSE(is_valid_hostname("."));
	CHECK_FALSE(is_valid_hostname(".example.com"));
	CHECK_FALSE(is_valid_hostname("example..com"));
}
TEST_CASE(
	"dns: hostname accepts underscored labels (RFC 2181 §11)",
	"[dns][validator]") {
	CHECK(is_valid_hostname("_dmarc.example.com"));
	CHECK(is_valid_hostname("_443._tcp.example.com"));
}
TEST_CASE(
	"dns: hostname enforces 63-byte label cap",
	"[dns][validator]") {
	string const ok63(63, 'a');
	string const bad64(64, 'a');
	CHECK(is_valid_hostname(ok63 + ".com"));
	CHECK_FALSE(is_valid_hostname(bad64 + ".com"));
}
TEST_CASE(
	"dns: hostname enforces 253-byte total cap",
	"[dns][validator]") {
	// 4 × 63-char labels + 3 dots = 255 chars — too long.
	string const lab(63, 'a');
	auto const long_name = lab + "." + lab + "." + lab + "." + lab;
	CHECK(long_name.size() == 255);
	CHECK_FALSE(is_valid_hostname(long_name));

	// 3 × 63 + 3 dots + 59-char label = 251 chars — OK.
	auto const ok_name = lab + "." + lab + "." + lab + "." + string(59, 'a');
	CHECK(ok_name.size() == 251);
	CHECK(is_valid_hostname(ok_name));
}
TEST_CASE(
	"dns: hostname rejects NUL byte",
	"[dns][validator]") {
	string nul = "abc";
	nul.push_back('\0');
	nul += "xyz.com";
	CHECK_FALSE(is_valid_hostname(nul));
}
// ---------------------------------------------------------------------------
// parse_nameserver
// ---------------------------------------------------------------------------

TEST_CASE(
	"dns: parse_nameserver bare IPv4 default port",
	"[dns][nameserver]") {
	auto r = parse_nameserver("8.8.8.8");
	REQUIRE(r.has_value());
	CHECK(r->port == 53);
	CHECK(r->addr_len == sizeof(::sockaddr_in));
	auto const &sin = *reinterpret_cast<::sockaddr_in const *>(&r->addr);
	CHECK(sin.sin_family == AF_INET);
	CHECK(ntohs(sin.sin_port) == 53);
}
TEST_CASE(
	"dns: parse_nameserver IPv4 with port",
	"[dns][nameserver]") {
	auto r = parse_nameserver("8.8.8.8:5353");
	REQUIRE(r.has_value());
	CHECK(r->port == 5353);
	auto const &sin = *reinterpret_cast<::sockaddr_in const *>(&r->addr);
	CHECK(ntohs(sin.sin_port) == 5353);
}
TEST_CASE(
	"dns: parse_nameserver bare IPv6",
	"[dns][nameserver]") {
	auto r = parse_nameserver("2001:4860:4860::8888");
	REQUIRE(r.has_value());
	CHECK(r->port == 53);
	CHECK(r->addr_len == sizeof(::sockaddr_in6));
	auto const &sin6 = *reinterpret_cast<::sockaddr_in6 const *>(&r->addr);
	CHECK(sin6.sin6_family == AF_INET6);
}
TEST_CASE(
	"dns: parse_nameserver bracketed IPv6 with port",
	"[dns][nameserver]") {
	auto r = parse_nameserver("[2001:4860:4860::8888]:5353");
	REQUIRE(r.has_value());
	CHECK(r->port == 5353);
	auto const &sin6 = *reinterpret_cast<::sockaddr_in6 const *>(&r->addr);
	CHECK(sin6.sin6_family == AF_INET6);
	CHECK(ntohs(sin6.sin6_port) == 5353);
}
TEST_CASE(
	"dns: parse_nameserver rejects garbage",
	"[dns][nameserver]") {
	CHECK_FALSE(parse_nameserver("").has_value());
	CHECK_FALSE(parse_nameserver("not-an-ip").has_value());
	CHECK_FALSE(parse_nameserver("[unterminated").has_value());
	CHECK_FALSE(parse_nameserver("8.8.8.8:").has_value());
	CHECK_FALSE(parse_nameserver("8.8.8.8:99999").has_value());
}
// ---------------------------------------------------------------------------
// try_parse_ip_literal — numeric short-circuit
// ---------------------------------------------------------------------------

TEST_CASE(
	"dns: ip literal v4 round-trips port",
	"[dns][literal]") {
	auto ep = try_parse_ip_literal("127.0.0.1", 8080);
	REQUIRE(ep.has_value());
	CHECK(ep->family == AddressFamily::v4);
	auto const &sin = *reinterpret_cast<::sockaddr_in const *>(&ep->addr);
	CHECK(sin.sin_family == AF_INET);
	CHECK(ntohs(sin.sin_port) == 8080);
}
TEST_CASE(
	"dns: ip literal v6 round-trips port",
	"[dns][literal]") {
	auto ep = try_parse_ip_literal("::1", 443);
	REQUIRE(ep.has_value());
	CHECK(ep->family == AddressFamily::v6);
	auto const &sin6 = *reinterpret_cast<::sockaddr_in6 const *>(&ep->addr);
	CHECK(sin6.sin6_family == AF_INET6);
	CHECK(ntohs(sin6.sin6_port) == 443);
}
TEST_CASE(
	"dns: ip literal returns nullopt for hostnames",
	"[dns][literal]") {
	CHECK_FALSE(try_parse_ip_literal("example.com", 80).has_value());
	CHECK_FALSE(try_parse_ip_literal("", 80).has_value());
}
// ---------------------------------------------------------------------------
// encode_query — RFC 1035 §4.1.1, §4.1.2; EDNS0 OPT (RFC 6891)
// ---------------------------------------------------------------------------

TEST_CASE(
	"dns: encode_query basic A record without EDNS0",
	"[dns][codec]") {
	auto wire = encode_query(0x1234, "example.com", QType::a, nullopt);

	// Header: id(2) + flags(2) + 4×std::uint16_t counts = 12.
	REQUIRE(wire.size() >= 12);
	CHECK(wire[0] == 0x12);
	CHECK(wire[1] == 0x34);
	CHECK(wire[2] == 0x01); // RD set
	CHECK(wire[3] == 0x00);
	CHECK(wire[4] == 0x00);
	CHECK(wire[5] == 0x01); // qdcount=1
	CHECK(wire[6] == 0x00);
	CHECK(wire[7] == 0x00); // ancount=0
	CHECK(wire[8] == 0x00);
	CHECK(wire[9] == 0x00); // nscount=0
	CHECK(wire[10] == 0x00);
	CHECK(wire[11] == 0x00); // arcount=0 (no EDNS0)

	// QNAME: 7 example 3 com 0 → 13 bytes.
	REQUIRE(wire.size() == 12 + 13 + 4);
	CHECK(wire[12] == 7);
	CHECK(string_view(reinterpret_cast<char const *>(&wire[13]), 7) == "example");
	CHECK(wire[20] == 3);
	CHECK(string_view(reinterpret_cast<char const *>(&wire[21]), 3) == "com");
	CHECK(wire[24] == 0);

	// QTYPE/QCLASS
	CHECK(wire[25] == 0x00);
	CHECK(wire[26] == 0x01); // A
	CHECK(wire[27] == 0x00);
	CHECK(wire[28] == 0x01); // IN
}
TEST_CASE(
	"dns: encode_query AAAA with EDNS0 OPT",
	"[dns][codec][edns0]") {
	Edns0Options edns{.udp_size = 4096, .ext_rcode = 0, .version = 0, .flags = 0};
	auto wire = encode_query(0xBEEF, "v6.example.com", QType::aaaa, edns);

	REQUIRE(wire.size() >= 12);
	CHECK(wire[0] == 0xBE);
	CHECK(wire[1] == 0xEF);
	CHECK(wire[10] == 0x00);
	CHECK(wire[11] == 0x01); // arcount=1 (OPT)

	// Round-trip via decode_message.
	auto msg = decode_message(span<std::uint8_t const>{wire.data(), wire.size()});
	CHECK(msg.header.id == 0xBEEF);
	CHECK(msg.header.qdcount == 1);
	CHECK(msg.header.arcount == 1);
	REQUIRE(msg.questions.size() == 1);
	CHECK(msg.questions[0].name == "v6.example.com");
	CHECK(msg.questions[0].qtype == QType::aaaa);
	REQUIRE(msg.additional.size() == 1);
	CHECK(msg.additional[0].type == QType::opt);
	CHECK(static_cast<std::uint16_t>(msg.additional[0].rclass) == 4096); // udp_size in CLASS slot
	CHECK(msg.additional[0].rdata.empty());
}
TEST_CASE(
	"dns: encode_query lowercases nothing — caller responsibility",
	"[dns][codec]") {
	// Encoder copies bytes as-is. Tests doc this contract; the resolver
	// lowercases before encoding.
	auto wire = encode_query(1, "Example.COM", QType::a, nullopt);
	auto msg = decode_message(span<std::uint8_t const>{wire.data(), wire.size()});
	REQUIRE(msg.questions.size() == 1);
	// decode_name() lowercases as it reads.
	CHECK(msg.questions[0].name == "example.com");
}
TEST_CASE(
	"dns: encode_query rejects oversize label",
	"[dns][codec]") {
	string const bad = string(64, 'a') + ".com";
	CHECK_THROWS_AS(encode_query(1, bad, QType::a, nullopt), DnsError);
}
TEST_CASE(
	"dns: encode_query underscored labels accepted",
	"[dns][codec]") {
	auto wire = encode_query(7, "_dmarc.example.com", QType::txt, nullopt);
	auto msg = decode_message(span<std::uint8_t const>{wire.data(), wire.size()});
	REQUIRE(msg.questions.size() == 1);
	CHECK(msg.questions[0].name == "_dmarc.example.com");
}
// ---------------------------------------------------------------------------
// decode_message — A/AAAA RR parsing, RDLENGTH bounds, compression pointers
// ---------------------------------------------------------------------------

TEST_CASE(
	"dns: decode A response with single answer",
	"[dns][codec]") {
	// Hand-craft a NOERROR response: id=1, QR|RD|RA, qdcount=1, ancount=1.
	// Question: example.com IN A
	// Answer:   pointer-to-question-name, type A, class IN, ttl 300, rdlen 4, 93.184.216.34
	vector<std::uint8_t> wire = {
		// header
		0x00,
		0x01, // id
		0x81,
		0x80, // QR=1, RD=1, RA=1
		0x00,
		0x01, // qdcount
		0x00,
		0x01, // ancount
		0x00,
		0x00,
		0x00,
		0x00, // ns/ar
		// question
		7,
		'e',
		'x',
		'a',
		'm',
		'p',
		'l',
		'e',
		3,
		'c',
		'o',
		'm',
		0,
		0x00,
		0x01, // QTYPE A
		0x00,
		0x01, // QCLASS IN
		// answer
		0xC0,
		0x0C, // pointer to offset 12 (question name)
		0x00,
		0x01, // TYPE A
		0x00,
		0x01, // CLASS IN
		0x00,
		0x00,
		0x01,
		0x2C, // TTL 300
		0x00,
		0x04, // RDLENGTH 4
		93,
		184,
		216,
		34,
	};

	auto msg = decode_message(wire);
	CHECK(msg.header.id == 1);
	CHECK(msg.header.qr());
	CHECK(msg.header.ra());
	CHECK(msg.header.rcode() == RCode::noerror);
	REQUIRE(msg.questions.size() == 1);
	CHECK(msg.questions[0].name == "example.com");
	REQUIRE(msg.answers.size() == 1);
	CHECK(msg.answers[0].name == "example.com");
	CHECK(msg.answers[0].type == QType::a);
	CHECK(msg.answers[0].ttl == 300);
	REQUIRE(msg.answers[0].rdata.size() == 4);

	auto ep = rdata_to_endpoint(msg.answers[0], 80);
	REQUIRE(ep.has_value());
	CHECK(ep->family == AddressFamily::v4);
	auto const &sin = *reinterpret_cast<::sockaddr_in const *>(&ep->addr);
	auto const ip = ntohl(sin.sin_addr.s_addr);
	CHECK(((ip >> 24) & 0xFF) == 93);
	CHECK(((ip >> 16) & 0xFF) == 184);
	CHECK(((ip >> 8) & 0xFF) == 216);
	CHECK((ip & 0xFF) == 34);
	CHECK(ntohs(sin.sin_port) == 80);
}
TEST_CASE(
	"dns: decode AAAA response",
	"[dns][codec]") {
	vector<std::uint8_t> wire = {
		0x00, 0x02, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 2,    'v',  '6',  1,    'x',
		0, // v6.x — labels: "v6" then "x"
		0x00, 0x1C, 0x00,
		0x01, // QTYPE AAAA, QCLASS IN
		0xC0,
		0x0C, // pointer
		0x00, 0x1C, 0x00,
		0x01, // AAAA, IN
		0x00, 0x00, 0x00,
		0x3C, // TTL 60
		0x00,
		0x10, // RDLENGTH 16
		0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	};
	auto msg = decode_message(wire);
	REQUIRE(msg.answers.size() == 1);
	CHECK(msg.answers[0].type == QType::aaaa);
	auto ep = rdata_to_endpoint(msg.answers[0], 443);
	REQUIRE(ep.has_value());
	CHECK(ep->family == AddressFamily::v6);
	auto const &sin6 = *reinterpret_cast<::sockaddr_in6 const *>(&ep->addr);
	CHECK(ntohs(sin6.sin6_port) == 443);
}
TEST_CASE(
	"dns: decode rejects forward pointer",
	"[dns][codec][safety]") {
	// Pointer at offset 12 points to offset 14 (forward) — must be rejected.
	vector<std::uint8_t> wire = {
		0x00,
		0x01,
		0x81,
		0x80,
		0x00,
		0x01,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0xC0,
		0x0E, // pointer → 14 (forward)
		0x00,
		0x01,
		0x00,
		0x01,
	};
	CHECK_THROWS_AS(decode_message(wire), DnsError);
}
TEST_CASE(
	"dns: decode rejects pointer-loop chain depth",
	"[dns][codec][safety]") {
	// Two-pointer cycle: at offset 12 a pointer to 14, at offset 14 a pointer to 12.
	// The "no forward refs" rule (target < cursor) catches this; first pointer
	// targets offset 14 from cursor 12 → invalid.
	vector<std::uint8_t> wire(12 + 2 + 2);
	wire[0] = 0x00;
	wire[1] = 0x01;
	wire[2] = 0x81;
	wire[3] = 0x80;
	wire[4] = 0x00;
	wire[5] = 0x01;
	wire[12] = 0xC0;
	wire[13] = 0x0E;
	wire[14] = 0xC0;
	wire[15] = 0x0C;
	CHECK_THROWS_AS(decode_message(wire), DnsError);
}
TEST_CASE(
	"dns: decode rejects RDLENGTH overrun",
	"[dns][codec][safety]") {
	vector<std::uint8_t> wire = {
		0x00, 0x01, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 1,    'a',  0,
		0x00, 0x01, 0x00, 0x01, 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0xFF,
		0xFF, // RDLENGTH 65535 —
		// overruns wire
	};
	CHECK_THROWS_AS(decode_message(wire), DnsError);
}
TEST_CASE(
	"dns: decode rejects label > 63 bytes",
	"[dns][codec][safety]") {
	// Length byte 0x40 = 64 (b01000000) — collides with reserved flag bits;
	// codec must reject.
	vector<std::uint8_t> wire(12);
	wire[0] = 0x00;
	wire[1] = 0x01;
	wire[2] = 0x81;
	wire[3] = 0x80;
	wire[4] = 0x00;
	wire[5] = 0x01;
	wire.push_back(0x40);
	wire.insert(wire.end(), 64, 'a');
	CHECK_THROWS_AS(decode_message(wire), DnsError);
}
TEST_CASE(
	"dns: decode tolerates 63-byte label",
	"[dns][codec]") {
	vector<std::uint8_t> wire(12);
	wire[0] = 0x00;
	wire[1] = 0x01;
	wire[2] = 0x81;
	wire[3] = 0x80;
	wire[4] = 0x00;
	wire[5] = 0x01;
	wire.push_back(63);
	wire.insert(wire.end(), 63, 'a');
	wire.push_back(0); // root
	wire.push_back(0x00);
	wire.push_back(0x01); // QTYPE A
	wire.push_back(0x00);
	wire.push_back(0x01); // QCLASS IN
	auto msg = decode_message(wire);
	REQUIRE(msg.questions.size() == 1);
	CHECK(msg.questions[0].name == string(63, 'a'));
}
// ---------------------------------------------------------------------------
// rcode_to_error — RCODE → DnsErrorKind mapping
// ---------------------------------------------------------------------------

TEST_CASE(
	"dns: rcode_to_error maps RCODE values",
	"[dns][codec]") {
	CHECK_FALSE(rcode_to_error(RCode::noerror).has_value());
	CHECK(*rcode_to_error(RCode::nxdomain) == DnsErrorKind::nxdomain);
	CHECK(*rcode_to_error(RCode::servfail) == DnsErrorKind::servfail);
	CHECK(*rcode_to_error(RCode::refused) == DnsErrorKind::refused);
	CHECK(*rcode_to_error(RCode::formerr) == DnsErrorKind::formerr);
}
// ---------------------------------------------------------------------------
// Resolver — construction / IP literal short-circuit / NSS lookup
// ---------------------------------------------------------------------------

TEST_CASE(
	"dns: Resolver(WorkPool) reports nss_thread backend",
	"[dns][resolver]") {
	using namespace conflux;
	WorkPool pool{WorkPoolOptions{.threads = 1}};
	Resolver r{pool, ResolverOptions{}};
	CHECK(r.backend() == ResolverBackend::nss_thread);
}
TEST_CASE(
	"dns: resolve_blocking IP literal short-circuits",
	"[dns][resolver]") {
	using namespace conflux;
	WorkPool pool{WorkPoolOptions{.threads = 1}};
	Resolver r{pool};
	auto rr = r.resolve_blocking("1.1.1.1", 53);
	REQUIRE(rr.has_value());
	REQUIRE(rr->endpoints.size() == 1);
	CHECK(rr->endpoints[0].family == AddressFamily::v4);
}
TEST_CASE(
	"dns: resolve_blocking rejects bad hostname",
	"[dns][resolver]") {
	using namespace conflux;
	WorkPool pool{WorkPoolOptions{.threads = 1}};
	Resolver r{pool};
	auto rr = r.resolve_blocking("bad..host", 53);
	REQUIRE_FALSE(rr.has_value());
	CHECK(rr.error().kind == DnsErrorKind::invalid_hostname);
}
TEST_CASE(
	"dns: resolve_blocking resolves localhost through NSS",
	"[dns][resolver]") {
	using namespace conflux;
	WorkPool pool{WorkPoolOptions{.threads = 1}};
	Resolver r{pool};
	auto rr = r.resolve_blocking("localhost", 80);
	REQUIRE(rr.has_value());
	CHECK_FALSE(rr->endpoints.empty());
}
