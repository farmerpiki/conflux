module;
#include "net/compress_backend_zlib_like.hxx"
#include <zlib.h>

export module conflux.net.compress.backend.zlib;
import std;
import conflux.types;
export namespace conflux::compress_backends {

namespace detail {

struct ZlibTraits {
	using Stream = z_stream;

	static constexpr int ok() noexcept { return Z_OK; }
	static constexpr int stream_end() noexcept { return Z_STREAM_END; }
	static constexpr std::size_t max_avail() noexcept { return std::numeric_limits<uInt>::max(); }

	static int init(
		Stream &stream) {
		return deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY);
	}

	static void set_input(
		Stream &stream,
		std::string_view input) {
		stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
		stream.avail_in = backend_size<uInt>(input.size());
	}

	static std::size_t bound(
		Stream &stream,
		std::size_t input_size) {
		return deflateBound(&stream, input_size);
	}

	static void set_output(
		Stream &stream,
		std::string &out) {
		stream.next_out = reinterpret_cast<Bytef *>(out.data());
		stream.avail_out = backend_size<uInt>(out.size());
	}

	static int deflate(
		Stream &stream) {
		return ::deflate(&stream, Z_FINISH);
	}

	static void end(
		Stream &stream) {
		deflateEnd(&stream);
	}

	static std::size_t total_out(
		Stream const &stream) noexcept {
		return stream.total_out;
	}
};

} // namespace detail

std::string zlib_gzip_compress(
	std::string_view input) {
	return detail::gzip_compress_zlib_like<detail::ZlibTraits>(input);
}

} // namespace conflux::compress_backends
