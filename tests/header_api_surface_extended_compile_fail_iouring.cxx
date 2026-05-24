// Intentionally invalid: io_uring remains complete-only.
#include <conflux/extended.hxx>

int main() {
	IoUring ring;
	(void)ring;
}
