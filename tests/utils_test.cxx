// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <poll.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.utils;

using namespace conflux::utils;

// ---------------------------------------------------------------------------
// url_decode
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: url_decode passthrough",
	"[utils]") {
	CHECK(url_decode("hello") == "hello");
}
TEST_CASE(
	"utils: url_decode percent-encoded",
	"[utils]") {
	CHECK(url_decode("hello%20world") == "hello world");
	CHECK(url_decode("%2F") == "/");
	CHECK(url_decode("%41") == "A");
}
TEST_CASE(
	"utils: url_decode plus to space",
	"[utils]") {
	CHECK(url_decode("hello+world") == "hello world");
}
TEST_CASE(
	"utils: url_decode invalid percent left as-is",
	"[utils]") {
	CHECK(url_decode("100%") == "100%");
	CHECK(url_decode("%zz") == "%zz");
}
TEST_CASE(
	"utils: url_decode empty",
	"[utils]") {
	CHECK(url_decode("").empty());
}
// ---------------------------------------------------------------------------
// url_decode_path
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: url_decode_path decodes percent-encoded chars",
	"[utils]") {
	CHECK(url_decode_path("hello%20world") == "hello world");
	CHECK(url_decode_path("%2F") == "/");
}
TEST_CASE(
	"utils: url_decode_path leaves plus as-is",
	"[utils]") {
	CHECK(url_decode_path("hello+world") == "hello+world");
}
TEST_CASE(
	"utils: url_decode_path passthrough when no encoding",
	"[utils]") {
	CHECK(url_decode_path("hello") == "hello");
}
TEST_CASE(
	"utils: url_decode_path invalid percent left as-is",
	"[utils]") {
	CHECK(url_decode_path("%zz") == "%zz");
	CHECK(url_decode_path("100%") == "100%");
}
TEST_CASE(
	"utils: url_decode_path empty",
	"[utils]") {
	CHECK(url_decode_path("").empty());
}
// ---------------------------------------------------------------------------
// ascii_lower / ascii_lower_inplace / ascii_upper / ascii_upper_inplace
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: ascii_lower converts uppercase",
	"[utils]") {
	CHECK(ascii_lower("Hello World") == "hello world");
}
TEST_CASE(
	"utils: ascii_lower leaves lowercase unchanged",
	"[utils]") {
	CHECK(ascii_lower("already") == "already");
}
TEST_CASE(
	"utils: ascii_lower_inplace modifies in-place",
	"[utils]") {
	std::string s{"FOO"};
	ascii_lower_inplace(s);
	CHECK(s == "foo");
}
TEST_CASE(
	"utils: ascii_upper converts lowercase",
	"[utils]") {
	CHECK(ascii_upper("Hello World") == "HELLO WORLD");
}
TEST_CASE(
	"utils: ascii_upper_inplace modifies in-place",
	"[utils]") {
	std::string s{"foo"};
	ascii_upper_inplace(s);
	CHECK(s == "FOO");
}
// ---------------------------------------------------------------------------
// trim
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: trim strips leading and trailing whitespace",
	"[utils]") {
	static_assert(trim("  hello  ") == "hello");
	CHECK(trim("  hello  ") == "hello");
	CHECK(trim("\t\nhello\r\n") == "hello");
}
TEST_CASE(
	"utils: trim empty stays empty",
	"[utils]") {
	CHECK(trim("").empty());
}
TEST_CASE(
	"utils: trim only whitespace returns empty view",
	"[utils]") {
	CHECK(trim("   ").empty());
}

TEST_CASE(
	"utils: line helpers split text without allocation",
	"[utils]") {
	CHECK(strip_cr("abc\r") == "abc");
	CHECK(strip_cr("abc") == "abc");

	auto kv = split_once("alpha=beta", '=');
	REQUIRE(kv.has_value());
	CHECK(kv->first == "alpha");
	CHECK(kv->second == "beta");
	CHECK_FALSE(split_once("alpha", '=').has_value());

	std::vector<std::string> lines;
	std::vector<std::size_t> line_nos;
	for (auto const line: LineRange{"one\r\ntwo\nthree"}) {
		lines.push_back(std::string{line.text});
		line_nos.push_back(line.line_no);
	}
	REQUIRE(lines.size() == 3);
	CHECK(lines[0] == "one");
	CHECK(lines[1] == "two");
	CHECK(lines[2] == "three");
	CHECK(line_nos[0] == 1);
	CHECK(line_nos[1] == 2);
	CHECK(line_nos[2] == 3);
}

