// Intentionally invalid: IoUring should remain hidden behind complete surface.
#include <conflux/curated.hxx>

int main() {
	conflux::uring::IoUringCaps caps;
	(void)caps;
}
