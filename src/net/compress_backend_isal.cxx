module;
#include <isa-l/igzip_lib.h>

export module conflux.net.compress.backend.isal;
import std;
import conflux.types;
export namespace conflux::compress_backends {

namespace detail {

[[nodiscard]] constexpr std::uint32_t isal_backend_size(
	std::size_t value) noexcept {
	return static_cast<std::uint32_t>(value);
}

} // namespace detail

std::string isal_gzip_compress(
	std::string_view input) {
	if (input.size() > std::numeric_limits<std::uint32_t>::max()) {
		return {};
	}
	isal_zstream stream{};
	isal_deflate_stateless_init(&stream);
	stream.next_in = reinterpret_cast<std::uint8_t *>(const_cast<char *>(input.data()));
	stream.avail_in = detail::isal_backend_size(input.size());
	stream.level = 1;
	stream.end_of_stream = 1;
	stream.flush = FULL_FLUSH;
	stream.gzip_flag = IGZIP_GZIP;

	std::string out(input.size() + input.size() / 16 + 128, '\0');
	if (out.size() > std::numeric_limits<std::uint32_t>::max()) {
		return {};
	}
	stream.next_out = reinterpret_cast<std::uint8_t *>(out.data());
	stream.avail_out = detail::isal_backend_size(out.size());

	int const rc = isal_deflate_stateless(&stream);
	if (rc != COMP_OK) {
		return {};
	}
	out.resize(stream.total_out);
	return out;
}

} // namespace conflux::compress_backends
