// Intentionally invalid: io_uring remains complete-only.
#include <conflux/extended.hxx>

int main() {
	static_assert(CONFLUX_SURFACE_HAS_URING, "conflux_api_surface_extended_unexpected_iouring_visible");
}
