module;
#include "net/compress_backend_zlib_like.hxx"
#include <zlib-ng.h>

export module conflux.net.compress.backend.zlibng;
import std;
import conflux.types;
export namespace conflux::compress_backends {

namespace detail {

struct ZlibNgTraits {
	using Stream = zng_stream;

	static constexpr int ok() noexcept { return Z_OK; }
	static constexpr int stream_end() noexcept { return Z_STREAM_END; }
	static constexpr std::size_t max_avail() noexcept { return std::numeric_limits<std::uint32_t>::max(); }

	static int init(
		Stream &stream) {
		return zng_deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY);
	}

	static void set_input(
		Stream &stream,
		std::string_view input) {
		stream.next_in = reinterpret_cast<std::uint8_t const *>(input.data());
		stream.avail_in = backend_size<std::uint32_t>(input.size());
	}

	static std::size_t bound(
		Stream &stream,
		std::size_t input_size) {
		return zng_deflateBound(&stream, input_size);
	}

	static void set_output(
		Stream &stream,
		std::string &out) {
		stream.next_out = reinterpret_cast<std::uint8_t *>(out.data());
		stream.avail_out = backend_size<std::uint32_t>(out.size());
	}

	static int deflate(
		Stream &stream) {
		return zng_deflate(&stream, Z_FINISH);
	}

	static void end(
		Stream &stream) {
		zng_deflateEnd(&stream);
	}

	static std::size_t total_out(
		Stream const &stream) noexcept {
		return stream.total_out;
	}
};

} // namespace detail

std::string zlib_ng_gzip_compress(
	std::string_view input) {
	return detail::gzip_compress_zlib_like<detail::ZlibNgTraits>(input);
}

} // namespace conflux::compress_backends