// ---------------------------------------------------------------------------
// parse_ip / ip_to_string
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: parse_ipv4 returns host-order value",
	"[utils]") {
	auto const addr = parse_ipv4("127.0.0.1");
	REQUIRE(addr.has_value());
	CHECK(*addr == 0x7F000001U);
}
TEST_CASE(
	"utils: parse_ipv4 distinguishes 0.0.0.0 from invalid input",
	"[utils]") {
	auto const zero = parse_ipv4("0.0.0.0");
	REQUIRE(zero.has_value());
	CHECK(*zero == 0U);
	CHECK_FALSE(parse_ipv4("not.an.ip").has_value());
}
TEST_CASE(
	"utils: parse_ip IPv4 round-trip",
	"[utils]") {
	auto const addr = parse_ip("127.0.0.1");
	REQUIRE(addr.has_value());
	CHECK(ip_to_string(*addr) == "127.0.0.1");
}
TEST_CASE(
	"utils: parse_ip IPv6 round-trip",
	"[utils]") {
	auto const addr = parse_ip("::1");
	REQUIRE(addr.has_value());
	CHECK(ip_to_string(*addr) == "::1");
}
TEST_CASE(
	"utils: parse_ip invalid returns std::nullopt",
	"[utils]") {
	CHECK_FALSE(parse_ip("not.an.ip").has_value());
	CHECK_FALSE(parse_ip("").has_value());
}
// ---------------------------------------------------------------------------
// parse_cidr / cidr_match
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: cidr_match IPv4 loopback in 127.0.0.0/8",
	"[utils]") {
	auto const net = parse_cidr("127.0.0.0/8");
	REQUIRE(net.has_value());
	auto const addr = parse_ip("127.0.0.1");
	REQUIRE(addr.has_value());
	CHECK(cidr_match(*net, *addr));
}
TEST_CASE(
	"utils: cidr_match rejects out-of-range address",
	"[utils]") {
	auto const net = parse_cidr("10.0.0.0/8");
	REQUIRE(net.has_value());
	auto const addr = parse_ip("192.168.1.1");
	REQUIRE(addr.has_value());
	CHECK_FALSE(cidr_match(*net, *addr));
}
TEST_CASE(
	"utils: cidr_match IPv6 loopback in ::1/128",
	"[utils]") {
	auto const net = parse_cidr("::1/128");
	REQUIRE(net.has_value());
	auto const addr = parse_ip("::1");
	REQUIRE(addr.has_value());
	CHECK(cidr_match(*net, *addr));
}
TEST_CASE(
	"utils: parse_cidr invalid returns std::nullopt",
	"[utils]") {
	CHECK_FALSE(parse_cidr("not/valid").has_value());
	CHECK_FALSE(parse_cidr("").has_value());
}
// ---------------------------------------------------------------------------
// parse_cidr_list
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: parse_cidr_list two entries",
	"[utils]") {
	auto const list = parse_cidr_list({"10.0.0.0/8", "192.168.0.0/16"});
	CHECK(list.size() == 2);
}
TEST_CASE(
	"utils: parse_cidr_list empty V",
	"[utils]") {
	auto const list = parse_cidr_list({});
	CHECK(list.empty());
}
TEST_CASE(
	"utils: parse_cidr_list skips invalid entries",
	"[utils]") {
	auto const list = parse_cidr_list({"10.0.0.0/8", "not/valid"});
	CHECK(list.size() == 1);
}
// ---------------------------------------------------------------------------
// hex_char_to_int
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: hex_char_to_int digits and letters",
	"[utils]") {
	CHECK(hex_char_to_int('0') == 0);
	CHECK(hex_char_to_int('9') == 9);
	CHECK(hex_char_to_int('a') == 10);
	CHECK(hex_char_to_int('f') == 15);
	CHECK(hex_char_to_int('A') == 10);
	CHECK(hex_char_to_int('F') == 15);
}
TEST_CASE(
	"utils: hex_char_to_int invalid returns -1",
	"[utils]") {
	CHECK(hex_char_to_int('g') == -1);
	CHECK(hex_char_to_int('z') == -1);
	CHECK(hex_char_to_int(' ') == -1);
}

static_assert(conflux::support::fnv1a64("") == 0xcbf29ce484222325ULL);
static_assert(conflux::support::fnv1a64("hello") == 0xa430d84680aabd0bULL);
static_assert(conflux::support::fnv1a64_ascii_fold("Content-Type") == 0xf4dd5cf6a7a0235ULL);

