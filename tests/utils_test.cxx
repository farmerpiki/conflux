// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <poll.h>
#include <unistd.h>

import std;
import conflux.utils;

using namespace std;

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
// ascii_lower / ascii_lower_inplace
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
	string s{"FOO"};
	ascii_lower_inplace(s);
	CHECK(s == "foo");
}

// ---------------------------------------------------------------------------
// trim
// ---------------------------------------------------------------------------

TEST_CASE(
	"utils: trim strips leading and trailing whitespace",
	"[utils]") {
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
	"utils: parse_ip invalid returns nullopt",
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
	"utils: parse_cidr invalid returns nullopt",
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
	"utils: parse_cidr_list empty vector",
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
