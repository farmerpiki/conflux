module;
#include <zlib-ng.h>

export module conflux.net.compress.backend.zlibng;
import std;
import conflux.types;
export namespace conflux::compress_backends {

S zlib_ng_gzip_compress(
	SV input) {
	zng_stream zs{};
	if (zng_deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
		return {};
	}
	zs.next_in = reinterpret_cast<u8 const *>(input.data());
	zs.avail_in = static_cast<u32>(input.size());

	S out(zng_deflateBound(&zs, input.size()), '\0');
	zs.next_out = reinterpret_cast<u8 *>(out.data());
	zs.avail_out = static_cast<u32>(out.size());

	int const rc = zng_deflate(&zs, Z_FINISH);
	zng_deflateEnd(&zs);
	if (rc != Z_STREAM_END) {
		return {};
	}
	out.resize(zs.total_out);
	return out;
}

} // namespace conflux::compress_backends
