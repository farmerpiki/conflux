#include <cstddef>
#include <locale.h>
#include <stdlib.h>
#include <sys/random.h>

extern "C" void *conflux_json_new_c_locale() noexcept {
	return ::newlocale(LC_ALL_MASK, "C", nullptr);
}

extern "C" double conflux_json_strtod_c_locale(
	char const *text,
	char **end,
	void *locale) noexcept {
	return ::strtod_l(text, end, static_cast<locale_t>(locale));
}

extern "C" std::ptrdiff_t conflux_json_getrandom(
	void *buffer,
	std::size_t length,
	unsigned int flags) noexcept {
	return ::getrandom(buffer, length, flags);
}
