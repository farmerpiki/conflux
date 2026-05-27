// Intentionally invalid: direct-slot pool is hidden even from complete façade.
#include <conflux/complete.hxx>

int main() {
	static_assert(
		CONFLUX_SURFACE_HAS_DIRECT_SLOT_POOL,
		"conflux_api_surface_complete_unexpected_direct_slot_pool_visible");
}
