module;
#include <isa-l/igzip_lib.h>

export module conflux.net.compress.backend.isal;
import std;
import conflux.types;

export namespace conflux::compress_backends {

std::string isal_gzip_compress(
	std::string_view input) {
	isal_zstream stream{};
	isal_deflate_stateless_init(&stream);
	stream.next_in = reinterpret_cast<u8 *>(const_cast<char *>(input.data()));
	stream.avail_in = static_cast<u32>(input.size());
	stream.level = 1;
	stream.end_of_stream = 1;
	stream.flush = FULL_FLUSH;
	stream.gzip_flag = IGZIP_GZIP;

	std::string out(input.size() + input.size() / 16 + 128, '\0');
	stream.next_out = reinterpret_cast<u8 *>(out.data());
	stream.avail_out = static_cast<u32>(out.size());

	int const rc = isal_deflate_stateless(&stream);
	if (rc != COMP_OK) {
		return {};
	}
	out.resize(stream.total_out);
	return out;
}

} // namespace conflux::compress_backends
