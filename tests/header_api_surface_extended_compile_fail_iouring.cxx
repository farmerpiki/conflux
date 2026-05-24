// Intentionally invalid: io_uring remains complete-only.
#include <conflux/extended.hxx>

int main() {
	conflux::uring::IoUringCaps caps;
	(void)caps;
}
