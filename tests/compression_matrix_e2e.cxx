// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#if CONFLUX_HAS_BROTLI
	#include <brotli/decode.h>
	#include <brotli/encode.h>
#endif
#include <stdlib.h>
#include <unistd.h>
#include <zlib.h>
#if CONFLUX_HAS_ZSTD
	#include <zstd.h>
#endif

import std;
import conflux.types;
import conflux.net.compress;
#if CONFLUX_HAS_ISAL
import conflux.net.compress.backend.isal;
#endif
#if CONFLUX_HAS_ZLIB
import conflux.net.compress.backend.zlib;
#endif
#if CONFLUX_HAS_ZLIB_NG
import conflux.net.compress.backend.zlibng;
#endif
import conflux.net.config;
import conflux.net.http.realtime;
import conflux.net.http.static_files;
import conflux.net.http_server;
import conflux.net.router;
import conflux.net.compress;
import conflux.tests.support;

using namespace conflux::tests;
using conflux::http::Config;
namespace {

class TempDir {
	std::string path_;

public:
	explicit TempDir(
		std::string pattern) {
		path_ = std::move(pattern);
		auto *raw = path_.data();
		if (::mkdtemp(raw) == nullptr) {
			throw std::runtime_error{"mkdtemp failed"};
		}
	}
	~TempDir() {
		std::error_code ec;
		std::filesystem::remove_all(path_, ec);
	}
	TempDir(TempDir const &) = delete;
	TempDir &operator =(TempDir const &) = delete;
	[[nodiscard]] std::string const &path() const noexcept { return path_; }
	void write(
		std::string_view name,
		std::string_view body) const {
		auto const full = std::filesystem::path{path_} / std::string{name};
		std::ofstream out{full, std::ios::binary};
		if (!out) {
			throw std::runtime_error{"open temp file failed"};
		}
		out.write(body.data(), static_cast<std::streamsize>(body.size()));
	}
};

std::string body_of(
	std::string_view response) {
	auto const pos = response.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{response.substr(pos + 4)};
}

std::string header_value(
	std::string_view response,
	std::string_view name) {
	auto const pos = response.find(name);
	if (pos == std::string_view::npos) {
		return {};
	}
	auto const value_begin = pos + name.size();
	auto const value_end = response.find("\r\n", value_begin);
	if (value_end == std::string_view::npos) {
		return {};
	}
	return std::string{response.substr(value_begin, value_end - value_begin)};
}

std::string gzip_decompress(
	std::string_view compressed) {
	z_stream zs{};
	if (inflateInit2(&zs, 15 | 16) != Z_OK) {
		return {};
	}
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-const-cast)
	zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
	zs.avail_in = static_cast<uInt>(compressed.size());

	std::string out;
	std::array<char, 4096> chunk{};
	int rc = Z_OK;
	while (rc == Z_OK) {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
		zs.next_out = reinterpret_cast<Bytef *>(chunk.data());
		zs.avail_out = static_cast<uInt>(chunk.size());
		rc = inflate(&zs, Z_NO_FLUSH);
		out.append(chunk.data(), chunk.size() - zs.avail_out);
	}
	inflateEnd(&zs);
	return rc == Z_STREAM_END ? out : std::string{};
}

std::string gzip_compress(
	std::string_view body) {
	z_stream zs{};
	if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	std::string out;
	out.resize(deflateBound(&zs, body.size()));
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-const-cast)
	zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(body.data()));
	zs.avail_in = static_cast<uInt>(body.size());
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
	zs.next_out = reinterpret_cast<Bytef *>(out.data());
	zs.avail_out = static_cast<uInt>(out.size());
	auto const rc = deflate(&zs, Z_FINISH);
	out.resize(out.size() - zs.avail_out);
	deflateEnd(&zs);
	return rc == Z_STREAM_END ? out : std::string{};
}

