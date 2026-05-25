#pragma once

#include <string>
#include <string_view>

namespace conflux::compress_backends::detail {

template<class Traits>
std::string gzip_compress_zlib_like(
	std::string_view input) {
	typename Traits::Stream stream{};
	if (Traits::init(stream) != Traits::ok()) {
		return {};
	}
	Traits::set_input(stream, input);

	std::string out(Traits::bound(stream, input.size()), '\0');
	Traits::set_output(stream, out);

	int const rc = Traits::deflate(stream);
	Traits::end(stream);
	if (rc != Traits::stream_end()) {
		return {};
	}
	out.resize(Traits::total_out(stream));
	return out;
}

} // namespace conflux::compress_backends::detail
