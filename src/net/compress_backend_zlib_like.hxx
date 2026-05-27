#pragma once

#include <limits>
#include <string>
#include <string_view>

namespace conflux::compress_backends::detail {

template<class Size>
[[nodiscard]] constexpr Size backend_size(
	std::size_t value) noexcept {
	return static_cast<Size>(value);
}

template<class Traits>
std::string gzip_compress_zlib_like(
	std::string_view input) {
	typename Traits::Stream stream{};
	if (Traits::init(stream) != Traits::ok()) {
		return {};
	}
	if (input.size() > Traits::max_avail()) {
		Traits::end(stream);
		return {};
	}
	Traits::set_input(stream, input);

	std::size_t const bound = Traits::bound(stream, input.size());
	if (bound > Traits::max_avail()) {
		Traits::end(stream);
		return {};
	}
	std::string out(bound, '\0');
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
