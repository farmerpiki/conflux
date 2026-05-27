// Intentionally invalid: IoUring should remain hidden behind complete surface.
#include <conflux/curated.hxx>

int main() {
	static_assert(CONFLUX_SURFACE_HAS_URING, "conflux_api_surface_curated_unexpected_iouring_visible");
}
