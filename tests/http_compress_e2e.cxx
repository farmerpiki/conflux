#include <catch2/catch_test_macros.hpp>

#include <zlib.h>

import std;
import conflux.net.compress;
import conflux.net.config;
import conflux.net.cors;
import conflux.net.router;
import conflux.tests.support;

using conflux::http::Config;
using namespace conflux::tests;

namespace {

std::uint16_t g_codec_port = 0;
void ensure_codec_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		router.use(conflux::http::compress_middleware({.min_body_size = 0}));
		router.get("/data", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text(std::string(512, 'A'));
		});
		router.get("/vary", [](conflux::http::OwnedRequest const &) {
			auto r = conflux::http::Response::text(std::string(512, 'A'));
			r.headers["Vary"] = "X-Test";
			return r;
		});
		g_codec_port = test_servers().start(cfg, std::move(router));
	});
}

std::uint16_t g_default_compress_port = 0;
void ensure_default_compress_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		router.use(conflux::http::compress_middleware());
		router.get("/big", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::html(std::string(512, 'A'));
		});
		router.get("/small", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::html("hi"); });
		router.get("/bin", [](conflux::http::OwnedRequest const &) {
			conflux::http::Response r;
			r.status = 200;
			r.status_text = "OK";
			r.content_type = "application/octet-stream";
			r.set_text_body(std::string(512, '\x00'));
			return r;
		});
		g_default_compress_port = test_servers().start(cfg, std::move(router));
	});
}

std::uint16_t g_cors_compress_port = 0;
void ensure_cors_compress_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		router.use(conflux::http::cors_middleware({.allowed_origins = {"https://test.example"}}));
		router.use(conflux::http::compress_middleware({.min_body_size = 0}));
		router.get("/big", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text(std::string(512, 'A'));
		});
		g_cors_compress_port = test_servers().start(cfg, std::move(router));
	});
}

std::string extract_header(
	std::string_view resp,
	std::string_view name) {
	auto needle = std::string{name} + ": ";
	auto pos = resp.find(needle);
	if (pos == std::string_view::npos) {
		return {};
	}
	pos += needle.size();
	auto end = resp.find("\r\n", pos);
	if (end == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos, end - pos)};
}

std::string gzip_decompress(
	std::string_view compressed) {
	z_stream zs{};
	if (inflateInit2(&zs, 15 | 16) != Z_OK) {
		return {};
	}
	zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
	zs.avail_in = static_cast<uInt>(compressed.size());

	std::string out;
	std::array<char, 4096> chunk{};
	int rc = Z_OK;
	while (rc == Z_OK) {
		zs.next_out = reinterpret_cast<Bytef *>(chunk.data());
		zs.avail_out = static_cast<uInt>(chunk.size());
		rc = inflate(&zs, Z_NO_FLUSH);
		out.append(chunk.data(), chunk.size() - zs.avail_out);
	}
	inflateEnd(&zs);
	return rc == Z_STREAM_END ? out : std::string{};
}

} // namespace

