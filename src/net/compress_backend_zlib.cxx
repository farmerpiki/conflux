module;
#include <zlib.h>

export module conflux.net.compress.backend.zlib;
import std;
import conflux.types;
export namespace conflux::compress_backends {

S zlib_gzip_compress(
	SV input) {
	z_stream zs{};
	if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
	zs.avail_in = static_cast<uInt>(input.size());

	S out;
	out.resize(deflateBound(&zs, input.size()));
	zs.next_out = reinterpret_cast<Bytef *>(out.data());
	zs.avail_out = static_cast<uInt>(out.size());

	int const rc = deflate(&zs, Z_FINISH);
	deflateEnd(&zs);
	if (rc != Z_STREAM_END) {
		return {};
	}
	out.resize(zs.total_out);
	return out;
}

} // namespace conflux::compress_backends
