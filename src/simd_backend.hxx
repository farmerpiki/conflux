#pragma once

#include <cstddef>

#if defined(CONFLUX_STDSIMD)
extern "C" {
void conflux_ascii_lower_inplace_stdsimd(char *, std::size_t) noexcept;
int conflux_constant_time_eq_stdsimd(unsigned char const *, unsigned char const *, std::size_t) noexcept;
std::size_t conflux_url_scan_plain_run_stdsimd(char const *, std::size_t, int) noexcept;
void conflux_ws_unmask_stdsimd(unsigned char *, std::size_t, unsigned char const *) noexcept;
}
#endif