#if CONFLUX_HAS_ZSTD
std::string zstd_decompress(
	std::string_view compressed) {
	unsigned long long const size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
	if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
		return {};
	}
	std::string out(static_cast<std::size_t>(size), '\0');
	auto const rc = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
	if (ZSTD_isError(rc) != 0U || rc != out.size()) {
		return {};
	}
	return out;
}
#endif

#if CONFLUX_HAS_BROTLI
std::string brotli_compress(
	std::string_view body) {
	auto out_size = BrotliEncoderMaxCompressedSize(body.size());
	std::string out(out_size, '\0');
	auto const ok = BrotliEncoderCompress(
		BROTLI_DEFAULT_QUALITY,
		BROTLI_DEFAULT_WINDOW,
		BROTLI_DEFAULT_MODE,
		body.size(),
		reinterpret_cast<std::uint8_t const *>(body.data()),
		&out_size,
		reinterpret_cast<std::uint8_t *>(out.data()));
	if (ok == BROTLI_FALSE) {
		return {};
	}
	out.resize(out_size);
	return out;
}

std::string brotli_decompress(
	std::string_view body) {
	std::string out(4096, '\0');
	for (;;) {
		auto out_size = out.size();
		auto const rc = BrotliDecoderDecompress(
			body.size(),
			reinterpret_cast<std::uint8_t const *>(body.data()),
			&out_size,
			reinterpret_cast<std::uint8_t *>(out.data()));
		if (rc == BROTLI_DECODER_RESULT_SUCCESS) {
			out.resize(out_size);
			return out;
		}
		if (rc != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
			return {};
		}
		out.resize(out.size() * 2);
	}
}
#endif

std::uint16_t compression_port() {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Config cfg = mw_config();
		Router router;
		router.use(compress_middleware({.min_body_size = 64}));
		router.get("/large", [](Request const &) { return Response::text(std::string(4096, 'A')); });
		router.get("/small", [](Request const &) { return Response::text(std::string(32, 's')); });
		router.get("/binary", [](Request const &) {
			Response response;
			response.status = 200;
			response.status_text = "OK";
			response.content_type = "application/octet-stream";
			response.set_text_body(std::string(4096, '\0'));
			return response;
		});
		router.post("/echo", [](Request const &req) { return Response::text(req.body); });
		router.sse("/events", [](Request const &, std::shared_ptr<conflux::http::SseChannel> const &channel) {
			(void)channel->send("data: hello\n\n");
			channel->close();
		});
		port = test_servers().start(cfg, std::move(router));
	});
	return port;
}

std::string get_sse(
	std::uint16_t port,
	std::string_view extra_headers) {
	LocalTcpClient const client{port};
	auto request =
		std::format("GET /events HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n{}\r\n", extra_headers);
	(void)client.send(request);
	client.set_recv_timeout(std::chrono::seconds{5});
	return client.read_until_close();
}

struct CompressionStateGuard {
	CompressionCalibration calibration{compression_calibration()};
	GzipBackend backend{current_gzip_backend()};
	~CompressionStateGuard() {
		set_compression_calibration(calibration);
		(void)set_gzip_backend(backend);
	}
};

} // namespace

TEST_CASE(
	"compression matrix: 32-bit gzip backends reject oversize input",
	"[compression]") {
	[[maybe_unused]] std::string_view const oversize{
		nullptr,
		static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1U};
#if CONFLUX_HAS_ZLIB
	CHECK(conflux::compress_backends::zlib_gzip_compress(oversize).empty());
#endif
#if CONFLUX_HAS_ZLIB_NG
	CHECK(conflux::compress_backends::zlib_ng_gzip_compress(oversize).empty());
#endif
#if CONFLUX_HAS_ISAL
	CHECK(conflux::compress_backends::isal_gzip_compress(oversize).empty());
#endif
}

