// Intentionally invalid: direct-slot pool is hidden even from complete façade.
#include <conflux/complete.hxx>

int main() {
	direct_slot_pool pool;
	(void)pool;
}
