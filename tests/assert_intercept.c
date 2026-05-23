/* LD_PRELOAD interceptor: redirect __assert_fail to exit(42).
 * 42 = "assert fired" sentinel distinguishable from abort (134) and normal exit (0).
 * Must be compiled as a shared library (-shared -fPIC). */
#include <stdlib.h>

void __assert_fail(
	char const *assertion,
	char const *file,
	unsigned line,
	char const *function) {
	(void)assertion;
	(void)file;
	(void)line;
	(void)function;
	exit(42);
}