// ---------------------------------------------------------------------------
// random_bytes
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: random_bytes fills buffer",
	"[utils]") {
	std::array<unsigned char, 16> buf{};
	random_bytes(buf);
	bool all_zero = true;
	for (auto b: buf) {
		if (b != 0) {
			all_zero = false;
			break;
		}
	}
	CHECK(!all_zero);
}
TEST_CASE(
	"utils: random_bytes two calls differ",
	"[utils]") {
	std::array<unsigned char, 16> a{};
	std::array<unsigned char, 16> b{};
	random_bytes(a);
	random_bytes(b);
	CHECK(a != b);
}
TEST_CASE(
	"utils: random_bytes partial fill",
	"[utils]") {
	std::array<unsigned char, 3> buf{};
	random_bytes(buf);
	// Just verifying it doesn't crash; partial size exercises the tail branch.
}
// ---------------------------------------------------------------------------
// wait_fd
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// url_percent_encode
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: url_percent_encode unreserved passthrough",
	"[utils]") {
	CHECK(url_percent_encode("abcXYZ019") == "abcXYZ019");
	CHECK(url_percent_encode("-._~") == "-._~");
}
TEST_CASE(
	"utils: url_percent_encode reserved chars",
	"[utils]") {
	CHECK(url_percent_encode(" ") == "%20");
	CHECK(url_percent_encode("/") == "%2F");
	CHECK(url_percent_encode("a b") == "a%20b");
	CHECK(url_percent_encode("100%") == "100%25");
	CHECK(url_percent_encode("hello world!") == "hello%20world%21");
}
TEST_CASE(
	"utils: url_percent_encode append and size forms",
	"[utils]") {
	static_assert(url_percent_encoded_size("a b") == 5);
	std::string out{"prefix:"};
	append_url_percent_encoded(out, "a b");
	CHECK(out == "prefix:a%20b");
	CHECK(url_percent_encode("").empty());
}
// ---------------------------------------------------------------------------
// hex_encode / hex_decode
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: hex_encode empty",
	"[utils]") {
	CHECK(hex_encode({}).empty());
}
TEST_CASE(
	"utils: hex_encode known values",
	"[utils]") {
	std::array<unsigned char, 3> data{0xDE, 0xAD, 0x01};
	CHECK(hex_encode(data) == "dead01");
}
TEST_CASE(
	"utils: hex_decode empty",
	"[utils]") {
	auto r = hex_decode("");
	REQUIRE(r.has_value());
	CHECK(r->empty());
}
TEST_CASE(
	"utils: hex_decode known values",
	"[utils]") {
	auto r = hex_decode("dead01");
	REQUIRE(r.has_value());
	REQUIRE(r->size() == 3);
	CHECK((*r)[0] == 0xDE);
	CHECK((*r)[1] == 0xAD);
	CHECK((*r)[2] == 0x01);
}
TEST_CASE(
	"utils: hex_decode case insensitive",
	"[utils]") {
	auto r = hex_decode("DeAd");
	REQUIRE(r.has_value());
	CHECK((*r)[0] == 0xDE);
	CHECK((*r)[1] == 0xAD);
}
TEST_CASE(
	"utils: hex_decode odd length fails",
	"[utils]") {
	auto r = hex_decode("abc");
	CHECK(!r.has_value());
}
TEST_CASE(
	"utils: hex_decode invalid chars fails",
	"[utils]") {
	auto r = hex_decode("zz");
	CHECK(!r.has_value());
}
TEST_CASE(
	"utils: hex_encode/decode round-trip",
	"[utils]") {
	std::array<unsigned char, 5> data{0x00, 0xFF, 0x7F, 0x80, 0x42};
	auto enc = hex_encode(data);
	auto dec = hex_decode(enc);
	REQUIRE(dec.has_value());
	CHECK(std::ranges::equal(*dec, data));
}
// ---------------------------------------------------------------------------
// wait_fd
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: wait_fd returns true when data available",
	"[utils]") {
	int fds[2]{};
	REQUIRE(::pipe(fds) == 0);
	char const ch = 'x';
	REQUIRE(::write(fds[1], &ch, 1) == 1);
	bool const ready = wait_fd(fds[0], POLLIN, 1);
	::close(fds[0]);
	::close(fds[1]);
	CHECK(ready);
}
TEST_CASE(
	"utils: wait_fd returns false on timeout",
	"[utils]") {
	int fds[2]{};
	REQUIRE(::pipe(fds) == 0);
	bool const ready = wait_fd(fds[0], POLLIN, 0); // 0-second timeout → immediate
	::close(fds[0]);
	::close(fds[1]);
	CHECK(!ready);
}