TEST_CASE(
	"compression matrix: every configured gzip backend can serve dynamic gzip",
	"[compression][http][e2e]") {
	CompressionStateGuard const guard;
	set_compression_calibration(CompressionCalibration::disabled);
	auto const backends = available_gzip_backends();

	if (backends.empty()) {
		auto resp = http_get_on(compression_port(), "/large", "Accept-Encoding: gzip\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		CHECK(header_value(resp, "Content-Encoding: ").empty());
		CHECK(body_of(resp) == std::string(4096, 'A'));
		return;
	}

	for (auto const backend: backends) {
		CAPTURE(gzip_backend_name(backend));
		REQUIRE(set_gzip_backend(backend));
		auto resp = http_get_on(compression_port(), "/large", "Accept-Encoding: gzip\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		CHECK(header_value(resp, "Content-Encoding: ") == "gzip");
		CHECK(header_value(resp, "Vary: ") == "Accept-Encoding");
		auto const body = body_of(resp);
		REQUIRE(!body.empty());
		CHECK(gzip_decompress(body) == std::string(4096, 'A'));
	}
}

TEST_CASE(
	"compression matrix: negotiation, thresholds, MIME, and malformed encoded input",
	"[compression][http][e2e]") {
	SECTION("zstd wins when available and preferred; gzip remains fallback") {
		auto resp = http_get_on(compression_port(), "/large", "Accept-Encoding: zstd;q=1, gzip;q=0.1\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
#if CONFLUX_HAS_ZSTD
		CHECK(header_value(resp, "Content-Encoding: ") == "zstd");
		CHECK(zstd_decompress(body_of(resp)) == std::string(4096, 'A'));
#elif CONFLUX_HAS_COMPRESS
		CHECK(header_value(resp, "Content-Encoding: ") == "gzip");
		CHECK(gzip_decompress(body_of(resp)) == std::string(4096, 'A'));
#else
		CHECK(header_value(resp, "Content-Encoding: ").empty());
		CHECK(body_of(resp) == std::string(4096, 'A'));
#endif
	}

	SECTION("identity-only request is not compressed") {
		auto resp = http_get_on(compression_port(), "/large", "Accept-Encoding: identity\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		CHECK(header_value(resp, "Content-Encoding: ").empty());
		CHECK(body_of(resp) == std::string(4096, 'A'));
	}

	SECTION("q=0 excludes every supported dynamic coding") {
		auto resp = http_get_on(compression_port(), "/large", "Accept-Encoding: gzip;q=0, zstd;q=0, *;q=0\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		CHECK(header_value(resp, "Content-Encoding: ").empty());
		CHECK(body_of(resp) == std::string(4096, 'A'));
	}

	SECTION("small body remains below compression threshold") {
		auto resp = http_get_on(compression_port(), "/small", "Accept-Encoding: gzip\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		CHECK(header_value(resp, "Content-Encoding: ").empty());
		CHECK(body_of(resp) == std::string(32, 's'));
	}

	SECTION("non-compressible MIME type is left alone") {
		auto resp = http_get_on(compression_port(), "/binary", "Accept-Encoding: gzip\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		CHECK(header_value(resp, "Content-Encoding: ").empty());
		CHECK(body_of(resp).size() == 4096);
	}

	SECTION("malformed gzip request body is treated as opaque input") {
		auto resp = http_post_on(
			compression_port(),
			"/echo",
			"application/octet-stream",
			"not-a-gzip-stream",
			"Content-Encoding: gzip\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		CHECK(body_of(resp) == "not-a-gzip-stream");
	}
}

TEST_CASE(
	"compression matrix: streaming responses are not dynamically compressed",
	"[compression][http][e2e]") {
	auto resp = get_sse(compression_port(), "Accept-Encoding: gzip\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	CHECK(resp.find("Content-Type: text/event-stream") != std::string::npos);
	CHECK(header_value(resp, "Content-Encoding: ").empty());
	CHECK(resp.find("data: hello\n\n") != std::string::npos);
}

TEST_CASE(
	"compression matrix: static precompressed sidecars negotiate br, gzip, and identity",
	"[compression][http][e2e][static]") {
	TempDir dir{"/tmp/conflux_compression_matrix_XXXXXX"};
	std::string const identity = "identity-body";
	auto gzip_sidecar = gzip_compress(identity);
	REQUIRE(!gzip_sidecar.empty());
	dir.write("asset.txt", identity);
	dir.write("asset.txt.gz", gzip_sidecar);
#if CONFLUX_HAS_BROTLI
	auto brotli_sidecar = brotli_compress(identity);
	REQUIRE(!brotli_sidecar.empty());
	dir.write("asset.txt.br", brotli_sidecar);
#endif

	Config cfg = mw_config();
	Router router;
	router.serve_static("/static", dir.path());
	ScopedTestServer const server{cfg, std::move(router)};

#if CONFLUX_HAS_BROTLI
	SECTION("br sidecar wins over gzip when both are accepted") {
		auto resp = http_get_on(server.port(), "/static/asset.txt", "Accept-Encoding: br, gzip\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		CHECK(header_value(resp, "Content-Encoding: ") == "br");
		CHECK(brotli_decompress(body_of(resp)) == identity);
	}
#endif

	SECTION("gzip sidecar is used when br is excluded") {
		auto resp = http_get_on(server.port(), "/static/asset.txt", "Accept-Encoding: br;q=0, gzip\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		CHECK(header_value(resp, "Content-Encoding: ") == "gzip");
		CHECK(gzip_decompress(body_of(resp)) == identity);
	}

	SECTION("identity file is used when compressed sidecars are excluded") {
		auto resp = http_get_on(server.port(), "/static/asset.txt", "Accept-Encoding: br;q=0, gzip;q=0\r\n");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		CHECK(header_value(resp, "Content-Encoding: ").empty());
		CHECK(body_of(resp) == identity);
	}
}

TEST_CASE(
	"compression matrix: static PUT invalidates cached precompressed sidecars",
	"[compression][http][e2e][static]") {
	TempDir dir{"/tmp/conflux_static_sidecar_cache_XXXXXX"};
	std::string const old_identity = "old-body";
	std::string const new_identity = "new-body";
	auto old_gzip = gzip_compress(old_identity);
	auto new_gzip = gzip_compress(new_identity);
	REQUIRE(!old_gzip.empty());
	REQUIRE(!new_gzip.empty());
	dir.write("asset.txt", old_identity);
	dir.write("asset.txt.gz", old_gzip);

	Config cfg = mw_config();
	Router router;
	StaticOptions sopts{};
	sopts.allow_put = true;
	sopts.file_cache.enabled = true;
	sopts.file_cache.small_file_max_bytes = 1024 * 1024;
	sopts.file_cache.max_total_bytes = 1024 * 1024;
	router.serve_static("/static", dir.path(), sopts);
	ScopedTestServer const server{cfg, std::move(router)};

	auto first = http_get_on(server.port(), "/static/asset.txt", "Accept-Encoding: gzip\r\n");
	REQUIRE(first.starts_with("HTTP/1.1 200 OK"));
	CHECK(header_value(first, "Content-Encoding: ") == "gzip");
	CHECK(gzip_decompress(body_of(first)) == old_identity);

	dir.write("asset.txt.gz", new_gzip);
	auto put = http_request_on(server.port(), "PUT", "/static/asset.txt", "text/plain", new_identity);
	REQUIRE(put.starts_with("HTTP/1.1 204 No Content"));

	auto second = http_get_on(server.port(), "/static/asset.txt", "Accept-Encoding: gzip\r\n");
	REQUIRE(second.starts_with("HTTP/1.1 200 OK"));
	CHECK(header_value(second, "Content-Encoding: ") == "gzip");
	CHECK(gzip_decompress(body_of(second)) == new_identity);
}