TEST_CASE(
	"compress: large body with Accept-Encoding gzip is compressed") {
	ensure_default_compress_server();
	auto resp = http_get_on(g_default_compress_port, "/big", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") != std::string::npos);
	REQUIRE(resp.find("Vary: Accept-Encoding") != std::string::npos);
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	auto body = resp.substr(hdr_end + 4);
	auto decompressed = gzip_decompress(body);
	REQUIRE(decompressed == std::string(512, 'A'));
}

TEST_CASE(
	"compress: large body without Accept-Encoding is not compressed") {
	ensure_default_compress_server();
	auto resp = http_get_on(g_default_compress_port, "/big");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") == std::string::npos);
}

TEST_CASE(
	"compress: body smaller than min_body_size is not compressed") {
	ensure_default_compress_server();
	auto resp = http_get_on(g_default_compress_port, "/small", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") == std::string::npos);
}

TEST_CASE(
	"compress: non-compressible MIME type is not compressed") {
	ensure_default_compress_server();
	auto resp = http_get_on(g_default_compress_port, "/bin", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("Content-Encoding: gzip") == std::string::npos);
}

TEST_CASE(
	"compress negotiation header: brotli is ignored for dynamic responses") {
	ensure_codec_server();
	auto resp = http_get_on(g_codec_port, "/data", "Accept-Encoding: br, gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}

TEST_CASE(
	"compress negotiation header: zstd accepted when client prefers it") {
	ensure_codec_server();
	auto resp = http_get_on(g_codec_port, "/data", "Accept-Encoding: zstd;q=1, gzip;q=0.5\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_ZSTD
	REQUIRE(resp.find("Content-Encoding: zstd\r\n") != std::string::npos);
#elif CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}

TEST_CASE(
	"compress negotiation header: gzip returned when only gzip offered") {
	ensure_codec_server();
	auto resp = http_get_on(g_codec_port, "/data", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}

TEST_CASE(
	"compress negotiation header: Accept-Encoding token matching is case-insensitive") {
	ensure_codec_server();
	auto resp = http_get_on(g_codec_port, "/data", "Accept-Encoding: GZip;Q=1\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}

TEST_CASE(
	"compress: appends Accept-Encoding to existing Vary") {
	ensure_codec_server();
	auto resp = http_get_on(g_codec_port, "/vary", "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Vary: X-Test, Accept-Encoding\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Vary: X-Test\r\n") != std::string::npos);
#endif
}

TEST_CASE(
	"compress negotiation header: q=0 exclusion: gzip;q=0 gives zstd") {
	ensure_codec_server();
	auto resp = http_get_on(g_codec_port, "/data", "Accept-Encoding: gzip;q=0, zstd\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_ZSTD
	REQUIRE(resp.find("Content-Encoding: zstd\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}

TEST_CASE(
	"compress: backend names expose stable labels") {
	CHECK(conflux::http::gzip_backend_name(conflux::http::GzipBackend::auto_select) == "auto");
	CHECK(conflux::http::gzip_backend_name(conflux::http::GzipBackend::zlib) == "zlib");
	CHECK(conflux::http::gzip_backend_name(conflux::http::GzipBackend::libdeflate) == "libdeflate");
	CHECK(conflux::http::gzip_backend_name(conflux::http::GzipBackend::zlib_ng) == "zlib-ng");
	CHECK(conflux::http::gzip_backend_name(conflux::http::GzipBackend::isa_l) == "isa-l");
}

TEST_CASE(
	"compress negotiation header: wildcard * selects preferred dynamic codec") {
	ensure_codec_server();
	auto resp = http_get_on(g_codec_port, "/data", "Accept-Encoding: *\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_COMPRESS && CONFLUX_HAS_ZSTD
	if (conflux::http::current_dynamic_encoding_preference() == conflux::http::DynamicEncodingPreference::gzip_first) {
		REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
	} else {
		REQUIRE(resp.find("Content-Encoding: zstd\r\n") != std::string::npos);
	}
#elif CONFLUX_HAS_ZSTD
	REQUIRE(resp.find("Content-Encoding: zstd\r\n") != std::string::npos);
#elif CONFLUX_HAS_COMPRESS
	REQUIRE(resp.find("Content-Encoding: gzip\r\n") != std::string::npos);
#else
	REQUIRE(resp.find("Content-Encoding:") == std::string::npos);
#endif
}

TEST_CASE(
	"compress+cors: Vary header accumulates both Origin and Accept-Encoding") {
	ensure_cors_compress_server();
	auto resp = http_get_on(
		g_cors_compress_port,
		"/big",
		"Origin: https://test.example\r\n"
		"Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto vary = extract_header(resp, "Vary");
	REQUIRE(vary.find("Origin") != std::string::npos);
	REQUIRE(vary.find("Accept-Encoding") != std::string::npos);
}
